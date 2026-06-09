#include "GB_SystemProcess.h"

#include "../GB_Utf8String.h"
#include "GB_WinHandleScope.h"
#include "GB_WindowsCommandLineInternal.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <system_error>
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
#  include <psapi.h>
#  include <tlhelp32.h>
#endif

namespace
{
    const char* const GB_ProcessOperationStart = "GB_SystemProcess::Start";
    const char* const GB_ProcessOperationRun = "GB_SystemProcess::Run";
    const char* const GB_ProcessOperationEnumerate = "GB_SystemProcess::GetAllProcesses";
    const char* const GB_ProcessOperationGetInfo = "GB_SystemProcess::GetProcessInfo";
    const char* const GB_ProcessOperationFind = "GB_SystemProcess::FindProcesses";
    const char* const GB_ProcessOperationWait = "GB_SystemProcess::WaitForProcess";
    const char* const GB_ProcessOperationWaitExit = "GB_SystemProcess::WaitForProcessExit";
    const char* const GB_ProcessOperationClose = "GB_SystemProcess::CloseProcess";
    const char* const GB_ProcessOperationTerminate = "GB_SystemProcess::TerminateProcess";

    GB_SystemResult MakeUnsupportedPlatformResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, "当前平台不支持 Windows 进程管理。");
    }

    GB_SystemResult MakeAllocationFailedResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, "分配进程模块内部资源失败。");
    }

    bool ContainsEmbeddedNull(const std::string& text)
    {
        return text.find('\0') != std::string::npos;
    }

    GB_SystemResult ValidateWaitOptions(const GB_ProcessWaitOptions& waitOptions, const std::string& operationName)
    {
        if (waitOptions.timeoutMilliseconds < -1 || waitOptions.pollIntervalMilliseconds == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "等待超时必须大于等于 -1，轮询间隔必须大于 0。");
        }
        if (waitOptions.timeoutMilliseconds >= static_cast<int64_t>((std::numeric_limits<uint32_t>::max)()))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "有限等待超时不能等于或超过 Win32 INFINITE。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    bool IsCancellationRequested(const GB_ProcessWaitOptions& waitOptions)
    {
        return waitOptions.cancellationFlag != nullptr && waitOptions.cancellationFlag->load(std::memory_order_acquire);
    }

    bool SleepForWaitPoll(const GB_ProcessWaitOptions& waitOptions, uint64_t sleepMilliseconds)
    {
        while (sleepMilliseconds != 0)
        {
            if (IsCancellationRequested(waitOptions))
            {
                return false;
            }
            const uint64_t chunkMilliseconds = waitOptions.cancellationFlag == nullptr ? sleepMilliseconds : (std::min)(sleepMilliseconds, static_cast<uint64_t>(50));
            std::this_thread::sleep_for(std::chrono::milliseconds(chunkMilliseconds));
            sleepMilliseconds -= chunkMilliseconds;
        }
        return !IsCancellationRequested(waitOptions);
    }

