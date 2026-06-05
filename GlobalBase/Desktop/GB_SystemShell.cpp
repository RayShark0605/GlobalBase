#include "GB_SystemShell.h"
#include "GB_ComScope.h"
#include "GB_WinHandleScope.h"
#include "GB_WindowsCommandLineInternal.h"
#include "../GB_FileSystem.h"
#include "../GB_Utf8String.h"

#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <objbase.h>
#  include <shellapi.h>
#  include <shlobj.h>

#  ifdef _MSC_VER
#    pragma comment(lib, "Shell32.lib")
#    pragma comment(lib, "Ole32.lib")
#  endif
#endif

namespace
{
    enum class ShellTargetMode
    {
        Generic,
        Url,
        SettingsUri,
        ExistingFile,
        ExistingDirectory,
        Application
    };

#if defined(_WIN32)
    struct PreparedShellExecute
    {
        std::wstring target;
        std::wstring parameters;
        std::wstring workingDirectory;
        const wchar_t* verb = nullptr;
        int showCommand = 0;
        DWORD waitMilliseconds = 0;
        bool waitForExit = false;
        bool requestProcessHandle = false;
    };
#endif

    static bool ContainsNullCharacter(const std::string& text)
    {
        return text.find('\0') != std::string::npos;
    }

    static char ToLowerAscii(const char character)
    {
        if (character >= 'A' && character <= 'Z')
        {
            return static_cast<char>(character - 'A' + 'a');
        }
        return character;
    }

    static bool IsAsciiSchemeFirstCharacter(const char character)
    {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
    }

    static bool IsAsciiSchemeCharacter(const char character)
    {
        return IsAsciiSchemeFirstCharacter(character) || (character >= '0' && character <= '9') || character == '+' || character == '-' || character == '.';
    }

    static bool IsWindowsDrivePath(const std::string& text)
    {
        return text.size() >= 2 && IsAsciiSchemeFirstCharacter(text[0]) && text[1] == ':';
    }

    static bool TryGetUriScheme(const std::string& text, std::string& scheme)
    {
        scheme.clear();
        if (text.empty() || IsWindowsDrivePath(text) || !IsAsciiSchemeFirstCharacter(text[0]))
        {
            return false;
        }

        const size_t colonIndex = text.find(':');
        if (colonIndex == std::string::npos)
        {
            return false;
        }

        for (size_t index = 1; index < colonIndex; index++)
        {
            if (!IsAsciiSchemeCharacter(text[index]))
            {
                return false;
            }
        }

        scheme.reserve(colonIndex);
        for (size_t index = 0; index < colonIndex; index++)
        {
            scheme.push_back(ToLowerAscii(text[index]));
        }
        return true;
    }

    static bool IsDefaultAllowedUriScheme(const std::string& scheme)
    {
        return scheme == "http" || scheme == "https" || scheme == "file" || scheme == "ms-settings";
    }

    static bool IsOpenUrlAllowedScheme(const std::string& scheme)
    {
        return scheme == "http" || scheme == "https" || scheme == "file";
    }

    static bool IsPathLikeTarget(const std::string& target)
    {
        return IsWindowsDrivePath(target) || target.find('/') != std::string::npos || target.find('\\') != std::string::npos || target == "." || target == "..";
    }

