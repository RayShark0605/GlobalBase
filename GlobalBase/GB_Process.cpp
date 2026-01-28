#include "GB_Process.h"
#include "GB_Utf8String.h"

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#  include <shellapi.h>
#  include <tlhelp32.h>
#  include <psapi.h>

// IMAGE_FILE_MACHINE_* 常量通常由 <winnt.h> 提供（Windows SDK）。
// 为了兼容极少数旧 SDK，这里做一个保守兜底。
#  ifndef IMAGE_FILE_MACHINE_ARM64
#    define IMAGE_FILE_MACHINE_ARM64 0xAA64
#  endif
#else
#  include <dirent.h>
#  include <unistd.h>
#  include <errno.h>
#  include <fstream>
#  include <iterator>
#  include <pwd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

namespace
{
#ifdef _WIN32
    static std::wstring QuoteWindowsArg(const std::wstring& arg)
    {
        // 为了稳妥传参：按 CreateProcess 兼容的常见规则进行 quoting/escaping。
        // 目标：确保参数中含空格/制表符/引号时不会被错误拆分。
        // 注意：空参数必须写成 "" 才能在重启后保持 argv 语义一致。
        const bool needQuotes = arg.empty() || (arg.find_first_of(L" \t\"") != std::wstring::npos);
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
        std::wstring path;
        path.resize(260);

        while (true)
        {
            const DWORD len = ::GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
            if (len == 0)
            {
                return std::wstring();
            }

            // When the buffer is too small, GetModuleFileNameW returns nSize (the buffer length).
            if (len < path.size())
            {
                path.resize(len);
                return path;
            }

            if (path.size() >= 32768)
            {
                return std::wstring();
            }

            path.resize(path.size() * 2);
        }
    }

    static std::wstring GetCurrentDirectoryWindows()
    {
        const DWORD required = ::GetCurrentDirectoryW(0, nullptr);
        if (required == 0)
        {
            return std::wstring();
        }

        std::wstring buffer;
        buffer.resize(required);
        const DWORD len = ::GetCurrentDirectoryW(required, &buffer[0]);
        if (len == 0 || len >= required)
        {
            return std::wstring();
        }

        buffer.resize(len);
        return buffer;
    }
#else
    static std::string GetSelfExePathLinux()
    {
        std::vector<char> buffer(256);
        while (true)
        {
            const ssize_t len = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
            if (len < 0)
            {
                return std::string();
            }

            if (static_cast<size_t>(len) < buffer.size() - 1)
            {
                buffer[static_cast<size_t>(len)] = '\0';
                return std::string(buffer.data(), static_cast<size_t>(len));
            }

            // Sanity limit: avoid runaway allocations on pathological environments.
            if (buffer.size() > (1u << 20))
            {
                return std::string();
            }

            buffer.resize(buffer.size() * 2);
        }
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
        // 注意：argv 中允许出现空参数（""），/proc/self/cmdline 会用连续的 '\0' 表示。
        // 因此这里不能简单过滤空项，否则会破坏原 argv 语义。
        std::vector<std::string> args;
        args.reserve(16);

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

        // 极少数情况下文件不以 '\0' 结尾，这里补齐最后一个参数。
        if (!current.empty())
        {
            args.push_back(current);
        }

        // /proc/self/cmdline 可能为空（例如某些受限容器/异常场景）。
        // 返回原样即可。
        return args;
    }
#endif

#ifdef _WIN32
    struct HandleGuard
    {
        HANDLE handle = nullptr;

        HandleGuard() = default;
        explicit HandleGuard(HANDLE h) : handle(h) {}

        ~HandleGuard()
        {
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(handle);
                handle = nullptr;
            }
        }

        HandleGuard(const HandleGuard&) = delete;
        HandleGuard& operator=(const HandleGuard&) = delete;

        HandleGuard(HandleGuard&& other) noexcept : handle(other.handle)
        {
            other.handle = nullptr;
        }

