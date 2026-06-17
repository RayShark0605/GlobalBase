#ifndef GLOBALBASE_SYSTEM_SESSION_H_H
#define GLOBALBASE_SYSTEM_SESSION_H_H

#include "../GlobalBasePort.h"
#include "GB_EventDispatcher.h"
#include "GB_SystemResult.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/** @brief Windows 会话连接状态。 */
enum class GB_SystemSessionConnectState : uint16_t
{
    Unknown = 0,
    Active = 1,
    Connected = 2,
    ConnectQuery = 3,
    Shadow = 4,
    Disconnected = 5,
    Idle = 6,
    Listen = 7,
    Reset = 8,
    Down = 9,
    Init = 10
};

/** @brief 已知锁屏状态。 */
enum class GB_SystemSessionLockState : uint16_t
{
    Unknown = 0,
    Locked = 1,
    Unlocked = 2
};

/** @brief 会话事件类型。 */
enum class GB_SystemSessionEventType : uint16_t
{
    Unknown = 0,
    ConsoleConnect = 1,
    ConsoleDisconnect = 2,
    RemoteConnect = 3,
    RemoteDisconnect = 4,
    Logon = 5,
    Logoff = 6,
    Lock = 7,
    Unlock = 8,
    RemoteControlChanged = 9,
    SessionCreated = 10,
    SessionTerminated = 11,
    DesktopReady = 12
};

/** @brief WTS 会话通知范围。 */
enum class GB_SystemSessionNotificationScope : uint16_t
{
    CurrentSession = 0,
    AllSessions = 1
};

/** @brief 当前 UI 自动化可执行性判断结果。 */
enum class GB_SystemSessionAutomationAvailability : uint16_t
{
    Available = 0,
    NotInteractiveSession = 1,
    SessionZero = 2,
    Locked = 3,
    Disconnected = 4,
    NoActiveDesktop = 5,
    Unknown = 6
};

/**
 * @brief Windows 会话信息快照。
 *
 * @remarks
 * - 所有 std::string 均约定为 UTF-8 编码。
 * - lockedState 是进程内事件缓存，不是 Windows 提供的即时绝对真值。
 * - idleMilliseconds 仅对当前进程所在会话可靠。
 */
struct GB_SystemSessionInfo
{
    uint32_t sessionId = 0;
    std::string winStationNameUtf8 = "";
    std::string userNameUtf8 = "";
    std::string domainNameUtf8 = "";
    GB_SystemSessionConnectState connectState = GB_SystemSessionConnectState::Unknown;
    GB_SystemSessionLockState lockedState = GB_SystemSessionLockState::Unknown;
    bool isCurrentProcessSession = false;
    bool isActiveConsoleSession = false;
    bool isRemoteSession = false;
    bool isSessionZero = false;
    bool isProbablyInteractive = false;
    uint64_t idleMilliseconds = 0;
    bool hasIdleMilliseconds = false;
    uint64_t timestampMilliseconds = 0;
    uint64_t nativeErrorCode = 0;
    std::string diagnosticMessage = "";
};

/**
 * @brief 会话状态变化事件。
 *
 * @remarks eventName 形如 "SystemSession.Lock"，payload 可通过 GB_Variant::AnyCast<GB_SystemSessionEvent>() 取回。
 */
struct GB_SystemSessionEvent
{
    GB_SystemSessionEventType eventType = GB_SystemSessionEventType::Unknown;
    std::string eventName = "";
    std::string sourceName = "WTSRegisterSessionNotification";
    uint64_t timestampMilliseconds = 0;
    uint32_t nativeEvent = 0;
    uint32_t nativeMessage = 0;
    uint32_t sessionId = 0;
    GB_SystemSessionInfo sessionInfo;
    bool hasSessionInfo = false;
    bool isCurrentProcessSession = false;
    bool isActiveConsoleSession = false;
    bool isRemoteSession = false;
};

/** @brief UI 自动化可用性判断快照。 */
struct GB_SystemSessionAvailability
{
    bool isAvailable = false;
    GB_SystemSessionAutomationAvailability availability = GB_SystemSessionAutomationAvailability::Unknown;
    uint32_t currentSessionId = 0;
    uint32_t activeConsoleSessionId = 0;
    bool hasActiveConsoleSessionId = false;
    GB_SystemSessionConnectState connectState = GB_SystemSessionConnectState::Unknown;
    GB_SystemSessionLockState lockedState = GB_SystemSessionLockState::Unknown;
    std::string diagnosticMessage = "";
};

