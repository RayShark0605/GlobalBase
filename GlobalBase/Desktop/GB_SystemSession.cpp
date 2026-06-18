#include "GB_SystemSession.h"

#include "../GB_Utf8String.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <new>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <wtsapi32.h>
#  pragma comment(lib, "User32.lib")
#  pragma comment(lib, "Wtsapi32.lib")
#endif

namespace
{
    const uint32_t InvalidConsoleSessionId = 0xFFFFFFFFu;

    struct KnownLockStateEntry
    {
        GB_SystemSessionLockState lockState = GB_SystemSessionLockState::Unknown;
        uint64_t timestampMilliseconds = 0;
    };

    std::mutex knownLockStateMutex;
    std::condition_variable knownLockStateCondition;
    std::unordered_map<uint32_t, KnownLockStateEntry> knownLockStates;

    GB_SystemResult MakeUnsupportedPlatformResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, "当前平台不支持 Windows 会话能力。");
    }

    void AppendDiagnostic(std::string& diagnosticMessage, const std::string& message)
    {
        if (message.empty())
        {
            return;
        }
        if (!diagnosticMessage.empty())
        {
            diagnosticMessage += " ";
        }
        diagnosticMessage += message;
    }

    bool IsValidLockStateTarget(const GB_SystemSessionLockState lockState)
    {
        return lockState == GB_SystemSessionLockState::Locked || lockState == GB_SystemSessionLockState::Unlocked;
    }

    bool IsDisconnectedLikeState(const GB_SystemSessionConnectState connectState)
    {
        return connectState == GB_SystemSessionConnectState::Disconnected || connectState == GB_SystemSessionConnectState::Reset || connectState == GB_SystemSessionConnectState::Down;
    }

    void UpdateKnownLockState(const uint32_t sessionId, const GB_SystemSessionLockState lockState)
    {
        if (!IsValidLockStateTarget(lockState))
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(knownLockStateMutex);
            KnownLockStateEntry& entry = knownLockStates[sessionId];
            entry.lockState = lockState;
            entry.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
        }
        knownLockStateCondition.notify_all();
    }

    GB_SystemSessionLockState ReadKnownLockState(const uint32_t sessionId)
    {
        std::lock_guard<std::mutex> lock(knownLockStateMutex);
        const std::unordered_map<uint32_t, KnownLockStateEntry>::const_iterator iter = knownLockStates.find(sessionId);
        return iter == knownLockStates.end() ? GB_SystemSessionLockState::Unknown : iter->second.lockState;
    }

    void ClearKnownLockState(const uint32_t sessionId)
    {
        {
            std::lock_guard<std::mutex> lock(knownLockStateMutex);
            knownLockStates.erase(sessionId);
        }
        knownLockStateCondition.notify_all();
    }