        HandleGuard& operator=(HandleGuard&& other) noexcept
        {
            if (this != &other)
            {
                if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(handle);
                }
                handle = other.handle;
                other.handle = nullptr;
            }
            return *this;
        }
    };

    static double FileTimeToSeconds(const FILETIME& ft)
    {
        ULARGE_INTEGER ui;
        ui.LowPart = ft.dwLowDateTime;
        ui.HighPart = ft.dwHighDateTime;
        return static_cast<double>(ui.QuadPart) / 10000000.0;
    }

    static long long FileTimeToUnixMs(const FILETIME& ft)
    {
        // FILETIME: 100ns ticks since 1601-01-01.
        // UNIX epoch: 1970-01-01.
        ULARGE_INTEGER ui;
        ui.LowPart = ft.dwLowDateTime;
        ui.HighPart = ft.dwHighDateTime;

        const unsigned long long windowsTicks = ui.QuadPart;
        const unsigned long long epochDiffTicks = 116444736000000000ULL; // 1601->1970
        if (windowsTicks < epochDiffTicks)
        {
            return 0;
        }
        return static_cast<long long>((windowsTicks - epochDiffTicks) / 10000ULL);
    }

    static bool IsOs64Bit()
    {
#if defined(_WIN64)
        return true;
#else
        // 优先使用 IsWow64Process2（能区分 x64/arm64 等更多架构），再回退到 IsWow64Process。
        using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);

        const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        if (kernel32 != nullptr)
        {
            const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(::GetProcAddress(kernel32, "IsWow64Process2"));
            if (isWow64Process2 != nullptr)
            {
                USHORT processMachine = 0;
                USHORT nativeMachine = 0;
                if (isWow64Process2(::GetCurrentProcess(), &processMachine, &nativeMachine))
                {
                    return nativeMachine == IMAGE_FILE_MACHINE_AMD64 || nativeMachine == IMAGE_FILE_MACHINE_ARM64;
                }
            }
        }

        BOOL isWow64 = FALSE;
        if (::IsWow64Process(::GetCurrentProcess(), &isWow64))
        {
            return isWow64 != FALSE;
        }
        return false;