    static GB_SystemResult MakeUnsupportedPlatformResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, u8"GB_SystemShell 仅在 Windows 平台提供实际 Shell 启动能力。");
    }

    static GB_SystemResult ValidateUtf8String(const std::string& text, const std::string& parameterName, const bool allowEmpty, const std::string& operationName)
    {
        if (!allowEmpty && text.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, parameterName + u8" 不能为空。");
        }
        if (ContainsNullCharacter(text))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, parameterName + u8" 不能包含嵌入式 NUL 字符。");
        }
        if (!GB_IsUtf8(text))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, operationName, parameterName + u8" 不是合法的 UTF-8 字节序列。");
        }
        if (!allowEmpty && GB_Utf8Trim(text).empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, parameterName + u8" 不能只包含空白字符。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult ConvertUtf8ToWide(const std::string& text, std::wstring& wideText, const std::string& parameterName, const std::string& operationName)
    {
        try
        {
            wideText = GB_Utf8ToWString(text);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, parameterName + u8" 从 UTF-8 转换为 UTF-16 时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, operationName, parameterName + u8" 从 UTF-8 转换为 UTF-16 失败。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

#if defined(_WIN32)
    static GB_SystemResult ResolveAbsolutePath(const std::string& path, std::string& absolutePathUtf8, std::wstring& absolutePath, const std::string& parameterName, const std::string& operationName)
    {
        std::wstring pathWide;
        GB_SystemResult convertResult = ConvertUtf8ToWide(path, pathWide, parameterName, operationName);
        if (convertResult.IsFailed())
        {
            return convertResult;
        }

        const DWORD maximumFullPathCharacterCount = 32768;
        std::vector<wchar_t> buffer;
        try
        {
            buffer.resize(static_cast<size_t>(maximumFullPathCharacterCount));
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, parameterName + u8" 转换为绝对路径时内存不足。");
        }

        const DWORD copiedLength = ::GetFullPathNameW(pathWide.c_str(), maximumFullPathCharacterCount, buffer.data(), nullptr);
        if (copiedLength == 0)
        {
            return GB_SystemResult::FromLastWin32Error(operationName, parameterName + u8" 转换为绝对路径失败。");
        }
        if (copiedLength >= maximumFullPathCharacterCount)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, parameterName + u8" 转换后的绝对路径超过 Windows 可支持的最大长度。");
        }
        absolutePath.assign(buffer.data(), copiedLength);

        try
        {
            absolutePathUtf8 = GB_WStringToUtf8(absolutePath);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, parameterName + u8" 的绝对路径从 UTF-16 转换为 UTF-8 时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, operationName, parameterName + u8" 的绝对路径从 UTF-16 转换为 UTF-8 失败。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    static const wchar_t* GetVerbText(const GB_ShellExecuteVerb verb)
    {
        switch (verb)
        {
        case GB_ShellExecuteVerb::Default:
            return nullptr;
        case GB_ShellExecuteVerb::Open:
            return L"open";
        case GB_ShellExecuteVerb::Explore:
            return L"explore";
        case GB_ShellExecuteVerb::Edit:
            return L"edit";
        case GB_ShellExecuteVerb::Print:
            return L"print";
        case GB_ShellExecuteVerb::RunAs:
            return L"runas";
        default:
            break;
        }
        return nullptr;
    }

    static int GetShowCommand(const GB_ShellShowMode showMode)
    {
        switch (showMode)
        {
        case GB_ShellShowMode::Default:
            return SW_SHOWDEFAULT;
        case GB_ShellShowMode::Hide:
            return SW_HIDE;
        case GB_ShellShowMode::Normal:
            return SW_SHOWNORMAL;
        case GB_ShellShowMode::Minimized:
            return SW_SHOWMINIMIZED;
        case GB_ShellShowMode::Maximized:
            return SW_SHOWMAXIMIZED;
        default:
            break;
        }
        return SW_SHOWNORMAL;
    }

    static DWORD GetShellExecuteFailureCode(const SHELLEXECUTEINFOW& executeInfo)
    {
        const DWORD lastError = ::GetLastError();
        if (lastError != ERROR_SUCCESS)
        {
            return lastError;
        }

        switch (reinterpret_cast<INT_PTR>(executeInfo.hInstApp))
        {
        case SE_ERR_FNF:
            return ERROR_FILE_NOT_FOUND;
        case SE_ERR_PNF:
            return ERROR_PATH_NOT_FOUND;
        case SE_ERR_ACCESSDENIED:
            return ERROR_ACCESS_DENIED;
        case SE_ERR_OOM:
            return ERROR_NOT_ENOUGH_MEMORY;
        case SE_ERR_DLLNOTFOUND:
            return ERROR_DLL_NOT_FOUND;
        case SE_ERR_SHARE:
            return ERROR_SHARING_VIOLATION;
        case SE_ERR_ASSOCINCOMPLETE:
        case SE_ERR_NOASSOC:
            return ERROR_NO_ASSOCIATION;
        case SE_ERR_DDETIMEOUT:
            return ERROR_TIMEOUT;
        case SE_ERR_DDEBUSY:
            return ERROR_BUSY;
        case SE_ERR_DDEFAIL:
            return ERROR_DDE_FAIL;
        default:
            break;
        }
        return ERROR_GEN_FAILURE;
    }
#endif

    static GB_SystemResult ValidateAndPrepareTarget(const GB_ShellExecuteOptions& options, const ShellTargetMode targetMode, std::wstring& target, const std::string& operationName)
    {
        GB_SystemResult validationResult = ValidateUtf8String(options.target, u8"target", false, operationName);
        if (validationResult.IsFailed())
        {
            return validationResult;
        }

        std::string uriScheme;
        const bool hasUriScheme = TryGetUriScheme(options.target, uriScheme);
        const bool pathLikeTarget = IsPathLikeTarget(options.target);
        const bool hasSurroundingWhitespace = GB_Utf8Trim(options.target) != options.target;
        if (targetMode == ShellTargetMode::Url)
        {
            if (hasSurroundingWhitespace)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"URL 不能包含首尾空白字符。");
            }
            if (!hasUriScheme || !IsOpenUrlAllowedScheme(uriScheme))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"URL 必须使用 http、https 或 file scheme。");
            }
            return ConvertUtf8ToWide(options.target, target, u8"target", operationName);
        }

        if (targetMode == ShellTargetMode::SettingsUri)
        {
            if (hasSurroundingWhitespace)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"设置 URI 不能包含首尾空白字符。");
            }
            if (!hasUriScheme || uriScheme != "ms-settings")
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"设置 URI 必须使用 ms-settings scheme。");
            }
            return ConvertUtf8ToWide(options.target, target, u8"target", operationName);
        }

