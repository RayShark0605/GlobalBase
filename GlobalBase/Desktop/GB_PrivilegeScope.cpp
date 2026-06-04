#include "GB_PrivilegeScope.h"
#include "GB_WinHandleScope.h"

#include <limits>
#include <new>
#include <sstream>
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

#  ifdef _MSC_VER
#    pragma comment(lib, "Advapi32.lib")
#  endif
#endif

namespace
{
    const uint32_t GB_SePrivilegeEnabledByDefault = 0x00000001u;
    const uint32_t GB_SePrivilegeEnabled = 0x00000002u;
    const uint32_t GB_SePrivilegeRemoved = 0x00000004u;
    const uint32_t GB_SePrivilegeUsedForAccess = 0x80000000u;

    static std::string ResolveOperationName(const std::string& operationName, const std::string& defaultOperationName)
    {
        return operationName.empty() ? defaultOperationName : operationName;
    }

#if defined(_WIN32)
    static GB_PrivilegeTokenTarget NormalizeTokenTarget(const GB_PrivilegeTokenTarget tokenTarget)
    {
        const uint64_t tokenTargetValue = static_cast<uint64_t>(static_cast<uint16_t>(tokenTarget));
        if (!GB_PrivilegeScope::IsValidTokenTargetValue(tokenTargetValue))
        {
            return GB_PrivilegeTokenTarget::CurrentProcess;
        }

        return tokenTarget;
    }
#endif

    static GB_PrivilegeAction NormalizeAction(const GB_PrivilegeAction action)
    {
        const uint64_t actionValue = static_cast<uint64_t>(static_cast<uint16_t>(action));
        if (!GB_PrivilegeScope::IsValidActionValue(actionValue))
        {
            return GB_PrivilegeAction::Enable;
        }

        return action;
    }

    static bool ContainsNullCharacter(const std::string& text)
    {
        for (size_t i = 0; i < text.size(); i++)
        {
            if (text[i] == '\0')
            {
                return true;
            }
        }

        return false;
    }

    static GB_SystemResult MakeUnsupportedPlatformResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, u8"GB_PrivilegeScope 仅在 Windows 平台提供实际权限调整能力。");
    }

    static GB_SystemResult MakeAlreadyAdjustedResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::AlreadyInitialized, ResolveOperationName(operationName, u8"GB_PrivilegeScope::Adjust"), u8"当前 GB_PrivilegeScope 对象已经持有一次成功的权限调整；请先调用 Restore()、Detach() 或 Reset()。");
    }

    static GB_SystemResult MakeNoNeedRestoreResult()
    {
        return GB_SystemResult::Succeeded(u8"GB_PrivilegeScope::Restore", u8"当前作用域未持有权限调整责任，未执行权限恢复。");
    }

    static GB_SystemResult MakeDetachSucceededResult()
    {
        return GB_SystemResult::Succeeded(u8"GB_PrivilegeScope::Detach", u8"已释放权限恢复责任，未恢复权限旧状态。");
    }

    static GB_SystemResult MakeNoStateChangedRestoreResult()
    {
        return GB_SystemResult::Succeeded(u8"GB_PrivilegeScope::Restore", u8"权限调整未改变令牌状态，无需恢复权限旧状态。");
    }

    static GB_SystemResult MakeRestoreSkippedResult()
    {
        return GB_SystemResult::Succeeded(u8"GB_PrivilegeScope::Restore", u8"restoreOnDestruct=false，按配置保留权限调整后的状态。");
    }

    static GB_SystemResult MakeInvalidOptionsResult(const GB_PrivilegeScopeOptions& options, const std::string& operationName)
    {
        std::ostringstream stream;
        stream << u8"权限作用域选项非法。";
        stream << u8" privilegeName=" << options.privilegeName;
        stream << u8", actionValue=" << static_cast<uint64_t>(static_cast<uint16_t>(options.action));
        stream << u8", tokenTargetValue=" << static_cast<uint64_t>(static_cast<uint16_t>(options.tokenTarget));
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, ResolveOperationName(operationName, u8"GB_PrivilegeScope::Adjust"), stream.str());
    }

    static uint64_t MakeTokenHandleValue(void* tokenHandle)
    {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tokenHandle));
    }

#if defined(_WIN32)
    static GB_SystemResult MakePrivilegeNotPresentResult(const std::string& privilegeName, const std::string& operationName)
    {
        std::ostringstream stream;
        stream << u8"当前访问令牌不包含指定权限，AdjustTokenPrivileges 不能向令牌新增权限。";
        stream << u8" privilegeName=" << privilegeName;
        return GB_SystemResult::FromWin32Error(1300u, ResolveOperationName(operationName, u8"AdjustTokenPrivileges"), stream.str());
    }

    static GB_SystemResult MakePrivilegeNameNotFoundResult(const std::string& privilegeName, const uint32_t win32ErrorCode, const std::string& operationName)
    {
        GB_SystemResult result = GB_SystemResult::FromWin32Error(win32ErrorCode, ResolveOperationName(operationName, u8"LookupPrivilegeValue"), u8"指定的 Windows 权限名称不存在或当前系统不支持该权限。 privilegeName=" + privilegeName);
        result.errorCode = GB_SystemErrorCode::NotFound;
        result.hresult = GB_SystemResult::ErrorCodeToHResult(result.errorCode);
        return result;
    }

    static GB_PrivilegeInfo MakePrivilegeInfo(const std::string& privilegeName, const uint32_t luidLowPart, const int32_t luidHighPart, const uint32_t attributes, const bool exists)
    {
        GB_PrivilegeInfo privilegeInfo;
        privilegeInfo.privilegeName = privilegeName;
        privilegeInfo.luidLowPart = luidLowPart;
        privilegeInfo.luidHighPart = luidHighPart;
        privilegeInfo.attributes = attributes;
        privilegeInfo.exists = exists;
        privilegeInfo.enabled = (attributes & GB_SePrivilegeEnabled) != 0;
        privilegeInfo.enabledByDefault = (attributes & GB_SePrivilegeEnabledByDefault) != 0;
        privilegeInfo.removed = (attributes & GB_SePrivilegeRemoved) != 0;
        privilegeInfo.usedForAccess = (attributes & GB_SePrivilegeUsedForAccess) != 0;
        return privilegeInfo;
    }

    static std::string BuildAdjustDetailMessage(const GB_PrivilegeScopeOptions& options, const GB_PrivilegeTokenTarget openedTokenTarget, const bool stateChanged, const GB_PrivilegeInfo& previousPrivilegeInfo, const GB_PrivilegeInfo& adjustedPrivilegeInfo)
    {
        std::ostringstream stream;
        stream << u8"调整 Windows 访问令牌权限。";
        stream << u8" privilegeName=" << options.privilegeName;
        stream << u8", action=" << GB_PrivilegeScope::GetActionName(options.action);
        stream << u8", requestedTokenTarget=" << GB_PrivilegeScope::GetTokenTargetName(options.tokenTarget);
        stream << u8", openedTokenTarget=" << GB_PrivilegeScope::GetTokenTargetName(openedTokenTarget);
        stream << u8", stateChanged=" << (stateChanged ? "true" : "false");
        stream << u8", previousExists=" << (previousPrivilegeInfo.exists ? "true" : "false");
        stream << u8", previousEnabled=" << (previousPrivilegeInfo.enabled ? "true" : "false");
        stream << u8", adjustedExists=" << (adjustedPrivilegeInfo.exists ? "true" : "false");
        stream << u8", adjustedEnabled=" << (adjustedPrivilegeInfo.enabled ? "true" : "false");
        return stream.str();
    }

    static std::string BuildRestoreDetailMessage(const GB_PrivilegeScopeOptions& options, const GB_PrivilegeTokenTarget openedTokenTarget, const GB_PrivilegeInfo& previousPrivilegeInfo)
    {
        std::ostringstream stream;
        stream << u8"恢复 Windows 访问令牌权限旧状态。";
        stream << u8" privilegeName=" << options.privilegeName;
        stream << u8", openedTokenTarget=" << GB_PrivilegeScope::GetTokenTargetName(openedTokenTarget);
        stream << u8", previousEnabled=" << (previousPrivilegeInfo.enabled ? "true" : "false");
        return stream.str();
    }

