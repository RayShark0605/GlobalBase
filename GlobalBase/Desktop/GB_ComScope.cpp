#include "GB_ComScope.h"

#include <sstream>
#include <utility>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <objbase.h>

#  ifdef _MSC_VER
#    pragma comment(lib, "Ole32.lib")
#  endif
#endif

namespace
{
    const uint32_t GB_CoInitMultiThreaded = 0x0u;
    const uint32_t GB_CoInitApartmentThreaded = 0x2u;
    const uint32_t GB_CoInitDisableOle1Dde = 0x4u;
    const uint32_t GB_CoInitSpeedOverMemory = 0x8u;
#if defined(_WIN32)
    static uint64_t MakeUnsignedHResultCode(const int32_t hresult)
    {
        return static_cast<uint64_t>(static_cast<uint32_t>(hresult));
    }
#endif

    static GB_ComApartmentModel NormalizeApartmentModel(const GB_ComApartmentModel apartmentModel)
    {
        const uint64_t apartmentModelValue = static_cast<uint64_t>(static_cast<uint16_t>(apartmentModel));
        if (!GB_ComScope::IsValidApartmentModelValue(apartmentModelValue))
        {
            return GB_ComApartmentModel::MultiThreaded;
        }

        return apartmentModel;
    }

    static std::string ResolveOperationName(const std::string& operationName, const std::string& defaultOperationName)
    {
        return operationName.empty() ? defaultOperationName : operationName;
    }

#if defined(_WIN32)
    static std::string BuildInitializeDetailMessage(const GB_ComInitializeOptions& options)
    {
        std::ostringstream stream;
        stream << u8"初始化当前线程 COM。";
        stream << u8" apartmentModel=" << GB_ComScope::GetApartmentModelName(options.apartmentModel);
        stream << u8", disableOle1Dde=" << (options.disableOle1Dde ? "true" : "false");
        stream << u8", speedOverMemory=" << (options.speedOverMemory ? "true" : "false");
        return stream.str();
    }
#endif

    static GB_SystemResult MakeInvalidOptionsResult(const GB_ComInitializeOptions& options, const std::string& operationName)
    {
        std::ostringstream stream;
        stream << u8"COM 初始化选项非法。";
        stream << u8" apartmentModelValue=" << static_cast<uint64_t>(static_cast<uint16_t>(options.apartmentModel));
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, ResolveOperationName(operationName, u8"GB_ComScope::Initialize"), stream.str());
    }

    static GB_SystemResult MakeAlreadyInitializedResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::AlreadyInitialized, ResolveOperationName(operationName, u8"GB_ComScope::Initialize"), u8"当前 GB_ComScope 对象已经持有一次成功的 COM 初始化；请先调用 Uninitialize() 或 Reset()。");
    }

    static GB_SystemResult MakeDifferentThreadResult(const uint64_t ownerThreadId, const uint64_t currentThreadId)
    {
        std::ostringstream stream;
        stream << u8"COM 必须在执行 CoInitializeEx 的同一线程调用 CoUninitialize。";
        stream << u8" ownerThreadId=" << ownerThreadId;
        stream << u8", currentThreadId=" << currentThreadId;
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_ComScope::Uninitialize", stream.str());
    }

#if defined(_WIN32)
    static GB_SystemResult MakeUninitializeSucceededResult()
    {
        return GB_SystemResult::Succeeded(u8"GB_ComScope::Uninitialize", u8"已完成当前线程 COM 反初始化。");
    }
#endif

    static GB_SystemResult MakeNoNeedUninitializeResult()
    {
        return GB_SystemResult::Succeeded(u8"GB_ComScope::Uninitialize", u8"当前作用域未持有 COM 初始化配平责任，未执行 CoUninitialize。");
    }

    static GB_SystemResult MakeDetachSucceededResult()
    {
        return GB_SystemResult::Succeeded(u8"GB_ComScope::Detach", u8"已释放 COM 初始化配平责任，未调用 CoUninitialize。");
    }