#if !defined(_WIN32)
        (void)targetMode;
        (void)hasUriScheme;
        (void)uriScheme;
        (void)pathLikeTarget;
        (void)hasSurroundingWhitespace;
        (void)target;
        return MakeUnsupportedPlatformResult(operationName);
#else
        const bool mustResolvePath = targetMode == ShellTargetMode::ExistingFile || targetMode == ShellTargetMode::ExistingDirectory || (targetMode == ShellTargetMode::Application && pathLikeTarget) || (targetMode == ShellTargetMode::Generic && pathLikeTarget);

        if (hasSurroundingWhitespace && (hasUriScheme || !pathLikeTarget))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"URI 或应用程序别名不能包含首尾空白字符。");
        }

        if (targetMode == ShellTargetMode::Application && hasUriScheme)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"应用程序目标不能是 URI。");
        }

        if (targetMode == ShellTargetMode::Generic && hasUriScheme)
        {
            if (options.verb != GB_ShellExecuteVerb::Default && options.verb != GB_ShellExecuteVerb::Open)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"URI 目标只允许使用 Default 或 Open verb。");
            }
            if (!options.allowUnsafeUriScheme && !IsDefaultAllowedUriScheme(uriScheme))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::PermissionDenied, operationName, u8"URI scheme 不在默认安全白名单中；如确有需要，必须显式设置 allowUnsafeUriScheme。");
            }
            return ConvertUtf8ToWide(options.target, target, u8"target", operationName);
        }

        if (!mustResolvePath)
        {
            return ConvertUtf8ToWide(options.target, target, u8"target", operationName);
        }

        std::string absolutePathUtf8;
        GB_SystemResult resolveResult = ResolveAbsolutePath(options.target, absolutePathUtf8, target, u8"target", operationName);
        if (resolveResult.IsFailed())
        {
            return resolveResult;
        }

        const GB_FileType fileType = GB_GetFileType(absolutePathUtf8);
        if (fileType == GB_FileType::NotExists)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"目标路径不存在：" + absolutePathUtf8);
        }
        if (targetMode == ShellTargetMode::ExistingFile && fileType != GB_FileType::RegularFile)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"目标路径不是常规文件：" + absolutePathUtf8);
        }
        if (targetMode == ShellTargetMode::ExistingDirectory && fileType != GB_FileType::Directory)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"目标路径不是目录：" + absolutePathUtf8);
        }
        if (targetMode == ShellTargetMode::Application && fileType != GB_FileType::RegularFile)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"应用程序路径不是常规文件：" + absolutePathUtf8);
        }
        if (targetMode == ShellTargetMode::Generic)
        {
            if (options.verb == GB_ShellExecuteVerb::Explore && fileType != GB_FileType::Directory)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"Explore verb 的目标必须是目录。");
            }
            if ((options.verb == GB_ShellExecuteVerb::Edit || options.verb == GB_ShellExecuteVerb::Print || options.verb == GB_ShellExecuteVerb::RunAs) && fileType != GB_FileType::RegularFile)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"当前 Shell verb 的路径目标必须是常规文件。");
            }
        }
        return GB_SystemResult::Succeeded(operationName);