#endif

#if defined(_WIN32)
    class Win32LastErrorValueScope
    {
    public:
        Win32LastErrorValueScope() noexcept
            : lastErrorCode(::GetLastError())
        {
        }

        ~Win32LastErrorValueScope() noexcept
        {
            ::SetLastError(lastErrorCode);
        }

        Win32LastErrorValueScope(const Win32LastErrorValueScope&) = delete;
        Win32LastErrorValueScope& operator=(const Win32LastErrorValueScope&) = delete;

    private:
        DWORD lastErrorCode = ERROR_SUCCESS;
    };

    static bool TryConvertUtf8ToWideString(const std::string& utf8Text, std::wstring& wideText)
    {
        wideText.clear();
        if (utf8Text.empty())
        {
            return true;
        }

        if (utf8Text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        const int utf8Length = static_cast<int>(utf8Text.size());
        const int requiredWideLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.data(), utf8Length, nullptr, 0);
        if (requiredWideLength <= 0)
        {
            return false;
        }

        wideText.assign(static_cast<size_t>(requiredWideLength), L'\0');
        const int convertedWideLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.data(), utf8Length, &wideText[0], requiredWideLength);
        if (convertedWideLength != requiredWideLength)
        {
            wideText.clear();
            return false;
        }

        return true;
    }

    static GB_SystemResult MakeUtf8ToWideFailedResult(const std::string& privilegeName, const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, ResolveOperationName(operationName, u8"GB_PrivilegeScope::ConvertPrivilegeName"), u8"权限名称不是合法 UTF-8 文本，无法调用 Windows 宽字符 API。 privilegeName=" + privilegeName);
    }

    static GB_SystemResult MakeWin32FailureResult(const DWORD lastError, const std::string& operationName, const std::string& message)
    {
        if (lastError != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(static_cast<uint32_t>(lastError), operationName, message);
        }

        return GB_SystemResult::Failed(GB_SystemErrorCode::NativeApiFailed, operationName, message);
    }

    static bool EqualLuidValue(const LUID& left, const LUID& right)
    {
        return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
    }

    static GB_PrivilegeInfo MakePrivilegeInfoFromLuidAndAttributes(const std::string& privilegeName, const LUID& luid, const DWORD attributes, const bool exists)
    {
        return MakePrivilegeInfo(privilegeName, static_cast<uint32_t>(luid.LowPart), static_cast<int32_t>(luid.HighPart), static_cast<uint32_t>(attributes), exists);
    }

    static GB_SystemResult LookupPrivilegeLuid(const std::string& privilegeName, LUID& privilegeLuid, const std::string& operationName)
    {
        std::wstring widePrivilegeName;
        if (!TryConvertUtf8ToWideString(privilegeName, widePrivilegeName))
        {
            return MakeUtf8ToWideFailedResult(privilegeName, operationName);
        }

        ::SetLastError(ERROR_SUCCESS);
        if (::LookupPrivilegeValueW(nullptr, widePrivilegeName.c_str(), &privilegeLuid) == FALSE)
        {
            const DWORD lastError = ::GetLastError();
            if (lastError == ERROR_NO_SUCH_PRIVILEGE)
            {
                return MakePrivilegeNameNotFoundResult(privilegeName, static_cast<uint32_t>(lastError), operationName);
            }

            return MakeWin32FailureResult(lastError, ResolveOperationName(operationName, u8"LookupPrivilegeValue"), u8"解析 Windows 权限名称对应的 LUID 失败。 privilegeName=" + privilegeName);
        }

        return GB_SystemResult::Succeeded(ResolveOperationName(operationName, u8"LookupPrivilegeValue"), u8"已解析 Windows 权限名称对应的 LUID。");
    }

    static GB_SystemResult OpenProcessTokenHandle(const DWORD desiredAccess, HANDLE& tokenHandle, const std::string& operationName)
    {
        tokenHandle = nullptr;
        ::SetLastError(ERROR_SUCCESS);
        if (::OpenProcessToken(::GetCurrentProcess(), desiredAccess, &tokenHandle) == FALSE)
        {
            const DWORD lastError = ::GetLastError();
            return MakeWin32FailureResult(lastError, ResolveOperationName(operationName, u8"OpenProcessToken"), u8"打开当前进程访问令牌失败。");
        }

        return GB_SystemResult::Succeeded(ResolveOperationName(operationName, u8"OpenProcessToken"), u8"已打开当前进程访问令牌。");
    }

    static GB_SystemResult OpenThreadTokenHandle(const DWORD desiredAccess, const bool openThreadAsSelf, HANDLE& tokenHandle, const std::string& operationName)
    {
        tokenHandle = nullptr;
        ::SetLastError(ERROR_SUCCESS);
        if (::OpenThreadToken(::GetCurrentThread(), desiredAccess, openThreadAsSelf ? TRUE : FALSE, &tokenHandle) == FALSE)
        {
            const DWORD lastError = ::GetLastError();
            return MakeWin32FailureResult(lastError, ResolveOperationName(operationName, u8"OpenThreadToken"), u8"打开当前线程访问令牌失败。");
        }

        return GB_SystemResult::Succeeded(ResolveOperationName(operationName, u8"OpenThreadToken"), u8"已打开当前线程访问令牌。");
    }

    static GB_SystemResult OpenTokenHandleForTarget(const GB_PrivilegeScopeOptions& options, const DWORD desiredAccess, HANDLE& tokenHandle, GB_PrivilegeTokenTarget& openedTokenTarget, const std::string& operationName)
    {
        tokenHandle = nullptr;
        openedTokenTarget = NormalizeTokenTarget(options.tokenTarget);

        switch (NormalizeTokenTarget(options.tokenTarget))
        {
        case GB_PrivilegeTokenTarget::CurrentProcess:
            openedTokenTarget = GB_PrivilegeTokenTarget::CurrentProcess;
            return OpenProcessTokenHandle(desiredAccess, tokenHandle, ResolveOperationName(operationName, u8"OpenProcessToken"));

        case GB_PrivilegeTokenTarget::CurrentThread:
            openedTokenTarget = GB_PrivilegeTokenTarget::CurrentThread;
            return OpenThreadTokenHandle(desiredAccess, options.openThreadAsSelf, tokenHandle, ResolveOperationName(operationName, u8"OpenThreadToken"));

        case GB_PrivilegeTokenTarget::CurrentThreadThenProcess:
        {
            GB_SystemResult threadResult = OpenThreadTokenHandle(desiredAccess, options.openThreadAsSelf, tokenHandle, ResolveOperationName(operationName, u8"OpenThreadToken"));
            if (threadResult.IsSucceeded())
            {
                openedTokenTarget = GB_PrivilegeTokenTarget::CurrentThread;
                return threadResult;
            }

            if (threadResult.errorSource == GB_NativeErrorSource::Win32 && threadResult.nativeErrorCode == ERROR_NO_TOKEN)
            {
                openedTokenTarget = GB_PrivilegeTokenTarget::CurrentProcess;
                return OpenProcessTokenHandle(desiredAccess, tokenHandle, ResolveOperationName(operationName, u8"OpenProcessToken"));
            }

            return threadResult;
        }

        default:
            break;
        }

        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, ResolveOperationName(operationName, u8"GB_PrivilegeScope::OpenToken"), u8"访问令牌目标非法。");
    }

    static GB_SystemResult QueryPrivilegeInfoFromToken(HANDLE tokenHandle, const std::string& privilegeName, const LUID& privilegeLuid, GB_PrivilegeInfo& privilegeInfo, const std::string& operationName)
    {
        privilegeInfo = MakePrivilegeInfoFromLuidAndAttributes(privilegeName, privilegeLuid, 0, false);

        DWORD requiredLength = 0;
        ::SetLastError(ERROR_SUCCESS);
        (void)::GetTokenInformation(tokenHandle, TokenPrivileges, nullptr, 0, &requiredLength);
        const DWORD firstLastError = ::GetLastError();
        if (requiredLength == 0)
        {
            return MakeWin32FailureResult(firstLastError, ResolveOperationName(operationName, u8"GetTokenInformation"), u8"查询访问令牌权限列表所需缓冲区大小失败。");
        }

        std::vector<unsigned char> buffer;
        try
        {
            buffer.resize(static_cast<size_t>(requiredLength));
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, ResolveOperationName(operationName, u8"GetTokenInformation"), u8"分配访问令牌权限信息缓冲区失败。");
        }

        ::SetLastError(ERROR_SUCCESS);
        if (::GetTokenInformation(tokenHandle, TokenPrivileges, &buffer[0], requiredLength, &requiredLength) == FALSE)
        {
            const DWORD lastError = ::GetLastError();
            return MakeWin32FailureResult(lastError, ResolveOperationName(operationName, u8"GetTokenInformation"), u8"查询访问令牌权限列表失败。");
        }

        const TOKEN_PRIVILEGES* tokenPrivileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(&buffer[0]);
        for (DWORD i = 0; i < tokenPrivileges->PrivilegeCount; i++)
        {
            const LUID_AND_ATTRIBUTES& item = tokenPrivileges->Privileges[i];
            if (EqualLuidValue(item.Luid, privilegeLuid))
            {
                privilegeInfo = MakePrivilegeInfoFromLuidAndAttributes(privilegeName, privilegeLuid, item.Attributes, true);
                return GB_SystemResult::Succeeded(ResolveOperationName(operationName, u8"GetTokenInformation"), u8"已查询访问令牌中的指定权限信息。");
            }
        }

        return GB_SystemResult::Succeeded(ResolveOperationName(operationName, u8"GetTokenInformation"), u8"访问令牌中不存在指定权限。");
    }

    static DWORD BuildRestoreAttributesFromPreviousInfo(const GB_PrivilegeInfo& previousPrivilegeInfo)
    {
        return static_cast<DWORD>(previousPrivilegeInfo.attributes & ~GB_SePrivilegeRemoved);
    }

    static GB_SystemResult AdjustPrivilegeOnToken(HANDLE tokenHandle, const LUID& privilegeLuid, const DWORD attributes, TOKEN_PRIVILEGES& previousState, DWORD& previousStateLength, const std::string& privilegeName, const std::string& operationName)
    {
        TOKEN_PRIVILEGES newState = {};
        newState.PrivilegeCount = 1;
        newState.Privileges[0].Luid = privilegeLuid;
        newState.Privileges[0].Attributes = attributes;

        previousState = {};
        previousStateLength = 0;
        ::SetLastError(ERROR_SUCCESS);
        const BOOL adjustResult = ::AdjustTokenPrivileges(tokenHandle, FALSE, &newState, static_cast<DWORD>(sizeof(previousState)), &previousState, &previousStateLength);
        const DWORD lastError = ::GetLastError();

        if (adjustResult == FALSE)
        {
            return MakeWin32FailureResult(lastError, ResolveOperationName(operationName, u8"AdjustTokenPrivileges"), u8"调整访问令牌权限失败。 privilegeName=" + privilegeName);
        }

        if (lastError == ERROR_NOT_ALL_ASSIGNED)
        {
            return MakePrivilegeNotPresentResult(privilegeName, operationName);
        }

        return GB_SystemResult::Succeeded(ResolveOperationName(operationName, u8"AdjustTokenPrivileges"), u8"已调整访问令牌权限。");
    }

    static GB_SystemResult RestorePrivilegeOnToken(HANDLE tokenHandle, const GB_PrivilegeInfo& previousPrivilegeInfo, const std::string& operationName)
    {
        LUID privilegeLuid = {};
        privilegeLuid.LowPart = static_cast<DWORD>(previousPrivilegeInfo.luidLowPart);
        privilegeLuid.HighPart = static_cast<LONG>(previousPrivilegeInfo.luidHighPart);

        TOKEN_PRIVILEGES previousState = {};
        DWORD previousStateLength = 0;
        return AdjustPrivilegeOnToken(tokenHandle, privilegeLuid, BuildRestoreAttributesFromPreviousInfo(previousPrivilegeInfo), previousState, previousStateLength, previousPrivilegeInfo.privilegeName, operationName);
    }

    static GB_SystemResult CloseTokenHandleWithResult(void*& tokenHandle, const std::string& operationName)
    {
        if (tokenHandle == nullptr)
        {
            return GB_SystemResult::Succeeded(ResolveOperationName(operationName, u8"GB_PrivilegeScope::CloseTokenHandle"), u8"访问令牌句柄为空，无需关闭。");
        }

        GB_WinHandleScope tokenHandleScope = GB_WinHandleScope::FromKernelHandle(tokenHandle, u8"AccessToken");
        tokenHandle = nullptr;

        GB_SystemResult closeResult = tokenHandleScope.Close();
        if (closeResult.IsFailed())
        {
            closeResult.WithOperationName(ResolveOperationName(operationName, u8"GB_PrivilegeScope::CloseTokenHandle"));
            return closeResult;
        }

        return GB_SystemResult::Succeeded(ResolveOperationName(operationName, u8"GB_PrivilegeScope::CloseTokenHandle"), u8"已关闭访问令牌句柄。");
    }

    static void CloseTokenHandleSilently(void*& tokenHandle) noexcept
    {
        if (tokenHandle == nullptr)
        {
            return;
        }

        GB_WinHandleScope tokenHandleScope = GB_WinHandleScope::FromKernelHandle(tokenHandle, u8"AccessToken");
        tokenHandle = nullptr;
    }

    static void RestorePrivilegeOnTokenSilently(HANDLE tokenHandle, const GB_PrivilegeInfo& previousPrivilegeInfo) noexcept
    {
        if (tokenHandle == nullptr)
        {
            return;
        }

        const Win32LastErrorValueScope lastErrorValueScope;

        LUID privilegeLuid = {};
        privilegeLuid.LowPart = static_cast<DWORD>(previousPrivilegeInfo.luidLowPart);
        privilegeLuid.HighPart = static_cast<LONG>(previousPrivilegeInfo.luidHighPart);

        TOKEN_PRIVILEGES newState = {};
        newState.PrivilegeCount = 1;
        newState.Privileges[0].Luid = privilegeLuid;
        newState.Privileges[0].Attributes = BuildRestoreAttributesFromPreviousInfo(previousPrivilegeInfo);

        (void)::AdjustTokenPrivileges(tokenHandle, FALSE, &newState, 0, nullptr, nullptr);
    }