#if defined(_WIN32)
    const UINT SessionWatcherStopMessage = WM_APP + 0x0512;

    class WtsMemoryScope final
    {
    public:
        WtsMemoryScope() = default;

        explicit WtsMemoryScope(void* inputMemory) : memory(inputMemory)
        {
        }

        ~WtsMemoryScope() noexcept
        {
            Reset();
        }

        WtsMemoryScope(const WtsMemoryScope&) = delete;
        WtsMemoryScope& operator=(const WtsMemoryScope&) = delete;

        void Reset(void* inputMemory = nullptr) noexcept
        {
            if (memory != nullptr)
            {
                ::WTSFreeMemory(memory);
            }
            memory = inputMemory;
        }

        void* Get() const noexcept
        {
            return memory;
        }

        template<typename ValueType>
        ValueType* GetAs() const noexcept
        {
            return static_cast<ValueType*>(memory);
        }

    private:
        void* memory = nullptr;
    };

    class DesktopHandleScope final
    {
    public:
        DesktopHandleScope() = default;

        explicit DesktopHandleScope(HDESK inputDesktopHandle) : desktopHandle(inputDesktopHandle)
        {
        }

        ~DesktopHandleScope() noexcept
        {
            Reset();
        }

        DesktopHandleScope(const DesktopHandleScope&) = delete;
        DesktopHandleScope& operator=(const DesktopHandleScope&) = delete;

        void Reset(HDESK inputDesktopHandle = nullptr) noexcept
        {
            if (desktopHandle != nullptr)
            {
                (void)::CloseDesktop(desktopHandle);
            }
            desktopHandle = inputDesktopHandle;
        }

        bool IsValid() const noexcept
        {
            return desktopHandle != nullptr;
        }

    private:
        HDESK desktopHandle = nullptr;
    };

    GB_SystemSessionConnectState MapWtsConnectState(const WTS_CONNECTSTATE_CLASS connectState)
    {
        switch (connectState)
        {
        case WTSActive:
            return GB_SystemSessionConnectState::Active;
        case WTSConnected:
            return GB_SystemSessionConnectState::Connected;
        case WTSConnectQuery:
            return GB_SystemSessionConnectState::ConnectQuery;
        case WTSShadow:
            return GB_SystemSessionConnectState::Shadow;
        case WTSDisconnected:
            return GB_SystemSessionConnectState::Disconnected;
        case WTSIdle:
            return GB_SystemSessionConnectState::Idle;
        case WTSListen:
            return GB_SystemSessionConnectState::Listen;
        case WTSReset:
            return GB_SystemSessionConnectState::Reset;
        case WTSDown:
            return GB_SystemSessionConnectState::Down;
        case WTSInit:
            return GB_SystemSessionConnectState::Init;
        default:
            return GB_SystemSessionConnectState::Unknown;
        }
    }

    bool QueryWtsString(const DWORD sessionId, const WTS_INFO_CLASS infoClass, std::string& valueUtf8, uint32_t& nativeErrorCode)
    {
        valueUtf8.clear();
        nativeErrorCode = 0;

        LPWSTR rawBuffer = nullptr;
        DWORD bytesReturned = 0;
        if (::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, infoClass, &rawBuffer, &bytesReturned) == FALSE)
        {
            nativeErrorCode = ::GetLastError();
            return false;
        }

        WtsMemoryScope buffer(rawBuffer);
        if (rawBuffer == nullptr || bytesReturned == 0)
        {
            return true;
        }
        if ((bytesReturned % sizeof(wchar_t)) != 0)
        {
            nativeErrorCode = ERROR_INVALID_DATA;
            return false;
        }

        size_t wcharCount = static_cast<size_t>(bytesReturned / sizeof(wchar_t));
        while (wcharCount > 0 && rawBuffer[wcharCount - 1] == L'\0')
        {
            wcharCount--;
        }

        if (wcharCount == 0)
        {
            return true;
        }

        const std::wstring valueWide(rawBuffer, rawBuffer + wcharCount);
        valueUtf8 = GB_WStringToUtf8(valueWide);
        return true;
    }

    bool QueryWtsConnectState(const DWORD sessionId, GB_SystemSessionConnectState& connectState, uint32_t& nativeErrorCode)
    {
        connectState = GB_SystemSessionConnectState::Unknown;
        nativeErrorCode = 0;

        LPWSTR rawBuffer = nullptr;
        DWORD bytesReturned = 0;
        if (::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSConnectState, &rawBuffer, &bytesReturned) == FALSE)
        {
            nativeErrorCode = ::GetLastError();
            return false;
        }

        WtsMemoryScope buffer(rawBuffer);
        if (rawBuffer == nullptr || bytesReturned < sizeof(WTS_CONNECTSTATE_CLASS))
        {
            nativeErrorCode = ERROR_INVALID_DATA;
            return false;
        }

        WTS_CONNECTSTATE_CLASS nativeConnectState = WTSDown;
        std::memcpy(&nativeConnectState, rawBuffer, sizeof(nativeConnectState));
        connectState = MapWtsConnectState(nativeConnectState);
        return true;
    }

    bool QueryWtsClientProtocolType(const DWORD sessionId, uint16_t& clientProtocolType, uint32_t& nativeErrorCode)
    {
        clientProtocolType = 0;
        nativeErrorCode = 0;

        LPWSTR rawBuffer = nullptr;
        DWORD bytesReturned = 0;
        if (::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSClientProtocolType, &rawBuffer, &bytesReturned) == FALSE)
        {
            nativeErrorCode = ::GetLastError();
            return false;
        }

        WtsMemoryScope buffer(rawBuffer);
        if (rawBuffer == nullptr || bytesReturned < sizeof(USHORT))
        {
            nativeErrorCode = ERROR_INVALID_DATA;
            return false;
        }

        USHORT nativeClientProtocolType = 0;
        std::memcpy(&nativeClientProtocolType, rawBuffer, sizeof(nativeClientProtocolType));
        clientProtocolType = static_cast<uint16_t>(nativeClientProtocolType);
        return true;
    }

    bool QueryCurrentProcessSessionIdNoResult(uint32_t& sessionId)
    {
        DWORD nativeSessionId = 0;
        if (::ProcessIdToSessionId(::GetCurrentProcessId(), &nativeSessionId) == FALSE)
        {
            sessionId = 0;
            return false;
        }
        sessionId = nativeSessionId;
        return true;
    }

    bool IsCurrentProcessSession(const uint32_t sessionId)
    {
        uint32_t currentSessionId = 0;
        return QueryCurrentProcessSessionIdNoResult(currentSessionId) && currentSessionId == sessionId;
    }

    bool QueryActiveConsoleSessionIdNoResult(uint32_t& sessionId, bool& available)
    {
        const DWORD nativeSessionId = ::WTSGetActiveConsoleSessionId();
        sessionId = static_cast<uint32_t>(nativeSessionId);
        available = nativeSessionId != InvalidConsoleSessionId;
        return true;
    }

    bool CanOpenInputDesktop(uint32_t& nativeErrorCode)
    {
        nativeErrorCode = 0;
        DesktopHandleScope inputDesktop(::OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP));
        if (!inputDesktop.IsValid())
        {
            nativeErrorCode = ::GetLastError();
            return false;
        }
        return true;
    }

    bool IsProbablyInteractiveState(const uint32_t sessionId, const GB_SystemSessionConnectState connectState)
    {
        if (sessionId == 0)
        {
            return false;
        }
        return connectState == GB_SystemSessionConnectState::Active;
    }

    void FillOptionalSessionFields(GB_SystemSessionInfo& sessionInfo, const bool queryWinStationName)
    {
        uint32_t nativeErrorCode = 0;
        if (queryWinStationName)
        {
            if (!QueryWtsString(sessionInfo.sessionId, WTSWinStationName, sessionInfo.winStationNameUtf8, nativeErrorCode))
            {
                sessionInfo.nativeErrorCode = nativeErrorCode;
                AppendDiagnostic(sessionInfo.diagnosticMessage, "读取 WinStation 名称失败，Win32 错误码：" + std::to_string(nativeErrorCode) + "。");
            }
        }
        if (!QueryWtsString(sessionInfo.sessionId, WTSUserName, sessionInfo.userNameUtf8, nativeErrorCode))
        {
            sessionInfo.nativeErrorCode = nativeErrorCode;
            AppendDiagnostic(sessionInfo.diagnosticMessage, "读取会话用户名失败，Win32 错误码：" + std::to_string(nativeErrorCode) + "。");
        }
        if (!QueryWtsString(sessionInfo.sessionId, WTSDomainName, sessionInfo.domainNameUtf8, nativeErrorCode))
        {
            sessionInfo.nativeErrorCode = nativeErrorCode;
            AppendDiagnostic(sessionInfo.diagnosticMessage, "读取会话域名失败，Win32 错误码：" + std::to_string(nativeErrorCode) + "。");
        }

        uint16_t clientProtocolType = 0;
        if (QueryWtsClientProtocolType(sessionInfo.sessionId, clientProtocolType, nativeErrorCode))
        {
            sessionInfo.isRemoteSession = clientProtocolType != 0;
        }
        else
        {
            sessionInfo.nativeErrorCode = nativeErrorCode;
            AppendDiagnostic(sessionInfo.diagnosticMessage, "读取会话客户端协议类型失败，Win32 错误码：" + std::to_string(nativeErrorCode) + "。");
        }

        if (sessionInfo.isCurrentProcessSession && ::GetSystemMetrics(SM_REMOTESESSION) != 0)
        {
            sessionInfo.isRemoteSession = true;
        }

        uint32_t activeConsoleSessionId = 0;
        bool activeConsoleAvailable = false;
        QueryActiveConsoleSessionIdNoResult(activeConsoleSessionId, activeConsoleAvailable);
        sessionInfo.isActiveConsoleSession = activeConsoleAvailable && activeConsoleSessionId == sessionInfo.sessionId;
        sessionInfo.isSessionZero = sessionInfo.sessionId == 0;
        sessionInfo.isProbablyInteractive = IsProbablyInteractiveState(sessionInfo.sessionId, sessionInfo.connectState);
        sessionInfo.lockedState = ReadKnownLockState(sessionInfo.sessionId);
        sessionInfo.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();

        if (sessionInfo.isCurrentProcessSession)
        {
            uint64_t idleMilliseconds = 0;
            const GB_SystemResult idleResult = GB_SystemSession::GetIdleMilliseconds(idleMilliseconds);
            if (idleResult.IsSucceeded())
            {
                sessionInfo.idleMilliseconds = idleMilliseconds;
                sessionInfo.hasIdleMilliseconds = true;
            }
            else
            {
                AppendDiagnostic(sessionInfo.diagnosticMessage, "读取当前会话空闲时间失败：" + idleResult.GetDisplayMessage());
            }
        }
    }

    GB_SystemResult FillSessionInfoFromQuery(const uint32_t sessionId, GB_SystemSessionInfo& sessionInfo)
    {
        sessionInfo = GB_SystemSessionInfo();
        sessionInfo.sessionId = sessionId;
        sessionInfo.isCurrentProcessSession = IsCurrentProcessSession(sessionId);

        uint32_t nativeErrorCode = 0;
        if (!QueryWtsConnectState(sessionId, sessionInfo.connectState, nativeErrorCode))
        {
            sessionInfo.nativeErrorCode = nativeErrorCode;
            return GB_SystemResult::FromWin32Error(nativeErrorCode, "GB_SystemSession::GetSessionInfo", "读取会话连接状态失败。");
        }

        FillOptionalSessionFields(sessionInfo, true);
        return GB_SystemResult::Succeeded("GB_SystemSession::GetSessionInfo");
    }

    void FillSessionInfoFromEnumeratedSession(const WTS_SESSION_INFOW& nativeSession, GB_SystemSessionInfo& sessionInfo)
    {
        sessionInfo = GB_SystemSessionInfo();
        sessionInfo.sessionId = static_cast<uint32_t>(nativeSession.SessionId);
        sessionInfo.connectState = MapWtsConnectState(nativeSession.State);
        sessionInfo.isCurrentProcessSession = IsCurrentProcessSession(sessionInfo.sessionId);
        if (nativeSession.pWinStationName != nullptr)
        {
            sessionInfo.winStationNameUtf8 = GB_WStringToUtf8(std::wstring(nativeSession.pWinStationName));
        }
        FillOptionalSessionFields(sessionInfo, false);
    }

    GB_SystemSessionEventType MapNativeSessionEvent(const uint32_t nativeEvent)
    {
        switch (nativeEvent)
        {
        case WTS_CONSOLE_CONNECT:
            return GB_SystemSessionEventType::ConsoleConnect;
        case WTS_CONSOLE_DISCONNECT:
            return GB_SystemSessionEventType::ConsoleDisconnect;
        case WTS_REMOTE_CONNECT:
            return GB_SystemSessionEventType::RemoteConnect;
        case WTS_REMOTE_DISCONNECT:
            return GB_SystemSessionEventType::RemoteDisconnect;
        case WTS_SESSION_LOGON:
            return GB_SystemSessionEventType::Logon;
        case WTS_SESSION_LOGOFF:
            return GB_SystemSessionEventType::Logoff;
        case WTS_SESSION_LOCK:
            return GB_SystemSessionEventType::Lock;
        case WTS_SESSION_UNLOCK:
            return GB_SystemSessionEventType::Unlock;
        case WTS_SESSION_REMOTE_CONTROL:
            return GB_SystemSessionEventType::RemoteControlChanged;
        case WTS_SESSION_CREATE:
            return GB_SystemSessionEventType::SessionCreated;
        case WTS_SESSION_TERMINATE:
            return GB_SystemSessionEventType::SessionTerminated;
        case WTS_SESSION_DESKTOP_READY:
            return GB_SystemSessionEventType::DesktopReady;
        default:
            return GB_SystemSessionEventType::Unknown;
        }
    }

    std::string MakeSessionEventName(const GB_SystemSessionEventType eventType)
    {
        return "SystemSession." + GB_SystemSession::GetSessionEventTypeName(eventType);
    }

    GB_SystemResult MakeWtsRegisterSessionNotificationFailureResult(const uint32_t nativeErrorCode)
    {
        GB_SystemResult result = GB_SystemResult::FromWin32Error(nativeErrorCode, "GB_SystemSessionWatcher::Start", "注册 WTS 会话通知失败。");
        if (nativeErrorCode == RPC_S_INVALID_BINDING)
        {
            result.errorCode = GB_SystemErrorCode::ResourceBusy;
            result.message = "注册 WTS 会话通知失败：Remote Desktop Services 依赖服务可能尚未启动完成，可在 Global\\TermSrvReadyEvent 置位后重试。";
        }
        return result;
    }