#endif
    }

#if defined(_WIN32)
    static GB_SystemResult PrepareExecute(const GB_ShellExecuteOptions& options, const ShellTargetMode targetMode, const bool hasExecuteResult, PreparedShellExecute& preparedExecute, const std::string& operationName)
    {
        if (!GB_SystemShell::IsValidExecuteVerbValue(static_cast<uint64_t>(options.verb)))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"verb 枚举值非法。");
        }
        if (!GB_SystemShell::IsValidShowModeValue(static_cast<uint64_t>(options.showMode)))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"showMode 枚举值非法。");
        }
        if (options.waitTimeoutMilliseconds < -1)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"waitTimeoutMilliseconds 只能为 -1 或非负数。");
        }
        if (options.waitTimeoutMilliseconds > static_cast<int64_t>(MAXDWORD - 1))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"waitTimeoutMilliseconds 超出 WaitForSingleObject 可表达范围。");
        }

        GB_SystemResult targetResult = ValidateAndPrepareTarget(options, targetMode, preparedExecute.target, operationName);
        if (targetResult.IsFailed())
        {
            return targetResult;
        }

        std::vector<std::wstring> wideArguments;
        try
        {
            wideArguments.reserve(options.arguments.size());
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"为应用程序参数分配内存失败。");
        }

        for (size_t argumentIndex = 0; argumentIndex < options.arguments.size(); argumentIndex++)
        {
            const std::string parameterName = u8"arguments[" + std::to_string(argumentIndex) + "]";
            const std::string& argument = options.arguments[argumentIndex];
            GB_SystemResult validationResult = ValidateUtf8String(argument, parameterName, true, operationName);
            if (validationResult.IsFailed())
            {
                return validationResult;
            }

            std::wstring wideArgument;
            GB_SystemResult convertResult = ConvertUtf8ToWide(argument, wideArgument, parameterName, operationName);
            if (convertResult.IsFailed())
            {
                return convertResult;
            }
            try
            {
                wideArguments.push_back(std::move(wideArgument));
            }
            catch (...)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"保存应用程序参数时内存不足。");
            }
        }

        try
        {
            preparedExecute.parameters = GB_WindowsCommandLineInternal::BuildParameters(wideArguments);
        }
        catch (const std::length_error&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"构造后的 Windows 应用程序参数字符串过长。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"构造 Windows 应用程序参数字符串时内存不足。");
        }

        if (!options.workingDirectory.empty())
        {
            GB_SystemResult validationResult = ValidateUtf8String(options.workingDirectory, u8"workingDirectory", false, operationName);
            if (validationResult.IsFailed())
            {
                return validationResult;
            }

            std::string absoluteWorkingDirectoryUtf8;
            GB_SystemResult resolveResult = ResolveAbsolutePath(options.workingDirectory, absoluteWorkingDirectoryUtf8, preparedExecute.workingDirectory, u8"workingDirectory", operationName);
            if (resolveResult.IsFailed())
            {
                return resolveResult;
            }
            if (!GB_IsDirectoryExists(absoluteWorkingDirectoryUtf8))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"工作目录不存在或不是目录：" + absoluteWorkingDirectoryUtf8);
            }
        }

        preparedExecute.verb = GetVerbText(options.verb);
        preparedExecute.showCommand = GetShowCommand(options.showMode);
        preparedExecute.waitMilliseconds = options.waitTimeoutMilliseconds < 0 ? INFINITE : static_cast<DWORD>(options.waitTimeoutMilliseconds);
        preparedExecute.waitForExit = options.waitForExit;
        preparedExecute.requestProcessHandle = options.waitForExit || hasExecuteResult;
        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult ExecutePreparedOnCurrentSta(const PreparedShellExecute& preparedExecute, GB_ShellExecuteResult* executeResult, const std::string& operationName)
    {
        GB_WinHandleScope processHandle;
        SHELLEXECUTEINFOW executeInfo = {};
        executeInfo.cbSize = sizeof(executeInfo);
        executeInfo.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_UNICODE | SEE_MASK_NOASYNC;
        if (preparedExecute.requestProcessHandle)
        {
            executeInfo.fMask |= SEE_MASK_NOCLOSEPROCESS;
        }
        executeInfo.lpVerb = preparedExecute.verb;
        executeInfo.lpFile = preparedExecute.target.c_str();
        executeInfo.lpParameters = preparedExecute.parameters.empty() ? nullptr : preparedExecute.parameters.c_str();
        executeInfo.lpDirectory = preparedExecute.workingDirectory.empty() ? nullptr : preparedExecute.workingDirectory.c_str();
        executeInfo.nShow = preparedExecute.showCommand;

        ::SetLastError(ERROR_SUCCESS);
        if (::ShellExecuteExW(&executeInfo) == FALSE)
        {
            const DWORD failureCode = GetShellExecuteFailureCode(executeInfo);
            return GB_SystemResult::FromWin32Error(failureCode, operationName, u8"ShellExecuteExW 执行失败。");
        }

        if (executeInfo.hProcess != nullptr)
        {
            try
            {
                GB_SystemResult adoptResult = processHandle.Reset(executeInfo.hProcess, GB_WinHandleCloseMethod::CloseHandle, nullptr, u8"ShellExecuteExW process handle");
                if (adoptResult.IsFailed())
                {
                    (void)::CloseHandle(executeInfo.hProcess);
                    return adoptResult.WithOperationName(operationName);
                }
            }
            catch (...)
            {
                if (processHandle.GetHandle() != executeInfo.hProcess)
                {
                    (void)::CloseHandle(executeInfo.hProcess);
                }
                throw;
            }
        }

        if (executeInfo.hProcess != nullptr && executeResult != nullptr)
        {
            executeResult->processHandleReturned = true;
            const DWORD processId = ::GetProcessId(executeInfo.hProcess);
            if (processId != 0)
            {
                executeResult->processId = processId;
                executeResult->hasProcessId = true;
            }
        }

        if (!preparedExecute.waitForExit)
        {
            return GB_SystemResult::Succeeded(operationName, u8"Windows Shell 已接受执行请求。");
        }
        if (executeInfo.hProcess == nullptr)
        {
            return GB_SystemResult::Succeeded(operationName, u8"Windows Shell 已接受执行请求，但未返回可等待的进程句柄。");
        }

        const DWORD waitResult = ::WaitForSingleObject(executeInfo.hProcess, preparedExecute.waitMilliseconds);
        if (waitResult == WAIT_TIMEOUT)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, operationName, u8"等待 Shell 启动的进程退出超时；目标进程未被终止。");
        }
        if (waitResult == WAIT_FAILED)
        {
            return GB_SystemResult::FromLastWin32Error(operationName, u8"WaitForSingleObject 等待 Shell 启动的进程失败。");
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NativeApiFailed, operationName, u8"WaitForSingleObject 返回了非预期状态。");
        }

        if (executeResult != nullptr)
        {
            executeResult->waitCompleted = true;
        }

        DWORD exitCode = 0;
        if (::GetExitCodeProcess(executeInfo.hProcess, &exitCode) == FALSE)
        {
            return GB_SystemResult::FromLastWin32Error(operationName, u8"GetExitCodeProcess 获取进程退出码失败。");
        }

        if (executeResult != nullptr)
        {
            executeResult->exitCode = exitCode;
            executeResult->hasExitCode = true;
        }
        return GB_SystemResult::Succeeded(operationName, u8"Windows Shell 已接受执行请求，目标进程已经退出。");
    }