#endif
}

GB_PrivilegeScope::GB_PrivilegeScope()
    : adjustResult(GB_SystemResult::Succeeded(u8"GB_PrivilegeScope"))
    , lastRestoreResult(GB_SystemResult::Succeeded(u8"GB_PrivilegeScope"))
{
}

GB_PrivilegeScope::GB_PrivilegeScope(const std::string& privilegeName, const GB_PrivilegeAction action, const std::string& operationName)
    : GB_PrivilegeScope()
{
    GB_PrivilegeScopeOptions adjustOptions;
    adjustOptions.privilegeName = privilegeName;
    adjustOptions.action = action;
    Adjust(adjustOptions, operationName);
}

GB_PrivilegeScope::GB_PrivilegeScope(const GB_WindowsPrivilege privilege, const GB_PrivilegeAction action, const std::string& operationName)
    : GB_PrivilegeScope(GetWindowsPrivilegeName(privilege), action, operationName)
{
}

GB_PrivilegeScope::GB_PrivilegeScope(const GB_PrivilegeScopeOptions& options, const std::string& operationName)
    : GB_PrivilegeScope()
{
    Adjust(options, operationName);
}

GB_PrivilegeScope::~GB_PrivilegeScope() noexcept
{
    CloseSilently();
}

GB_PrivilegeScope::GB_PrivilegeScope(GB_PrivilegeScope&& other)
    : GB_PrivilegeScope()
{
    MoveFrom(other);
}