#endif
}

GB_SystemResult GB_SystemSession::GetCurrentProcessSessionId(uint32_t& sessionId)
{
    sessionId = 0;
#if defined(_WIN32)
    DWORD nativeSessionId = 0;
    if (::ProcessIdToSessionId(::GetCurrentProcessId(), &nativeSessionId) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error("GB_SystemSession::GetCurrentProcessSessionId", "读取当前进程会话 ID 失败。");
    }
    sessionId = static_cast<uint32_t>(nativeSessionId);
    return GB_SystemResult::Succeeded("GB_SystemSession::GetCurrentProcessSessionId");
#else
    return MakeUnsupportedPlatformResult("GB_SystemSession::GetCurrentProcessSessionId");
#endif
}

GB_SystemResult GB_SystemSession::GetProcessSessionId(const uint32_t processId, uint32_t& sessionId)
{
    sessionId = 0;
#if defined(_WIN32)
    if (processId == 0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemSession::GetProcessSessionId", "processId 不能为 0。");
    }

    DWORD nativeSessionId = 0;
    if (::ProcessIdToSessionId(static_cast<DWORD>(processId), &nativeSessionId) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error("GB_SystemSession::GetProcessSessionId", "读取指定进程会话 ID 失败。");
    }
    sessionId = static_cast<uint32_t>(nativeSessionId);
    return GB_SystemResult::Succeeded("GB_SystemSession::GetProcessSessionId");
#else
    (void)processId;
    return MakeUnsupportedPlatformResult("GB_SystemSession::GetProcessSessionId");
#endif
}

GB_SystemResult GB_SystemSession::GetActiveConsoleSessionId(uint32_t& sessionId, bool& available)
{
    sessionId = InvalidConsoleSessionId;
    available = false;
#if defined(_WIN32)
    QueryActiveConsoleSessionIdNoResult(sessionId, available);
    return GB_SystemResult::Succeeded("GB_SystemSession::GetActiveConsoleSessionId");
#else
    return MakeUnsupportedPlatformResult("GB_SystemSession::GetActiveConsoleSessionId");
#endif
}

GB_SystemResult GB_SystemSession::GetCurrentSessionInfo(GB_SystemSessionInfo& sessionInfo)
{
    sessionInfo = GB_SystemSessionInfo();
    uint32_t sessionId = 0;
    GB_SystemResult result = GetCurrentProcessSessionId(sessionId);
    if (result.IsFailed())
    {
        return result.WithOperationName("GB_SystemSession::GetCurrentSessionInfo");
    }
    GB_SystemResult sessionResult = GetSessionInfo(sessionId, sessionInfo);
    return sessionResult.WithOperationName("GB_SystemSession::GetCurrentSessionInfo");
}

GB_SystemResult GB_SystemSession::GetSessionInfo(const uint32_t sessionId, GB_SystemSessionInfo& sessionInfo)
{
    sessionInfo = GB_SystemSessionInfo();
#if defined(_WIN32)
    return FillSessionInfoFromQuery(sessionId, sessionInfo);
#else
    (void)sessionId;
    return MakeUnsupportedPlatformResult("GB_SystemSession::GetSessionInfo");
#endif
}

GB_SystemResult GB_SystemSession::EnumerateSessions(std::vector<GB_SystemSessionInfo>& sessions)
{
    sessions.clear();
#if defined(_WIN32)
    WTS_SESSION_INFOW* nativeSessions = nullptr;
    DWORD sessionCount = 0;
    if (::WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &nativeSessions, &sessionCount) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error("GB_SystemSession::EnumerateSessions", "枚举 Windows 会话失败。");
    }

    WtsMemoryScope sessionsBuffer(nativeSessions);
    try
    {
        sessions.reserve(sessionCount);
        for (DWORD index = 0; index < sessionCount; index++)
        {
            GB_SystemSessionInfo sessionInfo;
            FillSessionInfoFromEnumeratedSession(nativeSessions[index], sessionInfo);
            sessions.push_back(sessionInfo);
        }
    }
    catch (const std::bad_alloc&)
    {
        sessions.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemSession::EnumerateSessions", "保存会话枚举结果时内存不足。");
    }

    return GB_SystemResult::Succeeded("GB_SystemSession::EnumerateSessions");