#if defined(_WIN32)
    static GB_SystemResult MakeSecurityAlreadyInitializedResult(const int32_t hresult, const std::string& operationName)
    {
        GB_SystemResult result = GB_SystemResult::Succeeded(ResolveOperationName(operationName, u8"GB_ComScope::InitializeSecurity"), u8"COM 安全已经由进程内其它代码初始化，按配置视为成功。");
        result.errorSource = GB_NativeErrorSource::Com;
        result.nativeErrorCode = MakeUnsignedHResultCode(hresult);
        result.hresult = hresult;
        return result;
    }
#endif

    static GB_SystemResult MakeUnsupportedPlatformResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, u8"GB_ComScope 仅在 Windows 平台提供实际 COM 初始化能力。");
    }

    static bool IsValidAuthenticationLevel(const uint32_t authenticationLevel)
    {
        return authenticationLevel <= 6;
    }

    static bool IsValidImpersonationLevel(const uint32_t impersonationLevel)
    {
        return impersonationLevel >= 1 && impersonationLevel <= 4;
    }

    static uint64_t GetCurrentNativeThreadId()
    {
#if defined(_WIN32)
        return static_cast<uint64_t>(::GetCurrentThreadId());
#else
        return 0;
#endif
    }

    static bool IsCurrentNativeThreadOwner(const uint64_t ownerThreadId)
    {
        const uint64_t currentThreadId = GetCurrentNativeThreadId();
        return ownerThreadId != 0 && currentThreadId == ownerThreadId;
    }

#if defined(_WIN32)
    static GB_SystemResult MakeComInitializeResult(const HRESULT hresult, const GB_ComInitializeOptions& options, const std::string& operationName)
    {
        std::string message = BuildInitializeDetailMessage(options);
        if (hresult == S_FALSE)
        {
            message += u8" 当前线程此前已经以相同单元模型初始化 COM，本次调用仍需要配平 CoUninitialize。";
        }

        return GB_SystemResult::FromComHResult(static_cast<int32_t>(hresult), ResolveOperationName(operationName, u8"CoInitializeEx"), message);
    }

    static GB_SystemResult MakeComSecurityResult(const HRESULT hresult, const std::string& operationName)
    {
        return GB_SystemResult::FromComHResult(static_cast<int32_t>(hresult), ResolveOperationName(operationName, u8"CoInitializeSecurity"), u8"初始化进程级 COM 安全。");
    }
#endif
}

GB_ComScope::GB_ComScope()
    : initializeResult(GB_SystemResult::Succeeded(u8"GB_ComScope"))
    , lastUninitializeResult(GB_SystemResult::Succeeded(u8"GB_ComScope"))
{
}

GB_ComScope::GB_ComScope(const GB_ComApartmentModel apartmentModel, const std::string& operationName)
    : GB_ComScope()
{
    Initialize(apartmentModel, operationName);
}

GB_ComScope::GB_ComScope(const GB_ComInitializeOptions& options, const std::string& operationName)
    : GB_ComScope()
{
    Initialize(options, operationName);
}

GB_ComScope::~GB_ComScope() noexcept
{
    CloseSilently();
}

GB_ComScope::GB_ComScope(GB_ComScope&& other)
    : GB_ComScope()
{
    MoveFrom(other);
}

GB_ComScope& GB_ComScope::operator=(GB_ComScope&& other)
{
    if (this == &other)
    {
        return *this;
    }

    const GB_SystemResult uninitializeResult = Uninitialize();
    if (uninitializeResult.IsFailed())
    {
        return *this;
    }

    MoveFrom(other);
    return *this;
}

GB_ComScope GB_ComScope::InitializeSta(const std::string& operationName, const bool disableOle1Dde)
{
    return GB_ComScope(MakeStaOptions(disableOle1Dde), ResolveOperationName(operationName, u8"GB_ComScope::InitializeSta"));
}

GB_ComScope GB_ComScope::InitializeMta(const std::string& operationName, const bool disableOle1Dde)
{
    return GB_ComScope(MakeMtaOptions(disableOle1Dde), ResolveOperationName(operationName, u8"GB_ComScope::InitializeMta"));
}