GB_PrivilegeScope& GB_PrivilegeScope::operator=(GB_PrivilegeScope&& other)
{
    if (this == &other)
    {
        return *this;
    }

    const GB_SystemResult restoreResult = Restore();
    if (restoreResult.IsFailed())
    {
        return *this;
    }

    MoveFrom(other);
    return *this;
}

GB_PrivilegeScopeOptions GB_PrivilegeScope::MakeEnableOptions(const std::string& privilegeName, const GB_PrivilegeTokenTarget tokenTarget, const bool restoreOnDestruct)
{
    GB_PrivilegeScopeOptions options;
    options.privilegeName = privilegeName;
    options.action = GB_PrivilegeAction::Enable;
    options.tokenTarget = tokenTarget;
    options.openThreadAsSelf = true;
    options.restoreOnDestruct = restoreOnDestruct;
    return options;
}

GB_PrivilegeScopeOptions GB_PrivilegeScope::MakeDisableOptions(const std::string& privilegeName, const GB_PrivilegeTokenTarget tokenTarget, const bool restoreOnDestruct)
{
    GB_PrivilegeScopeOptions options;
    options.privilegeName = privilegeName;
    options.action = GB_PrivilegeAction::Disable;
    options.tokenTarget = tokenTarget;
    options.openThreadAsSelf = true;
    options.restoreOnDestruct = restoreOnDestruct;
    return options;
}

GB_PrivilegeScopeOptions GB_PrivilegeScope::MakeEnableOptions(const GB_WindowsPrivilege privilege, const GB_PrivilegeTokenTarget tokenTarget, const bool restoreOnDestruct)
{
    return MakeEnableOptions(GetWindowsPrivilegeName(privilege), tokenTarget, restoreOnDestruct);
}

GB_PrivilegeScopeOptions GB_PrivilegeScope::MakeDisableOptions(const GB_WindowsPrivilege privilege, const GB_PrivilegeTokenTarget tokenTarget, const bool restoreOnDestruct)
{
    return MakeDisableOptions(GetWindowsPrivilegeName(privilege), tokenTarget, restoreOnDestruct);
}

GB_PrivilegeScope GB_PrivilegeScope::EnableProcessPrivilege(const std::string& privilegeName, const std::string& operationName)
{
    return GB_PrivilegeScope(MakeEnableOptions(privilegeName, GB_PrivilegeTokenTarget::CurrentProcess, true), ResolveOperationName(operationName, u8"GB_PrivilegeScope::EnableProcessPrivilege"));
}

GB_PrivilegeScope GB_PrivilegeScope::EnableProcessPrivilege(const GB_WindowsPrivilege privilege, const std::string& operationName)
{
    return EnableProcessPrivilege(GetWindowsPrivilegeName(privilege), operationName);
}

GB_PrivilegeScope GB_PrivilegeScope::EnableThreadPrivilege(const std::string& privilegeName, const std::string& operationName, const bool openThreadAsSelf)
{
    GB_PrivilegeScopeOptions options = MakeEnableOptions(privilegeName, GB_PrivilegeTokenTarget::CurrentThread, true);
    options.openThreadAsSelf = openThreadAsSelf;
    return GB_PrivilegeScope(options, ResolveOperationName(operationName, u8"GB_PrivilegeScope::EnableThreadPrivilege"));
}

GB_PrivilegeScope GB_PrivilegeScope::EnableThreadPrivilege(const GB_WindowsPrivilege privilege, const std::string& operationName, const bool openThreadAsSelf)
{
    return EnableThreadPrivilege(GetWindowsPrivilegeName(privilege), operationName, openThreadAsSelf);
}