#if defined(_WIN32)
    uint64_t FileTimeToUInt64(const FILETIME& fileTime)
    {
        ULARGE_INTEGER value = {};
        value.LowPart = fileTime.dwLowDateTime;
        value.HighPart = fileTime.dwHighDateTime;
        return value.QuadPart;
    }

    long long FileTimeToUnixMilliseconds(const FILETIME& fileTime)
    {
        const uint64_t windowsEpochDifference = 116444736000000000ULL;
        const uint64_t value = FileTimeToUInt64(fileTime);
        return value < windowsEpochDifference ? 0 : static_cast<long long>((value - windowsEpochDifference) / 10000ULL);
    }

    double FileTimeToSeconds(const FILETIME& fileTime)
    {
        return static_cast<double>(FileTimeToUInt64(fileTime)) / 10000000.0;
    }

    void AddFieldError(GB_ProcessInfo& processInfo, const GB_ProcessQueryOptions& queryOptions, const std::string& fieldName, const DWORD errorCode, const std::string& message)
    {
        if (!queryOptions.recordFieldErrors)
        {
            return;
        }
        GB_ProcessFieldError fieldError;
        fieldError.fieldName = fieldName;
        fieldError.errorCode = errorCode == ERROR_SUCCESS ? GB_SystemErrorCode::NativeApiFailed : GB_SystemError::GuessErrorCodeFromWin32ErrorCode(errorCode);
        fieldError.nativeErrorCode = errorCode;
        fieldError.message = message;
        processInfo.fieldErrors.push_back(std::move(fieldError));
    }

    void AddFieldResultError(GB_ProcessInfo& processInfo, const GB_ProcessQueryOptions& queryOptions, const std::string& fieldName, const GB_SystemResult& result, const std::string& message)
    {
        if (!queryOptions.recordFieldErrors)
        {
            return;
        }
        GB_ProcessFieldError fieldError;
        fieldError.fieldName = fieldName;
        fieldError.errorCode = result.errorCode;
        fieldError.nativeErrorCode = result.nativeErrorCode;
        fieldError.message = message;
        processInfo.fieldErrors.push_back(std::move(fieldError));
    }

    std::wstring ToWideStringChecked(const std::string& text, const std::string& fieldName)
    {
        if (!GB_IsUtf8(text) || ContainsEmbeddedNull(text))
        {
            throw std::invalid_argument(fieldName);
        }
        return GB_Utf8ToWString(text);
    }

    int CompareWideOrdinalIgnoreCase(const std::wstring& leftValue, const std::wstring& rightValue)
    {
        const int compareResult = ::CompareStringOrdinal(leftValue.c_str(), -1, rightValue.c_str(), -1, TRUE);
        if (compareResult == CSTR_LESS_THAN)
        {
            return -1;
        }
        if (compareResult == CSTR_GREATER_THAN)
        {
            return 1;
        }
        return 0;
    }

    struct WideOrdinalLess
    {
        bool operator()(const std::wstring& leftValue, const std::wstring& rightValue) const
        {
            return CompareWideOrdinalIgnoreCase(leftValue, rightValue) < 0;
        }
    };

    DWORD GetPriorityCreationFlag(const GB_ProcessPriority priority)
    {
        switch (priority)
        {
        case GB_ProcessPriority::Idle:
            return IDLE_PRIORITY_CLASS;
        case GB_ProcessPriority::BelowNormal:
            return BELOW_NORMAL_PRIORITY_CLASS;
        case GB_ProcessPriority::AboveNormal:
            return ABOVE_NORMAL_PRIORITY_CLASS;
        case GB_ProcessPriority::High:
            return HIGH_PRIORITY_CLASS;
        case GB_ProcessPriority::Normal:
        default:
            return NORMAL_PRIORITY_CLASS;
        }
    }

    int GetShowWindowValue(const GB_ProcessShowMode showMode)
    {
        switch (showMode)
        {
        case GB_ProcessShowMode::Hidden:
            return SW_HIDE;
        case GB_ProcessShowMode::Normal:
            return SW_SHOWNORMAL;
        case GB_ProcessShowMode::Minimized:
            return SW_SHOWMINIMIZED;
        case GB_ProcessShowMode::Maximized:
            return SW_SHOWMAXIMIZED;
        case GB_ProcessShowMode::Default:
        default:
            return SW_SHOWDEFAULT;
        }
    }

    GB_SystemResult ValidateStartOptions(const GB_ProcessStartOptions& options)
    {
        if (options.executablePath.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "可执行文件路径不能为空。");
        }
        if (!GB_SystemProcess::IsValidShowModeValue(static_cast<uint64_t>(options.showMode)) || !GB_SystemProcess::IsValidConsoleModeValue(static_cast<uint64_t>(options.consoleMode)) || !GB_SystemProcess::IsValidPriorityValue(static_cast<uint64_t>(options.priority)) || !GB_SystemProcess::IsValidOutputEncodingValue(static_cast<uint64_t>(options.outputEncoding)))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "启动选项包含非法枚举值。");
        }
        if (!GB_IsUtf8(options.executablePath) || ContainsEmbeddedNull(options.executablePath) || !GB_IsUtf8(options.workingDirectory) || ContainsEmbeddedNull(options.workingDirectory) || !GB_IsUtf8(options.rawCommandLine) || ContainsEmbeddedNull(options.rawCommandLine))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "路径或原始命令行不是合法 UTF-8，或包含嵌入式 NUL。");
        }
        if (!options.rawCommandLine.empty() && !options.arguments.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "rawCommandLine 与 arguments 不能同时使用。");
        }
        for (size_t argumentIndex = 0; argumentIndex < options.arguments.size(); argumentIndex++)
        {
            if (!GB_IsUtf8(options.arguments[argumentIndex]) || ContainsEmbeddedNull(options.arguments[argumentIndex]))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "命令行参数不是合法 UTF-8 或包含嵌入式 NUL。");
            }
        }
        if (options.mergeStandardErrorToOutput && !options.redirectStandardOutput)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "合并 stderr 到 stdout 时必须启用标准输出重定向。");
        }
        if (options.outputCallback && options.maximumPendingOutputCallbacks == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "启用输出回调时，回调队列容量必须大于 0。");
        }
        if (options.maximumCapturedBytesPerStream > static_cast<size_t>((std::numeric_limits<int>::max)()))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "单个标准流的捕获上限不能超过 INT_MAX 字节。");
        }
        if (options.runTimeoutMilliseconds < -1 || options.runTimeoutMilliseconds >= static_cast<int64_t>((std::numeric_limits<uint32_t>::max)()))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "运行超时必须为 -1 或小于 Win32 INFINITE 的非负值。");
        }
        if (options.jobOptions.cpuRatePercent > 100)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "Job CPU 上限百分比必须位于 1 到 100，或使用 0 表示不限制。");
        }
        if (!options.jobOptions.enabled && (options.jobOptions.terminateOnJobClose || options.jobOptions.maximumActiveProcessCount != 0 || options.jobOptions.processMemoryLimitBytes != 0 || options.jobOptions.jobMemoryLimitBytes != 0 || options.jobOptions.cpuRatePercent != 0 || options.jobOptions.breakawayAllowed))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "Job 未启用时不能设置 Job 限制。");
        }
        for (size_t variableIndex = 0; variableIndex < options.environmentVariables.size(); variableIndex++)
        {
            const GB_ProcessEnvironmentVariable& variable = options.environmentVariables[variableIndex];
            if (variable.name.empty() || !GB_IsUtf8(variable.name) || !GB_IsUtf8(variable.value) || ContainsEmbeddedNull(variable.name) || ContainsEmbeddedNull(variable.value) || variable.name.find('=') != std::string::npos)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "环境变量名称或值非法。");
            }
        }
        return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
    }

    GB_SystemResult ValidateExecutableAndWorkingDirectory(const std::wstring& executablePath, const std::wstring& workingDirectory)
    {
        const DWORD executableAttributes = ::GetFileAttributesW(executablePath.c_str());
        if (executableAttributes == INVALID_FILE_ATTRIBUTES)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "可执行文件不存在或无法访问。");
        }
        if ((executableAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "可执行文件路径不能指向文件夹。");
        }
        if (!workingDirectory.empty())
        {
            const DWORD directoryAttributes = ::GetFileAttributesW(workingDirectory.c_str());
            if (directoryAttributes == INVALID_FILE_ATTRIBUTES)
            {
                return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "工作目录不存在或无法访问。");
            }
            if ((directoryAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "工作目录路径不是文件夹。");
            }
        }
        return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
    }

    GB_SystemResult BuildCommandLine(const GB_ProcessStartOptions& options, const std::wstring& executablePath, std::wstring& commandLine)
    {
        try
        {
            if (!options.rawCommandLine.empty())
            {
                commandLine = GB_Utf8ToWString(options.rawCommandLine);
            }
            else
            {
                commandLine = GB_WindowsCommandLineInternal::QuoteArgument(executablePath);
                std::vector<std::wstring> wideArguments;
                wideArguments.reserve(options.arguments.size());
                for (size_t argumentIndex = 0; argumentIndex < options.arguments.size(); argumentIndex++)
                {
                    wideArguments.push_back(GB_Utf8ToWString(options.arguments[argumentIndex]));
                }
                const std::wstring parameters = GB_WindowsCommandLineInternal::BuildParameters(wideArguments);
                if (!parameters.empty())
                {
                    commandLine.push_back(L' ');
                    commandLine.append(parameters);
                }
            }
        }
        catch (const std::length_error&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "命令行参数过长。");
        }
        catch (const std::bad_alloc&)
        {
            return MakeAllocationFailedResult(GB_ProcessOperationStart);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_ProcessOperationStart, "命令行 UTF-8 转换失败。");
        }
        if (commandLine.size() >= 32767)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "完整命令行超过 Windows 32767 个宽字符的上限。");
        }
        return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
    }

    size_t FindEnvironmentNameSeparator(const std::wstring& environmentEntry)
    {
        return !environmentEntry.empty() && environmentEntry[0] == L'=' ? environmentEntry.find(L'=', 1) : environmentEntry.find(L'=');
    }

    GB_SystemResult BuildEnvironmentBlock(const GB_ProcessStartOptions& options, std::vector<wchar_t>& environmentBlock, bool& usesCustomEnvironment)
    {
        usesCustomEnvironment = !options.inheritCurrentEnvironment || !options.environmentVariables.empty();
        environmentBlock.clear();
        if (!usesCustomEnvironment)
        {
            return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
        }

        try
        {
            std::map<std::wstring, std::wstring, WideOrdinalLess> environment;
            if (options.inheritCurrentEnvironment)
            {
                struct EnvironmentStringsScope
                {
                    explicit EnvironmentStringsScope(LPWCH stringsValue) : strings(stringsValue)
                    {
                    }

                    ~EnvironmentStringsScope()
                    {
                        if (strings != nullptr)
                        {
                            ::FreeEnvironmentStringsW(strings);
                        }
                    }

                    LPWCH strings = nullptr;
                };
                EnvironmentStringsScope environmentStrings(::GetEnvironmentStringsW());
                if (environmentStrings.strings == nullptr)
                {
                    return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "读取当前进程环境变量失败。");
                }
                const wchar_t* currentEntry = environmentStrings.strings;
                while (*currentEntry != L'\0')
                {
                    const std::wstring entry(currentEntry);
                    const size_t separator = FindEnvironmentNameSeparator(entry);
                    if (separator != std::wstring::npos)
                    {
                        environment[entry.substr(0, separator)] = entry.substr(separator + 1);
                    }
                    currentEntry += entry.size() + 1;
                }
            }
            for (size_t variableIndex = 0; variableIndex < options.environmentVariables.size(); variableIndex++)
            {
                const GB_ProcessEnvironmentVariable& variable = options.environmentVariables[variableIndex];
                environment[GB_Utf8ToWString(variable.name)] = GB_Utf8ToWString(variable.value);
            }

            size_t characterCount = 1;
            for (std::map<std::wstring, std::wstring, WideOrdinalLess>::const_iterator iterator = environment.begin(); iterator != environment.end(); iterator++)
            {
                size_t entryLength = GB_WindowsCommandLineInternal::CheckedAdd(iterator->first.size(), 1);
                entryLength = GB_WindowsCommandLineInternal::CheckedAdd(entryLength, iterator->second.size());
                entryLength = GB_WindowsCommandLineInternal::CheckedAdd(entryLength, 1);
                if (entryLength > (std::numeric_limits<size_t>::max)() - characterCount)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "环境变量块过大。");
                }
                characterCount += entryLength;
            }
            environmentBlock.reserve(characterCount);
            for (std::map<std::wstring, std::wstring, WideOrdinalLess>::const_iterator iterator = environment.begin(); iterator != environment.end(); iterator++)
            {
                environmentBlock.insert(environmentBlock.end(), iterator->first.begin(), iterator->first.end());
                environmentBlock.push_back(L'=');
                environmentBlock.insert(environmentBlock.end(), iterator->second.begin(), iterator->second.end());
                environmentBlock.push_back(L'\0');
            }
            environmentBlock.push_back(L'\0');
            if (environmentBlock.size() == 1)
            {
                environmentBlock.push_back(L'\0');
            }
        }
        catch (const std::length_error&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "环境变量块过大。");
        }
        catch (const std::bad_alloc&)
        {
            return MakeAllocationFailedResult(GB_ProcessOperationStart);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_ProcessOperationStart, "环境变量 UTF-8 转换失败。");
        }
        return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
    }

    std::wstring GetFileNameFromPath(const std::wstring& filePath)
    {
        const size_t separator = filePath.find_last_of(L"\\/");
        return separator == std::wstring::npos ? filePath : filePath.substr(separator + 1);
    }

    bool QueryCurrentDirectoryUtf8(std::string& workingDirectory)
    {
        workingDirectory.clear();
        std::vector<wchar_t> directoryBuffer(260);
        while (directoryBuffer.size() <= 32768)
        {
            const DWORD writtenLength = ::GetCurrentDirectoryW(static_cast<DWORD>(directoryBuffer.size()), directoryBuffer.data());
            if (writtenLength == 0)
            {
                return false;
            }
            if (writtenLength < directoryBuffer.size())
            {
                workingDirectory = GB_WStringToUtf8(std::wstring(directoryBuffer.data(), writtenLength));
                return true;
            }
            if (writtenLength > 32768)
            {
                ::SetLastError(ERROR_FILENAME_EXCED_RANGE);
                return false;
            }
            directoryBuffer.resize(writtenLength > directoryBuffer.size() ? writtenLength : directoryBuffer.size() * 2);
        }
        ::SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }

    bool TryGetProcessTimes(HANDLE processHandle, FILETIME& creationTime, FILETIME& exitTime, FILETIME& kernelTime, FILETIME& userTime)
    {
        return ::GetProcessTimes(processHandle, &creationTime, &exitTime, &kernelTime, &userTime) != FALSE;
    }

    GB_SystemResult ValidateProcessIdentity(HANDLE processHandle, const GB_ProcessIdentity& identity, const std::string& operationName)
    {
        if (!identity.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "进程身份无效。");
        }
        if (!identity.hasCreationTime)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "破坏性或等待操作要求包含创建时间的强进程身份。");
        }
        FILETIME creationTime = {};
        FILETIME exitTime = {};
        FILETIME kernelTime = {};
        FILETIME userTime = {};
        if (!TryGetProcessTimes(processHandle, creationTime, exitTime, kernelTime, userTime))
        {
            return GB_SystemResult::FromLastWin32Error(operationName, "读取进程创建时间失败，无法验证 PID 是否已被复用。");
        }
        if (FileTimeToUInt64(creationTime) != identity.creationTime100Nanoseconds)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, "进程 ID 已被复用，当前进程不是身份快照所指向的原进程。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    bool QueryProcessPath(HANDLE processHandle, std::string& executablePath)
    {
        std::vector<wchar_t> pathBuffer(32768);
        DWORD pathLength = static_cast<DWORD>(pathBuffer.size());
        if (::QueryFullProcessImageNameW(processHandle, 0, pathBuffer.data(), &pathLength) == FALSE || pathLength == 0)
        {
            return false;
        }
        executablePath = GB_WStringToUtf8(std::wstring(pathBuffer.data(), pathLength));
        return true;
    }

    bool QueryProcessUserName(HANDLE processHandle, std::string& userName)
    {
        HANDLE rawTokenHandle = nullptr;
        if (::OpenProcessToken(processHandle, TOKEN_QUERY, &rawTokenHandle) == FALSE)
        {
            return false;
        }
        GB_WinHandleScope tokenHandle = GB_WinHandleScope::FromKernelHandle(rawTokenHandle, "ProcessToken");
        DWORD tokenSize = 0;
        (void)::GetTokenInformation(rawTokenHandle, TokenUser, nullptr, 0, &tokenSize);
        if (tokenSize == 0)
        {
            return false;
        }
        std::vector<unsigned char> tokenBuffer(tokenSize);
        if (::GetTokenInformation(rawTokenHandle, TokenUser, tokenBuffer.data(), tokenSize, &tokenSize) == FALSE)
        {
            return false;
        }
        const TOKEN_USER* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.data());
        DWORD nameLength = 0;
        DWORD domainLength = 0;
        SID_NAME_USE sidType = SidTypeUnknown;
        (void)::LookupAccountSidW(nullptr, tokenUser->User.Sid, nullptr, &nameLength, nullptr, &domainLength, &sidType);
        if (nameLength == 0)
        {
            return false;
        }
        std::vector<wchar_t> nameBuffer((std::max)(nameLength, 1UL));
        std::vector<wchar_t> domainBuffer((std::max)(domainLength, 1UL));
        DWORD nameCapacity = static_cast<DWORD>(nameBuffer.size());
        DWORD domainCapacity = static_cast<DWORD>(domainBuffer.size());
        if (::LookupAccountSidW(nullptr, tokenUser->User.Sid, nameBuffer.data(), &nameCapacity, domainBuffer.data(), &domainCapacity, &sidType) == FALSE)
        {
            return false;
        }
        while (nameCapacity > 0 && nameBuffer[nameCapacity - 1] == L'\0')
        {
            nameCapacity--;
        }
        while (domainCapacity > 0 && domainBuffer[domainCapacity - 1] == L'\0')
        {
            domainCapacity--;
        }
        std::wstring fullName;
        if (domainCapacity != 0)
        {
            fullName.assign(domainBuffer.data(), domainCapacity);
            fullName.push_back(L'\\');
        }
        fullName.append(nameBuffer.data(), nameCapacity);
        userName = GB_WStringToUtf8(fullName);
        return true;
    }

    bool QueryProcessElevation(HANDLE processHandle, bool& elevated)
    {
        HANDLE rawTokenHandle = nullptr;
        if (::OpenProcessToken(processHandle, TOKEN_QUERY, &rawTokenHandle) == FALSE)
        {
            return false;
        }
        GB_WinHandleScope tokenHandle = GB_WinHandleScope::FromKernelHandle(rawTokenHandle, "ProcessToken");
        TOKEN_ELEVATION tokenElevation = {};
        DWORD returnedSize = 0;
        if (::GetTokenInformation(rawTokenHandle, TokenElevation, &tokenElevation, sizeof(tokenElevation), &returnedSize) == FALSE)
        {
            return false;
        }
        elevated = tokenElevation.TokenIsElevated != 0;
        return true;
    }

    GB_ProcessArchitecture MachineToArchitecture(const USHORT machine)
    {
        switch (machine)
        {
        case IMAGE_FILE_MACHINE_I386:
            return GB_ProcessArchitecture::X86;
        case IMAGE_FILE_MACHINE_AMD64:
            return GB_ProcessArchitecture::X64;
        case IMAGE_FILE_MACHINE_ARMNT:
            return GB_ProcessArchitecture::Arm32;
        case IMAGE_FILE_MACHINE_ARM64:
            return GB_ProcessArchitecture::Arm64;
        default:
            return GB_ProcessArchitecture::Unknown;
        }
    }

    bool QueryProcessArchitecture(HANDLE processHandle, GB_ProcessArchitecture& architecture)
    {
        typedef BOOL(WINAPI* IsWow64Process2Function)(HANDLE, USHORT*, USHORT*);
        const HMODULE kernelModule = ::GetModuleHandleW(L"kernel32.dll");
        const IsWow64Process2Function isWow64Process2 = kernelModule == nullptr ? nullptr : reinterpret_cast<IsWow64Process2Function>(::GetProcAddress(kernelModule, "IsWow64Process2"));
        if (isWow64Process2 != nullptr)
        {
            USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
            USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
            if (isWow64Process2(processHandle, &processMachine, &nativeMachine) == FALSE)
            {
                return false;
            }
            architecture = MachineToArchitecture(processMachine == IMAGE_FILE_MACHINE_UNKNOWN ? nativeMachine : processMachine);
            return architecture != GB_ProcessArchitecture::Unknown;
        }

        SYSTEM_INFO systemInfo = {};
        ::GetNativeSystemInfo(&systemInfo);
        BOOL isWow64 = FALSE;
        if (::IsWow64Process(processHandle, &isWow64) == FALSE)
        {
            return false;
        }
        if (isWow64 != FALSE)
        {
            architecture = GB_ProcessArchitecture::X86;
            return true;
        }
        switch (systemInfo.wProcessorArchitecture)
        {
        case PROCESSOR_ARCHITECTURE_AMD64:
            architecture = GB_ProcessArchitecture::X64;
            return true;
        case PROCESSOR_ARCHITECTURE_ARM64:
            architecture = GB_ProcessArchitecture::Arm64;
            return true;
        case PROCESSOR_ARCHITECTURE_INTEL:
            architecture = GB_ProcessArchitecture::X86;
            return true;
        case PROCESSOR_ARCHITECTURE_ARM:
            architecture = GB_ProcessArchitecture::Arm32;
            return true;
        default:
            return false;
        }
    }

    bool QueryCriticalProcess(HANDLE processHandle, bool& critical)
    {
        typedef BOOL(WINAPI* IsProcessCriticalFunction)(HANDLE, PBOOL);
        const HMODULE kernelModule = ::GetModuleHandleW(L"kernel32.dll");
        const IsProcessCriticalFunction isProcessCritical = kernelModule == nullptr ? nullptr : reinterpret_cast<IsProcessCriticalFunction>(::GetProcAddress(kernelModule, "IsProcessCritical"));
        if (isProcessCritical == nullptr)
        {
            ::SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
            return false;
        }
        BOOL nativeCritical = FALSE;
        if (isProcessCritical(processHandle, &nativeCritical) == FALSE)
        {
            return false;
        }
        critical = nativeCritical != FALSE;
        return true;
    }

    bool QueryProtectedProcess(HANDLE processHandle, bool& protectedProcess)
    {
        PROCESS_PROTECTION_LEVEL_INFORMATION protectionInformation = {};
        if (::GetProcessInformation(processHandle, ProcessProtectionLevelInfo, &protectionInformation, sizeof(protectionInformation)) == FALSE)
        {
            return false;
        }
        protectedProcess = protectionInformation.ProtectionLevel != PROTECTION_LEVEL_NONE;
        return true;
    }

    GB_SystemResult QueryMainWindow(const uint32_t processId, GB_WindowInfo& windowInfo, bool& found)
    {
        windowInfo = GB_WindowInfo();
        found = false;
        GB_WindowFindOptions findOptions;
        findOptions.processId = processId;
        findOptions.visibleOnly = true;
        findOptions.includeToolWindows = false;
        findOptions.includeCloakedWindows = true;
        findOptions.includeUntitledWindows = true;
        findOptions.applicationWindowsOnly = true;
        return GB_SystemWindow::FindFirstWindow(findOptions, windowInfo, found);
    }

    bool IsMainWindowCandidate(const GB_WindowInfo& windowInfo)
    {
        return !windowInfo.isChildWindow && windowInfo.isVisible && !windowInfo.isToolWindow && windowInfo.isAppWindow;
    }

    void FillCurrentProcessOnlyFields(GB_ProcessInfo& processInfo, const GB_ProcessQueryOptions& queryOptions)
    {
        if (static_cast<uint32_t>(processInfo.processId) != ::GetCurrentProcessId())
        {
            return;
        }
        const wchar_t* commandLine = ::GetCommandLineW();
        if (commandLine != nullptr)
        {
            try
            {
                processInfo.commandLineUtf8 = GB_WStringToUtf8(commandLine);
                processInfo.hasCommandLine = true;
            }
            catch (const std::bad_alloc&)
            {
                throw;
            }
            catch (...)
            {
                AddFieldError(processInfo, queryOptions, "commandLine", ERROR_NO_UNICODE_TRANSLATION, "当前进程命令行转换为 UTF-8 失败。");
            }
        }
        try
        {
            if (QueryCurrentDirectoryUtf8(processInfo.workingDirectoryUtf8))
            {
                processInfo.hasWorkingDirectory = true;
            }
            else
            {
                AddFieldError(processInfo, queryOptions, "workingDirectory", ::GetLastError(), "读取当前进程工作目录失败。");
            }
        }
        catch (const std::bad_alloc&)
        {
            throw;
        }
        catch (...)
        {
            AddFieldError(processInfo, queryOptions, "workingDirectory", ERROR_NO_UNICODE_TRANSLATION, "当前进程工作目录转换为 UTF-8 失败。");
        }
    }

    void FillProcessInfoFromEntry(const PROCESSENTRY32W& processEntry, const GB_ProcessQueryOptions& queryOptions, GB_ProcessInfo& processInfo)
    {
        processInfo = GB_ProcessInfo();
        processInfo.processId = static_cast<int>(processEntry.th32ProcessID);
        processInfo.parentProcessId = static_cast<int>(processEntry.th32ParentProcessID);
        processInfo.threadCount = static_cast<unsigned int>(processEntry.cntThreads);
        processInfo.identity.processId = processEntry.th32ProcessID;
        processInfo.isCurrentProcess = processEntry.th32ProcessID == ::GetCurrentProcessId();
        processInfo.isSystemProcess = processEntry.th32ProcessID == 0 || processEntry.th32ProcessID == 4;
        try
        {
            processInfo.processNameUtf8 = GB_WStringToUtf8(processEntry.szExeFile);
        }
        catch (const std::bad_alloc&)
        {
            throw;
        }
        catch (...)
        {
            AddFieldError(processInfo, queryOptions, "processName", ERROR_NO_UNICODE_TRANSLATION, "进程名称转换为 UTF-8 失败。");
        }

        DWORD sessionId = 0;
        if (::ProcessIdToSessionId(processEntry.th32ProcessID, &sessionId) != FALSE)
        {
            processInfo.sessionId = sessionId;
            processInfo.hasSessionId = true;
        }
        else
        {
            AddFieldError(processInfo, queryOptions, "sessionId", ::GetLastError(), "读取进程会话 ID 失败。");
        }

        const DWORD desiredAccess = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
        HANDLE rawProcessHandle = ::OpenProcess(desiredAccess, FALSE, processEntry.th32ProcessID);
        if (rawProcessHandle == nullptr)
        {
            const DWORD openError = ::GetLastError();
            processInfo.processState = GB_ProcessState::Unknown;
            processInfo.stateUtf8 = "Unknown";
            AddFieldError(processInfo, queryOptions, "processHandle", openError, "无法以最小查询权限打开进程。");
            FillCurrentProcessOnlyFields(processInfo, queryOptions);
            return;
        }
        GB_WinHandleScope processHandle = GB_WinHandleScope::FromKernelHandle(rawProcessHandle, "ProcessQueryHandle");

        const DWORD processWaitResult = ::WaitForSingleObject(rawProcessHandle, 0);
        if (processWaitResult == WAIT_TIMEOUT)
        {
            processInfo.processState = GB_ProcessState::Running;
            processInfo.stateUtf8 = "Running";
        }
        else if (processWaitResult == WAIT_OBJECT_0)
        {
            DWORD nativeExitCode = 0;
            if (::GetExitCodeProcess(rawProcessHandle, &nativeExitCode) != FALSE)
            {
                processInfo.processState = GB_ProcessState::Exited;
                processInfo.stateUtf8 = "Exited";
                processInfo.exitCode = nativeExitCode;
                processInfo.hasExitCode = true;
            }
            else
            {
                AddFieldError(processInfo, queryOptions, "exitCode", ::GetLastError(), "读取已退出进程的退出码失败。");
            }
        }
        else
        {
            AddFieldError(processInfo, queryOptions, "processState", ::GetLastError(), "等待进程内核对象状态失败。");
        }

        if (queryOptions.queryTimes)
        {
            FILETIME creationTime = {};
            FILETIME exitTime = {};
            FILETIME kernelTime = {};
            FILETIME userTime = {};
            if (TryGetProcessTimes(rawProcessHandle, creationTime, exitTime, kernelTime, userTime))
            {
                processInfo.identity.creationTime100Nanoseconds = FileTimeToUInt64(creationTime);
                processInfo.identity.hasCreationTime = true;
                processInfo.startTimeUnixMs = FileTimeToUnixMilliseconds(creationTime);
                processInfo.hasStartTime = processInfo.startTimeUnixMs != 0;
                processInfo.cpuKernelSeconds = FileTimeToSeconds(kernelTime);
                processInfo.cpuUserSeconds = FileTimeToSeconds(userTime);
                processInfo.hasCpuTimes = true;
                if (processInfo.processState == GB_ProcessState::Exited)
                {
                    processInfo.exitTimeUnixMs = FileTimeToUnixMilliseconds(exitTime);
                    processInfo.hasExitTime = processInfo.exitTimeUnixMs != 0;
                }
            }
            else
            {
                AddFieldError(processInfo, queryOptions, "processTimes", ::GetLastError(), "读取进程时间失败。");
            }
        }
        else
        {
            FILETIME creationTime = {};
            FILETIME exitTime = {};
            FILETIME kernelTime = {};
            FILETIME userTime = {};
            if (TryGetProcessTimes(rawProcessHandle, creationTime, exitTime, kernelTime, userTime))
            {
                processInfo.identity.creationTime100Nanoseconds = FileTimeToUInt64(creationTime);
                processInfo.identity.hasCreationTime = true;
            }
            else
            {
                AddFieldError(processInfo, queryOptions, "identity", ::GetLastError(), "读取进程创建时间失败，无法建立强身份。");
            }
        }

        if (queryOptions.queryExecutablePath)
        {
            try
            {
                if (QueryProcessPath(rawProcessHandle, processInfo.executablePathUtf8))
                {
                    processInfo.hasExecutablePath = true;
                }
                else
                {
                    AddFieldError(processInfo, queryOptions, "executablePath", ::GetLastError(), "读取进程可执行路径失败。");
                }
            }
            catch (const std::bad_alloc&)
            {
                throw;
            }
            catch (...)
            {
                AddFieldError(processInfo, queryOptions, "executablePath", ERROR_NO_UNICODE_TRANSLATION, "进程路径转换为 UTF-8 失败。");
            }
        }

        if (queryOptions.queryUserName)
        {
            try
            {
                if (QueryProcessUserName(rawProcessHandle, processInfo.userNameUtf8))
                {
                    processInfo.hasUserName = true;
                }
                else
                {
                    AddFieldError(processInfo, queryOptions, "userName", ::GetLastError(), "读取进程用户名失败。");
                }
            }
            catch (const std::bad_alloc&)
            {
                throw;
            }
            catch (...)
            {
                AddFieldError(processInfo, queryOptions, "userName", ERROR_NO_UNICODE_TRANSLATION, "进程用户名转换为 UTF-8 失败。");
            }
        }

        if (queryOptions.queryElevation)
        {
            if (QueryProcessElevation(rawProcessHandle, processInfo.isElevated))
            {
                processInfo.hasElevationState = true;
            }
            else
            {
                AddFieldError(processInfo, queryOptions, "elevation", ::GetLastError(), "读取进程提升状态失败。");
            }
        }

        if (queryOptions.queryArchitecture)
        {
            if (QueryProcessArchitecture(rawProcessHandle, processInfo.architecture))
            {
                processInfo.hasArchitecture = true;
                processInfo.is64Bit = processInfo.architecture == GB_ProcessArchitecture::X64 || processInfo.architecture == GB_ProcessArchitecture::Arm64;
            }
            else
            {
                AddFieldError(processInfo, queryOptions, "architecture", ::GetLastError(), "读取进程架构失败。");
            }
        }

        if (queryOptions.queryHandleCount)
        {
            DWORD handleCount = 0;
            if (::GetProcessHandleCount(rawProcessHandle, &handleCount) != FALSE)
            {
                processInfo.handleCount = handleCount;
                processInfo.hasHandleCount = true;
            }
            else
            {
                AddFieldError(processInfo, queryOptions, "handleCount", ::GetLastError(), "读取进程句柄数量失败。");
            }
        }

        if (queryOptions.queryPriority)
        {
            const DWORD priorityClass = ::GetPriorityClass(rawProcessHandle);
            if (priorityClass != 0)
            {
                processInfo.priorityClass = priorityClass;
                processInfo.hasPriorityClass = true;
            }
            else
            {
                AddFieldError(processInfo, queryOptions, "priorityClass", ::GetLastError(), "读取进程优先级失败。");
            }
        }

        if (queryOptions.queryProtection)
        {
            if (QueryCriticalProcess(rawProcessHandle, processInfo.isCriticalProcess))
            {
                processInfo.hasCriticalProcessState = true;
            }
            else
            {
                AddFieldError(processInfo, queryOptions, "criticalProcess", ::GetLastError(), "读取关键进程状态失败。");
            }
            if (QueryProtectedProcess(rawProcessHandle, processInfo.isProtectedProcess))
            {
                processInfo.hasProtectedProcessState = true;
            }
            else
            {
                AddFieldError(processInfo, queryOptions, "protectedProcess", ::GetLastError(), "读取受保护进程状态失败。");
            }
        }

        if (queryOptions.queryMemory)
        {
            HANDLE rawMemoryHandle = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processEntry.th32ProcessID);
            if (rawMemoryHandle != nullptr)
            {
                GB_WinHandleScope memoryHandle = GB_WinHandleScope::FromKernelHandle(rawMemoryHandle, "ProcessMemoryQueryHandle");
                PROCESS_MEMORY_COUNTERS_EX memoryCounters = {};
                memoryCounters.cb = sizeof(memoryCounters);
                if (::GetProcessMemoryInfo(rawMemoryHandle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memoryCounters), sizeof(memoryCounters)) != FALSE)
                {
                    processInfo.residentSetBytes = static_cast<unsigned long long>(memoryCounters.WorkingSetSize);
                    processInfo.peakResidentSetBytes = static_cast<unsigned long long>(memoryCounters.PeakWorkingSetSize);
                    processInfo.privateMemoryBytes = static_cast<unsigned long long>(memoryCounters.PrivateUsage);
                    processInfo.virtualMemoryBytes = static_cast<unsigned long long>(memoryCounters.PagefileUsage != 0 ? memoryCounters.PagefileUsage : memoryCounters.PrivateUsage);
                    processInfo.hasMemoryInfo = true;
                }
                else
                {
                    AddFieldError(processInfo, queryOptions, "memory", ::GetLastError(), "读取进程内存统计失败。");
                }
            }
            else
            {
                AddFieldError(processInfo, queryOptions, "memory", ::GetLastError(), "无法以读取内存统计所需权限打开进程。");
            }
        }

        FillCurrentProcessOnlyFields(processInfo, queryOptions);
        if (queryOptions.queryMainWindow)
        {
            GB_WindowInfo mainWindowInfo;
            bool found = false;
            const GB_SystemResult mainWindowResult = QueryMainWindow(processEntry.th32ProcessID, mainWindowInfo, found);
            processInfo.hasMainWindow = mainWindowResult.IsSucceeded() && found;
            if (processInfo.hasMainWindow)
            {
                processInfo.mainWindowId = mainWindowInfo.windowId;
                processInfo.mainWindowTitle = mainWindowInfo.title;
            }
            if (mainWindowResult.IsFailed())
            {
                AddFieldResultError(processInfo, queryOptions, "mainWindow", mainWindowResult, "查询进程主窗口失败。");
            }
        }
    }

    GB_SystemResult FindProcessEntry(const uint32_t processId, PROCESSENTRY32W& processEntry)
    {
        GB_WinHandleScope snapshotHandle = GB_WinHandleScope::FromKernelHandle(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), "ProcessSnapshot");
        if (!snapshotHandle.IsValid())
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationGetInfo, "创建进程快照失败。");
        }
        PROCESSENTRY32W currentEntry = {};
        currentEntry.dwSize = sizeof(currentEntry);
        if (::Process32FirstW(snapshotHandle.GetHandleAs<HANDLE>(), &currentEntry) == FALSE)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationGetInfo, "读取进程快照首项失败。");
        }
        do
        {
            if (currentEntry.th32ProcessID == processId)
            {
                processEntry = currentEntry;
                return GB_SystemResult::Succeeded(GB_ProcessOperationGetInfo);
            }
        } while (::Process32NextW(snapshotHandle.GetHandleAs<HANDLE>(), &currentEntry) != FALSE);
        const DWORD enumerationError = ::GetLastError();
        if (enumerationError != ERROR_NO_MORE_FILES)
        {
            return GB_SystemResult::FromWin32Error(enumerationError, GB_ProcessOperationGetInfo, "遍历进程快照失败。");
        }
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_ProcessOperationGetInfo, "目标进程不存在。");
    }

    bool TextMatches(const std::string& value, const std::string& expected, const bool exactMatch, const bool caseSensitive)
    {
        if (expected.empty())
        {
            return true;
        }
        return exactMatch ? GB_Utf8Equals(value, expected, caseSensitive) : GB_Utf8Find(value, expected, caseSensitive) >= 0;
    }

    GB_SystemResult ValidateFindOptions(const GB_ProcessFindOptions& findOptions)
    {
        if (!GB_IsUtf8(findOptions.processName) || !GB_IsUtf8(findOptions.executablePath) || !GB_IsUtf8(findOptions.commandLineContains) || ContainsEmbeddedNull(findOptions.processName) || ContainsEmbeddedNull(findOptions.executablePath) || ContainsEmbeddedNull(findOptions.commandLineContains))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationFind, "进程查找条件包含非法 UTF-8 或嵌入式 NUL。");
        }
        return GB_SystemResult::Succeeded(GB_ProcessOperationFind);
    }

    GB_SystemResult OpenAndValidateStrongIdentity(const GB_ProcessIdentity& identity, const DWORD desiredAccess, GB_WinHandleScope& processHandle, const std::string& operationName)
    {
        processHandle.Reset();
        if (!identity.IsStrong())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "该操作要求包含创建时间的强进程身份。");
        }
        HANDLE rawProcessHandle = ::OpenProcess(desiredAccess, FALSE, identity.processId);
        if (rawProcessHandle == nullptr)
        {
            const DWORD errorCode = ::GetLastError();
            if (errorCode == ERROR_INVALID_PARAMETER)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, "目标进程不存在。");
            }
            return GB_SystemResult::FromWin32Error(errorCode, operationName, "打开目标进程失败。");
        }
        processHandle = GB_WinHandleScope::FromKernelHandle(rawProcessHandle, "ValidatedProcessHandle");
        return ValidateProcessIdentity(rawProcessHandle, identity, operationName);
    }

    GB_SystemResult ValidateTerminationSafety(const GB_ProcessInfo& processInfo, const std::string& operationName)
    {
        if (processInfo.processId <= 0 || static_cast<uint32_t>(processInfo.processId) == ::GetCurrentProcessId())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::PermissionDenied, operationName, "默认安全策略禁止终止当前进程或无效进程。");
        }
        if (processInfo.processId == 4 || processInfo.isSystemProcess || (processInfo.hasCriticalProcessState && processInfo.isCriticalProcess) || (processInfo.hasProtectedProcessState && processInfo.isProtectedProcess))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::PermissionDenied, operationName, "默认安全策略禁止终止系统、关键或受保护进程。");
        }
        if (!processInfo.hasCriticalProcessState || !processInfo.hasProtectedProcessState)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::PermissionDenied, operationName, "无法完整确认目标进程安全属性，拒绝执行强制终止。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    GB_ProcessQueryOptions MakeTerminationQueryOptions()
    {
        GB_ProcessQueryOptions queryOptions;
        queryOptions.queryExecutablePath = false;
        queryOptions.queryUserName = false;
        queryOptions.queryElevation = false;
        queryOptions.queryArchitecture = false;
        queryOptions.queryTimes = false;
        queryOptions.queryMemory = false;
        queryOptions.queryHandleCount = false;
        queryOptions.queryPriority = false;
        queryOptions.queryProtection = true;
        queryOptions.queryMainWindow = false;
        return queryOptions;
    }

    GB_SystemResult ConvertCapturedBytes(std::string bytes, const bool truncated, const GB_ProcessOutputEncoding encoding, std::string& text)
    {
        text.clear();
        try
        {
            if (encoding == GB_ProcessOutputEncoding::Utf8)
            {
                const size_t offset = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF ? 3 : 0;
                text.assign(bytes.data() + offset, bytes.size() - offset);
                if (truncated && !GB_IsUtf8(text))
                {
                    for (size_t removedByteCount = 0; removedByteCount < 3 && !text.empty() && !GB_IsUtf8(text); removedByteCount++)
                    {
                        text.pop_back();
                    }
                }
                if (!GB_IsUtf8(text))
                {
                    text.clear();
                    return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, "ConvertProcessOutput", "捕获输出不是合法 UTF-8。");
                }
                return GB_SystemResult::Succeeded("ConvertProcessOutput");
            }

            std::wstring wideText;
            if (encoding == GB_ProcessOutputEncoding::Utf16LittleEndian)
            {
                size_t byteOffset = 0;
                if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE)
                {
                    byteOffset = 2;
                }
                else if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, "ConvertProcessOutput", "不支持 UTF-16BE 子进程输出。");
                }
                if ((bytes.size() - byteOffset) % sizeof(wchar_t) != 0)
                {
                    if (!truncated)
                    {
                        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, "ConvertProcessOutput", "UTF-16LE 输出包含不完整代码单元。");
                    }
                    bytes.pop_back();
                }
                wideText.resize((bytes.size() - byteOffset) / sizeof(wchar_t));
                if (!wideText.empty())
                {
                    std::memcpy(&wideText[0], bytes.data() + byteOffset, bytes.size() - byteOffset);
                }
                if (truncated && !wideText.empty() && wideText.back() >= 0xD800 && wideText.back() <= 0xDBFF)
                {
                    wideText.pop_back();
                }
            }
            else
            {
                const UINT codePage = encoding == GB_ProcessOutputEncoding::Oem ? CP_OEMCP : CP_ACP;
                if (bytes.empty())
                {
                    return GB_SystemResult::Succeeded("ConvertProcessOutput");
                }
                const int wideLength = ::MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
                if (wideLength <= 0)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, "ConvertProcessOutput", "按指定代码页解析子进程输出失败。");
                }
                wideText.resize(static_cast<size_t>(wideLength));
                if (::MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), &wideText[0], wideLength) != wideLength)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, "ConvertProcessOutput", "按指定代码页转换子进程输出失败。");
                }
            }
            text = GB_WStringToUtf8(wideText);
            return GB_SystemResult::Succeeded("ConvertProcessOutput");
        }
        catch (const std::bad_alloc&)
        {
            return MakeAllocationFailedResult("ConvertProcessOutput");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, "ConvertProcessOutput", "子进程输出转换为 UTF-8 失败。");
        }
    }