GB_ComInitializeOptions GB_ComScope::MakeStaOptions(const bool disableOle1Dde, const bool speedOverMemory)
{
    GB_ComInitializeOptions options;
    options.apartmentModel = GB_ComApartmentModel::SingleThreaded;
    options.disableOle1Dde = disableOle1Dde;
    options.speedOverMemory = speedOverMemory;
    return options;
}

GB_ComInitializeOptions GB_ComScope::MakeMtaOptions(const bool disableOle1Dde, const bool speedOverMemory)
{
    GB_ComInitializeOptions options;
    options.apartmentModel = GB_ComApartmentModel::MultiThreaded;
    options.disableOle1Dde = disableOle1Dde;
    options.speedOverMemory = speedOverMemory;
    return options;
}

GB_ComSecurityOptions GB_ComScope::MakeDefaultSecurityOptions(const bool treatAlreadyInitializedAsSucceeded)
{
    GB_ComSecurityOptions options;
    options.authenticationServiceCount = -1;
    options.authenticationLevel = 0;
    options.impersonationLevel = 3;
    options.capabilities = 0;
    options.treatAlreadyInitializedAsSucceeded = treatAlreadyInitializedAsSucceeded;
    return options;
}

GB_SystemResult GB_ComScope::Initialize(const GB_ComInitializeOptions& options, const std::string& operationName)
{
    if (initialized)
    {
        initializeResult = MakeAlreadyInitializedResult(operationName);
        return initializeResult;
    }

    if (!IsValidInitializeOptions(options))
    {
        initializeResult = MakeInvalidOptionsResult(options, operationName);
        return initializeResult;
    }

#if defined(_WIN32)
    const uint32_t coInitializeFlags = BuildCoInitializeExFlags(options);
    const HRESULT hresult = ::CoInitializeEx(nullptr, static_cast<DWORD>(coInitializeFlags));
    initializeHResult = static_cast<int32_t>(hresult);
    initializeResult = MakeComInitializeResult(hresult, options, operationName);

    if (GB_SystemError::IsHResultFailed(initializeHResult))
    {
        ClearInitializationState();
        initializeHResult = static_cast<int32_t>(hresult);
        return initializeResult;
    }

    initialized = true;
    alreadyInitializedOnThread = hresult == S_FALSE;
    initializeOptions = options;
    ownerThreadId = GetCurrentNativeThreadId();
    lastUninitializeResult = GB_SystemResult::Succeeded(u8"GB_ComScope::Initialize");
    return initializeResult;
#else
    initializeResult = MakeUnsupportedPlatformResult(ResolveOperationName(operationName, u8"GB_ComScope::Initialize"));
    return initializeResult;
#endif
}

GB_SystemResult GB_ComScope::Initialize(const GB_ComApartmentModel apartmentModel, const std::string& operationName)
{
    GB_ComInitializeOptions options;
    options.apartmentModel = apartmentModel;
    return Initialize(options, operationName);
}

GB_SystemResult GB_ComScope::Uninitialize()
{
    if (!initialized)
    {
        ClearInitializationState();
        lastUninitializeResult = MakeNoNeedUninitializeResult();
        return lastUninitializeResult;
    }

    const uint64_t currentThreadId = GetCurrentNativeThreadId();
    if (ownerThreadId != 0 && currentThreadId != ownerThreadId)
    {
        lastUninitializeResult = MakeDifferentThreadResult(ownerThreadId, currentThreadId);
        return lastUninitializeResult;
    }

#if defined(_WIN32)
    ::CoUninitialize();
    ClearInitializationState();
    lastUninitializeResult = MakeUninitializeSucceededResult();
    return lastUninitializeResult;
#else
    lastUninitializeResult = MakeUnsupportedPlatformResult(u8"GB_ComScope::Uninitialize");
    return lastUninitializeResult;
#endif
}

GB_SystemResult GB_ComScope::Reset(const GB_ComInitializeOptions& options, const std::string& operationName)
{
    const GB_SystemResult uninitializeResult = Uninitialize();
    if (uninitializeResult.IsFailed())
    {
        return uninitializeResult;
    }

    return Initialize(options, operationName);
}