GB_SystemResult GB_PrivilegeScope::Adjust(const GB_PrivilegeScopeOptions& adjustOptions, const std::string& operationName)
{
    if (adjusted)
    {
        adjustResult = MakeAlreadyAdjustedResult(operationName);
        return adjustResult;
    }

    if (!IsValidOptions(adjustOptions))
    {
        adjustResult = MakeInvalidOptionsResult(adjustOptions, operationName);
        return adjustResult;
    }

#if defined(_WIN32)
    LUID privilegeLuid = {};
    adjustResult = LookupPrivilegeLuid(adjustOptions.privilegeName, privilegeLuid, ResolveOperationName(operationName, u8"LookupPrivilegeValue"));
    if (adjustResult.IsFailed())
    {
        ClearPrivilegeState();
        return adjustResult;
    }

    HANDLE rawTokenHandle = nullptr;
    GB_PrivilegeTokenTarget actualOpenedTokenTarget = GB_PrivilegeTokenTarget::CurrentProcess;
    const DWORD desiredAccess = TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY;
    adjustResult = OpenTokenHandleForTarget(adjustOptions, desiredAccess, rawTokenHandle, actualOpenedTokenTarget, ResolveOperationName(operationName, u8"GB_PrivilegeScope::OpenToken"));
    if (adjustResult.IsFailed())
    {
        ClearPrivilegeState();
        return adjustResult;
    }

    GB_WinHandleScope tokenHandleScope = GB_WinHandleScope::FromKernelHandle(rawTokenHandle, u8"AccessToken");

    GB_PrivilegeInfo beforeInfo;
    adjustResult = QueryPrivilegeInfoFromToken(rawTokenHandle, adjustOptions.privilegeName, privilegeLuid, beforeInfo, ResolveOperationName(operationName, u8"GetTokenInformation"));
    if (adjustResult.IsFailed())
    {
        ClearPrivilegeState();
        return adjustResult;
    }

    TOKEN_PRIVILEGES previousState = {};
    DWORD previousStateLength = 0;
    const DWORD targetAttributes = static_cast<DWORD>(BuildPrivilegeAttributes(adjustOptions.action));
    adjustResult = AdjustPrivilegeOnToken(rawTokenHandle, privilegeLuid, targetAttributes, previousState, previousStateLength, adjustOptions.privilegeName, ResolveOperationName(operationName, u8"AdjustTokenPrivileges"));
    if (adjustResult.IsFailed())
    {
        ClearPrivilegeState();
        return adjustResult;
    }

    GB_PrivilegeInfo authoritativePreviousInfo = beforeInfo;
    if (previousState.PrivilegeCount > 0)
    {
        authoritativePreviousInfo = MakePrivilegeInfoFromLuidAndAttributes(adjustOptions.privilegeName, previousState.Privileges[0].Luid, previousState.Privileges[0].Attributes, true);
    }

    GB_PrivilegeInfo afterInfo;
    const GB_SystemResult afterQueryResult = QueryPrivilegeInfoFromToken(rawTokenHandle, adjustOptions.privilegeName, privilegeLuid, afterInfo, ResolveOperationName(operationName, u8"GetTokenInformation"));
    if (afterQueryResult.IsFailed())
    {
        if (previousState.PrivilegeCount > 0)
        {
            (void)RestorePrivilegeOnToken(rawTokenHandle, authoritativePreviousInfo, u8"GB_PrivilegeScope::Adjust.RollbackAfterQueryFailed");
        }

        ClearPrivilegeState();
        adjustResult = afterQueryResult;
        return adjustResult;
    }

    tokenHandle = tokenHandleScope.Detach();
    adjusted = true;
    stateChanged = previousState.PrivilegeCount > 0;
    restoreOnDestruct = adjustOptions.restoreOnDestruct;
    options = adjustOptions;
    openedTokenTarget = actualOpenedTokenTarget;
    requestedAttributes = static_cast<uint32_t>(targetAttributes);
    previousPrivilegeInfo = authoritativePreviousInfo;
    adjustedPrivilegeInfo = afterInfo;
    adjustResult = GB_SystemResult::Succeeded(ResolveOperationName(operationName, u8"GB_PrivilegeScope::Adjust"), BuildAdjustDetailMessage(options, openedTokenTarget, stateChanged, previousPrivilegeInfo, adjustedPrivilegeInfo));
    lastRestoreResult = GB_SystemResult::Succeeded(u8"GB_PrivilegeScope::Adjust");
    return adjustResult;
#else
    adjustResult = MakeUnsupportedPlatformResult(ResolveOperationName(operationName, u8"GB_PrivilegeScope::Adjust"));
    return adjustResult;
#endif
}

GB_SystemResult GB_PrivilegeScope::Enable(const std::string& privilegeName, const std::string& operationName)
{
    return Adjust(MakeEnableOptions(privilegeName), ResolveOperationName(operationName, u8"GB_PrivilegeScope::Enable"));
}

GB_SystemResult GB_PrivilegeScope::Disable(const std::string& privilegeName, const std::string& operationName)
{
    return Adjust(MakeDisableOptions(privilegeName), ResolveOperationName(operationName, u8"GB_PrivilegeScope::Disable"));
}

GB_SystemResult GB_PrivilegeScope::Enable(const GB_WindowsPrivilege privilege, const std::string& operationName)
{
    return Enable(GetWindowsPrivilegeName(privilege), operationName);
}

GB_SystemResult GB_PrivilegeScope::Disable(const GB_WindowsPrivilege privilege, const std::string& operationName)
{
    return Disable(GetWindowsPrivilegeName(privilege), operationName);
}

GB_SystemResult GB_PrivilegeScope::Reset(const GB_PrivilegeScopeOptions& adjustOptions, const std::string& operationName)
{
    const GB_SystemResult restoreResult = Restore();
    if (restoreResult.IsFailed())
    {
        return restoreResult;
    }

    return Adjust(adjustOptions, operationName);
}

GB_SystemResult GB_PrivilegeScope::Restore()
{
    if (!adjusted)
    {
        ClearPrivilegeState();
        lastRestoreResult = MakeNoNeedRestoreResult();
        return lastRestoreResult;
    }

    if (!restoreOnDestruct)
    {
#if defined(_WIN32)
        const GB_SystemResult closeResult = CloseTokenHandleWithResult(tokenHandle, u8"GB_PrivilegeScope::Restore.CloseToken");
#endif
        ClearPrivilegeState();
#if defined(_WIN32)
        if (closeResult.IsFailed())
        {
            lastRestoreResult = closeResult;
            return lastRestoreResult;
        }
#endif
        lastRestoreResult = MakeRestoreSkippedResult();
        return lastRestoreResult;
    }

    if (!stateChanged)
    {
#if defined(_WIN32)
        const GB_SystemResult closeResult = CloseTokenHandleWithResult(tokenHandle, u8"GB_PrivilegeScope::Restore.CloseToken");
#endif
        ClearPrivilegeState();
#if defined(_WIN32)
        if (closeResult.IsFailed())
        {
            lastRestoreResult = closeResult;
            return lastRestoreResult;
        }
#endif
        lastRestoreResult = MakeNoStateChangedRestoreResult();
        return lastRestoreResult;
    }

#if defined(_WIN32)
    if (tokenHandle == nullptr)
    {
        lastRestoreResult = GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_PrivilegeScope::Restore", u8"内部令牌句柄为空，无法恢复权限旧状态。");
        return lastRestoreResult;
    }

    lastRestoreResult = RestorePrivilegeOnToken(static_cast<HANDLE>(tokenHandle), previousPrivilegeInfo, u8"GB_PrivilegeScope::Restore");
    if (lastRestoreResult.IsFailed())
    {
        return lastRestoreResult;
    }

    const GB_SystemResult closeResult = CloseTokenHandleWithResult(tokenHandle, u8"GB_PrivilegeScope::Restore.CloseToken");
    if (closeResult.IsFailed())
    {
        ClearPrivilegeState();
        lastRestoreResult = closeResult;
        return lastRestoreResult;
    }

    lastRestoreResult = GB_SystemResult::Succeeded(u8"GB_PrivilegeScope::Restore", BuildRestoreDetailMessage(options, openedTokenTarget, previousPrivilegeInfo));
    ClearPrivilegeState();
    return lastRestoreResult;
#else
    lastRestoreResult = MakeUnsupportedPlatformResult(u8"GB_PrivilegeScope::Restore");
    return lastRestoreResult;
#endif
}