#endif

    template<typename FunctionType>
    static GB_SystemResult InvokeInSta(const FunctionType& functionObject, const std::string& operationName)
    {
#if !defined(_WIN32)
        (void)functionObject;
        return MakeUnsupportedPlatformResult(operationName);
#else
        GB_ComScope comScope = GB_ComScope::InitializeSta(operationName + u8"::InitializeCom", true);
        const GB_SystemResult initializeResult = comScope.GetInitializeResult();
        if (initializeResult.IsSucceeded())
        {
            return functionObject();
        }

        const int32_t rpcChangedMode = static_cast<int32_t>(0x80010106u);
        if (initializeResult.hresult != rpcChangedMode)
        {
            GB_SystemResult result = initializeResult;
            result.WithOperationName(operationName);
            return result;
        }

        GB_SystemResult threadResult = GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, operationName, u8"STA 工作线程未返回执行结果。");
        try
        {
            std::thread staThread([&functionObject, &threadResult, &operationName]()
                {
                    try
                    {
                        GB_ComScope threadComScope = GB_ComScope::InitializeSta(operationName + u8"::InitializeWorkerCom", true);
                        const GB_SystemResult threadInitializeResult = threadComScope.GetInitializeResult();
                        if (threadInitializeResult.IsFailed())
                        {
                            threadResult = threadInitializeResult;
                            threadResult.WithOperationName(operationName);
                            return;
                        }
                        threadResult = functionObject();
                    }
                    catch (const std::bad_alloc&)
                    {
                        threadResult = GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"STA 工作线程执行时内存不足。");
                    }
                    catch (...)
                    {
                        threadResult = GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, operationName, u8"STA 工作线程执行时发生未预期异常。");
                    }
                });
            staThread.join();
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"创建 STA 工作线程时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, operationName, u8"创建 STA 工作线程失败。");
        }
        return threadResult;