#endif
}

#if defined(_WIN32)
namespace
{
    struct ParentChildPipe
    {
        GB_WinHandleScope parentHandle;
        GB_WinHandleScope childHandle;
    };

    std::atomic<uint64_t> pipeSequence(0);

    GB_SystemResult CreateNamedPipePair(const bool parentReads, const std::string& resourceName, ParentChildPipe& pipePair)
    {
        pipePair = ParentChildPipe();
        const uint64_t sequence = pipeSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::wstring pipeName = L"\\\\.\\pipe\\GlobalBase.SystemProcess." + std::to_wstring(::GetCurrentProcessId()) + L"." + std::to_wstring(::GetTickCount64()) + L"." + std::to_wstring(sequence);
        const DWORD openMode = (parentReads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
        const HANDLE parentHandle = ::CreateNamedPipeW(pipeName.c_str(), openMode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
        if (parentHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "创建父进程命名管道端失败。");
        }
        pipePair.parentHandle = GB_WinHandleScope::FromKernelHandle(parentHandle, resourceName + "Parent");

        SECURITY_ATTRIBUTES securityAttributes = {};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;
        const DWORD childAccess = parentReads ? GENERIC_WRITE : GENERIC_READ;
        const HANDLE childHandle = ::CreateFileW(pipeName.c_str(), childAccess, 0, &securityAttributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (childHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "连接子进程命名管道端失败。");
        }
        pipePair.childHandle = GB_WinHandleScope::FromKernelHandle(childHandle, resourceName + "Child");
        if (::SetHandleInformation(childHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) == FALSE || ::SetHandleInformation(parentHandle, HANDLE_FLAG_INHERIT, 0) == FALSE)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "设置进程管道句柄继承属性失败。");
        }
        return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
    }

    GB_SystemResult DuplicateStandardHandleForChild(const DWORD standardHandleId, const DWORD nullAccess, const std::string& resourceName, GB_WinHandleScope& childHandle)
    {
        childHandle.Reset();
        SECURITY_ATTRIBUTES securityAttributes = {};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;

        const HANDLE standardHandle = ::GetStdHandle(standardHandleId);
        HANDLE duplicatedHandle = nullptr;
        if (standardHandle != nullptr && standardHandle != INVALID_HANDLE_VALUE && ::DuplicateHandle(::GetCurrentProcess(), standardHandle, ::GetCurrentProcess(), &duplicatedHandle, 0, TRUE, DUPLICATE_SAME_ACCESS) != FALSE)
        {
            childHandle = GB_WinHandleScope::FromKernelHandle(duplicatedHandle, resourceName);
            return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
        }

        duplicatedHandle = ::CreateFileW(L"NUL", nullAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, &securityAttributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (duplicatedHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "无法复制当前标准句柄，也无法创建 NUL 降级句柄。");
        }
        childHandle = GB_WinHandleScope::FromKernelHandle(duplicatedHandle, resourceName);
        return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
    }

    void AddUniqueHandle(std::vector<HANDLE>& handles, const HANDLE handle)
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE && std::find(handles.begin(), handles.end(), handle) == handles.end())
        {
            handles.push_back(handle);
        }
    }

    GB_SystemResult ConfigureJobObject(const GB_ProcessJobOptions& options, GB_WinHandleScope& jobHandle)
    {
        jobHandle.Reset();
        if (!options.enabled)
        {
            return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
        }
        const HANDLE rawJobHandle = ::CreateJobObjectW(nullptr, nullptr);
        if (rawJobHandle == nullptr)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "创建 Job Object 失败。");
        }
        jobHandle = GB_WinHandleScope::FromKernelHandle(rawJobHandle, "ProcessJob");

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limitInformation = {};
        if (options.terminateOnJobClose)
        {
            limitInformation.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        }
        if (options.maximumActiveProcessCount != 0)
        {
            limitInformation.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
            limitInformation.BasicLimitInformation.ActiveProcessLimit = options.maximumActiveProcessCount;
        }
        if (options.processMemoryLimitBytes != 0)
        {
            if (options.processMemoryLimitBytes > static_cast<uint64_t>((std::numeric_limits<SIZE_T>::max)()))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "单进程内存限制超过当前平台 SIZE_T 可表示范围。");
            }
            limitInformation.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            limitInformation.ProcessMemoryLimit = static_cast<SIZE_T>(options.processMemoryLimitBytes);
        }
        if (options.jobMemoryLimitBytes != 0)
        {
            if (options.jobMemoryLimitBytes > static_cast<uint64_t>((std::numeric_limits<SIZE_T>::max)()))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationStart, "Job 内存限制超过当前平台 SIZE_T 可表示范围。");
            }
            limitInformation.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
            limitInformation.JobMemoryLimit = static_cast<SIZE_T>(options.jobMemoryLimitBytes);
        }
        if (options.breakawayAllowed)
        {
            limitInformation.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        }
        if (limitInformation.BasicLimitInformation.LimitFlags != 0 && ::SetInformationJobObject(rawJobHandle, JobObjectExtendedLimitInformation, &limitInformation, sizeof(limitInformation)) == FALSE)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "设置 Job Object 扩展限制失败。");
        }

        if (options.cpuRatePercent != 0)
        {
            JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpuInformation = {};
            cpuInformation.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
            cpuInformation.CpuRate = options.cpuRatePercent * 100;
            if (::SetInformationJobObject(rawJobHandle, JobObjectCpuRateControlInformation, &cpuInformation, sizeof(cpuInformation)) == FALSE)
            {
                return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "当前系统不支持请求的 Job CPU 硬上限，或设置失败。");
            }
        }
        return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
    }

    GB_SystemResult WaitForHandleWithCancellation(const HANDLE waitHandle, const GB_ProcessWaitOptions& waitOptions, const std::string& operationName)
    {
        const GB_SystemResult validationResult = ValidateWaitOptions(waitOptions, operationName);
        if (validationResult.IsFailed())
        {
            return validationResult;
        }
        const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
        while (true)
        {
            if (IsCancellationRequested(waitOptions))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, operationName, "进程等待操作已取消。");
            }

            DWORD waitMilliseconds = waitOptions.pollIntervalMilliseconds;
            if (waitOptions.timeoutMilliseconds == 0)
            {
                waitMilliseconds = 0;
            }
            else if (waitOptions.timeoutMilliseconds > 0)
            {
                const uint64_t elapsedMilliseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count());
                if (elapsedMilliseconds >= static_cast<uint64_t>(waitOptions.timeoutMilliseconds))
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, operationName, "等待进程状态超时。");
                }
                const uint64_t remainingMilliseconds = static_cast<uint64_t>(waitOptions.timeoutMilliseconds) - elapsedMilliseconds;
                waitMilliseconds = static_cast<DWORD>((std::min)(remainingMilliseconds, static_cast<uint64_t>(waitOptions.pollIntervalMilliseconds)));
            }
            if (waitOptions.cancellationFlag != nullptr)
            {
                waitMilliseconds = (std::min)(waitMilliseconds, static_cast<DWORD>(50));
            }

            const DWORD waitResult = ::WaitForSingleObject(waitHandle, waitMilliseconds);
            if (waitResult == WAIT_OBJECT_0)
            {
                return GB_SystemResult::Succeeded(operationName);
            }
            if (waitResult == WAIT_FAILED)
            {
                return GB_SystemResult::FromLastWin32Error(operationName, "等待进程句柄失败。");
            }
            if (waitOptions.timeoutMilliseconds == 0)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, operationName, "目标状态尚未满足。");
            }
        }
    }
}