GB_SystemResult GB_PrivilegeScope::Detach()
{
    if (!adjusted)
    {
        ClearPrivilegeState();
        lastRestoreResult = MakeNoNeedRestoreResult();
        return lastRestoreResult;
    }

#if defined(_WIN32)
    const GB_SystemResult closeResult = CloseTokenHandleWithResult(tokenHandle, u8"GB_PrivilegeScope::Detach.CloseToken");
#endif

    ClearPrivilegeState();
#if defined(_WIN32)
    if (closeResult.IsFailed())
    {
        lastRestoreResult = closeResult;
        return lastRestoreResult;
    }
#endif
    lastRestoreResult = MakeDetachSucceededResult();
    return lastRestoreResult;
}

bool GB_PrivilegeScope::IsAdjusted() const
{
    return adjusted;
}

bool GB_PrivilegeScope::IsEmpty() const
{
    return !adjusted;
}

bool GB_PrivilegeScope::HasOwnership() const
{
    return adjusted;
}

bool GB_PrivilegeScope::WasStateChanged() const
{
    return stateChanged;
}

bool GB_PrivilegeScope::IsRestoreOnDestruct() const
{
    return restoreOnDestruct;
}

GB_PrivilegeScope::operator bool() const
{
    return IsAdjusted();
}

GB_PrivilegeScope::NativeTokenHandle GB_PrivilegeScope::GetTokenHandle() const
{
    return tokenHandle;
}

uint64_t GB_PrivilegeScope::GetTokenHandleValue() const
{
    return MakeTokenHandleValue(tokenHandle);
}

GB_PrivilegeScopeOptions GB_PrivilegeScope::GetOptions() const
{
    return options;
}

std::string GB_PrivilegeScope::GetPrivilegeName() const
{
    return options.privilegeName;
}

GB_PrivilegeAction GB_PrivilegeScope::GetAction() const
{
    return options.action;
}

GB_PrivilegeTokenTarget GB_PrivilegeScope::GetRequestedTokenTarget() const
{
    return options.tokenTarget;
}

GB_PrivilegeTokenTarget GB_PrivilegeScope::GetOpenedTokenTarget() const
{
    return openedTokenTarget;
}

uint32_t GB_PrivilegeScope::GetRequestedAttributes() const
{
    return requestedAttributes;
}

GB_PrivilegeInfo GB_PrivilegeScope::GetPreviousPrivilegeInfo() const
{
    return previousPrivilegeInfo;
}

GB_PrivilegeInfo GB_PrivilegeScope::GetAdjustedPrivilegeInfo() const
{
    return adjustedPrivilegeInfo;
}

GB_SystemResult GB_PrivilegeScope::GetAdjustResult() const
{
    return adjustResult;
}

GB_SystemResult GB_PrivilegeScope::GetLastRestoreResult() const
{
    return lastRestoreResult;
}

void GB_PrivilegeScope::Swap(GB_PrivilegeScope& other)
{
    using std::swap;
    swap(tokenHandle, other.tokenHandle);
    swap(adjusted, other.adjusted);
    swap(stateChanged, other.stateChanged);
    swap(restoreOnDestruct, other.restoreOnDestruct);
    swap(options, other.options);
    swap(openedTokenTarget, other.openedTokenTarget);
    swap(requestedAttributes, other.requestedAttributes);
    swap(previousPrivilegeInfo, other.previousPrivilegeInfo);
    swap(adjustedPrivilegeInfo, other.adjustedPrivilegeInfo);
    swap(adjustResult, other.adjustResult);
    swap(lastRestoreResult, other.lastRestoreResult);
}

bool GB_PrivilegeScope::IsValidTokenTargetValue(const uint64_t tokenTargetValue)
{
    switch (tokenTargetValue)
    {
    case static_cast<uint64_t>(GB_PrivilegeTokenTarget::CurrentProcess):
    case static_cast<uint64_t>(GB_PrivilegeTokenTarget::CurrentThread):
    case static_cast<uint64_t>(GB_PrivilegeTokenTarget::CurrentThreadThenProcess):
        return true;

    default:
        break;
    }

    return false;
}

bool GB_PrivilegeScope::IsValidActionValue(const uint64_t actionValue)
{
    switch (actionValue)
    {
    case static_cast<uint64_t>(GB_PrivilegeAction::Enable):
    case static_cast<uint64_t>(GB_PrivilegeAction::Disable):
        return true;

    default:
        break;
    }

    return false;
}

bool GB_PrivilegeScope::IsValidWindowsPrivilegeValue(const uint64_t privilegeValue)
{
    switch (privilegeValue)
    {
    case static_cast<uint64_t>(GB_WindowsPrivilege::CreateToken):
    case static_cast<uint64_t>(GB_WindowsPrivilege::AssignPrimaryToken):
    case static_cast<uint64_t>(GB_WindowsPrivilege::LockMemory):
    case static_cast<uint64_t>(GB_WindowsPrivilege::IncreaseQuota):
    case static_cast<uint64_t>(GB_WindowsPrivilege::MachineAccount):
    case static_cast<uint64_t>(GB_WindowsPrivilege::Tcb):
    case static_cast<uint64_t>(GB_WindowsPrivilege::Security):
    case static_cast<uint64_t>(GB_WindowsPrivilege::TakeOwnership):
    case static_cast<uint64_t>(GB_WindowsPrivilege::LoadDriver):
    case static_cast<uint64_t>(GB_WindowsPrivilege::SystemProfile):
    case static_cast<uint64_t>(GB_WindowsPrivilege::Systemtime):
    case static_cast<uint64_t>(GB_WindowsPrivilege::ProfileSingleProcess):
    case static_cast<uint64_t>(GB_WindowsPrivilege::IncreaseBasePriority):
    case static_cast<uint64_t>(GB_WindowsPrivilege::CreatePagefile):
    case static_cast<uint64_t>(GB_WindowsPrivilege::CreatePermanent):
    case static_cast<uint64_t>(GB_WindowsPrivilege::Backup):
    case static_cast<uint64_t>(GB_WindowsPrivilege::Restore):
    case static_cast<uint64_t>(GB_WindowsPrivilege::Shutdown):
    case static_cast<uint64_t>(GB_WindowsPrivilege::Debug):
    case static_cast<uint64_t>(GB_WindowsPrivilege::Audit):
    case static_cast<uint64_t>(GB_WindowsPrivilege::SystemEnvironment):
    case static_cast<uint64_t>(GB_WindowsPrivilege::ChangeNotify):
    case static_cast<uint64_t>(GB_WindowsPrivilege::RemoteShutdown):
    case static_cast<uint64_t>(GB_WindowsPrivilege::Undock):
    case static_cast<uint64_t>(GB_WindowsPrivilege::ManageVolume):
    case static_cast<uint64_t>(GB_WindowsPrivilege::Impersonate):
    case static_cast<uint64_t>(GB_WindowsPrivilege::CreateGlobal):
    case static_cast<uint64_t>(GB_WindowsPrivilege::IncreaseWorkingSet):
    case static_cast<uint64_t>(GB_WindowsPrivilege::TimeZone):
    case static_cast<uint64_t>(GB_WindowsPrivilege::CreateSymbolicLinkPrivilege):
    case static_cast<uint64_t>(GB_WindowsPrivilege::DelegateSessionUserImpersonate):
        return true;

    default:
        break;
    }

    return false;
}