#endif
    }

    static bool IsMachine64Bit(USHORT machine)
    {
        return machine == IMAGE_FILE_MACHINE_AMD64 || machine == IMAGE_FILE_MACHINE_ARM64;
    }

    static bool FillProcessUserName(HANDLE processHandle, std::string& userNameUtf8)
    {
        HandleGuard tokenHandle;
        if (!::OpenProcessToken(processHandle, TOKEN_QUERY, &tokenHandle.handle) || tokenHandle.handle == nullptr)
        {
            return false;
        }

        DWORD tokenInfoLen = 0;
        ::GetTokenInformation(tokenHandle.handle, TokenUser, nullptr, 0, &tokenInfoLen);
        if (tokenInfoLen == 0)
        {
            return false;
        }

        std::vector<unsigned char> tokenInfo(tokenInfoLen);
        if (!::GetTokenInformation(tokenHandle.handle, TokenUser, tokenInfo.data(), tokenInfoLen, &tokenInfoLen))
        {
            return false;
        }

        const TOKEN_USER* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenInfo.data());
        if (tokenUser == nullptr || tokenUser->User.Sid == nullptr)
        {
            return false;
        }

        SID_NAME_USE use = SidTypeUnknown;
        DWORD nameLen = 0;
        DWORD domainLen = 0;

        // First call to obtain required lengths (may include terminating NUL on failure).
        ::LookupAccountSidW(nullptr, tokenUser->User.Sid, nullptr, &nameLen, nullptr, &domainLen, &use);
        if (nameLen == 0)
        {
            return false;
        }

        // LookupAccountSidW 对输出缓冲区的指针/长度较为敏感：
        // - domainLen 可能为 0（例如本地账户），但依然建议提供至少 1 个 wchar 的缓冲区。
        // - 同时避免把 nullptr 传给 API（兼容不同 Windows 版本/实现）。
        std::vector<wchar_t> nameBuffer(std::max<DWORD>(nameLen, 1));
        std::vector<wchar_t> domainBuffer(std::max<DWORD>(domainLen, 1));

        DWORD nameCapacity = static_cast<DWORD>(nameBuffer.size());
        DWORD domainCapacity = static_cast<DWORD>(domainBuffer.size());

        if (!::LookupAccountSidW(nullptr, tokenUser->User.Sid,
            nameBuffer.data(), &nameCapacity,
            domainBuffer.data(), &domainCapacity,
            &use))
        {
            return false;
        }

        nameLen = nameCapacity;
        domainLen = domainCapacity;

        // Defensive trim in case the returned lengths include terminating NUL.
        while (nameLen > 0 && nameBuffer[nameLen - 1] == L'\0')
        {
            nameLen--;
        }
        while (domainLen > 0 && domainBuffer[domainLen - 1] == L'\0')
        {
            domainLen--;
        }

        std::wstring full;
        if (domainLen > 0)
        {
            full.assign(domainBuffer.data(), domainLen);
            full.push_back(L'\\');
        }
        full.append(nameBuffer.data(), nameLen);

        userNameUtf8 = WStringToUtf8(full);
        return !userNameUtf8.empty();
    }

    static bool IsProcessElevated(HANDLE processHandle, bool& isElevated)
    {
        HANDLE tokenHandle = nullptr;
        if (!::OpenProcessToken(processHandle, TOKEN_QUERY, &tokenHandle))
        {
            return false;
        }
        HandleGuard tokenGuard(tokenHandle);

        TOKEN_ELEVATION elevation;
        DWORD size = 0;
        if (!::GetTokenInformation(tokenHandle, TokenElevation, &elevation, sizeof(elevation), &size))
        {
            return false;
        }

        isElevated = (elevation.TokenIsElevated != 0);
        return true;
    }

    static bool FillProcessExePath(HANDLE processHandle, std::string& exePathUtf8)
    {
        std::wstring buffer;
        buffer.resize(32768);
        DWORD size = static_cast<DWORD>(buffer.size());
        if (!::QueryFullProcessImageNameW(processHandle, 0, &buffer[0], &size))
        {
            return false;
        }

        buffer.resize(size);
        exePathUtf8 = WStringToUtf8(buffer);
        return !exePathUtf8.empty();
    }

    static void FillProcessTimes(HANDLE processHandle, GB_ProcessInfo& info)
    {
        FILETIME createTime;
        FILETIME exitTime;
        FILETIME kernelTime;
        FILETIME userTime;
        if (!::GetProcessTimes(processHandle, &createTime, &exitTime, &kernelTime, &userTime))
        {
            return;
        }

        info.cpuKernelSeconds = FileTimeToSeconds(kernelTime);
        info.cpuUserSeconds = FileTimeToSeconds(userTime);
        info.startTimeUnixMs = FileTimeToUnixMs(createTime);
        info.hasCpuTimes = true;
        info.hasStartTime = (info.startTimeUnixMs != 0);
    }

    static void FillProcessMemory(HANDLE processHandle, GB_ProcessInfo& info)
    {
        PROCESS_MEMORY_COUNTERS_EX counters;
        ::ZeroMemory(&counters, sizeof(counters));
        counters.cb = sizeof(counters);

        if (!::GetProcessMemoryInfo(processHandle, reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters), sizeof(counters)))
        {
            return;
        }

        info.residentSetBytes = static_cast<unsigned long long>(counters.WorkingSetSize);
        info.peakResidentSetBytes = static_cast<unsigned long long>(counters.PeakWorkingSetSize);
        info.privateMemoryBytes = static_cast<unsigned long long>(counters.PrivateUsage);

        // 这里用 PagefileUsage 作为“已提交(Commit)字节数”的近似；若为 0 则 fallback 到 PrivateUsage。
        const SIZE_T commitBytes = (counters.PagefileUsage != 0) ? counters.PagefileUsage : counters.PrivateUsage;
        info.virtualMemoryBytes = static_cast<unsigned long long>(commitBytes);
        info.hasMemoryInfo = true;
    }

    static void FillProcessBitness(HANDLE processHandle, GB_ProcessInfo& info)
    {
        if (!IsOs64Bit())
        {
            info.is64Bit = false;
            return;
        }

        // 优先使用 IsWow64Process2：
        // - processMachine == IMAGE_FILE_MACHINE_UNKNOWN 表示不是 WOW（即与 nativeMachine 同架构）。
        // - 否则表示在 WOW 下运行（例如 x86 on x64 / x86 on arm64 / x64 on arm64）。
        using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
        const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        if (kernel32 != nullptr)
        {
            const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(::GetProcAddress(kernel32, "IsWow64Process2"));
            if (isWow64Process2 != nullptr)
            {
                USHORT processMachine = 0;
                USHORT nativeMachine = 0;
                if (isWow64Process2(processHandle, &processMachine, &nativeMachine))
                {
                    // 如果 native 不是 64 位架构，那么目标进程一定不是 64 位。
                    if (!IsMachine64Bit(nativeMachine))
                    {
                        info.is64Bit = false;
                        return;
                    }

                    // 非 WOW：与 native 同架构（在这里可认为是 64 位）。
                    if (processMachine == IMAGE_FILE_MACHINE_UNKNOWN)
                    {
                        info.is64Bit = true;
                        return;
                    }

                    // WOW：目标进程机器码由 processMachine 决定。
                    info.is64Bit = IsMachine64Bit(processMachine);
                    return;
                }
            }
        }

        // 回退到 IsWow64Process（只能判断“是否为 32-bit on 64-bit OS”的经典 WOW64）。
        BOOL isWow64 = FALSE;
        if (!::IsWow64Process(processHandle, &isWow64))
        {
            return;
        }

        info.is64Bit = (isWow64 == FALSE);
    }

    static void FillCurrentProcessCommandLineIfMatch(GB_ProcessInfo& info)
    {
        if (static_cast<DWORD>(info.processId) != ::GetCurrentProcessId())
        {
            return;
        }

        const wchar_t* cmdLine = ::GetCommandLineW();
        if (cmdLine == nullptr)
        {
            return;
        }

        info.commandLineUtf8 = WStringToUtf8(std::wstring(cmdLine));
        info.hasCommandLine = !info.commandLineUtf8.empty();
    }

    static void FillCurrentProcessWorkingDirectoryIfMatch(GB_ProcessInfo& info)
    {
        if (static_cast<DWORD>(info.processId) != ::GetCurrentProcessId())
        {
            return;
        }

        const DWORD required = ::GetCurrentDirectoryW(0, nullptr);
        if (required == 0)
        {
            return;
        }

        std::wstring buffer;
        buffer.resize(required);
        const DWORD len = ::GetCurrentDirectoryW(required, &buffer[0]);
        if (len == 0 || len >= required)
        {
            return;
        }

        buffer.resize(len);
        info.workingDirectoryUtf8 = WStringToUtf8(buffer);
        info.hasWorkingDirectory = !info.workingDirectoryUtf8.empty();
    }