/** @brief 等待会话状态变化时使用的选项。 */
struct GB_SystemSessionWaitOptions
{
    int64_t timeoutMilliseconds = 5000;
    uint32_t pollIntervalMilliseconds = 50;
    const std::atomic<bool>* cancellationFlag = nullptr;
};

/** @brief 会话事件监听器选项。 */
struct GB_SystemSessionWatcherOptions
{
    GB_SystemSessionNotificationScope notificationScope = GB_SystemSessionNotificationScope::CurrentSession;
    size_t maxPendingNativeEvents = 1024;
    size_t maxDispatchQueueSize = 1024;
};

/**
 * @brief Windows 用户会话状态查询与轻量控制能力。
 *
 * @remarks
 * - 本类只处理会话上下文、锁屏请求、空闲时间和 UI 自动化前置判断。
 * - 本类不提供解锁、注销、跨会话创建进程、服务通知 HandlerEx 或安全桌面绕过能力。
 * - 非 Windows 平台下相关能力返回 UnsupportedPlatform。
 */
class GLOBALBASE_PORT GB_SystemSession final
{
public:
    GB_SystemSession() = delete;
    ~GB_SystemSession() = delete;

    static GB_SystemResult GetCurrentProcessSessionId(uint32_t& sessionId);
    static GB_SystemResult GetProcessSessionId(uint32_t processId, uint32_t& sessionId);
    static GB_SystemResult GetActiveConsoleSessionId(uint32_t& sessionId, bool& available);
    static GB_SystemResult GetCurrentSessionInfo(GB_SystemSessionInfo& sessionInfo);
    static GB_SystemResult GetSessionInfo(uint32_t sessionId, GB_SystemSessionInfo& sessionInfo);
    static GB_SystemResult EnumerateSessions(std::vector<GB_SystemSessionInfo>& sessions);
    static GB_SystemResult LockWorkstation();
    static GB_SystemResult GetKnownLockState(GB_SystemSessionLockState& lockState);
    static GB_SystemResult GetKnownLockState(uint32_t sessionId, GB_SystemSessionLockState& lockState);
    static GB_SystemResult GetIdleMilliseconds(uint64_t& idleMilliseconds);
    static GB_SystemResult WaitForLockState(GB_SystemSessionLockState targetLockState, const GB_SystemSessionWaitOptions& waitOptions = GB_SystemSessionWaitOptions());
    static GB_SystemResult CheckAutomationAvailability(GB_SystemSessionAvailability& availability);
    static GB_SystemResult IsCurrentProcessSessionZero(bool& isSessionZero);
    static GB_SystemResult IsCurrentProcessActiveConsoleSession(bool& isActiveConsoleSession);
    static GB_SystemResult IsCurrentProcessRemoteSession(bool& isRemoteSession);

    static std::string GetConnectStateName(GB_SystemSessionConnectState connectState);
    static std::string GetLockStateName(GB_SystemSessionLockState lockState);
    static std::string GetSessionEventTypeName(GB_SystemSessionEventType eventType);
    static std::string GetAutomationAvailabilityName(GB_SystemSessionAutomationAvailability availability);
};

/**
 * @brief Windows 会话事件监听器。
 *
 * @remarks
 * - Windows 下使用隐藏消息窗口注册 WTSRegisterSessionNotification。
 * - 原生窗口线程只接收 WM_WTSSESSION_CHANGE 并入队，事件补充和用户回调通过内部工作线程与 GB_EventDispatcher 完成。
 * - Start()/Stop() 可重复调用；析构时自动停止；可以在强类型回调中调用 Stop()。
 */
class GLOBALBASE_PORT GB_SystemSessionWatcher final
{
public:
    using SessionEventCallback = std::function<void(const GB_SystemSessionEvent& event)>;

    GB_SystemSessionWatcher();
    explicit GB_SystemSessionWatcher(const GB_SystemSessionWatcherOptions& options);
    ~GB_SystemSessionWatcher() noexcept;

    GB_SystemSessionWatcher(const GB_SystemSessionWatcher&) = delete;
    GB_SystemSessionWatcher& operator=(const GB_SystemSessionWatcher&) = delete;

    GB_SystemResult Start();
    GB_SystemResult Stop();
    bool IsRunning() const;

    void SetSessionEventCallback(const SessionEventCallback& callback);
    GB_EventDispatcher& GetEventDispatcher();
    uint64_t GetDroppedNativeEventCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_SESSION_H_H