class GB_ProcessInstance::Impl final
{
public:
    Impl() = default;

    ~Impl()
    {
        Shutdown();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    GB_SystemResult StartWorkers()
    {
        if (!standardOutputReadHandle.IsValid() && !standardErrorReadHandle.IsValid())
        {
            return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
        }
        const HANDLE rawStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (rawStopEvent == nullptr)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "创建输出读取停止事件失败。");
        }
        ioStopEvent = GB_WinHandleScope::FromKernelHandle(rawStopEvent, "ProcessIoStopEvent");

        try
        {
            if (outputCallback)
            {
                callbackThread = std::thread(&Impl::CallbackThreadMain, this);
            }
            ioThread = std::thread(&Impl::IoThreadMain, this);
            ioStarted.store(true, std::memory_order_release);
        }
        catch (const std::system_error& exception)
        {
            callbackStopping.store(true, std::memory_order_release);
            callbackCondition.notify_all();
            if (callbackThread.joinable())
            {
                callbackThread.join();
            }
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ProcessOperationStart, std::string("创建进程 I/O 工作线程失败：") + exception.what());
        }
        return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
    }

    void Shutdown()
    {
        if (shutdownStarted.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        (void)CloseStandardInputInternal();
        if (terminateOnJobClose && jobHandle.IsValid())
        {
            (void)jobHandle.Close();
            if (processHandle.IsValid())
            {
                (void)::WaitForSingleObject(processHandle.GetHandleAs<HANDLE>(), 5000);
            }
        }
        StopIoWorker(false);
        callbackStopping.store(true, std::memory_order_release);
        callbackCondition.notify_all();
        if (callbackThread.joinable())
        {
            callbackThread.join();
        }
    }

    void StopIoWorker(const bool allowDrain)
    {
        std::lock_guard<std::mutex> workerLock(ioWorkerMutex);
        if (!ioThread.joinable())
        {
            return;
        }
        if (allowDrain)
        {
            std::unique_lock<std::mutex> lock(ioDoneMutex);
            (void)ioDoneCondition.wait_for(lock, std::chrono::milliseconds(5000), [this]() { return ioCompleted.load(std::memory_order_acquire); });
        }
        if (!ioCompleted.load(std::memory_order_acquire))
        {
            MarkOutputIncomplete();
            if (ioStopEvent.IsValid())
            {
                (void)::SetEvent(ioStopEvent.GetHandleAs<HANDLE>());
            }
            if (standardOutputReadHandle.IsValid())
            {
                (void)::CancelIoEx(standardOutputReadHandle.GetHandleAs<HANDLE>(), nullptr);
            }
            if (standardErrorReadHandle.IsValid())
            {
                (void)::CancelIoEx(standardErrorReadHandle.GetHandleAs<HANDLE>(), nullptr);
            }
        }
        ioThread.join();
    }

    GB_SystemResult CloseStandardInputInternal()
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        if (!standardInputWriteHandle.IsValid())
        {
            return GB_SystemResult::Succeeded("GB_ProcessInstance::CloseStandardInput");
        }
        return standardInputWriteHandle.Close().WithOperationName("GB_ProcessInstance::CloseStandardInput");
    }

    GB_SystemResult GetCapturedOutput(const bool standardError, std::string& output, bool& truncated) const
    {
        std::string bytes;
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            bytes = standardError ? standardErrorBytes : standardOutputBytes;
            truncated = standardError ? standardErrorTruncated : standardOutputTruncated;
        }
        const bool temporarilyIncomplete = ioStarted.load(std::memory_order_acquire) && !ioCompleted.load(std::memory_order_acquire);
        return ConvertCapturedBytes(std::move(bytes), truncated || temporarilyIncomplete, outputEncoding, output);
    }

    void MarkOutputTruncated(const GB_ProcessOutputStream outputStream)
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        if (outputStream == GB_ProcessOutputStream::StandardOutput)
        {
            standardOutputTruncated = true;
        }
        else
        {
            standardErrorTruncated = true;
        }
    }

    void MarkOutputIncomplete()
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        if (standardOutputReadHandle.IsValid())
        {
            standardOutputTruncated = true;
        }
        if (standardErrorReadHandle.IsValid())
        {
            standardErrorTruncated = true;
        }
    }

    void AppendOutput(const GB_ProcessOutputStream outputStream, const char* bytes, const size_t byteCount)
    {
        if (byteCount == 0)
        {
            return;
        }
        try
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            std::string& target = outputStream == GB_ProcessOutputStream::StandardOutput ? standardOutputBytes : standardErrorBytes;
            bool& truncated = outputStream == GB_ProcessOutputStream::StandardOutput ? standardOutputTruncated : standardErrorTruncated;
            const size_t remaining = target.size() >= maximumCapturedBytesPerStream ? 0 : maximumCapturedBytesPerStream - target.size();
            const size_t appendCount = (std::min)(remaining, byteCount);
            if (appendCount != 0)
            {
                target.append(bytes, appendCount);
            }
            if (appendCount != byteCount)
            {
                truncated = true;
            }
        }
        catch (...)
        {
            MarkOutputTruncated(outputStream);
        }
        if (outputCallback)
        {
            try
            {
                GB_ProcessOutputEvent outputEvent;
                outputEvent.outputStream = outputStream;
                outputEvent.bytes.assign(reinterpret_cast<const uint8_t*>(bytes), reinterpret_cast<const uint8_t*>(bytes) + byteCount);
                {
                    std::lock_guard<std::mutex> lock(callbackMutex);
                    if (callbackQueue.size() >= maximumPendingOutputCallbacks)
                    {
                        callbackQueue.pop_front();
                        droppedOutputCallbackCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    callbackQueue.push_back(std::move(outputEvent));
                }
                callbackCondition.notify_one();
            }
            catch (...)
            {
                droppedOutputCallbackCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void CallbackThreadMain()
    {
        while (true)
        {
            GB_ProcessOutputEvent outputEvent;
            {
                std::unique_lock<std::mutex> lock(callbackMutex);
                callbackCondition.wait(lock, [this]() { return callbackStopping.load(std::memory_order_acquire) || !callbackQueue.empty(); });
                if (callbackQueue.empty())
                {
                    if (callbackStopping.load(std::memory_order_acquire))
                    {
                        return;
                    }
                    continue;
                }
                outputEvent = std::move(callbackQueue.front());
                callbackQueue.pop_front();
            }
            try
            {
                outputCallback(outputEvent);
            }
            catch (...)
            {
            }
        }
    }

    struct ReaderState
    {
        HANDLE pipeHandle = nullptr;
        GB_WinHandleScope eventHandle;
        OVERLAPPED overlapped = {};
        std::vector<char> buffer;
        GB_ProcessOutputStream outputStream = GB_ProcessOutputStream::StandardOutput;
        bool pending = false;
        bool completed = false;
    };

    bool IssueRead(ReaderState& reader)
    {
        if (reader.completed || reader.pending)
        {
            return false;
        }
        (void)::ResetEvent(reader.eventHandle.GetHandleAs<HANDLE>());
        reader.overlapped = OVERLAPPED();
        reader.overlapped.hEvent = reader.eventHandle.GetHandleAs<HANDLE>();
        DWORD bytesRead = 0;
        if (::ReadFile(reader.pipeHandle, reader.buffer.data(), static_cast<DWORD>(reader.buffer.size()), &bytesRead, &reader.overlapped) != FALSE)
        {
            if (bytesRead == 0)
            {
                reader.completed = true;
                return false;
            }
            AppendOutput(reader.outputStream, reader.buffer.data(), bytesRead);
            return true;
        }
        const DWORD errorCode = ::GetLastError();
        if (errorCode == ERROR_IO_PENDING)
        {
            reader.pending = true;
            return false;
        }
        if (errorCode == ERROR_BROKEN_PIPE || errorCode == ERROR_PIPE_NOT_CONNECTED || errorCode == ERROR_OPERATION_ABORTED)
        {
            reader.completed = true;
            return false;
        }
        MarkOutputTruncated(reader.outputStream);
        reader.completed = true;
        return false;
    }

    void CompleteRead(ReaderState& reader)
    {
        DWORD bytesRead = 0;
        reader.pending = false;
        if (::GetOverlappedResult(reader.pipeHandle, &reader.overlapped, &bytesRead, FALSE) != FALSE)
        {
            if (bytesRead != 0)
            {
                AppendOutput(reader.outputStream, reader.buffer.data(), bytesRead);
            }
            else
            {
                reader.completed = true;
            }
            return;
        }
        const DWORD errorCode = ::GetLastError();
        if (errorCode == ERROR_IO_INCOMPLETE)
        {
            reader.pending = true;
            return;
        }
        if (errorCode != ERROR_BROKEN_PIPE && errorCode != ERROR_PIPE_NOT_CONNECTED && errorCode != ERROR_OPERATION_ABORTED)
        {
            MarkOutputTruncated(reader.outputStream);
        }
        reader.completed = true;
    }

    void CancelPendingRead(ReaderState& reader)
    {
        if (!reader.pending)
        {
            return;
        }
        (void)::CancelIoEx(reader.pipeHandle, &reader.overlapped);
        DWORD bytesRead = 0;
        if (::GetOverlappedResult(reader.pipeHandle, &reader.overlapped, &bytesRead, TRUE) != FALSE && bytesRead != 0)
        {
            AppendOutput(reader.outputStream, reader.buffer.data(), bytesRead);
        }
        reader.pending = false;
        reader.completed = true;
    }

    void IoThreadMain()
    {
        ReaderState standardOutputReader;
        ReaderState standardErrorReader;
        std::vector<ReaderState*> readers;
        try
        {
            if (standardOutputReadHandle.IsValid())
            {
                standardOutputReader.pipeHandle = standardOutputReadHandle.GetHandleAs<HANDLE>();
                standardOutputReader.eventHandle = GB_WinHandleScope::FromKernelHandle(::CreateEventW(nullptr, TRUE, FALSE, nullptr), "StandardOutputReadEvent");
                standardOutputReader.buffer.resize(65536);
                standardOutputReader.outputStream = GB_ProcessOutputStream::StandardOutput;
                if (!standardOutputReader.eventHandle.IsValid())
                {
                    throw std::bad_alloc();
                }
                readers.push_back(&standardOutputReader);
            }
            if (standardErrorReadHandle.IsValid())
            {
                standardErrorReader.pipeHandle = standardErrorReadHandle.GetHandleAs<HANDLE>();
                standardErrorReader.eventHandle = GB_WinHandleScope::FromKernelHandle(::CreateEventW(nullptr, TRUE, FALSE, nullptr), "StandardErrorReadEvent");
                standardErrorReader.buffer.resize(65536);
                standardErrorReader.outputStream = GB_ProcessOutputStream::StandardError;
                if (!standardErrorReader.eventHandle.IsValid())
                {
                    throw std::bad_alloc();
                }
                readers.push_back(&standardErrorReader);
            }
            while (true)
            {
                bool allCompleted = true;
                bool synchronousProgress = false;
                std::vector<HANDLE> waitHandles;
                std::vector<ReaderState*> waitReaders;
                waitHandles.push_back(ioStopEvent.GetHandleAs<HANDLE>());
                for (size_t readerIndex = 0; readerIndex < readers.size(); readerIndex++)
                {
                    ReaderState* reader = readers[readerIndex];
                    if (!reader->completed && !reader->pending)
                    {
                        synchronousProgress = IssueRead(*reader) || synchronousProgress;
                    }
                    allCompleted = allCompleted && reader->completed;
                    if (reader->pending && !reader->completed)
                    {
                        waitHandles.push_back(reader->eventHandle.GetHandleAs<HANDLE>());
                        waitReaders.push_back(reader);
                    }
                }
                if (allCompleted)
                {
                    break;
                }
                const DWORD waitResult = ::WaitForMultipleObjects(static_cast<DWORD>(waitHandles.size()), waitHandles.data(), FALSE, synchronousProgress ? 0 : INFINITE);
                if (waitResult == WAIT_TIMEOUT)
                {
                    continue;
                }
                if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED)
                {
                    if (waitResult == WAIT_FAILED)
                    {
                        MarkOutputIncomplete();
                    }
                    for (size_t readerIndex = 0; readerIndex < readers.size(); readerIndex++)
                    {
                        CancelPendingRead(*readers[readerIndex]);
                    }
                    break;
                }
                const DWORD signaledIndex = waitResult - WAIT_OBJECT_0 - 1;
                if (signaledIndex < waitReaders.size())
                {
                    CompleteRead(*waitReaders[signaledIndex]);
                }
            }
        }
        catch (...)
        {
            for (size_t readerIndex = 0; readerIndex < readers.size(); readerIndex++)
            {
                CancelPendingRead(*readers[readerIndex]);
            }
            MarkOutputIncomplete();
        }
        ioCompleted.store(true, std::memory_order_release);
        ioDoneCondition.notify_all();
        callbackStopping.store(true, std::memory_order_release);
        callbackCondition.notify_all();
    }

public:
    GB_WinHandleScope processHandle;
    GB_WinHandleScope mainThreadHandle;
    GB_WinHandleScope jobHandle;
    GB_WinHandleScope standardInputWriteHandle;
    GB_WinHandleScope standardOutputReadHandle;
    GB_WinHandleScope standardErrorReadHandle;
    GB_WinHandleScope ioStopEvent;
    GB_ProcessIdentity identity;
    uint32_t mainThreadId = 0;
    std::atomic<bool> mainThreadSuspended{ false };
    bool terminateOnJobClose = false;
    GB_ProcessOutputEncoding outputEncoding = GB_ProcessOutputEncoding::Utf8;
    size_t maximumCapturedBytesPerStream = 16 * 1024 * 1024;
    GB_ProcessOutputCallback outputCallback;
    size_t maximumPendingOutputCallbacks = 1024;

    mutable std::mutex outputMutex;
    std::string standardOutputBytes;
    std::string standardErrorBytes;
    bool standardOutputTruncated = false;
    bool standardErrorTruncated = false;

    std::mutex inputMutex;
    std::thread ioThread;
    mutable std::mutex ioWorkerMutex;
    std::atomic<bool> ioStarted{ false };
    std::thread callbackThread;
    std::atomic<bool> ioCompleted{ false };
    std::mutex ioDoneMutex;
    std::condition_variable ioDoneCondition;
    std::atomic<bool> callbackStopping{ false };
    std::mutex callbackMutex;
    std::condition_variable callbackCondition;
    std::deque<GB_ProcessOutputEvent> callbackQueue;
    std::atomic<uint64_t> droppedOutputCallbackCount{ 0 };
    std::atomic<bool> shutdownStarted{ false };
};
#endif

GB_ProcessInstance::GB_ProcessInstance() = default;

GB_ProcessInstance::GB_ProcessInstance(std::unique_ptr<Impl> implementationValue) : implementation(std::move(implementationValue))
{
}

GB_ProcessInstance::~GB_ProcessInstance() = default;

GB_ProcessInstance::GB_ProcessInstance(GB_ProcessInstance&& other) noexcept : implementation(std::move(other.implementation))
{
}

GB_ProcessInstance& GB_ProcessInstance::operator=(GB_ProcessInstance&& other) noexcept
{
    if (this != &other)
    {
        implementation = std::move(other.implementation);
    }
    return *this;
}

void GB_ProcessInstance::Swap(GB_ProcessInstance& other) noexcept
{
    implementation.swap(other.implementation);
}

bool GB_ProcessInstance::IsValid() const
{
#if defined(_WIN32)
    return implementation != nullptr && implementation->processHandle.IsValid();
#else
    return false;
#endif
}

GB_ProcessIdentity GB_ProcessInstance::GetIdentity() const
{
#if defined(_WIN32)
    return implementation == nullptr ? GB_ProcessIdentity() : implementation->identity;
#else
    return GB_ProcessIdentity();
#endif
}

uint32_t GB_ProcessInstance::GetProcessId() const
{
    return GetIdentity().processId;
}

uint32_t GB_ProcessInstance::GetMainThreadId() const
{
#if defined(_WIN32)
    return implementation == nullptr ? 0 : implementation->mainThreadId;
#else
    return 0;
#endif
}

GB_SystemResult GB_ProcessInstance::IsRunning(bool& running) const
{
    running = false;
#if defined(_WIN32)
    if (!IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::IsRunning", "进程实例为空。");
    }
    const DWORD waitResult = ::WaitForSingleObject(implementation->processHandle.GetHandleAs<HANDLE>(), 0);
    if (waitResult == WAIT_TIMEOUT)
    {
        running = true;
        return GB_SystemResult::Succeeded("GB_ProcessInstance::IsRunning");
    }
    if (waitResult == WAIT_OBJECT_0)
    {
        return GB_SystemResult::Succeeded("GB_ProcessInstance::IsRunning");
    }
    return GB_SystemResult::FromLastWin32Error("GB_ProcessInstance::IsRunning", "查询进程运行状态失败。");
#else
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::IsRunning");
#endif
}

GB_SystemResult GB_ProcessInstance::GetExitCode(uint32_t& exitCode, bool& hasExitCode) const
{
    exitCode = 0;
    hasExitCode = false;
#if defined(_WIN32)
    if (!IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::GetExitCode", "进程实例为空。");
    }
    const HANDLE processHandle = implementation->processHandle.GetHandleAs<HANDLE>();
    const DWORD waitResult = ::WaitForSingleObject(processHandle, 0);
    if (waitResult == WAIT_TIMEOUT)
    {
        return GB_SystemResult::Succeeded("GB_ProcessInstance::GetExitCode");
    }
    if (waitResult != WAIT_OBJECT_0)
    {
        return GB_SystemResult::FromLastWin32Error("GB_ProcessInstance::GetExitCode", "等待进程退出状态失败。");
    }
    DWORD nativeExitCode = 0;
    if (::GetExitCodeProcess(processHandle, &nativeExitCode) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error("GB_ProcessInstance::GetExitCode", "读取进程退出码失败。");
    }
    exitCode = nativeExitCode;
    hasExitCode = true;
    return GB_SystemResult::Succeeded("GB_ProcessInstance::GetExitCode");
#else
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::GetExitCode");
#endif
}

GB_SystemResult GB_ProcessInstance::WaitForExit(uint32_t& exitCode, const GB_ProcessWaitOptions& waitOptions)
{
    exitCode = 0;
#if defined(_WIN32)
    if (!IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::WaitForExit", "进程实例为空。");
    }
    GB_SystemResult result = WaitForHandleWithCancellation(implementation->processHandle.GetHandleAs<HANDLE>(), waitOptions, "GB_ProcessInstance::WaitForExit");
    if (result.IsFailed())
    {
        return result;
    }
    DWORD nativeExitCode = 0;
    if (::GetExitCodeProcess(implementation->processHandle.GetHandleAs<HANDLE>(), &nativeExitCode) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error("GB_ProcessInstance::WaitForExit", "进程退出后读取退出码失败。");
    }
    exitCode = nativeExitCode;
    implementation->StopIoWorker(true);
    return GB_SystemResult::Succeeded("GB_ProcessInstance::WaitForExit");
#else
    (void)waitOptions;
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::WaitForExit");
#endif
}

GB_SystemResult GB_ProcessInstance::WaitForInputIdle(const GB_ProcessWaitOptions& waitOptions)
{
#if defined(_WIN32)
    if (!IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::WaitForInputIdle", "进程实例为空。");
    }
    const GB_SystemResult validationResult = ValidateWaitOptions(waitOptions, "GB_ProcessInstance::WaitForInputIdle");
    if (validationResult.IsFailed())
    {
        return validationResult;
    }
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    while (true)
    {
        if (IsCancellationRequested(waitOptions))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, "GB_ProcessInstance::WaitForInputIdle", "等待输入空闲已取消。");
        }
        DWORD waitMilliseconds = waitOptions.timeoutMilliseconds == 0 ? 0 : waitOptions.pollIntervalMilliseconds;
        if (waitOptions.timeoutMilliseconds > 0)
        {
            const uint64_t elapsedMilliseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count());
            if (elapsedMilliseconds >= static_cast<uint64_t>(waitOptions.timeoutMilliseconds))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, "GB_ProcessInstance::WaitForInputIdle", "等待进程输入空闲超时。");
            }
            waitMilliseconds = static_cast<DWORD>((std::min)(static_cast<uint64_t>(waitOptions.pollIntervalMilliseconds), static_cast<uint64_t>(waitOptions.timeoutMilliseconds) - elapsedMilliseconds));
        }
        if (waitOptions.cancellationFlag != nullptr)
        {
            waitMilliseconds = (std::min)(waitMilliseconds, static_cast<DWORD>(50));
        }
        const DWORD waitResult = ::WaitForInputIdle(implementation->processHandle.GetHandleAs<HANDLE>(), waitMilliseconds);
        if (waitResult == 0)
        {
            return GB_SystemResult::Succeeded("GB_ProcessInstance::WaitForInputIdle");
        }
        if (waitResult == WAIT_FAILED)
        {
            return GB_SystemResult::FromLastWin32Error("GB_ProcessInstance::WaitForInputIdle", "WaitForInputIdle 调用失败；目标可能不是 GUI 进程。");
        }
        bool running = false;
        GB_SystemResult runningResult = IsRunning(running);
        if (runningResult.IsFailed())
        {
            return runningResult.WithOperationName("GB_ProcessInstance::WaitForInputIdle");
        }
        if (!running)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, "GB_ProcessInstance::WaitForInputIdle", "进程在进入输入空闲前已经退出。");
        }
        if (waitOptions.timeoutMilliseconds == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, "GB_ProcessInstance::WaitForInputIdle", "进程尚未进入输入空闲状态。");
        }
    }