#endif
    }

    static GB_SystemResult ExecuteWithTargetMode(const GB_ShellExecuteOptions& options, GB_ShellExecuteResult* executeResult, const ShellTargetMode targetMode, const std::string& operationName)
    {
        if (executeResult != nullptr)
        {
            *executeResult = GB_ShellExecuteResult();
        }

#if !defined(_WIN32)
        (void)options;
        (void)targetMode;
        return MakeUnsupportedPlatformResult(operationName);
#else
        PreparedShellExecute preparedExecute;
        GB_SystemResult prepareResult = PrepareExecute(options, targetMode, executeResult != nullptr, preparedExecute, operationName);
        if (prepareResult.IsFailed())
        {
            return prepareResult;
        }

        return InvokeInSta([&preparedExecute, executeResult, &operationName]()
            {
                return ExecutePreparedOnCurrentSta(preparedExecute, executeResult, operationName);
            }, operationName);
#endif
    }

    template<typename FunctionType>
    static GB_SystemResult GuardOperation(const char* operationName, const FunctionType& functionObject)
    {
        try
        {
            const std::string resolvedOperationName = operationName;
            return functionObject(resolvedOperationName);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"执行 GB_SystemShell 操作时内存不足。");
        }
        catch (const std::exception&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, operationName, u8"执行 GB_SystemShell 操作时发生未预期的标准异常。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, operationName, u8"执行 GB_SystemShell 操作时发生未预期异常。");
        }
    }
}

GB_SystemResult GB_SystemShell::OpenUrl(const std::string& url)
{
    return GuardOperation(u8"GB_SystemShell::OpenUrl", [&url](const std::string& operationName)
        {
            GB_ShellExecuteOptions options;
            options.target = url;
            options.verb = GB_ShellExecuteVerb::Open;
            return ExecuteWithTargetMode(options, nullptr, ShellTargetMode::Url, operationName);
        });
}

GB_SystemResult GB_SystemShell::OpenFile(const std::string& filePath)
{
    return GuardOperation(u8"GB_SystemShell::OpenFile", [&filePath](const std::string& operationName)
        {
            GB_ShellExecuteOptions options;
            options.target = filePath;
            options.verb = GB_ShellExecuteVerb::Open;
            return ExecuteWithTargetMode(options, nullptr, ShellTargetMode::ExistingFile, operationName);
        });
}

GB_SystemResult GB_SystemShell::OpenFolder(const std::string& folderPath)
{
    return GuardOperation(u8"GB_SystemShell::OpenFolder", [&folderPath](const std::string& operationName)
        {
            GB_ShellExecuteOptions options;
            options.target = folderPath;
            options.verb = GB_ShellExecuteVerb::Open;
            return ExecuteWithTargetMode(options, nullptr, ShellTargetMode::ExistingDirectory, operationName);
        });
}