GB_SystemResult GB_ComScope::Reset(const GB_ComApartmentModel apartmentModel, const std::string& operationName)
{
    GB_ComInitializeOptions options;
    options.apartmentModel = apartmentModel;
    return Reset(options, operationName);
}

GB_SystemResult GB_ComScope::Detach()
{
    if (!initialized)
    {
        lastUninitializeResult = MakeNoNeedUninitializeResult();
        return lastUninitializeResult;
    }

    ClearInitializationState();
    lastUninitializeResult = MakeDetachSucceededResult();
    return lastUninitializeResult;
}

bool GB_ComScope::IsInitialized() const
{
    return initialized;
}

bool GB_ComScope::IsEmpty() const
{
    return !initialized;
}

bool GB_ComScope::HasOwnership() const
{
    return initialized;
}

bool GB_ComScope::IsCurrentThreadOwner() const
{
    return initialized && IsCurrentNativeThreadOwner(ownerThreadId);
}

bool GB_ComScope::IsAlreadyInitializedOnThread() const
{
    return alreadyInitializedOnThread;
}

GB_ComScope::operator bool() const
{
    return IsInitialized();
}

GB_ComInitializeOptions GB_ComScope::GetInitializeOptions() const
{
    return initializeOptions;
}

GB_ComApartmentModel GB_ComScope::GetApartmentModel() const
{
    return initializeOptions.apartmentModel;
}

uint64_t GB_ComScope::GetOwnerThreadId() const
{
    return ownerThreadId;
}

int32_t GB_ComScope::GetInitializeHResult() const
{
    return initializeHResult;
}

GB_SystemResult GB_ComScope::GetInitializeResult() const
{
    return initializeResult;
}

GB_SystemResult GB_ComScope::GetLastUninitializeResult() const
{
    return lastUninitializeResult;
}

void GB_ComScope::Swap(GB_ComScope& other)
{
    using std::swap;
    swap(initialized, other.initialized);
    swap(alreadyInitializedOnThread, other.alreadyInitializedOnThread);
    swap(initializeOptions, other.initializeOptions);
    swap(ownerThreadId, other.ownerThreadId);
    swap(initializeHResult, other.initializeHResult);
    swap(initializeResult, other.initializeResult);
    swap(lastUninitializeResult, other.lastUninitializeResult);
}

bool GB_ComScope::IsValidApartmentModelValue(const uint64_t apartmentModelValue)
{
    switch (apartmentModelValue)
    {
    case static_cast<uint64_t>(GB_ComApartmentModel::MultiThreaded):
    case static_cast<uint64_t>(GB_ComApartmentModel::SingleThreaded):
        return true;

    default:
        break;
    }

    return false;
}

bool GB_ComScope::IsValidInitializeOptions(const GB_ComInitializeOptions& options)
{
    const uint64_t apartmentModelValue = static_cast<uint64_t>(options.apartmentModel);
    return IsValidApartmentModelValue(apartmentModelValue);
}

bool GB_ComScope::IsValidSecurityOptions(const GB_ComSecurityOptions& options)
{
    if (options.authenticationServiceCount != -1 && options.authenticationServiceCount != 0)
    {
        return false;
    }

    if (!IsValidAuthenticationLevel(options.authenticationLevel))
    {
        return false;
    }

    if (!IsValidImpersonationLevel(options.impersonationLevel))
    {
        return false;
    }

    return true;
}

std::string GB_ComScope::GetApartmentModelName(const GB_ComApartmentModel apartmentModel)
{
    switch (apartmentModel)
    {
    case GB_ComApartmentModel::MultiThreaded:
        return "MultiThreaded";

    case GB_ComApartmentModel::SingleThreaded:
        return "SingleThreaded";

    default:
        break;
    }

    return "Unknown";
}