#else
    return MakeUnsupportedPlatformResult("GB_SystemSession::EnumerateSessions");
#endif
}

GB_SystemResult GB_SystemSession::LockWorkstation()
{
#if defined(_WIN32)
    if (::LockWorkStation() == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error("GB_SystemSession::LockWorkstation", "发起锁定工作站请求失败。");
    }
    return GB_SystemResult::Succeeded("GB_SystemSession::LockWorkstation", "锁定工作站请求已发起；该结果不表示锁定已经完成。");
#else
    return MakeUnsupportedPlatformResult("GB_SystemSession::LockWorkstation");
#endif
}

GB_SystemResult GB_SystemSession::GetKnownLockState(GB_SystemSessionLockState& lockState)
{
    lockState = GB_SystemSessionLockState::Unknown;
    uint32_t sessionId = 0;
    GB_SystemResult result = GetCurrentProcessSessionId(sessionId);
    if (result.IsFailed())
    {
        return result.WithOperationName("GB_SystemSession::GetKnownLockState");
    }
    return GetKnownLockState(sessionId, lockState).WithOperationName("GB_SystemSession::GetKnownLockState");
}

GB_SystemResult GB_SystemSession::GetKnownLockState(const uint32_t sessionId, GB_SystemSessionLockState& lockState)
{
#if defined(_WIN32)
    lockState = ReadKnownLockState(sessionId);
    return GB_SystemResult::Succeeded("GB_SystemSession::GetKnownLockState");
#else
    (void)sessionId;
    lockState = GB_SystemSessionLockState::Unknown;
    return MakeUnsupportedPlatformResult("GB_SystemSession::GetKnownLockState");
#endif
}

GB_SystemResult GB_SystemSession::GetIdleMilliseconds(uint64_t& idleMilliseconds)
{
    idleMilliseconds = 0;
#if defined(_WIN32)
    LASTINPUTINFO lastInputInfo = {};
    lastInputInfo.cbSize = sizeof(lastInputInfo);
    if (::GetLastInputInfo(&lastInputInfo) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error("GB_SystemSession::GetIdleMilliseconds", "读取当前会话最后输入时间失败。");
    }

    // LASTINPUTINFO::dwTime 是 32 位 tick，取 GetTickCount64 低 32 位后做无符号差可保留回绕语义。
    const DWORD currentTick = static_cast<DWORD>(::GetTickCount64());
    const DWORD elapsedTick = currentTick - lastInputInfo.dwTime;
    idleMilliseconds = static_cast<uint64_t>(elapsedTick);
    return GB_SystemResult::Succeeded("GB_SystemSession::GetIdleMilliseconds");
#else
    return MakeUnsupportedPlatformResult("GB_SystemSession::GetIdleMilliseconds");
#endif
}

GB_SystemResult GB_SystemSession::WaitForLockState(const GB_SystemSessionLockState targetLockState, const GB_SystemSessionWaitOptions& waitOptions)
{
    if (!IsValidLockStateTarget(targetLockState))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemSession::WaitForLockState", "只能等待 Locked 或 Unlocked 状态。");
    }
    if (waitOptions.timeoutMilliseconds < -1)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemSession::WaitForLockState", "timeoutMilliseconds 必须为 -1 或非负数。");
    }
    if (waitOptions.pollIntervalMilliseconds == 0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemSession::WaitForLockState", "pollIntervalMilliseconds 必须大于 0。");
    }
    if (waitOptions.cancellationFlag != nullptr && waitOptions.cancellationFlag->load())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, "GB_SystemSession::WaitForLockState", "等待会话锁定状态前已收到取消请求。");
    }

    uint32_t sessionId = 0;
    GB_SystemResult result = GetCurrentProcessSessionId(sessionId);
    if (result.IsFailed())
    {
        return result.WithOperationName("GB_SystemSession::WaitForLockState");
    }

    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    const bool finiteTimeout = waitOptions.timeoutMilliseconds >= 0;
    const std::chrono::milliseconds timeoutDuration(waitOptions.timeoutMilliseconds < 0 ? 0 : waitOptions.timeoutMilliseconds);
    std::unique_lock<std::mutex> lock(knownLockStateMutex);

    while (true)
    {
        const std::unordered_map<uint32_t, KnownLockStateEntry>::const_iterator iter = knownLockStates.find(sessionId);
        if (iter != knownLockStates.end() && iter->second.lockState == targetLockState)
        {
            return GB_SystemResult::Succeeded("GB_SystemSession::WaitForLockState");
        }

        if (waitOptions.cancellationFlag != nullptr && waitOptions.cancellationFlag->load())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, "GB_SystemSession::WaitForLockState", "等待会话锁定状态时收到取消请求。");
        }

        if (finiteTimeout)
        {
            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            const std::chrono::milliseconds elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime);
            if (elapsed >= timeoutDuration)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, "GB_SystemSession::WaitForLockState", "等待会话锁定状态超时。");
            }
            const std::chrono::milliseconds remaining = timeoutDuration - elapsed;
            const std::chrono::milliseconds pollDuration(waitOptions.pollIntervalMilliseconds);
            knownLockStateCondition.wait_for(lock, (std::min)(remaining, pollDuration));
        }
        else
        {
            knownLockStateCondition.wait_for(lock, std::chrono::milliseconds(waitOptions.pollIntervalMilliseconds));
        }
    }
}