GB_SystemResult GB_SystemShell::ExploreFolder(const std::string& folderPath)
{
    return GuardOperation(u8"GB_SystemShell::ExploreFolder", [&folderPath](const std::string& operationName)
        {
            GB_ShellExecuteOptions options;
            options.target = folderPath;
            options.verb = GB_ShellExecuteVerb::Explore;
            return ExecuteWithTargetMode(options, nullptr, ShellTargetMode::ExistingDirectory, operationName);
        });
}

GB_SystemResult GB_SystemShell::RevealInExplorer(const std::string& path)
{
    return GuardOperation(u8"GB_SystemShell::RevealInExplorer", [&path](const std::string& operationName)
        {
            GB_SystemResult validationResult = ValidateUtf8String(path, u8"path", false, operationName);
            if (validationResult.IsFailed())
            {
                return validationResult;
            }

#if !defined(_WIN32)
            return MakeUnsupportedPlatformResult(operationName);
#else
            std::string absolutePathUtf8;
            std::wstring absolutePath;
            GB_SystemResult resolveResult = ResolveAbsolutePath(path, absolutePathUtf8, absolutePath, u8"path", operationName);
            if (resolveResult.IsFailed())
            {
                return resolveResult;
            }
            if (!GB_IsPathExists(absolutePathUtf8))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"目标路径不存在：" + absolutePathUtf8);
            }

            return InvokeInSta([&absolutePath, &operationName]()
                {
                    GB_WinHandleScope itemIdentifierListScope;
                    PIDLIST_ABSOLUTE itemIdentifierList = nullptr;
                    const HRESULT parseResult = ::SHParseDisplayName(absolutePath.c_str(), nullptr, &itemIdentifierList, 0, nullptr);
                    if (FAILED(parseResult))
                    {
                        return GB_SystemResult::FromHResult(static_cast<int32_t>(parseResult), operationName, u8"SHParseDisplayName 将路径转换为 PIDL 失败。");
                    }

                    try
                    {
                        GB_SystemResult adoptResult = itemIdentifierListScope.Reset(itemIdentifierList, GB_WinHandleCloseMethod::CoTaskMemFree, nullptr, u8"SHParseDisplayName PIDL");
                        if (adoptResult.IsFailed())
                        {
                            ::CoTaskMemFree(itemIdentifierList);
                            return adoptResult.WithOperationName(operationName);
                        }
                    }
                    catch (...)
                    {
                        if (itemIdentifierListScope.GetHandle() != itemIdentifierList)
                        {
                            ::CoTaskMemFree(itemIdentifierList);
                        }
                        throw;
                    }

                    const HRESULT openResult = ::SHOpenFolderAndSelectItems(itemIdentifierList, 0, nullptr, 0);
                    if (FAILED(openResult))
                    {
                        return GB_SystemResult::FromHResult(static_cast<int32_t>(openResult), operationName, u8"SHOpenFolderAndSelectItems 定位目标失败。");
                    }
                    return GB_SystemResult::Succeeded(operationName, u8"Windows Explorer 定位请求已提交。");
                }, operationName);
#endif
        });
}

GB_SystemResult GB_SystemShell::OpenSettings()
{
    return GuardOperation(u8"GB_SystemShell::OpenSettings", [](const std::string& operationName)
        {
            GB_ShellExecuteOptions options;
            options.target = GetSettingsPageUri(GB_SystemSettingsPage::Home);
            options.verb = GB_ShellExecuteVerb::Open;
            return ExecuteWithTargetMode(options, nullptr, ShellTargetMode::SettingsUri, operationName);
        });
}

GB_SystemResult GB_SystemShell::OpenSettingsPage(const GB_SystemSettingsPage settingsPage)
{
    return GuardOperation(u8"GB_SystemShell::OpenSettingsPage", [settingsPage](const std::string& operationName)
        {
            if (!IsValidSettingsPageValue(static_cast<uint64_t>(settingsPage)))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"settingsPage 枚举值非法。");
            }

            GB_ShellExecuteOptions options;
            options.target = GetSettingsPageUri(settingsPage);
            options.verb = GB_ShellExecuteVerb::Open;
            return ExecuteWithTargetMode(options, nullptr, ShellTargetMode::SettingsUri, operationName);
        });
}

