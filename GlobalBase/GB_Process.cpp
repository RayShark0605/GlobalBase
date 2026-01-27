#include "GB_Process.h"
#include <string>
#include <vector>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <unistd.h>
#  include <errno.h>
#  include <fstream>
#  include <iterator>
#endif

namespace
{
#ifdef _WIN32
    static std::wstring QuoteWindowsArg(const std::wstring& arg)
    {
        // 为了稳妥传参：按 CreateProcess 兼容的常见规则进行 quoting/escaping。
        // 目标：确保参数中含空格/制表符/引号时不会被错误拆分。
        const bool needQuotes = (arg.find_first_of(L" \t\"") != std::wstring::npos);
        if (!needQuotes)
        {
            return arg;
        }

        std::wstring result;
        result.push_back(L'"');

        size_t backslashCount = 0;
        for (wchar_t ch : arg)
        {
            if (ch == L'\\')
            {
                backslashCount++;
                continue;
            }

            if (ch == L'"')
            {
                // 先输出 2*n 个反斜杠，再输出转义引号
                result.append(backslashCount * 2, L'\\');
                backslashCount = 0;
                result.append(L"\\\"");
                continue;
            }

            // 普通字符：输出累计的反斜杠 + 字符
            if (backslashCount > 0)
            {
                result.append(backslashCount, L'\\');
                backslashCount = 0;
            }
            result.push_back(ch);
        }

        // 结尾反斜杠：在结束引号前需要翻倍
        if (backslashCount > 0)
        {
            result.append(backslashCount * 2, L'\\');
        }

        result.push_back(L'"');
        return result;
    }

    static std::wstring BuildWindowsParametersFromCommandLine()
    {
        // 用 Unicode 命令行获取 argv（避免 GetCommandLineA 的代码页转换丢失风险）
        const wchar_t* cmdLine = ::GetCommandLineW();
        int argCount = 0;
        LPWSTR* argList = ::CommandLineToArgvW(cmdLine, &argCount);
        if (argList == nullptr || argCount <= 1)
        {
            if (argList != nullptr)
            {
                ::LocalFree(argList);
            }
            return std::wstring();
        }

        struct LocalFreeGuard
        {
            HLOCAL ptr = nullptr;
            ~LocalFreeGuard()
            {
                if (ptr != nullptr)
                {
                    ::LocalFree(ptr);
                    ptr = nullptr;
                }
            }
        };

        LocalFreeGuard guard;
        guard.ptr = argList;

        std::wstring params;
        for (int i = 1; i < argCount; i++)
        {
            if (!params.empty())
            {
                params.push_back(L' ');
            }
            params += QuoteWindowsArg(argList[i]);
        }
        return params;
    }

    static std::wstring GetSelfExePathWindows()
    {
        std::wstring exePath;
        exePath.resize(32768);

        DWORD len = ::GetModuleFileNameW(nullptr, &exePath[0], static_cast<DWORD>(exePath.size()));
        if (len == 0)
        {
            return std::wstring();
        }
        exePath.resize(len);
        return exePath;
    }
#else
    static std::string GetSelfExePathLinux()
    {
        // Linux: /proc/self/exe 指向当前可执行文件
        std::string path;
        path.resize(4096);

        const ssize_t len = ::readlink("/proc/self/exe", &path[0], path.size() - 1);
        if (len <= 0)
        {
            return std::string();
        }
        path.resize(static_cast<size_t>(len));
        return path;
    }

    static std::vector<std::string> ReadProcSelfCmdlineLinux()
    {
        // /proc/self/cmdline: NUL 分隔的 argv 列表
        std::ifstream ifs("/proc/self/cmdline", std::ios::binary);
        if (!ifs)
        {
            return std::vector<std::string>();
        }

        std::vector<char> data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        std::vector<std::string> args;

        std::string current;
        for (char c : data)
        {
            if (c == '\0')
            {
                args.push_back(current);
                current.clear();
            }
            else
            {
                current.push_back(c);
            }
        }

        // 文件通常以 '\0' 结尾
        if (!current.empty())
        {
            args.push_back(current);
        }

        // 可能出现空项，过滤一下
        std::vector<std::string> filtered;
        filtered.reserve(args.size());
        for (const std::string& s : args)
        {
            if (!s.empty())
            {
                filtered.push_back(s);
            }
        }
        return filtered;
    }
#endif

}

bool GB_IsRunningAsAdmin()
{
#ifdef _WIN32
    HANDLE tokenHandle = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle))
    {
        return false;
    }

    struct TokenHandleGuard
    {
        HANDLE handle = nullptr;
        ~TokenHandleGuard()
        {
            if (handle != nullptr)
            {
                ::CloseHandle(handle);
                handle = nullptr;
            }
        }
    };

    TokenHandleGuard tokenGuard;
    tokenGuard.handle = tokenHandle;

    TOKEN_ELEVATION tokenElevation{};
    DWORD returnLength = 0;
    if (::GetTokenInformation(tokenHandle, TokenElevation, &tokenElevation, static_cast<DWORD>(sizeof(tokenElevation)), &returnLength) == FALSE)
    {
        return false;
    }

    // TokenIsElevated 非 0 => 进程 token 具有已提升的管理员权限。
    return tokenElevation.TokenIsElevated != 0;

#else
    // euid == 0 => root 权限
    return ::geteuid() == 0;
#endif
}


bool GB_EnsureRunningAsAdmin()
{
    if (GB_IsRunningAsAdmin())
    {
        return true;
    }

#ifdef _WIN32
    const std::wstring exePath = GetSelfExePathWindows();
    if (exePath.empty())
    {
        return false;
    }

    const std::wstring parameters = BuildWindowsParametersFromCommandLine();

    SHELLEXECUTEINFOW execInfo{};
    execInfo.cbSize = sizeof(execInfo);
    execInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    execInfo.lpVerb = L"runas"; // 触发 UAC 提权启动
    execInfo.lpFile = exePath.c_str();
    execInfo.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    execInfo.nShow = SW_SHOWNORMAL;

    if (::ShellExecuteExW(&execInfo) == FALSE)
    {
        // 常见失败：用户点了“否”（ERROR_CANCELLED=1223）
        return false;
    }

    if (execInfo.hProcess != nullptr)
    {
        ::CloseHandle(execInfo.hProcess);
        execInfo.hProcess = nullptr;
    }

    // 成功启动提权后的新进程 -> 退出当前进程
    ::ExitProcess(0);

    return false; // 理论上不会到达

#else
    // Linux: 用 sudo 重新启动自身。sudo 负责认证/提权。
    const std::string exePath = GetSelfExePathLinux();
    if (exePath.empty())
    {
        return false;
    }

    const std::vector<std::string> currentArgs = ReadProcSelfCmdlineLinux();
    // currentArgs[0] 可能不是绝对路径；这里强制用 /proc/self/exe 得到的 exePath
    std::vector<std::string> newArgs;
    newArgs.reserve(currentArgs.size() + 3);

    newArgs.push_back("sudo");
    newArgs.push_back("--");
    newArgs.push_back(exePath);

    for (size_t i = 1; i < currentArgs.size(); i++)
    {
        newArgs.push_back(currentArgs[i]);
    }

    std::vector<char*> argvPointers;
    argvPointers.reserve(newArgs.size() + 1);
    for (std::string& s : newArgs)
    {
        argvPointers.push_back(&s[0]);
    }
    argvPointers.push_back(nullptr);

    // execvp 成功则不返回；失败返回 -1（errno 可看原因）
    ::execvp("sudo", argvPointers.data());

    return false;
#endif
}