GB_SystemResult GB_SystemSession::CheckAutomationAvailability(GB_SystemSessionAvailability& availability)
{
    availability = GB_SystemSessionAvailability();
#if defined(_WIN32)
    uint32_t currentSessionId = 0;
    GB_SystemResult result = GetCurrentProcessSessionId(currentSessionId);
    if (result.IsFailed())
    {
        availability.availability = GB_SystemSessionAutomationAvailability::Unknown;
        availability.diagnosticMessage = result.GetDisplayMessage();
        return result.WithOperationName("GB_SystemSession::CheckAutomationAvailability");
    }
    availability.currentSessionId = currentSessionId;

    uint32_t activeConsoleSessionId = 0;
    bool activeConsoleAvailable = false;
    result = GetActiveConsoleSessionId(activeConsoleSessionId, activeConsoleAvailable);
    if (result.IsSucceeded())
    {
        availability.activeConsoleSessionId = activeConsoleSessionId;
        availability.hasActiveConsoleSessionId = activeConsoleAvailable;
    }

    if (currentSessionId == 0)
    {
        availability.availability = GB_SystemSessionAutomationAvailability::SessionZero;
        availability.diagnosticMessage = "当前进程运行在 Session 0，不适合直接执行 UI 自动化。";
        return GB_SystemResult::Succeeded("GB_SystemSession::CheckAutomationAvailability");
    }

    GB_SystemSessionInfo sessionInfo;
    result = GetCurrentSessionInfo(sessionInfo);
    if (result.IsFailed())
    {
        availability.availability = GB_SystemSessionAutomationAvailability::Unknown;
        availability.diagnosticMessage = result.GetDisplayMessage();
        return GB_SystemResult::Succeeded("GB_SystemSession::CheckAutomationAvailability", "无法完整读取当前会话信息。");
    }

    availability.connectState = sessionInfo.connectState;
    availability.lockedState = sessionInfo.lockedState;

    if (IsDisconnectedLikeState(sessionInfo.connectState))
    {
        availability.availability = GB_SystemSessionAutomationAvailability::Disconnected;
        availability.diagnosticMessage = "当前会话处于断开或不可用连接状态。";
        return GB_SystemResult::Succeeded("GB_SystemSession::CheckAutomationAvailability");
    }
    if (sessionInfo.lockedState == GB_SystemSessionLockState::Locked)
    {
        availability.availability = GB_SystemSessionAutomationAvailability::Locked;
        availability.diagnosticMessage = "当前会话已通过 WTS 事件确认锁定。";
        return GB_SystemResult::Succeeded("GB_SystemSession::CheckAutomationAvailability");
    }
    if (!sessionInfo.isProbablyInteractive)
    {
        availability.availability = GB_SystemSessionAutomationAvailability::NotInteractiveSession;
        availability.diagnosticMessage = "当前会话状态不满足交互式自动化的基本条件。";
        return GB_SystemResult::Succeeded("GB_SystemSession::CheckAutomationAvailability");
    }

    uint32_t nativeErrorCode = 0;
    if (!CanOpenInputDesktop(nativeErrorCode))
    {
        availability.availability = GB_SystemSessionAutomationAvailability::NoActiveDesktop;
        availability.diagnosticMessage = "当前线程无法打开输入桌面，Win32 错误码：" + std::to_string(nativeErrorCode) + "。";
        return GB_SystemResult::Succeeded("GB_SystemSession::CheckAutomationAvailability");
    }

    availability.isAvailable = true;
    availability.availability = GB_SystemSessionAutomationAvailability::Available;
    availability.diagnosticMessage = sessionInfo.lockedState == GB_SystemSessionLockState::Unknown ? "当前锁屏状态未知，但会话连接状态和输入桌面检查允许执行 UI 自动化。" : "";
    return GB_SystemResult::Succeeded("GB_SystemSession::CheckAutomationAvailability");
#else
    return MakeUnsupportedPlatformResult("GB_SystemSession::CheckAutomationAvailability");
#endif
}

GB_SystemResult GB_SystemSession::IsCurrentProcessSessionZero(bool& isSessionZero)
{
    isSessionZero = false;
    uint32_t sessionId = 0;
    GB_SystemResult result = GetCurrentProcessSessionId(sessionId);
    if (result.IsFailed())
    {
        return result.WithOperationName("GB_SystemSession::IsCurrentProcessSessionZero");
    }
    isSessionZero = sessionId == 0;
    return GB_SystemResult::Succeeded("GB_SystemSession::IsCurrentProcessSessionZero");
}

GB_SystemResult GB_SystemSession::IsCurrentProcessActiveConsoleSession(bool& isActiveConsoleSession)
{
    isActiveConsoleSession = false;
    uint32_t currentSessionId = 0;
    GB_SystemResult result = GetCurrentProcessSessionId(currentSessionId);
    if (result.IsFailed())
    {
        return result.WithOperationName("GB_SystemSession::IsCurrentProcessActiveConsoleSession");
    }

    uint32_t activeConsoleSessionId = 0;
    bool available = false;
    result = GetActiveConsoleSessionId(activeConsoleSessionId, available);
    if (result.IsFailed())
    {
        return result.WithOperationName("GB_SystemSession::IsCurrentProcessActiveConsoleSession");
    }
    isActiveConsoleSession = available && currentSessionId == activeConsoleSessionId;
    return GB_SystemResult::Succeeded("GB_SystemSession::IsCurrentProcessActiveConsoleSession");
}

GB_SystemResult GB_SystemSession::IsCurrentProcessRemoteSession(bool& isRemoteSession)
{
    isRemoteSession = false;
    GB_SystemSessionInfo sessionInfo;
    GB_SystemResult result = GetCurrentSessionInfo(sessionInfo);
    if (result.IsFailed())
    {
        return result.WithOperationName("GB_SystemSession::IsCurrentProcessRemoteSession");
    }
    isRemoteSession = sessionInfo.isRemoteSession;
    return GB_SystemResult::Succeeded("GB_SystemSession::IsCurrentProcessRemoteSession");
}

std::string GB_SystemSession::GetConnectStateName(const GB_SystemSessionConnectState connectState)
{
    switch (connectState)
    {
    case GB_SystemSessionConnectState::Active:
        return "Active";
    case GB_SystemSessionConnectState::Connected:
        return "Connected";
    case GB_SystemSessionConnectState::ConnectQuery:
        return "ConnectQuery";
    case GB_SystemSessionConnectState::Shadow:
        return "Shadow";
    case GB_SystemSessionConnectState::Disconnected:
        return "Disconnected";
    case GB_SystemSessionConnectState::Idle:
        return "Idle";
    case GB_SystemSessionConnectState::Listen:
        return "Listen";
    case GB_SystemSessionConnectState::Reset:
        return "Reset";
    case GB_SystemSessionConnectState::Down:
        return "Down";
    case GB_SystemSessionConnectState::Init:
        return "Init";
    case GB_SystemSessionConnectState::Unknown:
    default:
        return "Unknown";
    }
}

std::string GB_SystemSession::GetLockStateName(const GB_SystemSessionLockState lockState)
{
    switch (lockState)
    {
    case GB_SystemSessionLockState::Locked:
        return "Locked";
    case GB_SystemSessionLockState::Unlocked:
        return "Unlocked";
    case GB_SystemSessionLockState::Unknown:
    default:
        return "Unknown";
    }
}

std::string GB_SystemSession::GetSessionEventTypeName(const GB_SystemSessionEventType eventType)
{
    switch (eventType)
    {
    case GB_SystemSessionEventType::ConsoleConnect:
        return "ConsoleConnect";
    case GB_SystemSessionEventType::ConsoleDisconnect:
        return "ConsoleDisconnect";
    case GB_SystemSessionEventType::RemoteConnect:
        return "RemoteConnect";
    case GB_SystemSessionEventType::RemoteDisconnect:
        return "RemoteDisconnect";
    case GB_SystemSessionEventType::Logon:
        return "Logon";
    case GB_SystemSessionEventType::Logoff:
        return "Logoff";
    case GB_SystemSessionEventType::Lock:
        return "Lock";
    case GB_SystemSessionEventType::Unlock:
        return "Unlock";
    case GB_SystemSessionEventType::RemoteControlChanged:
        return "RemoteControlChanged";
    case GB_SystemSessionEventType::SessionCreated:
        return "SessionCreated";
    case GB_SystemSessionEventType::SessionTerminated:
        return "SessionTerminated";
    case GB_SystemSessionEventType::DesktopReady:
        return "DesktopReady";
    case GB_SystemSessionEventType::Unknown:
    default:
        return "Unknown";
    }
}