bool GB_PrivilegeScope::IsValidPrivilegeName(const std::string& privilegeName)
{
    return !privilegeName.empty() && !ContainsNullCharacter(privilegeName);
}

bool GB_PrivilegeScope::IsValidOptions(const GB_PrivilegeScopeOptions& adjustOptions)
{
    if (!IsValidPrivilegeName(adjustOptions.privilegeName))
    {
        return false;
    }

    const uint64_t actionValue = static_cast<uint64_t>(static_cast<uint16_t>(adjustOptions.action));
    if (!IsValidActionValue(actionValue))
    {
        return false;
    }

    const uint64_t tokenTargetValue = static_cast<uint64_t>(static_cast<uint16_t>(adjustOptions.tokenTarget));
    if (!IsValidTokenTargetValue(tokenTargetValue))
    {
        return false;
    }

    return true;
}

std::string GB_PrivilegeScope::GetTokenTargetName(const GB_PrivilegeTokenTarget tokenTarget)
{
    switch (tokenTarget)
    {
    case GB_PrivilegeTokenTarget::CurrentProcess:
        return "CurrentProcess";

    case GB_PrivilegeTokenTarget::CurrentThread:
        return "CurrentThread";

    case GB_PrivilegeTokenTarget::CurrentThreadThenProcess:
        return "CurrentThreadThenProcess";

    default:
        break;
    }

    return "Unknown";
}

std::string GB_PrivilegeScope::GetTokenTargetDescription(const GB_PrivilegeTokenTarget tokenTarget)
{
    switch (tokenTarget)
    {
    case GB_PrivilegeTokenTarget::CurrentProcess:
        return u8"当前进程主令牌，适合关机、重启、调试其它进程等常见系统自动化能力。";

    case GB_PrivilegeTokenTarget::CurrentThread:
        return u8"当前线程模拟令牌，适合 impersonation 场景。";

    case GB_PrivilegeTokenTarget::CurrentThreadThenProcess:
        return u8"优先当前线程模拟令牌；当前线程无令牌时回退到当前进程主令牌。";

    default:
        break;
    }

    return u8"未知访问令牌目标。";
}

std::string GB_PrivilegeScope::GetActionName(const GB_PrivilegeAction action)
{
    switch (action)
    {
    case GB_PrivilegeAction::Enable:
        return "Enable";

    case GB_PrivilegeAction::Disable:
        return "Disable";

    default:
        break;
    }

    return "Unknown";
}

std::string GB_PrivilegeScope::GetActionDescription(const GB_PrivilegeAction action)
{
    switch (action)
    {
    case GB_PrivilegeAction::Enable:
        return u8"启用访问令牌中已经存在的指定权限。";

    case GB_PrivilegeAction::Disable:
        return u8"禁用访问令牌中已经存在的指定权限。";

    default:
        break;
    }

    return u8"未知权限调整动作。";
}

std::string GB_PrivilegeScope::GetWindowsPrivilegeName(const GB_WindowsPrivilege privilege)
{
    switch (privilege)
    {
    case GB_WindowsPrivilege::CreateToken:
        return "SeCreateTokenPrivilege";

    case GB_WindowsPrivilege::AssignPrimaryToken:
        return "SeAssignPrimaryTokenPrivilege";

    case GB_WindowsPrivilege::LockMemory:
        return "SeLockMemoryPrivilege";

    case GB_WindowsPrivilege::IncreaseQuota:
        return "SeIncreaseQuotaPrivilege";

    case GB_WindowsPrivilege::MachineAccount:
        return "SeMachineAccountPrivilege";

    case GB_WindowsPrivilege::Tcb:
        return "SeTcbPrivilege";

    case GB_WindowsPrivilege::Security:
        return "SeSecurityPrivilege";

    case GB_WindowsPrivilege::TakeOwnership:
        return "SeTakeOwnershipPrivilege";

    case GB_WindowsPrivilege::LoadDriver:
        return "SeLoadDriverPrivilege";

    case GB_WindowsPrivilege::SystemProfile:
        return "SeSystemProfilePrivilege";

    case GB_WindowsPrivilege::Systemtime:
        return "SeSystemtimePrivilege";

    case GB_WindowsPrivilege::ProfileSingleProcess:
        return "SeProfileSingleProcessPrivilege";

    case GB_WindowsPrivilege::IncreaseBasePriority:
        return "SeIncreaseBasePriorityPrivilege";

    case GB_WindowsPrivilege::CreatePagefile:
        return "SeCreatePagefilePrivilege";

    case GB_WindowsPrivilege::CreatePermanent:
        return "SeCreatePermanentPrivilege";

    case GB_WindowsPrivilege::Backup:
        return "SeBackupPrivilege";

    case GB_WindowsPrivilege::Restore:
        return "SeRestorePrivilege";

    case GB_WindowsPrivilege::Shutdown:
        return "SeShutdownPrivilege";

    case GB_WindowsPrivilege::Debug:
        return "SeDebugPrivilege";

    case GB_WindowsPrivilege::Audit:
        return "SeAuditPrivilege";

    case GB_WindowsPrivilege::SystemEnvironment:
        return "SeSystemEnvironmentPrivilege";

    case GB_WindowsPrivilege::ChangeNotify:
        return "SeChangeNotifyPrivilege";

    case GB_WindowsPrivilege::RemoteShutdown:
        return "SeRemoteShutdownPrivilege";

    case GB_WindowsPrivilege::Undock:
        return "SeUndockPrivilege";

    case GB_WindowsPrivilege::ManageVolume:
        return "SeManageVolumePrivilege";

    case GB_WindowsPrivilege::Impersonate:
        return "SeImpersonatePrivilege";

    case GB_WindowsPrivilege::CreateGlobal:
        return "SeCreateGlobalPrivilege";

    case GB_WindowsPrivilege::IncreaseWorkingSet:
        return "SeIncreaseWorkingSetPrivilege";

    case GB_WindowsPrivilege::TimeZone:
        return "SeTimeZonePrivilege";

    case GB_WindowsPrivilege::CreateSymbolicLinkPrivilege:
        return "SeCreateSymbolicLinkPrivilege";

    case GB_WindowsPrivilege::DelegateSessionUserImpersonate:
        return "SeDelegateSessionUserImpersonatePrivilege";

    default:
        break;
    }

    return "";
}