std::string GB_ComScope::GetApartmentModelDescription(const GB_ComApartmentModel apartmentModel)
{
    switch (apartmentModel)
    {
    case GB_ComApartmentModel::MultiThreaded:
        return u8"多线程单元，适合无 UI 的后台线程、线程池任务或可并发调用的 COM 使用场景。";

    case GB_ComApartmentModel::SingleThreaded:
        return u8"单线程单元，适合 UI 线程、Shell 相关能力或需要窗口消息循环的 COM 使用场景。";

    default:
        break;
    }

    return u8"未知 COM 单元模型。";
}

uint32_t GB_ComScope::BuildCoInitializeExFlags(const GB_ComInitializeOptions& options)
{
    uint32_t flags = NormalizeApartmentModel(options.apartmentModel) == GB_ComApartmentModel::SingleThreaded ? GB_CoInitApartmentThreaded : GB_CoInitMultiThreaded;

    if (options.disableOle1Dde)
    {
        flags |= GB_CoInitDisableOle1Dde;
    }

    if (options.speedOverMemory)
    {
        flags |= GB_CoInitSpeedOverMemory;
    }

    return flags;
}

GB_SystemResult GB_ComScope::InitializeSecurity(const GB_ComSecurityOptions& options, const std::string& operationName)
{
    const std::string resolvedOperationName = ResolveOperationName(operationName, u8"GB_ComScope::InitializeSecurity");
    if (!IsValidSecurityOptions(options))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, resolvedOperationName, u8"COM 安全初始化选项非法：当前封装未暴露 asAuthSvc 数组，因此 authenticationServiceCount 只能为 -1 或 0；authenticationLevel 必须在 0..6 范围内；impersonationLevel 必须在 1..4 范围内。");
    }

#if defined(_WIN32)
    const HRESULT hresult = ::CoInitializeSecurity(nullptr, static_cast<LONG>(options.authenticationServiceCount), nullptr, nullptr, static_cast<DWORD>(options.authenticationLevel), static_cast<DWORD>(options.impersonationLevel), nullptr, static_cast<DWORD>(options.capabilities), nullptr);
    if (hresult == RPC_E_TOO_LATE && options.treatAlreadyInitializedAsSucceeded)
    {
        return MakeSecurityAlreadyInitializedResult(static_cast<int32_t>(hresult), resolvedOperationName);
    }

    return MakeComSecurityResult(hresult, resolvedOperationName);
#else
    (void)options;
    return MakeUnsupportedPlatformResult(resolvedOperationName);
#endif
}

void GB_ComScope::CloseSilently() noexcept
{
#if defined(_WIN32)
    if (initialized && IsCurrentNativeThreadOwner(ownerThreadId))
    {
        ::CoUninitialize();
    }
#endif

    ClearInitializationState();
}

void GB_ComScope::ClearInitializationState() noexcept
{
    initialized = false;
    alreadyInitializedOnThread = false;
    initializeOptions = GB_ComInitializeOptions();
    ownerThreadId = 0;
    initializeHResult = 0;
}

void GB_ComScope::MoveFrom(GB_ComScope& other)
{
    const GB_SystemResult transferredInitializeResult = other.initializeResult;
    const GB_SystemResult transferredLastUninitializeResult = other.lastUninitializeResult;
    GB_SystemResult movedFromInitializeResult = GB_SystemResult::Succeeded(u8"GB_ComScope::MoveFrom", u8"COM 初始化配平责任已经转移。普通移动对象不调用 CoUninitialize。");
    GB_SystemResult movedFromUninitializeResult = GB_SystemResult::Succeeded(u8"GB_ComScope::MoveFrom");

    initializeResult = transferredInitializeResult;
    lastUninitializeResult = transferredLastUninitializeResult;
    other.initializeResult = std::move(movedFromInitializeResult);
    other.lastUninitializeResult = std::move(movedFromUninitializeResult);

    initialized = other.initialized;
    alreadyInitializedOnThread = other.alreadyInitializedOnThread;
    initializeOptions = other.initializeOptions;
    ownerThreadId = other.ownerThreadId;
    initializeHResult = other.initializeHResult;
    other.ClearInitializationState();
}