std::string GB_SystemSession::GetAutomationAvailabilityName(const GB_SystemSessionAutomationAvailability availability)
{
    switch (availability)
    {
    case GB_SystemSessionAutomationAvailability::Available:
        return "Available";
    case GB_SystemSessionAutomationAvailability::NotInteractiveSession:
        return "NotInteractiveSession";
    case GB_SystemSessionAutomationAvailability::SessionZero:
        return "SessionZero";
    case GB_SystemSessionAutomationAvailability::Locked:
        return "Locked";
    case GB_SystemSessionAutomationAvailability::Disconnected:
        return "Disconnected";
    case GB_SystemSessionAutomationAvailability::NoActiveDesktop:
        return "NoActiveDesktop";
    case GB_SystemSessionAutomationAvailability::Unknown:
    default:
        return "Unknown";
    }
}

class GB_SystemSessionWatcher::Impl final
{
public:
    explicit Impl(const GB_SystemSessionWatcherOptions& inputOptions)
        : options(inputOptions), eventDispatcher(GB_EventDispatcher::MakeQueuedOptions(inputOptions.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropOldest, "GB_SystemSessionWatcher"))
    {
        callbackSetupResult = eventDispatcher.SubscribeAll([this](const GB_Event& event)
            {
                DispatchTypedCallback(event);
            }, typedSubscriptionToken);
    }

    ~Impl() noexcept
    {
        (void)Stop();
    }

    GB_SystemResult Start()
    {
#if !defined(_WIN32)
        return MakeUnsupportedPlatformResult("GB_SystemSessionWatcher::Start");
#else
        if (options.notificationScope != GB_SystemSessionNotificationScope::CurrentSession && options.notificationScope != GB_SystemSessionNotificationScope::AllSessions)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemSessionWatcher::Start", "notificationScope 枚举值非法。");
        }
        if (options.maxPendingNativeEvents == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemSessionWatcher::Start", "maxPendingNativeEvents 必须大于 0。");
        }
        if (options.maxDispatchQueueSize == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemSessionWatcher::Start", "maxDispatchQueueSize 必须大于 0。");
        }
        if (callbackSetupResult.IsFailed())
        {
            return callbackSetupResult.WithOperationName("GB_SystemSessionWatcher::Start");
        }

        std::unique_lock<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (running && !stopRequested)
            {
                return GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Start");
            }
        }

        GB_SystemResult result = JoinPreviousThreadsBeforeStart();
        if (result.IsFailed())
        {
            return result;
        }

        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            stopRequested = false;
            eventWorkerStopRequested = false;
            startCompleted = false;
            startResult = GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemSessionWatcher::Start", "会话监听线程未返回启动结果。");
            windowHandle = nullptr;
            messageThreadId = 0;
            nativeEventQueue.clear();
            droppedNativeEventCount.store(0, std::memory_order_release);
        }

        result = eventDispatcher.Start();
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemSessionWatcher::Start");
        }

        try
        {
            eventWorkerThread = std::thread(&Impl::EventWorkerMain, this);
            messageThread = std::thread(&Impl::MessageThreadMainSafe, this);
        }
        catch (const std::system_error& exception)
        {
            RequestEventWorkerStop();
            JoinThreadIfNeeded(eventWorkerThread);
            (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemSessionWatcher::Start", std::string("创建会话监听线程失败：") + exception.what());
        }
        catch (const std::bad_alloc&)
        {
            RequestEventWorkerStop();
            JoinThreadIfNeeded(eventWorkerThread);
            (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemSessionWatcher::Start", "创建会话监听线程时内存不足。");
        }

        {
            std::unique_lock<std::mutex> stateLock(stateMutex);
            startCondition.wait(stateLock, [this]() { return startCompleted; });
            result = startResult;
            if (result.IsSucceeded())
            {
                running = true;
            }
        }

        if (result.IsFailed())
        {
            RequestMessageThreadStop();
            RequestEventWorkerStop();
            JoinThreadIfNeeded(messageThread);
            JoinThreadIfNeeded(eventWorkerThread);
            (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return result;
        }

        return GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Start");
#endif
    }

    GB_SystemResult Stop()
    {
#if !defined(_WIN32)
        (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
        return GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Stop");
#else
        std::unique_lock<std::mutex> operationLock(operationMutex);
        bool hadThreads = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            hadThreads = messageThread.joinable() || eventWorkerThread.joinable();
            if (!running && !hadThreads)
            {
                (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
                return GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Stop");
            }
            running = false;
            stopRequested = true;
            eventWorkerStopRequested = true;
        }

        GB_SystemResult stopMessageResult = RequestMessageThreadStop();
        nativeEventCondition.notify_all();

        if (stopMessageResult.IsFailed())
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            running = true;
            stopRequested = false;
            eventWorkerStopRequested = false;
            return stopMessageResult;
        }

        JoinThreadIfNeeded(messageThread);
        JoinThreadIfNeeded(eventWorkerThread);

        GB_SystemResult dispatcherStopResult = eventDispatcher.Stop(GB_EventDispatcherStopMode::Drain);
        if (dispatcherStopResult.IsFailed())
        {
            return dispatcherStopResult.WithOperationName("GB_SystemSessionWatcher::Stop");
        }

        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            windowHandle = nullptr;
            messageThreadId = 0;
            nativeEventQueue.clear();
        }
        return GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Stop");
#endif
    }

    bool IsRunning() const
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        return running && !stopRequested;
    }

    void SetSessionEventCallback(const SessionEventCallback& callback)
    {
        std::lock_guard<std::mutex> callbackLock(callbackMutex);
        sessionEventCallback = callback;
    }

    GB_EventDispatcher& GetEventDispatcher()
    {
        return eventDispatcher;
    }

    uint64_t GetDroppedNativeEventCount() const
    {
        return droppedNativeEventCount.load(std::memory_order_acquire);
    }

private:
    struct NativeSessionEvent
    {
        uint32_t nativeEvent = 0;
        uint32_t sessionId = 0;
        uint64_t timestampMilliseconds = 0;
    };

    void DispatchTypedCallback(const GB_Event& event)
    {
        const GB_SystemSessionEvent* sessionEvent = event.payload.AnyCast<GB_SystemSessionEvent>();
        if (sessionEvent == nullptr)
        {
            return;
        }

        SessionEventCallback callback;
        {
            std::lock_guard<std::mutex> callbackLock(callbackMutex);
            callback = sessionEventCallback;
        }
        if (callback)
        {
            callback(*sessionEvent);
        }
    }