std::string GB_PrivilegeScope::GetWindowsPrivilegeDescription(const GB_WindowsPrivilege privilege)
{
    switch (privilege)
    {
    case GB_WindowsPrivilege::CreateToken:
        return u8"创建令牌对象。";

    case GB_WindowsPrivilege::AssignPrimaryToken:
        return u8"替换进程级主令牌。";

    case GB_WindowsPrivilege::LockMemory:
        return u8"锁定内存页。";

    case GB_WindowsPrivilege::IncreaseQuota:
        return u8"调整进程内存配额。";

    case GB_WindowsPrivilege::MachineAccount:
        return u8"将工作站添加到域。";

    case GB_WindowsPrivilege::Tcb:
        return u8"作为操作系统的一部分执行。";

    case GB_WindowsPrivilege::Security:
        return u8"管理审核和安全日志。";

    case GB_WindowsPrivilege::TakeOwnership:
        return u8"取得文件或其它对象所有权。";

    case GB_WindowsPrivilege::LoadDriver:
        return u8"加载和卸载设备驱动程序。";

    case GB_WindowsPrivilege::SystemProfile:
        return u8"分析系统性能。";

    case GB_WindowsPrivilege::Systemtime:
        return u8"更改系统时间。";

    case GB_WindowsPrivilege::ProfileSingleProcess:
        return u8"分析单个进程性能。";

    case GB_WindowsPrivilege::IncreaseBasePriority:
        return u8"提高计划优先级。";

    case GB_WindowsPrivilege::CreatePagefile:
        return u8"创建页面文件。";

    case GB_WindowsPrivilege::CreatePermanent:
        return u8"创建永久共享对象。";

    case GB_WindowsPrivilege::Backup:
        return u8"备份文件和目录。";

    case GB_WindowsPrivilege::Restore:
        return u8"还原文件和目录。";

    case GB_WindowsPrivilege::Shutdown:
        return u8"关闭本地系统。";

    case GB_WindowsPrivilege::Debug:
        return u8"调试程序。";

    case GB_WindowsPrivilege::Audit:
        return u8"生成安全审核。";

    case GB_WindowsPrivilege::SystemEnvironment:
        return u8"修改固件环境变量。";

    case GB_WindowsPrivilege::ChangeNotify:
        return u8"绕过遍历检查。";

    case GB_WindowsPrivilege::RemoteShutdown:
        return u8"从远程系统强制关机。";

    case GB_WindowsPrivilege::Undock:
        return u8"从扩展坞中移除计算机。";

    case GB_WindowsPrivilege::ManageVolume:
        return u8"执行卷维护任务。";

    case GB_WindowsPrivilege::Impersonate:
        return u8"在身份验证后模拟客户端。";

    case GB_WindowsPrivilege::CreateGlobal:
        return u8"创建全局对象。";

    case GB_WindowsPrivilege::IncreaseWorkingSet:
        return u8"增加进程工作集。";

    case GB_WindowsPrivilege::TimeZone:
        return u8"更改时区。";

    case GB_WindowsPrivilege::CreateSymbolicLinkPrivilege:
        return u8"创建符号链接。";

    case GB_WindowsPrivilege::DelegateSessionUserImpersonate:
        return u8"获取同一会话中其它用户的模拟令牌。";

    default:
        break;
    }

    return u8"未知 Windows 权限。";
}

uint32_t GB_PrivilegeScope::BuildPrivilegeAttributes(const GB_PrivilegeAction action)
{
    switch (NormalizeAction(action))
    {
    case GB_PrivilegeAction::Enable:
        return GB_SePrivilegeEnabled;

    case GB_PrivilegeAction::Disable:
        return 0;

    default:
        break;
    }

    return 0;
}

GB_SystemResult GB_PrivilegeScope::QueryPrivilegeInfo(const std::string& privilegeName, GB_PrivilegeInfo& privilegeInfo, const GB_PrivilegeTokenTarget tokenTarget, const bool openThreadAsSelf)
{
    privilegeInfo = GB_PrivilegeInfo();

    GB_PrivilegeScopeOptions queryOptions;
    queryOptions.privilegeName = privilegeName;
    queryOptions.action = GB_PrivilegeAction::Enable;
    queryOptions.tokenTarget = tokenTarget;
    queryOptions.openThreadAsSelf = openThreadAsSelf;
    queryOptions.restoreOnDestruct = true;

    const std::string operationName = u8"GB_PrivilegeScope::QueryPrivilegeInfo";
    if (!IsValidOptions(queryOptions))
    {
        return MakeInvalidOptionsResult(queryOptions, operationName);
    }

#if defined(_WIN32)
    LUID privilegeLuid = {};
    GB_SystemResult result = LookupPrivilegeLuid(privilegeName, privilegeLuid, u8"LookupPrivilegeValue");
    if (result.IsFailed())
    {
        return result;
    }

    HANDLE rawTokenHandle = nullptr;
    GB_PrivilegeTokenTarget actualOpenedTokenTarget = GB_PrivilegeTokenTarget::CurrentProcess;
    result = OpenTokenHandleForTarget(queryOptions, TOKEN_QUERY, rawTokenHandle, actualOpenedTokenTarget, operationName);
    if (result.IsFailed())
    {
        return result;
    }

    GB_WinHandleScope tokenHandleScope = GB_WinHandleScope::FromKernelHandle(rawTokenHandle, u8"AccessToken");
    result = QueryPrivilegeInfoFromToken(rawTokenHandle, privilegeName, privilegeLuid, privilegeInfo, u8"GetTokenInformation");
    (void)actualOpenedTokenTarget;
    return result;
#else
    return MakeUnsupportedPlatformResult(operationName);
#endif
}

GB_SystemResult GB_PrivilegeScope::QueryPrivilegeInfo(const GB_WindowsPrivilege privilege, GB_PrivilegeInfo& privilegeInfo, const GB_PrivilegeTokenTarget tokenTarget, const bool openThreadAsSelf)
{
    return QueryPrivilegeInfo(GetWindowsPrivilegeName(privilege), privilegeInfo, tokenTarget, openThreadAsSelf);
}

void GB_PrivilegeScope::CloseSilently() noexcept
{
#if defined(_WIN32)
    const Win32LastErrorValueScope lastErrorValueScope;

    if (adjusted && restoreOnDestruct && stateChanged && tokenHandle != nullptr)
    {
        RestorePrivilegeOnTokenSilently(static_cast<HANDLE>(tokenHandle), previousPrivilegeInfo);
    }

    CloseTokenHandleSilently(tokenHandle);
#endif

    ClearPrivilegeState();
}

void GB_PrivilegeScope::ClearPrivilegeState() noexcept
{
    tokenHandle = nullptr;
    adjusted = false;
    stateChanged = false;
    restoreOnDestruct = true;
    options = GB_PrivilegeScopeOptions();
    openedTokenTarget = GB_PrivilegeTokenTarget::CurrentProcess;
    requestedAttributes = 0;
    previousPrivilegeInfo = GB_PrivilegeInfo();
    adjustedPrivilegeInfo = GB_PrivilegeInfo();
}

void GB_PrivilegeScope::MoveFrom(GB_PrivilegeScope& other)
{
    tokenHandle = other.tokenHandle;
    adjusted = other.adjusted;
    stateChanged = other.stateChanged;
    restoreOnDestruct = other.restoreOnDestruct;
    options = other.options;
    openedTokenTarget = other.openedTokenTarget;
    requestedAttributes = other.requestedAttributes;
    previousPrivilegeInfo = other.previousPrivilegeInfo;
    adjustedPrivilegeInfo = other.adjustedPrivilegeInfo;
    adjustResult = other.adjustResult;
    lastRestoreResult = other.lastRestoreResult;

    other.tokenHandle = nullptr;
    other.ClearPrivilegeState();
    other.adjustResult = GB_SystemResult::Succeeded(u8"GB_PrivilegeScope::MoveFrom", u8"权限恢复责任已经转移。普通移动对象不恢复权限。");
    other.lastRestoreResult = GB_SystemResult::Succeeded(u8"GB_PrivilegeScope::MoveFrom");
}