GB_SystemResult GB_SystemShell::OpenSettingsUri(const std::string& settingsUri)
{
    return GuardOperation(u8"GB_SystemShell::OpenSettingsUri", [&settingsUri](const std::string& operationName)
        {
            GB_ShellExecuteOptions options;
            options.target = settingsUri;
            options.verb = GB_ShellExecuteVerb::Open;
            return ExecuteWithTargetMode(options, nullptr, ShellTargetMode::SettingsUri, operationName);
        });
}

GB_SystemResult GB_SystemShell::RunApplication(const std::string& applicationPath, const std::vector<std::string>& arguments)
{
    return GuardOperation(u8"GB_SystemShell::RunApplication", [&applicationPath, &arguments](const std::string& operationName)
        {
            GB_ShellExecuteOptions options;
            options.target = applicationPath;
            options.arguments = arguments;
            options.verb = GB_ShellExecuteVerb::Open;
            return ExecuteWithTargetMode(options, nullptr, ShellTargetMode::Application, operationName);
        });
}

GB_SystemResult GB_SystemShell::RunApplicationAsAdmin(const std::string& applicationPath, const std::vector<std::string>& arguments)
{
    return GuardOperation(u8"GB_SystemShell::RunApplicationAsAdmin", [&applicationPath, &arguments](const std::string& operationName)
        {
            GB_ShellExecuteOptions options;
            options.target = applicationPath;
            options.arguments = arguments;
            options.verb = GB_ShellExecuteVerb::RunAs;
            return ExecuteWithTargetMode(options, nullptr, ShellTargetMode::Application, operationName);
        });
}

GB_SystemResult GB_SystemShell::Execute(const GB_ShellExecuteOptions& options, GB_ShellExecuteResult* executeResult)
{
    return GuardOperation(u8"GB_SystemShell::Execute", [&options, executeResult](const std::string& operationName)
        {
            return ExecuteWithTargetMode(options, executeResult, ShellTargetMode::Generic, operationName);
        });
}

bool GB_SystemShell::IsValidShowModeValue(const uint64_t showModeValue)
{
    return showModeValue <= static_cast<uint64_t>(GB_ShellShowMode::Maximized);
}

bool GB_SystemShell::IsValidExecuteVerbValue(const uint64_t executeVerbValue)
{
    return executeVerbValue <= static_cast<uint64_t>(GB_ShellExecuteVerb::RunAs);
}

bool GB_SystemShell::IsValidSettingsPageValue(const uint64_t settingsPageValue)
{
    return settingsPageValue <= static_cast<uint64_t>(GB_SystemSettingsPage::PrivacyCamera);
}

std::string GB_SystemShell::GetSettingsPageUri(const GB_SystemSettingsPage settingsPage)
{
    switch (settingsPage)
    {
    case GB_SystemSettingsPage::Home:
        return "ms-settings:";
    case GB_SystemSettingsPage::Display:
        return "ms-settings:display";
    case GB_SystemSettingsPage::Sound:
        return "ms-settings:sound";
    case GB_SystemSettingsPage::Bluetooth:
        return "ms-settings:bluetooth";
    case GB_SystemSettingsPage::Wifi:
        return "ms-settings:network-wifi";
    case GB_SystemSettingsPage::Network:
        return "ms-settings:network-status";
    case GB_SystemSettingsPage::DefaultApps:
        return "ms-settings:defaultapps";
    case GB_SystemSettingsPage::AppsFeatures:
        return "ms-settings:appsfeatures";
    case GB_SystemSettingsPage::Clipboard:
        return "ms-settings:clipboard";
    case GB_SystemSettingsPage::PowerSleep:
        return "ms-settings:powersleep";
    case GB_SystemSettingsPage::PrivacyMicrophone:
        return "ms-settings:privacy-microphone";
    case GB_SystemSettingsPage::PrivacyCamera:
        return "ms-settings:privacy-webcam";
    default:
        break;
    }
    return std::string();
}