#if defined(_WIN32)
    GB_SystemResult JoinPreviousThreadsBeforeStart()
    {
        if (messageThread.joinable())
        {
            if (std::this_thread::get_id() == messageThread.get_id())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_SystemSessionWatcher::Start", "不能在会话消息线程内重新启动监听器。");
            }
            messageThread.join();
        }
        if (eventWorkerThread.joinable())
        {
            if (std::this_thread::get_id() == eventWorkerThread.get_id())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_SystemSessionWatcher::Start", "不能在会话事件工作线程内重新启动监听器。");
            }
            eventWorkerThread.join();
        }
        return GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Start");
    }

    void JoinThreadIfNeeded(std::thread& targetThread)
    {
        if (!targetThread.joinable())
        {
            return;
        }
        if (std::this_thread::get_id() == targetThread.get_id())
        {
            targetThread.detach();
            return;
        }
        targetThread.join();
    }

    GB_SystemResult RequestMessageThreadStop()
    {
        HWND localWindowHandle = nullptr;
        DWORD localThreadId = 0;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            localWindowHandle = windowHandle;
            localThreadId = messageThreadId;
        }

        DWORD postWindowErrorCode = ERROR_SUCCESS;
        if (localWindowHandle != nullptr)
        {
            if (::PostMessageW(localWindowHandle, SessionWatcherStopMessage, 0, 0) != FALSE)
            {
                return GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Stop");
            }
            postWindowErrorCode = ::GetLastError();
        }

        if (localThreadId == 0)
        {
            if (postWindowErrorCode == ERROR_SUCCESS || postWindowErrorCode == ERROR_INVALID_WINDOW_HANDLE)
            {
                return GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Stop");
            }
            return GB_SystemResult::FromWin32Error(postWindowErrorCode, "GB_SystemSessionWatcher::Stop", "向会话监听隐藏窗口投递停止消息失败，且消息线程 ID 不可用。");
        }

        if (localThreadId == ::GetCurrentThreadId())
        {
            ::PostQuitMessage(0);
            return GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Stop");
        }

        if (::PostThreadMessageW(localThreadId, WM_QUIT, 0, 0) != FALSE)
        {
            return GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Stop");
        }

        const DWORD postThreadErrorCode = ::GetLastError();
        if (postThreadErrorCode == ERROR_INVALID_THREAD_ID)
        {
            return GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Stop", "会话监听消息线程已经退出。");
        }
        if (postWindowErrorCode != ERROR_SUCCESS && postWindowErrorCode != ERROR_INVALID_WINDOW_HANDLE)
        {
            return GB_SystemResult::FromWin32Error(postWindowErrorCode, "GB_SystemSessionWatcher::Stop", "向会话监听隐藏窗口投递停止消息失败；线程级停止消息也失败，线程 Win32 错误码：" + std::to_string(postThreadErrorCode) + "。");
        }
        return GB_SystemResult::FromWin32Error(postThreadErrorCode, "GB_SystemSessionWatcher::Stop", "向会话监听消息线程投递停止消息失败。");
    }

    void RequestEventWorkerStop()
    {
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            eventWorkerStopRequested = true;
        }
        nativeEventCondition.notify_all();
    }

    void SignalStartResult(const GB_SystemResult& result)
    {
        bool shouldNotify = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (!startCompleted)
            {
                startResult = result;
                startCompleted = true;
                shouldNotify = true;
            }
        }

        if (shouldNotify)
        {
            startCondition.notify_all();
        }
    }

    std::wstring MakeWindowClassName() const
    {
        const uintptr_t thisValue = reinterpret_cast<uintptr_t>(this);
        return L"GB_SystemSessionWatcher_" + std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(::GetTickCount64()) + L"_" + std::to_wstring(static_cast<unsigned long long>(thisValue));
    }

    void MessageThreadMainSafe()
    {
        try
        {
            MessageThreadMain();
        }
        catch (const std::bad_alloc&)
        {
            SignalStartResult(GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemSessionWatcher::Start", "会话监听消息线程内存不足，线程已退出。"));
            ClearMessageThreadState();
        }
        catch (const std::exception& exception)
        {
            SignalStartResult(GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemSessionWatcher::Start", std::string("会话监听消息线程异常退出：") + exception.what()));
            ClearMessageThreadState();
        }
        catch (...)
        {
            SignalStartResult(GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemSessionWatcher::Start", "会话监听消息线程发生未知异常并退出。"));
            ClearMessageThreadState();
        }
    }

    void MessageThreadMain()
    {
        MSG initialMessage = {};
        (void)::PeekMessageW(&initialMessage, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            messageThreadId = ::GetCurrentThreadId();
        }

        const std::wstring className = MakeWindowClassName();
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Impl::WindowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className.c_str();

        if (::RegisterClassExW(&windowClass) == 0)
        {
            SignalStartResult(GB_SystemResult::FromLastWin32Error("GB_SystemSessionWatcher::Start", "注册会话监听隐藏窗口类失败。"));
            ClearMessageThreadState();
            return;
        }

        HWND createdWindowHandle = ::CreateWindowExW(0, className.c_str(), L"GB_SystemSessionWatcher", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, this);
        if (createdWindowHandle == nullptr)
        {
            const GB_SystemResult result = GB_SystemResult::FromLastWin32Error("GB_SystemSessionWatcher::Start", "创建会话监听隐藏窗口失败。");
            (void)::UnregisterClassW(className.c_str(), windowClass.hInstance);
            SignalStartResult(result);
            ClearMessageThreadState();
            return;
        }

        const DWORD notificationFlags = options.notificationScope == GB_SystemSessionNotificationScope::AllSessions ? NOTIFY_FOR_ALL_SESSIONS : NOTIFY_FOR_THIS_SESSION;
        if (::WTSRegisterSessionNotification(createdWindowHandle, notificationFlags) == FALSE)
        {
            const DWORD nativeErrorCode = ::GetLastError();
            const GB_SystemResult result = MakeWtsRegisterSessionNotificationFailureResult(static_cast<uint32_t>(nativeErrorCode));
            (void)::DestroyWindow(createdWindowHandle);
            (void)::UnregisterClassW(className.c_str(), windowClass.hInstance);
            SignalStartResult(result);
            ClearMessageThreadState();
            return;
        }

        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            windowHandle = createdWindowHandle;
        }
        SignalStartResult(GB_SystemResult::Succeeded("GB_SystemSessionWatcher::Start"));

        MSG message = {};
        while (::GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            (void)::TranslateMessage(&message);
            (void)::DispatchMessageW(&message);
        }

        (void)::WTSUnRegisterSessionNotification(createdWindowHandle);
        (void)::DestroyWindow(createdWindowHandle);
        (void)::UnregisterClassW(className.c_str(), windowClass.hInstance);
        ClearMessageThreadState();
    }

    void ClearMessageThreadState()
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        windowHandle = nullptr;
        messageThreadId = 0;
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Impl* impl = nullptr;
        if (message == WM_NCCREATE)
        {
            CREATESTRUCTW* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            impl = createStruct == nullptr ? nullptr : static_cast<Impl*>(createStruct->lpCreateParams);
            if (impl != nullptr)
            {
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
            }
        }
        else
        {
            impl = reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (impl != nullptr && message == SessionWatcherStopMessage)
        {
            ::PostQuitMessage(0);
            return 0;
        }

        if (impl != nullptr && message == WM_WTSSESSION_CHANGE)
        {
            impl->QueueNativeEvent(static_cast<uint32_t>(wParam), static_cast<uint32_t>(lParam));
            return 0;
        }

        if (message == WM_NCDESTROY)
        {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }

        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void QueueNativeEvent(const uint32_t nativeEvent, const uint32_t sessionId)
    {
        try
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (stopRequested)
            {
                return;
            }
            if (options.maxPendingNativeEvents != 0 && nativeEventQueue.size() >= options.maxPendingNativeEvents)
            {
                nativeEventQueue.pop_front();
                droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
            }

            NativeSessionEvent event;
            event.nativeEvent = nativeEvent;
            event.sessionId = sessionId;
            event.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
            nativeEventQueue.push_back(event);
        }
        catch (...)
        {
            droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        nativeEventCondition.notify_one();
    }

    void EventWorkerMain()
    {
        while (true)
        {
            NativeSessionEvent nativeEvent;
            {
                std::unique_lock<std::mutex> stateLock(stateMutex);
                nativeEventCondition.wait(stateLock, [this]() { return eventWorkerStopRequested || !nativeEventQueue.empty(); });
                if (eventWorkerStopRequested && nativeEventQueue.empty())
                {
                    break;
                }
                nativeEvent = nativeEventQueue.front();
                nativeEventQueue.pop_front();
            }

            try
            {
                GB_SystemSessionEvent sessionEvent = BuildSessionEvent(nativeEvent);
                GB_Event typedEvent(sessionEvent.eventName, GB_Variant(sessionEvent), sessionEvent.sourceName);
                typedEvent.timestampMilliseconds = sessionEvent.timestampMilliseconds;
                typedEvent.SetAttribute("eventType", GB_Variant(static_cast<unsigned int>(sessionEvent.eventType)));
                typedEvent.SetAttribute("eventTypeName", GB_Variant(GB_SystemSession::GetSessionEventTypeName(sessionEvent.eventType)));
                typedEvent.SetAttribute("sessionId", GB_Variant(static_cast<unsigned int>(sessionEvent.sessionId)));
                typedEvent.SetAttribute("nativeEvent", GB_Variant(static_cast<unsigned int>(sessionEvent.nativeEvent)));
                const GB_SystemResult postResult = eventDispatcher.Post(typedEvent);
                if (postResult.IsFailed())
                {
                    droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            catch (...)
            {
                droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
            }
        }
    }

    GB_SystemSessionEvent BuildSessionEvent(const NativeSessionEvent& nativeEvent)
    {
        const GB_SystemSessionEventType eventType = MapNativeSessionEvent(nativeEvent.nativeEvent);
        if (eventType == GB_SystemSessionEventType::Lock)
        {
            UpdateKnownLockState(nativeEvent.sessionId, GB_SystemSessionLockState::Locked);
        }
        else if (eventType == GB_SystemSessionEventType::Unlock || eventType == GB_SystemSessionEventType::Logon || eventType == GB_SystemSessionEventType::DesktopReady)
        {
            UpdateKnownLockState(nativeEvent.sessionId, GB_SystemSessionLockState::Unlocked);
        }
        else if (eventType == GB_SystemSessionEventType::Logoff || eventType == GB_SystemSessionEventType::SessionCreated || eventType == GB_SystemSessionEventType::SessionTerminated)
        {
            ClearKnownLockState(nativeEvent.sessionId);
        }

        GB_SystemSessionEvent sessionEvent;
        sessionEvent.eventType = eventType;
        sessionEvent.eventName = MakeSessionEventName(eventType);
        sessionEvent.sourceName = "WTSRegisterSessionNotification";
        sessionEvent.timestampMilliseconds = nativeEvent.timestampMilliseconds;
        sessionEvent.nativeEvent = nativeEvent.nativeEvent;
        sessionEvent.nativeMessage = WM_WTSSESSION_CHANGE;
        sessionEvent.sessionId = nativeEvent.sessionId;

        GB_SystemSessionInfo sessionInfo;
        const GB_SystemResult infoResult = GB_SystemSession::GetSessionInfo(nativeEvent.sessionId, sessionInfo);
        if (infoResult.IsSucceeded())
        {
            sessionEvent.sessionInfo = sessionInfo;
            sessionEvent.hasSessionInfo = true;
            sessionEvent.isCurrentProcessSession = sessionInfo.isCurrentProcessSession;
            sessionEvent.isActiveConsoleSession = sessionInfo.isActiveConsoleSession;
            sessionEvent.isRemoteSession = sessionInfo.isRemoteSession;
        }
        else
        {
            uint32_t currentSessionId = 0;
            sessionEvent.isCurrentProcessSession = QueryCurrentProcessSessionIdNoResult(currentSessionId) && currentSessionId == nativeEvent.sessionId;
            uint32_t consoleSessionId = 0;
            bool consoleAvailable = false;
            QueryActiveConsoleSessionIdNoResult(consoleSessionId, consoleAvailable);
            sessionEvent.isActiveConsoleSession = consoleAvailable && consoleSessionId == nativeEvent.sessionId;
        }

        return sessionEvent;
    }
#endif

private:
    GB_SystemSessionWatcherOptions options;
    GB_EventDispatcher eventDispatcher;
    GB_EventSubscriptionToken typedSubscriptionToken;
    GB_SystemResult callbackSetupResult;
    SessionEventCallback sessionEventCallback;
    mutable std::mutex callbackMutex;
    mutable std::mutex operationMutex;
    mutable std::mutex stateMutex;
    std::condition_variable startCondition;
    std::condition_variable nativeEventCondition;
    std::deque<NativeSessionEvent> nativeEventQueue;
    std::thread messageThread;
    std::thread eventWorkerThread;
    std::atomic<uint64_t> droppedNativeEventCount{ 0 };
    bool running = false;
    bool stopRequested = false;
    bool eventWorkerStopRequested = false;
    bool startCompleted = false;
    GB_SystemResult startResult;

#if defined(_WIN32)
    HWND windowHandle = nullptr;
    DWORD messageThreadId = 0;
#endif
};

GB_SystemSessionWatcher::GB_SystemSessionWatcher() : impl(new Impl(GB_SystemSessionWatcherOptions()))
{
}

GB_SystemSessionWatcher::GB_SystemSessionWatcher(const GB_SystemSessionWatcherOptions& options) : impl(new Impl(options))
{
}

GB_SystemSessionWatcher::~GB_SystemSessionWatcher() noexcept = default;

GB_SystemResult GB_SystemSessionWatcher::Start()
{
    return impl->Start();
}

GB_SystemResult GB_SystemSessionWatcher::Stop()
{
    return impl->Stop();
}

bool GB_SystemSessionWatcher::IsRunning() const
{
    return impl->IsRunning();
}

void GB_SystemSessionWatcher::SetSessionEventCallback(const SessionEventCallback& callback)
{
    impl->SetSessionEventCallback(callback);
}

GB_EventDispatcher& GB_SystemSessionWatcher::GetEventDispatcher()
{
    return impl->GetEventDispatcher();
}

uint64_t GB_SystemSessionWatcher::GetDroppedNativeEventCount() const
{
    return impl->GetDroppedNativeEventCount();
}