#else
    (void)waitOptions;
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::WaitForInputIdle");
#endif
}

GB_SystemResult GB_ProcessInstance::WaitForMainWindow(GB_WindowInfo& windowInfo, const GB_ProcessWaitOptions& waitOptions)
{
    windowInfo = GB_WindowInfo();
#if defined(_WIN32)
    if (!IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::WaitForMainWindow", "进程实例为空。");
    }
    const GB_SystemResult validationResult = ValidateWaitOptions(waitOptions, "GB_ProcessInstance::WaitForMainWindow");
    if (validationResult.IsFailed())
    {
        return validationResult;
    }
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    while (true)
    {
        if (IsCancellationRequested(waitOptions))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, "GB_ProcessInstance::WaitForMainWindow", "等待主窗口已取消。");
        }
        bool found = false;
        GB_SystemResult result = QueryMainWindow(GetProcessId(), windowInfo, found);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_ProcessInstance::WaitForMainWindow");
        }
        if (found)
        {
            return GB_SystemResult::Succeeded("GB_ProcessInstance::WaitForMainWindow");
        }
        const DWORD processWaitResult = ::WaitForSingleObject(implementation->processHandle.GetHandleAs<HANDLE>(), 0);
        if (processWaitResult == WAIT_OBJECT_0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, "GB_ProcessInstance::WaitForMainWindow", "进程在创建主窗口前已经退出。");
        }
        if (processWaitResult == WAIT_FAILED)
        {
            return GB_SystemResult::FromLastWin32Error("GB_ProcessInstance::WaitForMainWindow", "查询目标进程退出状态失败。");
        }
        if (waitOptions.timeoutMilliseconds == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, "GB_ProcessInstance::WaitForMainWindow", "目标进程尚未创建主窗口。");
        }
        uint32_t sleepMilliseconds = waitOptions.pollIntervalMilliseconds;
        if (waitOptions.timeoutMilliseconds > 0)
        {
            const uint64_t elapsedMilliseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count());
            if (elapsedMilliseconds >= static_cast<uint64_t>(waitOptions.timeoutMilliseconds))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, "GB_ProcessInstance::WaitForMainWindow", "等待主窗口超时。");
            }
            sleepMilliseconds = static_cast<uint32_t>((std::min)(static_cast<uint64_t>(sleepMilliseconds), static_cast<uint64_t>(waitOptions.timeoutMilliseconds) - elapsedMilliseconds));
        }
        if (!SleepForWaitPoll(waitOptions, sleepMilliseconds))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, "GB_ProcessInstance::WaitForMainWindow", "等待主窗口已取消。");
        }
    }
#else
    (void)waitOptions;
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::WaitForMainWindow");
#endif
}

GB_SystemResult GB_ProcessInstance::ResumeMainThread()
{
#if defined(_WIN32)
    if (!IsValid() || !implementation->mainThreadHandle.IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::ResumeMainThread", "进程实例或主线程句柄无效。");
    }
    bool expectedSuspended = true;
    if (!implementation->mainThreadSuspended.compare_exchange_strong(expectedSuspended, false, std::memory_order_acq_rel))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::ResumeMainThread", "主线程当前不是由本模块保持的挂起状态。");
    }
    if (::ResumeThread(implementation->mainThreadHandle.GetHandleAs<HANDLE>()) == static_cast<DWORD>(-1))
    {
        implementation->mainThreadSuspended.store(true, std::memory_order_release);
        return GB_SystemResult::FromLastWin32Error("GB_ProcessInstance::ResumeMainThread", "恢复进程主线程失败。");
    }
    return GB_SystemResult::Succeeded("GB_ProcessInstance::ResumeMainThread");
#else
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::ResumeMainThread");
#endif
}

GB_SystemResult GB_ProcessInstance::WriteStandardInput(const std::string& bytes)
{
#if defined(_WIN32)
    if (!IsValid() || !implementation->standardInputWriteHandle.IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::WriteStandardInput", "当前实例没有可写的标准输入管道。");
    }
    std::lock_guard<std::mutex> lock(implementation->inputMutex);
    size_t writtenTotal = 0;
    while (writtenTotal < bytes.size())
    {
        const DWORD chunkSize = static_cast<DWORD>((std::min)(bytes.size() - writtenTotal, static_cast<size_t>(65536)));
        GB_WinHandleScope eventHandle = GB_WinHandleScope::FromKernelHandle(::CreateEventW(nullptr, TRUE, FALSE, nullptr), "StandardInputWriteEvent");
        if (!eventHandle.IsValid())
        {
            return GB_SystemResult::FromLastWin32Error("GB_ProcessInstance::WriteStandardInput", "创建标准输入写事件失败。");
        }
        OVERLAPPED overlapped = {};
        overlapped.hEvent = eventHandle.GetHandleAs<HANDLE>();
        DWORD bytesWritten = 0;
        const HANDLE inputHandle = implementation->standardInputWriteHandle.GetHandleAs<HANDLE>();
        if (::WriteFile(inputHandle, bytes.data() + writtenTotal, chunkSize, &bytesWritten, &overlapped) == FALSE)
        {
            const DWORD writeError = ::GetLastError();
            if (writeError != ERROR_IO_PENDING)
            {
                return GB_SystemResult::FromWin32Error(writeError, "GB_ProcessInstance::WriteStandardInput", "写入子进程标准输入失败。");
            }
            HANDLE waitHandles[2] = { eventHandle.GetHandleAs<HANDLE>(), implementation->processHandle.GetHandleAs<HANDLE>() };
            const DWORD waitResult = ::WaitForMultipleObjects(2, waitHandles, FALSE, 30000);
            if (waitResult == WAIT_TIMEOUT)
            {
                (void)::CancelIoEx(inputHandle, &overlapped);
                DWORD ignoredByteCount = 0;
                (void)::GetOverlappedResult(inputHandle, &overlapped, &ignoredByteCount, TRUE);
                return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, "GB_ProcessInstance::WriteStandardInput", "写入子进程标准输入超过 30 秒，操作已取消。");
            }
            if (waitResult == WAIT_OBJECT_0 + 1)
            {
                (void)::CancelIoEx(inputHandle, &overlapped);
                DWORD ignoredByteCount = 0;
                (void)::GetOverlappedResult(inputHandle, &overlapped, &ignoredByteCount, TRUE);
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, "GB_ProcessInstance::WriteStandardInput", "子进程在接收全部标准输入前已经退出。");
            }
            if (waitResult != WAIT_OBJECT_0)
            {
                const DWORD waitError = ::GetLastError();
                (void)::CancelIoEx(inputHandle, &overlapped);
                DWORD ignoredByteCount = 0;
                (void)::GetOverlappedResult(inputHandle, &overlapped, &ignoredByteCount, TRUE);
                return GB_SystemResult::FromWin32Error(waitError, "GB_ProcessInstance::WriteStandardInput", "等待标准输入写入完成失败。");
            }
            if (::GetOverlappedResult(inputHandle, &overlapped, &bytesWritten, FALSE) == FALSE)
            {
                return GB_SystemResult::FromLastWin32Error("GB_ProcessInstance::WriteStandardInput", "等待标准输入写入完成失败。");
            }
        }
        if (bytesWritten == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, "GB_ProcessInstance::WriteStandardInput", "标准输入写操作未写入任何字节。");
        }
        writtenTotal += bytesWritten;
    }
    return GB_SystemResult::Succeeded("GB_ProcessInstance::WriteStandardInput");
#else
    (void)bytes;
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::WriteStandardInput");
#endif
}

GB_SystemResult GB_ProcessInstance::CloseStandardInput()
{
#if defined(_WIN32)
    return implementation == nullptr ? GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::CloseStandardInput", "进程实例为空。") : implementation->CloseStandardInputInternal();
#else
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::CloseStandardInput");
#endif
}

GB_SystemResult GB_ProcessInstance::GetStandardOutput(std::string& output, bool& truncated) const
{
#if defined(_WIN32)
    if (implementation == nullptr)
    {
        output.clear();
        truncated = false;
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::GetStandardOutput", "进程实例为空。");
    }
    return implementation->GetCapturedOutput(false, output, truncated).WithOperationName("GB_ProcessInstance::GetStandardOutput");
#else
    output.clear();
    truncated = false;
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::GetStandardOutput");
#endif
}

GB_SystemResult GB_ProcessInstance::GetStandardError(std::string& output, bool& truncated) const
{
#if defined(_WIN32)
    if (implementation == nullptr)
    {
        output.clear();
        truncated = false;
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::GetStandardError", "进程实例为空。");
    }
    return implementation->GetCapturedOutput(true, output, truncated).WithOperationName("GB_ProcessInstance::GetStandardError");
#else
    output.clear();
    truncated = false;
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::GetStandardError");
#endif
}