#else
    static bool IsAllDigits(const char* s)
    {
        if (s == nullptr || *s == '\0')
        {
            return false;
        }

        for (const char* p = s; *p != '\0'; p++)
        {
            if (!std::isdigit(static_cast<unsigned char>(*p)))
            {
                return false;
            }
        }
        return true;
    }

    static bool ReadFileToString(const std::string& path, std::string& content)
    {
        std::ifstream ifs(path.c_str(), std::ios::binary);
        if (!ifs)
        {
            return false;
        }

        content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        return true;
    }

    static std::string TrimRightNewlines(std::string s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '\0'))
        {
            s.pop_back();
        }
        return s;
    }

    static bool ReadLinkUtf8(const std::string& path, std::string& targetUtf8)
    {
        std::vector<char> buffer(256);
        while (true)
        {
            const ssize_t len = ::readlink(path.c_str(), buffer.data(), buffer.size() - 1);
            if (len < 0)
            {
                return false;
            }

            if (static_cast<size_t>(len) < buffer.size() - 1)
            {
                buffer[static_cast<size_t>(len)] = '\0';
                targetUtf8.assign(buffer.data(), static_cast<size_t>(len));
                return !targetUtf8.empty();
            }

            // Sanity limit: avoid runaway allocations on pathological environments.
            if (buffer.size() > (1u << 20))
            {
                return false;
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    static std::string QuotePosixShellArg(const std::string& arg)
    {
        // 用于把 argv 元素拼成一个“可读、尽量不歧义”的命令行字符串。
        // 采用 POSIX shell 的单引号引用规则：
        // - 空字符串 -> ''
        // - 含单引号的情况：'foo'\''bar'
        if (arg.empty())
        {
            return "''";
        }

        bool isSafe = true;
        for (char ch : arg)
        {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if ((uch >= 'a' && uch <= 'z') ||
                (uch >= 'A' && uch <= 'Z') ||
                (uch >= '0' && uch <= '9') ||
                ch == '_' || ch == '-' || ch == '.' || ch == '/' || ch == ':' || ch == '+' || ch == '@')
            {
                continue;
            }

            isSafe = false;
            break;
        }

        if (isSafe)
        {
            return arg;
        }

        std::string quoted;
        quoted.reserve(arg.size() + 2);
        quoted.push_back('\'');
        for (char ch : arg)
        {
            if (ch == '\'')
            {
                quoted.append("'\\''");
            }
            else
            {
                quoted.push_back(ch);
            }
        }
        quoted.push_back('\'');
        return quoted;
    }

    static bool ReadProcCmdline(int pid, std::string& commandLineUtf8)
    {
        std::string data;
        const std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
        if (!ReadFileToString(path, data) || data.empty())
        {
            return false;
        }

        // /proc/<pid>/cmdline：NUL 分隔的 argv 列表。
        // 注意：argv 允许出现空参数（""），因此不能丢弃空项。
        std::vector<std::string> args;
        args.reserve(16);

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

        // 极少数情况下文件不以 '\0' 结尾，这里补齐最后一个参数。
        if (!current.empty() || (data.size() > 0 && data.back() != '\0'))
        {
            args.push_back(current);
        }

        if (args.empty())
        {
            return false;
        }

        // 用 POSIX shell 风格引用后拼接，得到更可读/更不易歧义的命令行字符串。
        std::ostringstream oss;
        bool first = true;
        for (const std::string& arg : args)
        {
            if (!first)
            {
                oss << ' ';
            }
            oss << QuotePosixShellArg(arg);
            first = false;
        }

        commandLineUtf8 = oss.str();
        return !commandLineUtf8.empty();
    }

    static bool ReadProcComm(int pid, std::string& commUtf8)
    {
        const std::string path = "/proc/" + std::to_string(pid) + "/comm";
        std::string content;
        if (!ReadFileToString(path, content))
        {
            return false;
        }
        commUtf8 = TrimRightNewlines(content);
        return !commUtf8.empty();
    }

    static long long ReadBootTimeUnixSeconds()
    {
        std::string content;
        if (!ReadFileToString("/proc/stat", content))
        {
            return 0;
        }

        std::istringstream iss(content);
        std::string key;
        while (iss >> key)
        {
            if (key == "btime")
            {
                long long btime = 0;
                iss >> btime;
                return btime;
            }

            // 跳过当前行剩余内容
            std::string rest;
            std::getline(iss, rest);
        }

        return 0;
    }

    static bool ParseProcStat(const std::string& statContent, GB_ProcessInfo& info, long long bootTimeSeconds)
    {
        // 参考 proc(5)：格式为 "pid (comm) state ..."
        const size_t leftParen = statContent.find('(');
        const size_t rightParen = statContent.rfind(')');
        if (leftParen == std::string::npos || rightParen == std::string::npos || rightParen <= leftParen)
        {
            return false;
        }

        // comm 可能含空格，因此用括号定位
        if (info.processNameUtf8.empty())
        {
            info.processNameUtf8 = statContent.substr(leftParen + 1, rightParen - leftParen - 1);
        }

        const std::string after = statContent.substr(rightParen + 1);
        std::istringstream iss(after);
        std::vector<std::string> fields;
        fields.reserve(64);
        std::string token;
        while (iss >> token)
        {
            fields.push_back(token);
        }

        // 至少需要到 rss（第 24 字段）
        // 映射（fields 从 state 开始）：
        // 0:state 1:ppid ... 11:utime 12:stime ... 17:num_threads 19:starttime 20:vsize 21:rss
        if (fields.size() < 22)
        {
            return false;
        }

        const char stateChar = fields[0].empty() ? '\0' : fields[0][0];
        if (stateChar != '\0')
        {
            info.stateUtf8 = std::string(1, stateChar);
        }

        info.parentProcessId = std::atoi(fields[1].c_str());

        const long long utimeTicks = std::atoll(fields[11].c_str());
        const long long stimeTicks = std::atoll(fields[12].c_str());
        const int clockTicksPerSecond = static_cast<int>(::sysconf(_SC_CLK_TCK));
        if (clockTicksPerSecond > 0)
        {
            info.cpuUserSeconds = static_cast<double>(utimeTicks) / static_cast<double>(clockTicksPerSecond);
            info.cpuKernelSeconds = static_cast<double>(stimeTicks) / static_cast<double>(clockTicksPerSecond);
            info.hasCpuTimes = true;
        }

        info.niceValue = std::atoi(fields[16].c_str());
        info.threadCount = static_cast<unsigned int>(std::strtoul(fields[17].c_str(), nullptr, 10));

        const long long startTimeTicks = std::atoll(fields[19].c_str());
        if (bootTimeSeconds > 0 && clockTicksPerSecond > 0)
        {
            const double startSecondsSinceBoot = static_cast<double>(startTimeTicks) / static_cast<double>(clockTicksPerSecond);
            const long long startUnixMs = static_cast<long long>((static_cast<double>(bootTimeSeconds) + startSecondsSinceBoot) * 1000.0);
            info.startTimeUnixMs = startUnixMs;
            info.hasStartTime = (startUnixMs != 0);
        }

        const unsigned long long vsizeBytes = static_cast<unsigned long long>(std::strtoull(fields[20].c_str(), nullptr, 10));
        const long long rssPages = std::atoll(fields[21].c_str());
        const long long pageSize = ::sysconf(_SC_PAGESIZE);
        if (pageSize > 0 && rssPages >= 0)
        {
            info.virtualMemoryBytes = vsizeBytes;
            info.residentSetBytes = static_cast<unsigned long long>(rssPages) * static_cast<unsigned long long>(pageSize);
            info.hasMemoryInfo = true;
        }

        return true;
    }

    struct ProcStatusParsed
    {
        bool hasUids = false;
        unsigned int realUid = 0;
        unsigned int effectiveUid = 0;

        bool hasVmSize = false;
        bool hasVmRss = false;
        bool hasVmHwm = false;
        bool hasRssAnon = false;

        unsigned long long vmSizeBytes = 0;
        unsigned long long vmRssBytes = 0;
        unsigned long long vmHwmBytes = 0;
        unsigned long long rssAnonBytes = 0;
    };

    static bool TryParseProcStatusKbLine(const std::string& line, const char* keyWithColon, unsigned long long& bytes)
    {
        // 示例："VmRSS:\t   1234 kB" -> 1234 * 1024
        const size_t keyLen = std::strlen(keyWithColon);
        if (line.size() < keyLen || line.compare(0, keyLen, keyWithColon) != 0)
        {
            return false;
        }

        std::istringstream ls(line.substr(keyLen));
        unsigned long long valueKb = 0;
        std::string unit;
        ls >> valueKb >> unit;
        if (!ls)
        {
            return false;
        }

        // /proc/status 的这些字段单位一般为 kB；这里不强依赖 unit 字面值。
        bytes = valueKb * 1024ULL;
        return true;
    }

    static void ParseProcStatusContent(const std::string& statusContent, ProcStatusParsed& parsed)
    {
        std::istringstream iss(statusContent);
        std::string line;
        while (std::getline(iss, line))
        {
            if (!parsed.hasUids && line.rfind("Uid:", 0) == 0)
            {
                std::istringstream ls(line.substr(4));
                // Uid:    real    effective   saved    fs
                ls >> parsed.realUid;
                ls >> parsed.effectiveUid;
                parsed.hasUids = static_cast<bool>(ls);
                continue;
            }

            unsigned long long bytes = 0;
            if (!parsed.hasVmSize && TryParseProcStatusKbLine(line, "VmSize:", bytes))
            {
                parsed.vmSizeBytes = bytes;
                parsed.hasVmSize = true;
                continue;
            }
            if (!parsed.hasVmRss && TryParseProcStatusKbLine(line, "VmRSS:", bytes))
            {
                parsed.vmRssBytes = bytes;
                parsed.hasVmRss = true;
                continue;
            }
            if (!parsed.hasVmHwm && TryParseProcStatusKbLine(line, "VmHWM:", bytes))
            {
                parsed.vmHwmBytes = bytes;
                parsed.hasVmHwm = true;
                continue;
            }
            if (!parsed.hasRssAnon && TryParseProcStatusKbLine(line, "RssAnon:", bytes))
            {
                parsed.rssAnonBytes = bytes;
                parsed.hasRssAnon = true;
                continue;
            }
        }
    }

    static bool GetOpenFileDescriptorCountLinux(int pid, unsigned int& fdCount)
    {
        const std::string fdDirPath = "/proc/" + std::to_string(pid) + "/fd";
        DIR* fdDir = ::opendir(fdDirPath.c_str());
        if (fdDir == nullptr)
        {
            return false;
        }

        struct FdDirGuard
        {
            DIR* dir = nullptr;
            explicit FdDirGuard(DIR* d) : dir(d) {}
            ~FdDirGuard()
            {
                if (dir != nullptr)
                {
                    ::closedir(dir);
                    dir = nullptr;
                }
            }
        } guard(fdDir);

        unsigned int count = 0;
        struct dirent* entry = nullptr;
        while ((entry = ::readdir(fdDir)) != nullptr)
        {
            // 过滤 "." / ".."
            if (entry->d_name[0] == '.' && (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            {
                continue;
            }
            count++;
        }

        fdCount = count;
        return true;
    }
    static bool FillUserNameFromUid(unsigned int uid, std::string& userNameUtf8)
    {
        // getpwuid_r is thread-safe; getpwuid uses static storage.
        struct passwd pwd;
        struct passwd* result = nullptr;

        long bufferSize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufferSize < 0)
        {
            bufferSize = 16384;
        }

        std::vector<char> buffer(static_cast<size_t>(bufferSize));

        while (true)
        {
            result = nullptr;
            const int rc = ::getpwuid_r(static_cast<uid_t>(uid), &pwd, buffer.data(), buffer.size(), &result);
            if (rc == 0 && result != nullptr && result->pw_name != nullptr)
            {
                userNameUtf8 = result->pw_name;
                return !userNameUtf8.empty();
            }

            // 缓冲区不足：按 2 倍扩容重试
            if (rc == ERANGE)
            {
                if (buffer.size() > (1u << 20))
                {
                    return false;
                }
                buffer.resize(buffer.size() * 2);
                continue;
            }

            return false;
        }
    }

    static bool IsElf64Bit(const std::string& exePathUtf8, bool& is64Bit)
    {
        std::ifstream ifs(exePathUtf8.c_str(), std::ios::binary);
        if (!ifs)
        {
            return false;
        }

        unsigned char ident[5] = { 0 };
        ifs.read(reinterpret_cast<char*>(ident), sizeof(ident));
        if (ifs.gcount() != static_cast<std::streamsize>(sizeof(ident)))
        {
            return false;
        }

        // ELF magic: 0x7F 'E' 'L' 'F'
        if (ident[0] != 0x7F || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F')
        {
            return false;
        }

        // EI_CLASS: 1=32-bit, 2=64-bit
        is64Bit = (ident[4] == 2);
        return true;
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

    // ShellExecuteExW 默认的启动目录不一定与当前进程一致（尤其是控制台/快捷方式场景）。
    // 为了让提权后的新进程尽可能保持行为一致，显式传递当前工作目录。
    const std::wstring workingDirectory = GetCurrentDirectoryWindows();

    SHELLEXECUTEINFOW execInfo{};
    execInfo.cbSize = sizeof(execInfo);
    execInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    execInfo.lpVerb = L"runas"; // 触发 UAC 提权启动
    execInfo.lpFile = exePath.c_str();
    execInfo.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    execInfo.lpDirectory = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
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
    for (const std::string& s : newArgs)
    {
        argvPointers.push_back(const_cast<char*>(s.c_str()));
    }
    argvPointers.push_back(nullptr);

    // execvp 成功则不返回；失败返回 -1（errno 可看原因）
    ::execvp("sudo", argvPointers.data());

    return false;
#endif
}

std::vector<GB_ProcessInfo> GB_GetAllProcessesInfo()
{
    std::vector<GB_ProcessInfo> processesInfo;

#ifdef _WIN32
    HandleGuard snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.handle == INVALID_HANDLE_VALUE)
    {
        return processesInfo;
    }

    PROCESSENTRY32W pe;
    ::ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);

    if (!::Process32FirstW(snapshot.handle, &pe))
    {
        return processesInfo;
    }

    processesInfo.reserve(256);

    do
    {
        GB_ProcessInfo info;
        info.processId = static_cast<int>(pe.th32ProcessID);
        info.parentProcessId = static_cast<int>(pe.th32ParentProcessID);
        info.threadCount = static_cast<unsigned int>(pe.cntThreads);
        info.processNameUtf8 = WStringToUtf8(std::wstring(pe.szExeFile));

        const DWORD desiredAccess = PROCESS_QUERY_LIMITED_INFORMATION;
        HandleGuard processHandle(::OpenProcess(desiredAccess, FALSE, pe.th32ProcessID));
        if (processHandle.handle != nullptr)
        {
            FillProcessBitness(processHandle.handle, info);

            std::string exePath;
            if (FillProcessExePath(processHandle.handle, exePath))
            {
                info.executablePathUtf8 = exePath;
                info.hasExecutablePath = true;
            }

            std::string userName;
            if (FillProcessUserName(processHandle.handle, userName))
            {
                info.userNameUtf8 = userName;
                info.hasUserName = true;
            }

            bool elevated = false;
            if (IsProcessElevated(processHandle.handle, elevated))
            {
                info.isElevated = elevated;
            }

            DWORD handleCount = 0;
            if (::GetProcessHandleCount(processHandle.handle, &handleCount))
            {
                info.handleCount = static_cast<unsigned int>(handleCount);
            }

            const DWORD priority = ::GetPriorityClass(processHandle.handle);
            if (priority != 0)
            {
                info.priorityClass = static_cast<unsigned int>(priority);
            }

            FillProcessTimes(processHandle.handle, info);
            FillProcessMemory(processHandle.handle, info);
        }

        // Windows 下没有公开的“读取任意进程命令行”API；仅对当前进程填充。
        FillCurrentProcessCommandLineIfMatch(info);
        FillCurrentProcessWorkingDirectoryIfMatch(info);

        processesInfo.push_back(std::move(info));
    } while (::Process32NextW(snapshot.handle, &pe));

#else
    const long long bootTimeSeconds = ReadBootTimeUnixSeconds();

    DIR* procDir = ::opendir("/proc");
    if (procDir == nullptr)
    {
        return processesInfo;
    }

    struct DirGuard
    {
        DIR* dir = nullptr;
        explicit DirGuard(DIR* d) : dir(d) {}
        ~DirGuard()
        {
            if (dir != nullptr)
            {
                ::closedir(dir);
                dir = nullptr;
            }
        }
    } dirGuard(procDir);

    struct dirent* entry = nullptr;
    while ((entry = ::readdir(procDir)) != nullptr)
    {
        if (!IsAllDigits(entry->d_name))
        {
            continue;
        }

        const int pid = std::atoi(entry->d_name);
        if (pid <= 0)
        {
            continue;
        }

        GB_ProcessInfo info;
        info.processId = pid;

        std::string comm;
        if (ReadProcComm(pid, comm))
        {
            info.processNameUtf8 = comm;
        }

        std::string statContent;
        if (ReadFileToString("/proc/" + std::to_string(pid) + "/stat", statContent))
        {
            ParseProcStat(statContent, info, bootTimeSeconds);
        }

        std::string cmdline;
        if (ReadProcCmdline(pid, cmdline))
        {
            info.commandLineUtf8 = cmdline;
            info.hasCommandLine = true;
        }

        std::string exePath;
        if (ReadLinkUtf8("/proc/" + std::to_string(pid) + "/exe", exePath))
        {
            info.executablePathUtf8 = exePath;
            info.hasExecutablePath = true;

            bool is64Bit = false;
            if (IsElf64Bit(exePath, is64Bit))
            {
                info.is64Bit = is64Bit;
            }
        }

        std::string cwdPath;
        if (ReadLinkUtf8("/proc/" + std::to_string(pid) + "/cwd", cwdPath))
        {
            info.workingDirectoryUtf8 = cwdPath;
            info.hasWorkingDirectory = true;
        }

        // /proc/<pid>/status：补齐用户信息、峰值内存、匿名 RSS（近似 private bytes）等。
        std::string statusContent;
        if (ReadFileToString("/proc/" + std::to_string(pid) + "/status", statusContent))
        {
            ProcStatusParsed parsed;
            ParseProcStatusContent(statusContent, parsed);

            if (parsed.hasUids)
            {
                std::string userName;
                // 更贴近“当前权限”的语义：优先用有效 UID 获取用户名。
                if (FillUserNameFromUid(parsed.effectiveUid, userName))
                {
                    info.userNameUtf8 = userName;
                    info.hasUserName = true;
                }

                info.isElevated = (parsed.effectiveUid == 0);
            }

            // 内存：若 stat 里没读到（或想补齐 peak/private），尝试从 status 解析。
            if (parsed.hasVmHwm)
            {
                info.peakResidentSetBytes = parsed.vmHwmBytes;
                info.hasMemoryInfo = true;
            }
            if (parsed.hasRssAnon)
            {
                info.privateMemoryBytes = parsed.rssAnonBytes;
                info.hasMemoryInfo = true;
            }

            if (info.virtualMemoryBytes == 0 && parsed.hasVmSize)
            {
                info.virtualMemoryBytes = parsed.vmSizeBytes;
            }
            if (info.residentSetBytes == 0 && parsed.hasVmRss)
            {
                info.residentSetBytes = parsed.vmRssBytes;
            }

            if ((parsed.hasVmSize || parsed.hasVmRss) && (info.virtualMemoryBytes != 0 || info.residentSetBytes != 0))
            {
                info.hasMemoryInfo = true;
            }
        }

        // Linux 下没有“Windows HandleCount”的统一概念；这里用打开的文件描述符数量作为近似。
        unsigned int fdCount = 0;
        if (GetOpenFileDescriptorCountLinux(pid, fdCount))
        {
            info.handleCount = fdCount;
        }

        processesInfo.push_back(std::move(info));
    }

#endif

    return processesInfo;
}