uint64_t GB_ProcessInstance::GetDroppedOutputCallbackCount() const
{
#if defined(_WIN32)
    return implementation == nullptr ? 0 : implementation->droppedOutputCallbackCount.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

GB_SystemResult GB_ProcessInstance::Terminate(const uint32_t exitCode, const bool terminateJob)
{
#if defined(_WIN32)
    if (!IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::Terminate", "进程实例为空。");
    }
    if (terminateJob && implementation->jobHandle.IsValid())
    {
        if (::TerminateJobObject(implementation->jobHandle.GetHandleAs<HANDLE>(), exitCode) == FALSE)
        {
            return GB_SystemResult::FromLastWin32Error("GB_ProcessInstance::Terminate", "终止进程 Job 失败。");
        }
    }
    else if (::TerminateProcess(implementation->processHandle.GetHandleAs<HANDLE>(), exitCode) == FALSE)
    {
        const DWORD errorCode = ::GetLastError();
        const DWORD waitResult = ::WaitForSingleObject(implementation->processHandle.GetHandleAs<HANDLE>(), 0);
        if (waitResult != WAIT_OBJECT_0)
        {
            return GB_SystemResult::FromWin32Error(errorCode, "GB_ProcessInstance::Terminate", "强制终止进程失败。");
        }
    }
    if (::WaitForSingleObject(implementation->processHandle.GetHandleAs<HANDLE>(), 5000) != WAIT_OBJECT_0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, "GB_ProcessInstance::Terminate", "终止请求成功后进程未在 5 秒内进入退出状态。");
    }
    implementation->StopIoWorker(true);
    return GB_SystemResult::Succeeded("GB_ProcessInstance::Terminate");
#else
    (void)exitCode;
    (void)terminateJob;
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::Terminate");
#endif
}

GB_SystemResult GB_ProcessInstance::Close(const GB_ProcessCloseOptions& closeOptions)
{
#if defined(_WIN32)
    if (!IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_ProcessInstance::Close", "进程实例为空。");
    }
    const DWORD initialWaitResult = ::WaitForSingleObject(implementation->processHandle.GetHandleAs<HANDLE>(), 0);
    if (initialWaitResult == WAIT_OBJECT_0)
    {
        implementation->StopIoWorker(true);
        return GB_SystemResult::Succeeded("GB_ProcessInstance::Close", "目标进程已经退出。");
    }
    if (initialWaitResult == WAIT_FAILED)
    {
        return GB_SystemResult::FromLastWin32Error("GB_ProcessInstance::Close", "查询目标进程退出状态失败。");
    }
    GB_ProcessCloseOptions gracefulCloseOptions = closeOptions;
    gracefulCloseOptions.forceTerminateAfterTimeout = false;
    GB_SystemResult result = GB_SystemProcess::CloseProcess(implementation->identity, gracefulCloseOptions);
    if (result.IsSucceeded())
    {
        implementation->StopIoWorker(true);
        return result.WithOperationName("GB_ProcessInstance::Close");
    }
    if (closeOptions.forceTerminateAfterTimeout && result.errorCode != GB_SystemErrorCode::InvalidArgument)
    {
        return Terminate(closeOptions.forcedExitCode, closeOptions.terminateJobWhenAvailable).WithOperationName("GB_ProcessInstance::Close");
    }
    return result.WithOperationName("GB_ProcessInstance::Close");
#else
    (void)closeOptions;
    return MakeUnsupportedPlatformResult("GB_ProcessInstance::Close");
#endif
}

#if defined(_WIN32)
namespace
{
    class ProcessAttributeListScope final
    {
    public:
        ProcessAttributeListScope() = default;

        ~ProcessAttributeListScope()
        {
            if (attributeList != nullptr)
            {
                ::DeleteProcThreadAttributeList(attributeList);
            }
        }

        ProcessAttributeListScope(const ProcessAttributeListScope&) = delete;
        ProcessAttributeListScope& operator=(const ProcessAttributeListScope&) = delete;

        GB_SystemResult Initialize(const std::vector<HANDLE>& inheritedHandles)
        {
            SIZE_T requiredSize = 0;
            (void)::InitializeProcThreadAttributeList(nullptr, 1, 0, &requiredSize);
            if (requiredSize == 0)
            {
                return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "计算进程属性列表大小失败。");
            }
            try
            {
                storage.resize(requiredSize);
            }
            catch (const std::bad_alloc&)
            {
                return MakeAllocationFailedResult(GB_ProcessOperationStart);
            }
            attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            if (::InitializeProcThreadAttributeList(attributeList, 1, 0, &requiredSize) == FALSE)
            {
                attributeList = nullptr;
                return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "初始化进程属性列表失败。");
            }
            if (::UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, const_cast<HANDLE*>(inheritedHandles.data()), inheritedHandles.size() * sizeof(HANDLE), nullptr, nullptr) == FALSE)
            {
                return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "设置子进程句柄继承白名单失败。");
            }
            return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
        }

        LPPROC_THREAD_ATTRIBUTE_LIST Get() const
        {
            return attributeList;
        }

    private:
        std::vector<unsigned char> storage;
        LPPROC_THREAD_ATTRIBUTE_LIST attributeList = nullptr;
    };

    GB_SystemResult PopulateRunResult(GB_ProcessInstance& processInstance, GB_ProcessRunResult& runResult)
    {
        GB_SystemResult outputResult = processInstance.GetStandardOutput(runResult.standardOutput, runResult.standardOutputTruncated);
        if (outputResult.IsFailed())
        {
            if (outputResult.errorCode != GB_SystemErrorCode::EncodingConversionFailed)
            {
                return outputResult.WithOperationName(GB_ProcessOperationRun);
            }
            runResult.standardOutputEncodingValid = false;
            runResult.standardOutputEncodingError = outputResult.ToString();
        }
        outputResult = processInstance.GetStandardError(runResult.standardError, runResult.standardErrorTruncated);
        if (outputResult.IsFailed())
        {
            if (outputResult.errorCode != GB_SystemErrorCode::EncodingConversionFailed)
            {
                return outputResult.WithOperationName(GB_ProcessOperationRun);
            }
            runResult.standardErrorEncodingValid = false;
            runResult.standardErrorEncodingError = outputResult.ToString();
        }
        runResult.droppedOutputCallbackCount = processInstance.GetDroppedOutputCallbackCount();
        return GB_SystemResult::Succeeded(GB_ProcessOperationRun);
    }
}
#endif

GB_SystemResult GB_SystemProcess::Start(const GB_ProcessStartOptions& options, GB_ProcessInstance& processInstance)
{
#if defined(_WIN32)
    GB_ProcessInstance emptyInstance;
    processInstance.Swap(emptyInstance);

    GB_SystemResult result = ValidateStartOptions(options);
    if (result.IsFailed())
    {
        return result;
    }

    std::wstring executablePath;
    std::wstring workingDirectory;
    try
    {
        executablePath = ToWideStringChecked(options.executablePath, "executablePath");
        if (!options.workingDirectory.empty())
        {
            workingDirectory = ToWideStringChecked(options.workingDirectory, "workingDirectory");
        }
    }
    catch (const std::bad_alloc&)
    {
        return MakeAllocationFailedResult(GB_ProcessOperationStart);
    }
    catch (...)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_ProcessOperationStart, "路径 UTF-8 转换失败。");
    }
    result = ValidateExecutableAndWorkingDirectory(executablePath, workingDirectory);
    if (result.IsFailed())
    {
        return result;
    }

    std::wstring commandLine;
    result = BuildCommandLine(options, executablePath, commandLine);
    if (result.IsFailed())
    {
        return result;
    }
    std::vector<wchar_t> commandLineBuffer(commandLine.begin(), commandLine.end());
    commandLineBuffer.push_back(L'\0');

    std::vector<wchar_t> environmentBlock;
    bool usesCustomEnvironment = false;
    result = BuildEnvironmentBlock(options, environmentBlock, usesCustomEnvironment);
    if (result.IsFailed())
    {
        return result;
    }

    ParentChildPipe standardInputPipe;
    ParentChildPipe standardOutputPipe;
    ParentChildPipe standardErrorPipe;
    GB_WinHandleScope inheritedStandardInput;
    GB_WinHandleScope inheritedStandardOutput;
    GB_WinHandleScope inheritedStandardError;
    const bool usesStandardHandles = options.redirectStandardInput || options.redirectStandardOutput || options.redirectStandardError || options.mergeStandardErrorToOutput;

    HANDLE childStandardInput = nullptr;
    HANDLE childStandardOutput = nullptr;
    HANDLE childStandardError = nullptr;
    if (usesStandardHandles)
    {
        if (options.redirectStandardInput)
        {
            result = CreateNamedPipePair(false, "StandardInputPipe", standardInputPipe);
            if (result.IsFailed())
            {
                return result;
            }
            childStandardInput = standardInputPipe.childHandle.GetHandleAs<HANDLE>();
        }
        else
        {
            result = DuplicateStandardHandleForChild(STD_INPUT_HANDLE, GENERIC_READ, "InheritedStandardInput", inheritedStandardInput);
            if (result.IsFailed())
            {
                return result;
            }
            childStandardInput = inheritedStandardInput.GetHandleAs<HANDLE>();
        }

        if (options.redirectStandardOutput)
        {
            result = CreateNamedPipePair(true, "StandardOutputPipe", standardOutputPipe);
            if (result.IsFailed())
            {
                return result;
            }
            childStandardOutput = standardOutputPipe.childHandle.GetHandleAs<HANDLE>();
        }
        else
        {
            result = DuplicateStandardHandleForChild(STD_OUTPUT_HANDLE, GENERIC_WRITE, "InheritedStandardOutput", inheritedStandardOutput);
            if (result.IsFailed())
            {
                return result;
            }
            childStandardOutput = inheritedStandardOutput.GetHandleAs<HANDLE>();
        }

        if (options.mergeStandardErrorToOutput)
        {
            childStandardError = childStandardOutput;
        }
        else if (options.redirectStandardError)
        {
            result = CreateNamedPipePair(true, "StandardErrorPipe", standardErrorPipe);
            if (result.IsFailed())
            {
                return result;
            }
            childStandardError = standardErrorPipe.childHandle.GetHandleAs<HANDLE>();
        }
        else
        {
            result = DuplicateStandardHandleForChild(STD_ERROR_HANDLE, GENERIC_WRITE, "InheritedStandardError", inheritedStandardError);
            if (result.IsFailed())
            {
                return result;
            }
            childStandardError = inheritedStandardError.GetHandleAs<HANDLE>();
        }
    }

    std::vector<HANDLE> inheritedHandles;
    if (usesStandardHandles)
    {
        AddUniqueHandle(inheritedHandles, childStandardInput);
        AddUniqueHandle(inheritedHandles, childStandardOutput);
        AddUniqueHandle(inheritedHandles, childStandardError);
    }

    ProcessAttributeListScope attributeList;
    if (!inheritedHandles.empty())
    {
        result = attributeList.Initialize(inheritedHandles);
        if (result.IsFailed())
        {
            return result;
        }
    }

    GB_WinHandleScope jobHandle;
    result = ConfigureJobObject(options.jobOptions, jobHandle);
    if (result.IsFailed())
    {
        return result;
    }

    STARTUPINFOEXW startupInfo = {};
    startupInfo.StartupInfo.cb = inheritedHandles.empty() ? sizeof(STARTUPINFOW) : sizeof(STARTUPINFOEXW);
    if (options.showMode != GB_ProcessShowMode::Default)
    {
        startupInfo.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
        startupInfo.StartupInfo.wShowWindow = static_cast<WORD>(GetShowWindowValue(options.showMode));
    }
    if (usesStandardHandles)
    {
        startupInfo.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        startupInfo.StartupInfo.hStdInput = childStandardInput;
        startupInfo.StartupInfo.hStdOutput = childStandardOutput;
        startupInfo.StartupInfo.hStdError = childStandardError;
    }
    startupInfo.lpAttributeList = attributeList.Get();

    DWORD creationFlags = GetPriorityCreationFlag(options.priority);
    if (usesCustomEnvironment)
    {
        creationFlags |= CREATE_UNICODE_ENVIRONMENT;
    }
    if (options.createNewProcessGroup)
    {
        creationFlags |= CREATE_NEW_PROCESS_GROUP;
    }
    if (options.consoleMode == GB_ProcessConsoleMode::New)
    {
        creationFlags |= CREATE_NEW_CONSOLE;
    }
    else if (options.consoleMode == GB_ProcessConsoleMode::None)
    {
        creationFlags |= CREATE_NO_WINDOW;
    }
    const bool createSuspendedInternally = options.startSuspended || options.jobOptions.enabled;
    if (createSuspendedInternally)
    {
        creationFlags |= CREATE_SUSPENDED;
    }
    if (!inheritedHandles.empty())
    {
        creationFlags |= EXTENDED_STARTUPINFO_PRESENT;
    }

    PROCESS_INFORMATION processInformation = {};
    const BOOL processCreated = ::CreateProcessW(executablePath.c_str(), commandLineBuffer.data(), nullptr, nullptr, inheritedHandles.empty() ? FALSE : TRUE, creationFlags, usesCustomEnvironment ? environmentBlock.data() : nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startupInfo.StartupInfo, &processInformation);
    if (processCreated == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationStart, "CreateProcessW 创建进程失败。");
    }

    GB_WinHandleScope processHandle = GB_WinHandleScope::FromKernelHandle(processInformation.hProcess, "CreatedProcess");
    GB_WinHandleScope mainThreadHandle = GB_WinHandleScope::FromKernelHandle(processInformation.hThread, "CreatedProcessMainThread");
    if (jobHandle.IsValid() && ::AssignProcessToJobObject(jobHandle.GetHandleAs<HANDLE>(), processInformation.hProcess) == FALSE)
    {
        const DWORD assignError = ::GetLastError();
        (void)::TerminateProcess(processInformation.hProcess, 1);
        (void)::WaitForSingleObject(processInformation.hProcess, 5000);
        return GB_SystemResult::FromWin32Error(assignError, GB_ProcessOperationStart, "把挂起进程加入 Job Object 失败。");
    }
    if (createSuspendedInternally && !options.startSuspended)
    {
        if (::ResumeThread(processInformation.hThread) == static_cast<DWORD>(-1))
        {
            const DWORD resumeError = ::GetLastError();
            if (jobHandle.IsValid())
            {
                (void)::TerminateJobObject(jobHandle.GetHandleAs<HANDLE>(), 1);
            }
            else
            {
                (void)::TerminateProcess(processInformation.hProcess, 1);
            }
            (void)::WaitForSingleObject(processInformation.hProcess, 5000);
            return GB_SystemResult::FromWin32Error(resumeError, GB_ProcessOperationStart, "进程加入 Job 后恢复主线程失败。");
        }
    }

    std::unique_ptr<GB_ProcessInstance::Impl> implementation;
    try
    {
        implementation.reset(new GB_ProcessInstance::Impl());
    }
    catch (const std::bad_alloc&)
    {
        if (jobHandle.IsValid())
        {
            (void)::TerminateJobObject(jobHandle.GetHandleAs<HANDLE>(), 1);
        }
        else
        {
            (void)::TerminateProcess(processInformation.hProcess, 1);
        }
        (void)::WaitForSingleObject(processInformation.hProcess, 5000);
        return MakeAllocationFailedResult(GB_ProcessOperationStart);
    }

    implementation->processHandle = std::move(processHandle);
    implementation->mainThreadHandle = std::move(mainThreadHandle);
    implementation->jobHandle = std::move(jobHandle);
    implementation->standardInputWriteHandle = std::move(standardInputPipe.parentHandle);
    implementation->standardOutputReadHandle = std::move(standardOutputPipe.parentHandle);
    implementation->standardErrorReadHandle = std::move(standardErrorPipe.parentHandle);
    implementation->identity.processId = processInformation.dwProcessId;
    implementation->mainThreadId = processInformation.dwThreadId;
    implementation->mainThreadSuspended.store(options.startSuspended, std::memory_order_release);
    implementation->terminateOnJobClose = options.jobOptions.enabled && options.jobOptions.terminateOnJobClose;
    implementation->outputEncoding = options.outputEncoding;
    implementation->maximumCapturedBytesPerStream = options.maximumCapturedBytesPerStream;
    implementation->outputCallback = options.outputCallback;
    implementation->maximumPendingOutputCallbacks = options.maximumPendingOutputCallbacks;

    FILETIME creationTime = {};
    FILETIME exitTime = {};
    FILETIME kernelTime = {};
    FILETIME userTime = {};
    if (!TryGetProcessTimes(processInformation.hProcess, creationTime, exitTime, kernelTime, userTime))
    {
        const DWORD timeError = ::GetLastError();
        if (implementation->jobHandle.IsValid())
        {
            (void)::TerminateJobObject(implementation->jobHandle.GetHandleAs<HANDLE>(), 1);
        }
        else
        {
            (void)::TerminateProcess(implementation->processHandle.GetHandleAs<HANDLE>(), 1);
        }
        (void)::WaitForSingleObject(implementation->processHandle.GetHandleAs<HANDLE>(), 5000);
        return GB_SystemResult::FromWin32Error(timeError, GB_ProcessOperationStart, "读取新进程创建时间失败，无法建立强身份。");
    }
    implementation->identity.creationTime100Nanoseconds = FileTimeToUInt64(creationTime);
    implementation->identity.hasCreationTime = true;

    result = implementation->StartWorkers();
    if (result.IsFailed())
    {
        if (implementation->jobHandle.IsValid())
        {
            (void)::TerminateJobObject(implementation->jobHandle.GetHandleAs<HANDLE>(), 1);
        }
        else
        {
            (void)::TerminateProcess(implementation->processHandle.GetHandleAs<HANDLE>(), 1);
        }
        (void)::WaitForSingleObject(implementation->processHandle.GetHandleAs<HANDLE>(), 5000);
        return result;
    }

    GB_ProcessInstance createdInstance(std::move(implementation));
    processInstance.Swap(createdInstance);
    return GB_SystemResult::Succeeded(GB_ProcessOperationStart);
#else
    (void)options;
    (void)processInstance;
    return MakeUnsupportedPlatformResult(GB_ProcessOperationStart);
#endif
}

GB_SystemResult GB_SystemProcess::Run(const GB_ProcessStartOptions& options, GB_ProcessRunResult& runResult)
{
    runResult = GB_ProcessRunResult();
#if defined(_WIN32)
    if (options.startSuspended)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationRun, "Run 不接受 startSuspended；该模式只能通过 Start 返回实例后显式恢复。");
    }
    if (options.runTimeoutMilliseconds >= 0 && !options.terminateOnRunTimeout && options.jobOptions.enabled && options.jobOptions.terminateOnJobClose)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationRun, "Run 超时后不终止进程时，不能启用 terminateOnJobClose，否则局部实例析构会隐式终止 Job。");
    }
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    GB_ProcessInstance processInstance;
    GB_SystemResult result = Start(options, processInstance);
    if (result.IsFailed())
    {
        return result.WithOperationName(GB_ProcessOperationRun);
    }
    runResult.processId = processInstance.GetProcessId();
    if (options.redirectStandardInput)
    {
        (void)processInstance.CloseStandardInput();
    }

    GB_ProcessWaitOptions waitOptions;
    waitOptions.timeoutMilliseconds = options.runTimeoutMilliseconds;
    uint32_t exitCode = 0;
    result = processInstance.WaitForExit(exitCode, waitOptions);
    if (result.IsFailed() && result.errorCode == GB_SystemErrorCode::Timeout)
    {
        runResult.timedOut = true;
        if (options.terminateOnRunTimeout)
        {
            GB_SystemResult terminateResult = processInstance.Terminate(options.timeoutTerminationExitCode, true);
            if (terminateResult.IsFailed())
            {
                return terminateResult.WithOperationName(GB_ProcessOperationRun);
            }
            runResult.terminatedByModule = true;
            GB_ProcessWaitOptions finalWaitOptions;
            finalWaitOptions.timeoutMilliseconds = 5000;
            if (processInstance.WaitForExit(exitCode, finalWaitOptions).IsSucceeded())
            {
                runResult.exitCode = exitCode;
                runResult.hasExitCode = true;
            }
        }
        const GB_SystemResult outputResult = PopulateRunResult(processInstance, runResult);
        if (outputResult.IsFailed())
        {
            return outputResult;
        }
        if (!options.terminateOnRunTimeout)
        {
            runResult.standardOutputTruncated = runResult.standardOutputTruncated || options.redirectStandardOutput;
            runResult.standardErrorTruncated = runResult.standardErrorTruncated || (options.redirectStandardError && !options.mergeStandardErrorToOutput);
        }
        runResult.elapsedMilliseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count());
        return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, GB_ProcessOperationRun, "进程运行超过指定超时。");
    }
    if (result.IsFailed())
    {
        return result.WithOperationName(GB_ProcessOperationRun);
    }

    runResult.exitCode = exitCode;
    runResult.hasExitCode = true;
    result = PopulateRunResult(processInstance, runResult);
    if (result.IsFailed())
    {
        return result;
    }
    runResult.elapsedMilliseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count());
    return GB_SystemResult::Succeeded(GB_ProcessOperationRun);
#else
    (void)options;
    return MakeUnsupportedPlatformResult(GB_ProcessOperationRun);
#endif
}

GB_SystemResult GB_SystemProcess::GetAllProcesses(std::vector<GB_ProcessInfo>& processes, const GB_ProcessQueryOptions& queryOptions)
{
    processes.clear();
#if defined(_WIN32)
    try
    {
        GB_WinHandleScope snapshotHandle = GB_WinHandleScope::FromKernelHandle(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), "ProcessSnapshot");
        if (!snapshotHandle.IsValid())
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationEnumerate, "创建进程快照失败。");
        }
        PROCESSENTRY32W processEntry = {};
        processEntry.dwSize = sizeof(processEntry);
        if (::Process32FirstW(snapshotHandle.GetHandleAs<HANDLE>(), &processEntry) == FALSE)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationEnumerate, "读取进程快照首项失败。");
        }
        GB_ProcessQueryOptions perProcessOptions = queryOptions;
        perProcessOptions.queryMainWindow = false;
        processes.reserve(256);
        do
        {
            GB_ProcessInfo processInfo;
            FillProcessInfoFromEntry(processEntry, perProcessOptions, processInfo);
            processes.push_back(std::move(processInfo));
        } while (::Process32NextW(snapshotHandle.GetHandleAs<HANDLE>(), &processEntry) != FALSE);
        const DWORD enumerationError = ::GetLastError();
        if (enumerationError != ERROR_NO_MORE_FILES)
        {
            return GB_SystemResult::FromWin32Error(enumerationError, GB_ProcessOperationEnumerate, "遍历进程快照失败。");
        }

        if (queryOptions.queryMainWindow)
        {
            std::vector<GB_WindowInfo> windows;
            const GB_SystemResult windowResult = GB_SystemWindow::GetTopLevelWindows(windows);
            if (windowResult.IsSucceeded())
            {
                std::map<uint32_t, size_t> processIndexes;
                for (size_t processIndex = 0; processIndex < processes.size(); processIndex++)
                {
                    processIndexes[static_cast<uint32_t>(processes[processIndex].processId)] = processIndex;
                }
                for (size_t windowIndex = 0; windowIndex < windows.size(); windowIndex++)
                {
                    const std::map<uint32_t, size_t>::const_iterator processIterator = processIndexes.find(windows[windowIndex].windowId.processId);
                    if (processIterator == processIndexes.end() || processes[processIterator->second].hasMainWindow || !IsMainWindowCandidate(windows[windowIndex]))
                    {
                        continue;
                    }
                    GB_ProcessInfo& processInfo = processes[processIterator->second];
                    processInfo.mainWindowId = windows[windowIndex].windowId;
                    processInfo.mainWindowTitle = windows[windowIndex].title;
                    processInfo.hasMainWindow = true;
                }
            }
            else
            {
                for (size_t processIndex = 0; processIndex < processes.size(); processIndex++)
                {
                    AddFieldResultError(processes[processIndex], queryOptions, "mainWindow", windowResult, "批量枚举顶层窗口失败，主窗口字段不可用。");
                }
            }
        }
        return GB_SystemResult::Succeeded(GB_ProcessOperationEnumerate);
    }
    catch (const std::bad_alloc&)
    {
        processes.clear();
        return MakeAllocationFailedResult(GB_ProcessOperationEnumerate);
    }
    catch (...)
    {
        processes.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_ProcessOperationEnumerate, "枚举进程时转换系统文本失败。");
    }
#else
    (void)queryOptions;
    return MakeUnsupportedPlatformResult(GB_ProcessOperationEnumerate);
#endif
}

GB_SystemResult GB_SystemProcess::GetProcessInfo(const uint32_t processId, GB_ProcessInfo& processInfo, const GB_ProcessQueryOptions& queryOptions)
{
    processInfo = GB_ProcessInfo();
#if defined(_WIN32)
    if (processId == 0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationGetInfo, "进程 ID 不能为 0。");
    }
    PROCESSENTRY32W processEntry = {};
    GB_SystemResult result = FindProcessEntry(processId, processEntry);
    if (result.IsFailed())
    {
        return result;
    }
    try
    {
        FillProcessInfoFromEntry(processEntry, queryOptions, processInfo);
        return GB_SystemResult::Succeeded(GB_ProcessOperationGetInfo);
    }
    catch (const std::bad_alloc&)
    {
        return MakeAllocationFailedResult(GB_ProcessOperationGetInfo);
    }
    catch (...)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_ProcessOperationGetInfo, "查询进程时转换系统文本失败。");
    }
#else
    (void)processId;
    (void)queryOptions;
    return MakeUnsupportedPlatformResult(GB_ProcessOperationGetInfo);
#endif
}

GB_SystemResult GB_SystemProcess::GetProcessInfo(const GB_ProcessIdentity& identity, GB_ProcessInfo& processInfo, const GB_ProcessQueryOptions& queryOptions)
{
    processInfo = GB_ProcessInfo();
#if defined(_WIN32)
    if (!identity.IsStrong())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationGetInfo, "按身份查询要求包含进程创建时间。");
    }
    GB_WinHandleScope processHandle;
    GB_SystemResult result = OpenAndValidateStrongIdentity(identity, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, processHandle, GB_ProcessOperationGetInfo);
    if (result.IsFailed())
    {
        return result;
    }
    result = GetProcessInfo(identity.processId, processInfo, queryOptions);
    if (result.IsFailed())
    {
        return result;
    }
    if (!processInfo.identity.IsStrong() || processInfo.identity.creationTime100Nanoseconds != identity.creationTime100Nanoseconds)
    {
        processInfo = GB_ProcessInfo();
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_ProcessOperationGetInfo, "查询期间进程身份发生变化。");
    }
    return GB_SystemResult::Succeeded(GB_ProcessOperationGetInfo);
#else
    (void)identity;
    (void)queryOptions;
    return MakeUnsupportedPlatformResult(GB_ProcessOperationGetInfo);
#endif
}

GB_SystemResult GB_SystemProcess::GetCurrentProcessInfo(GB_ProcessInfo& processInfo, const GB_ProcessQueryOptions& queryOptions)
{
#if defined(_WIN32)
    return GetProcessInfo(::GetCurrentProcessId(), processInfo, queryOptions).WithOperationName("GB_SystemProcess::GetCurrentProcessInfo");
#else
    processInfo = GB_ProcessInfo();
    (void)queryOptions;
    return MakeUnsupportedPlatformResult("GB_SystemProcess::GetCurrentProcessInfo");
#endif
}

GB_SystemResult GB_SystemProcess::FindProcesses(const GB_ProcessFindOptions& findOptions, std::vector<GB_ProcessInfo>& processes)
{
    processes.clear();
#if defined(_WIN32)
    GB_SystemResult result = ValidateFindOptions(findOptions);
    if (result.IsFailed())
    {
        return result;
    }
    GB_ProcessQueryOptions queryOptions = findOptions.queryOptions;
    if (!findOptions.executablePath.empty())
    {
        queryOptions.queryExecutablePath = true;
    }
    DWORD currentSessionId = 0;
    if (findOptions.onlyCurrentSession && ::ProcessIdToSessionId(::GetCurrentProcessId(), &currentSessionId) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationFind, "读取当前会话 ID 失败。");
    }
    std::vector<GB_ProcessInfo> allProcesses;
    result = GetAllProcesses(allProcesses, queryOptions);
    if (result.IsFailed())
    {
        return result.WithOperationName(GB_ProcessOperationFind);
    }
    try
    {
        for (size_t processIndex = 0; processIndex < allProcesses.size(); processIndex++)
        {
            const GB_ProcessInfo& processInfo = allProcesses[processIndex];
            if (findOptions.processId != 0 && static_cast<uint32_t>(processInfo.processId) != findOptions.processId)
            {
                continue;
            }
            if (findOptions.parentProcessId != 0 && static_cast<uint32_t>(processInfo.parentProcessId) != findOptions.parentProcessId)
            {
                continue;
            }
            if (findOptions.hasSessionId && (!processInfo.hasSessionId || processInfo.sessionId != findOptions.sessionId))
            {
                continue;
            }
            if (findOptions.onlyCurrentSession && (!processInfo.hasSessionId || processInfo.sessionId != currentSessionId))
            {
                continue;
            }
            if (!findOptions.includeSystemProcesses && processInfo.isSystemProcess)
            {
                continue;
            }
            if (!TextMatches(processInfo.processNameUtf8, findOptions.processName, findOptions.exactNameMatch, findOptions.caseSensitive))
            {
                continue;
            }
            if (!TextMatches(processInfo.executablePathUtf8, findOptions.executablePath, true, findOptions.caseSensitive))
            {
                continue;
            }
            if (!TextMatches(processInfo.commandLineUtf8, findOptions.commandLineContains, false, findOptions.caseSensitive))
            {
                continue;
            }
            processes.push_back(processInfo);
        }
        return GB_SystemResult::Succeeded(GB_ProcessOperationFind);
    }
    catch (const std::bad_alloc&)
    {
        processes.clear();
        return MakeAllocationFailedResult(GB_ProcessOperationFind);
    }
#else
    (void)findOptions;
    return MakeUnsupportedPlatformResult(GB_ProcessOperationFind);
#endif
}

GB_SystemResult GB_SystemProcess::GetForegroundProcess(GB_ProcessInfo& processInfo, bool& found, const GB_ProcessQueryOptions& queryOptions)
{
    processInfo = GB_ProcessInfo();
    found = false;
#if defined(_WIN32)
    GB_WindowInfo windowInfo;
    bool windowFound = false;
    GB_SystemResult result = GB_SystemWindow::GetForegroundWindow(windowInfo, windowFound);
    if (result.IsFailed())
    {
        return result.WithOperationName("GB_SystemProcess::GetForegroundProcess");
    }
    if (!windowFound)
    {
        return GB_SystemResult::Succeeded("GB_SystemProcess::GetForegroundProcess", "当前没有前台窗口。");
    }
    result = GetProcessInfo(windowInfo.windowId.processId, processInfo, queryOptions);
    if (result.IsSucceeded())
    {
        found = true;
    }
    return result.WithOperationName("GB_SystemProcess::GetForegroundProcess");
#else
    (void)queryOptions;
    return MakeUnsupportedPlatformResult("GB_SystemProcess::GetForegroundProcess");
#endif
}

GB_SystemResult GB_SystemProcess::WaitForProcess(const GB_ProcessFindOptions& findOptions, GB_ProcessInfo& processInfo, const GB_ProcessWaitOptions& waitOptions)
{
    processInfo = GB_ProcessInfo();
    GB_SystemResult result = ValidateWaitOptions(waitOptions, GB_ProcessOperationWait);
    if (result.IsFailed())
    {
        return result;
    }
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    while (true)
    {
        if (IsCancellationRequested(waitOptions))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, GB_ProcessOperationWait, "等待进程出现已取消。");
        }
        std::vector<GB_ProcessInfo> processes;
        result = FindProcesses(findOptions, processes);
        if (result.IsFailed())
        {
            return result.WithOperationName(GB_ProcessOperationWait);
        }
        if (!processes.empty())
        {
            processInfo = std::move(processes.front());
            return GB_SystemResult::Succeeded(GB_ProcessOperationWait);
        }
        if (waitOptions.timeoutMilliseconds == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, GB_ProcessOperationWait, "目标进程尚未出现。");
        }
        uint64_t sleepMilliseconds = waitOptions.pollIntervalMilliseconds;
        if (waitOptions.timeoutMilliseconds > 0)
        {
            const uint64_t elapsedMilliseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count());
            if (elapsedMilliseconds >= static_cast<uint64_t>(waitOptions.timeoutMilliseconds))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, GB_ProcessOperationWait, "等待进程出现超时。");
            }
            sleepMilliseconds = (std::min)(sleepMilliseconds, static_cast<uint64_t>(waitOptions.timeoutMilliseconds) - elapsedMilliseconds);
        }
        if (!SleepForWaitPoll(waitOptions, sleepMilliseconds))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, GB_ProcessOperationWait, "等待进程出现已取消。");
        }
    }
}

GB_SystemResult GB_SystemProcess::WaitForProcessesExit(const GB_ProcessFindOptions& findOptions, const GB_ProcessWaitOptions& waitOptions)
{
    GB_SystemResult result = ValidateWaitOptions(waitOptions, "GB_SystemProcess::WaitForProcessesExit");
    if (result.IsFailed())
    {
        return result;
    }
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    while (true)
    {
        if (IsCancellationRequested(waitOptions))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, "GB_SystemProcess::WaitForProcessesExit", "等待进程消失已取消。");
        }
        std::vector<GB_ProcessInfo> processes;
        result = FindProcesses(findOptions, processes);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemProcess::WaitForProcessesExit");
        }
        if (processes.empty())
        {
            return GB_SystemResult::Succeeded("GB_SystemProcess::WaitForProcessesExit");
        }
        if (waitOptions.timeoutMilliseconds == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, "GB_SystemProcess::WaitForProcessesExit", "仍有匹配进程正在运行。");
        }
        uint64_t sleepMilliseconds = waitOptions.pollIntervalMilliseconds;
        if (waitOptions.timeoutMilliseconds > 0)
        {
            const uint64_t elapsedMilliseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count());
            if (elapsedMilliseconds >= static_cast<uint64_t>(waitOptions.timeoutMilliseconds))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, "GB_SystemProcess::WaitForProcessesExit", "等待匹配进程消失超时。");
            }
            sleepMilliseconds = (std::min)(sleepMilliseconds, static_cast<uint64_t>(waitOptions.timeoutMilliseconds) - elapsedMilliseconds);
        }
        if (!SleepForWaitPoll(waitOptions, sleepMilliseconds))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, "GB_SystemProcess::WaitForProcessesExit", "等待进程消失已取消。");
        }
    }
}

GB_SystemResult GB_SystemProcess::WaitForProcessExit(const GB_ProcessIdentity& identity, uint32_t& exitCode, const GB_ProcessWaitOptions& waitOptions)
{
    exitCode = 0;
#if defined(_WIN32)
    GB_WinHandleScope processHandle;
    GB_SystemResult result = OpenAndValidateStrongIdentity(identity, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, processHandle, GB_ProcessOperationWaitExit);
    if (result.IsFailed())
    {
        return result;
    }
    result = WaitForHandleWithCancellation(processHandle.GetHandleAs<HANDLE>(), waitOptions, GB_ProcessOperationWaitExit);
    if (result.IsFailed())
    {
        return result;
    }
    DWORD nativeExitCode = 0;
    if (::GetExitCodeProcess(processHandle.GetHandleAs<HANDLE>(), &nativeExitCode) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationWaitExit, "进程退出后读取退出码失败。");
    }
    exitCode = nativeExitCode;
    return GB_SystemResult::Succeeded(GB_ProcessOperationWaitExit);
#else
    (void)identity;
    (void)waitOptions;
    return MakeUnsupportedPlatformResult(GB_ProcessOperationWaitExit);
#endif
}

GB_SystemResult GB_SystemProcess::CloseProcess(const GB_ProcessIdentity& identity, const GB_ProcessCloseOptions& closeOptions)
{
#if defined(_WIN32)
    if (closeOptions.closeMessageTimeoutMilliseconds == 0 || closeOptions.waitForExitMilliseconds < -1 || closeOptions.waitForExitMilliseconds >= static_cast<int64_t>((std::numeric_limits<uint32_t>::max)()))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ProcessOperationClose, "关闭消息超时必须大于 0，退出等待必须为 -1 或小于 Win32 INFINITE 的非负值。");
    }
    GB_WinHandleScope processHandle;
    GB_SystemResult result = OpenAndValidateStrongIdentity(identity, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, processHandle, GB_ProcessOperationClose);
    if (result.IsFailed())
    {
        return result;
    }
    const DWORD initialWaitResult = ::WaitForSingleObject(processHandle.GetHandleAs<HANDLE>(), 0);
    if (initialWaitResult == WAIT_OBJECT_0)
    {
        return GB_SystemResult::Succeeded(GB_ProcessOperationClose, "目标进程已经退出。");
    }
    if (initialWaitResult == WAIT_FAILED)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationClose, "查询目标进程退出状态失败。");
    }

    GB_WindowFindOptions windowFindOptions;
    windowFindOptions.processId = identity.processId;
    windowFindOptions.visibleOnly = false;
    windowFindOptions.includeToolWindows = true;
    windowFindOptions.includeCloakedWindows = true;
    windowFindOptions.includeUntitledWindows = true;
    windowFindOptions.applicationWindowsOnly = false;
    std::vector<GB_WindowInfo> windows;
    result = GB_SystemWindow::FindWindows(windowFindOptions, windows);
    if (result.IsFailed())
    {
        return result.WithOperationName(GB_ProcessOperationClose);
    }
    bool closeRequestSent = false;
    GB_SystemResult firstCloseError = GB_SystemResult::Succeeded(GB_ProcessOperationClose);
    for (size_t windowIndex = 0; windowIndex < windows.size(); windowIndex++)
    {
        const GB_SystemResult closeResult = GB_SystemWindow::RequestCloseWindow(windows[windowIndex].windowId, closeOptions.closeMessageTimeoutMilliseconds);
        if (closeResult.IsSucceeded())
        {
            closeRequestSent = true;
        }
        else if (firstCloseError.IsSucceeded())
        {
            firstCloseError = closeResult;
        }
    }
    if (!closeRequestSent)
    {
        const DWORD closeWaitResult = ::WaitForSingleObject(processHandle.GetHandleAs<HANDLE>(), 0);
        if (closeWaitResult == WAIT_OBJECT_0)
        {
            return GB_SystemResult::Succeeded(GB_ProcessOperationClose);
        }
        if (closeWaitResult == WAIT_FAILED)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ProcessOperationClose, "发送关闭请求后查询进程退出状态失败。");
        }
        if (closeOptions.forceTerminateAfterTimeout)
        {
            return TerminateProcess(identity, closeOptions.forcedExitCode).WithOperationName(GB_ProcessOperationClose);
        }
        return firstCloseError.IsFailed() ? firstCloseError.WithOperationName(GB_ProcessOperationClose) : GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, GB_ProcessOperationClose, "目标进程没有可温和关闭的顶层窗口。");
    }

    GB_ProcessWaitOptions waitOptions;
    waitOptions.timeoutMilliseconds = closeOptions.waitForExitMilliseconds;
    result = WaitForHandleWithCancellation(processHandle.GetHandleAs<HANDLE>(), waitOptions, GB_ProcessOperationClose);
    if (result.IsSucceeded())
    {
        return GB_SystemResult::Succeeded(GB_ProcessOperationClose);
    }
    if (result.errorCode == GB_SystemErrorCode::Timeout && closeOptions.forceTerminateAfterTimeout)
    {
        return TerminateProcess(identity, closeOptions.forcedExitCode).WithOperationName(GB_ProcessOperationClose);
    }
    return result.WithOperationName(GB_ProcessOperationClose);
#else
    (void)identity;
    (void)closeOptions;
    return MakeUnsupportedPlatformResult(GB_ProcessOperationClose);
#endif
}

GB_SystemResult GB_SystemProcess::TerminateProcess(const GB_ProcessIdentity& identity, const uint32_t exitCode)
{
#if defined(_WIN32)
    const GB_ProcessQueryOptions queryOptions = MakeTerminationQueryOptions();
    GB_ProcessInfo processInfo;
    GB_SystemResult result = GetProcessInfo(identity, processInfo, queryOptions);
    if (result.IsFailed())
    {
        return result.WithOperationName(GB_ProcessOperationTerminate);
    }
    result = ValidateTerminationSafety(processInfo, GB_ProcessOperationTerminate);
    if (result.IsFailed())
    {
        return result;
    }
    GB_WinHandleScope processHandle;
    result = OpenAndValidateStrongIdentity(identity, PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, processHandle, GB_ProcessOperationTerminate);
    if (result.IsFailed())
    {
        return result;
    }
    if (::WaitForSingleObject(processHandle.GetHandleAs<HANDLE>(), 0) == WAIT_OBJECT_0)
    {
        return GB_SystemResult::Succeeded(GB_ProcessOperationTerminate, "目标进程已经退出。");
    }
    if (::TerminateProcess(processHandle.GetHandleAs<HANDLE>(), exitCode) == FALSE)
    {
        const DWORD terminateError = ::GetLastError();
        if (::WaitForSingleObject(processHandle.GetHandleAs<HANDLE>(), 0) != WAIT_OBJECT_0)
        {
            return GB_SystemResult::FromWin32Error(terminateError, GB_ProcessOperationTerminate, "强制终止外部进程失败。");
        }
    }
    if (::WaitForSingleObject(processHandle.GetHandleAs<HANDLE>(), 5000) != WAIT_OBJECT_0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, GB_ProcessOperationTerminate, "TerminateProcess 成功后目标进程未在 5 秒内进入退出状态。");
    }
    return GB_SystemResult::Succeeded(GB_ProcessOperationTerminate);
#else
    (void)identity;
    (void)exitCode;
    return MakeUnsupportedPlatformResult(GB_ProcessOperationTerminate);
#endif
}

GB_SystemResult GB_SystemProcess::TerminateProcessTreeSnapshot(const GB_ProcessIdentity& rootIdentity, const uint32_t exitCode)
{
#if defined(_WIN32)
    if (!rootIdentity.IsStrong())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemProcess::TerminateProcessTreeSnapshot", "进程树终止要求根进程强身份。");
    }
    const GB_ProcessQueryOptions queryOptions = MakeTerminationQueryOptions();
    std::vector<GB_ProcessInfo> allProcesses;
    GB_SystemResult result = GetAllProcesses(allProcesses, queryOptions);
    if (result.IsFailed())
    {
        return result.WithOperationName("GB_SystemProcess::TerminateProcessTreeSnapshot");
    }

    std::multimap<uint32_t, GB_ProcessIdentity> childrenByParent;
    for (size_t processIndex = 0; processIndex < allProcesses.size(); processIndex++)
    {
        const GB_ProcessInfo& processInfo = allProcesses[processIndex];
        if (processInfo.identity.IsStrong())
        {
            childrenByParent.emplace(static_cast<uint32_t>(processInfo.parentProcessId), processInfo.identity);
        }
    }

    std::vector<GB_ProcessIdentity> orderedTargets(1, rootIdentity);
    std::vector<uint32_t> pendingParents(1, rootIdentity.processId);
    std::set<uint32_t> discoveredProcessIds;
    discoveredProcessIds.insert(rootIdentity.processId);
    while (!pendingParents.empty())
    {
        const uint32_t parentId = pendingParents.back();
        pendingParents.pop_back();
        const std::pair<std::multimap<uint32_t, GB_ProcessIdentity>::const_iterator, std::multimap<uint32_t, GB_ProcessIdentity>::const_iterator> childRange = childrenByParent.equal_range(parentId);
        for (std::multimap<uint32_t, GB_ProcessIdentity>::const_iterator iterator = childRange.first; iterator != childRange.second; iterator++)
        {
            const GB_ProcessIdentity& childIdentity = iterator->second;
            if (discoveredProcessIds.insert(childIdentity.processId).second)
            {
                pendingParents.push_back(childIdentity.processId);
                orderedTargets.push_back(childIdentity);
            }
        }
    }

    std::vector<GB_ProcessIdentity> validatedTargets;
    validatedTargets.reserve(orderedTargets.size());
    for (size_t targetIndex = 0; targetIndex < orderedTargets.size(); targetIndex++)
    {
        GB_ProcessInfo processInfo;
        result = GetProcessInfo(orderedTargets[targetIndex], processInfo, queryOptions);
        if (result.IsFailed())
        {
            if (result.errorCode == GB_SystemErrorCode::NotFound)
            {
                continue;
            }
            return result.WithOperationName("GB_SystemProcess::TerminateProcessTreeSnapshot");
        }
        result = ValidateTerminationSafety(processInfo, "GB_SystemProcess::TerminateProcessTreeSnapshot");
        if (result.IsFailed())
        {
            return result;
        }
        validatedTargets.push_back(orderedTargets[targetIndex]);
    }
    for (std::vector<GB_ProcessIdentity>::reverse_iterator iterator = validatedTargets.rbegin(); iterator != validatedTargets.rend(); iterator++)
    {
        result = TerminateProcess(*iterator, exitCode);
        if (result.IsFailed() && result.errorCode != GB_SystemErrorCode::NotFound)
        {
            return result.WithOperationName("GB_SystemProcess::TerminateProcessTreeSnapshot");
        }
    }
    return GB_SystemResult::Succeeded("GB_SystemProcess::TerminateProcessTreeSnapshot");
#else
    (void)rootIdentity;
    (void)exitCode;
    return MakeUnsupportedPlatformResult("GB_SystemProcess::TerminateProcessTreeSnapshot");
#endif
}

GB_SystemResult GB_SystemProcess::SetProcessPriority(const GB_ProcessIdentity& identity, const GB_ProcessPriority priority)
{
#if defined(_WIN32)
    if (!IsValidPriorityValue(static_cast<uint64_t>(priority)))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemProcess::SetProcessPriority", "进程优先级枚举值非法。");
    }
    GB_WinHandleScope processHandle;
    GB_SystemResult result = OpenAndValidateStrongIdentity(identity, PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, processHandle, "GB_SystemProcess::SetProcessPriority");
    if (result.IsFailed())
    {
        return result;
    }
    if (::SetPriorityClass(processHandle.GetHandleAs<HANDLE>(), GetPriorityCreationFlag(priority)) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error("GB_SystemProcess::SetProcessPriority", "设置进程优先级失败。");
    }
    return GB_SystemResult::Succeeded("GB_SystemProcess::SetProcessPriority");
#else
    (void)identity;
    (void)priority;
    return MakeUnsupportedPlatformResult("GB_SystemProcess::SetProcessPriority");
#endif
}

GB_SystemResult GB_SystemProcess::GetCurrentExecutablePath(std::string& executablePath)
{
    executablePath.clear();
#if defined(_WIN32)
    try
    {
        std::vector<wchar_t> pathBuffer(260);
        while (pathBuffer.size() <= 32768)
        {
            const DWORD pathLength = ::GetModuleFileNameW(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
            if (pathLength == 0)
            {
                return GB_SystemResult::FromLastWin32Error("GB_SystemProcess::GetCurrentExecutablePath", "读取当前进程路径失败。");
            }
            if (pathLength < pathBuffer.size())
            {
                executablePath = GB_WStringToUtf8(std::wstring(pathBuffer.data(), pathLength));
                return GB_SystemResult::Succeeded("GB_SystemProcess::GetCurrentExecutablePath");
            }
            pathBuffer.resize(pathBuffer.size() * 2);
        }
        return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, "GB_SystemProcess::GetCurrentExecutablePath", "当前进程路径超过模块允许的上限。");
    }
    catch (const std::bad_alloc&)
    {
        return MakeAllocationFailedResult("GB_SystemProcess::GetCurrentExecutablePath");
    }
    catch (...)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, "GB_SystemProcess::GetCurrentExecutablePath", "当前进程路径转换为 UTF-8 失败。");
    }
#else
    return MakeUnsupportedPlatformResult("GB_SystemProcess::GetCurrentExecutablePath");
#endif
}

GB_SystemResult GB_SystemProcess::GetCurrentWorkingDirectory(std::string& workingDirectory)
{
    workingDirectory.clear();
#if defined(_WIN32)
    try
    {
        if (!QueryCurrentDirectoryUtf8(workingDirectory))
        {
            return GB_SystemResult::FromLastWin32Error("GB_SystemProcess::GetCurrentWorkingDirectory", "读取当前工作目录失败。");
        }
        return GB_SystemResult::Succeeded("GB_SystemProcess::GetCurrentWorkingDirectory");
    }
    catch (const std::bad_alloc&)
    {
        return MakeAllocationFailedResult("GB_SystemProcess::GetCurrentWorkingDirectory");
    }
    catch (...)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, "GB_SystemProcess::GetCurrentWorkingDirectory", "当前工作目录转换为 UTF-8 失败。");
    }
#else
    return MakeUnsupportedPlatformResult("GB_SystemProcess::GetCurrentWorkingDirectory");
#endif
}

bool GB_SystemProcess::IsValidArchitectureValue(const uint64_t value)
{
    return value <= static_cast<uint64_t>(GB_ProcessArchitecture::Arm64);
}

bool GB_SystemProcess::IsValidStateValue(const uint64_t value)
{
    return value <= static_cast<uint64_t>(GB_ProcessState::Exited);
}

bool GB_SystemProcess::IsValidShowModeValue(const uint64_t value)
{
    return value <= static_cast<uint64_t>(GB_ProcessShowMode::Maximized);
}

bool GB_SystemProcess::IsValidConsoleModeValue(const uint64_t value)
{
    return value <= static_cast<uint64_t>(GB_ProcessConsoleMode::None);
}

bool GB_SystemProcess::IsValidPriorityValue(const uint64_t value)
{
    return value <= static_cast<uint64_t>(GB_ProcessPriority::High);
}

bool GB_SystemProcess::IsValidOutputEncodingValue(const uint64_t value)
{
    return value <= static_cast<uint64_t>(GB_ProcessOutputEncoding::Utf16LittleEndian);
}
