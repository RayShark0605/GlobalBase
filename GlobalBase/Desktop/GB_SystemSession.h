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

/**
 * @brief Windows 会话连接状态。
 *
 * @details
 * 该枚举对应 Windows Remote Desktop Services 的 WTS_CONNECTSTATE_CLASS 语义，
 * 用于描述一个 Session/WinStation 当前处于登录、连接、断开、监听、重置等状态。
 */
enum class GB_SystemSessionConnectState : uint16_t
{
    /** @brief 未知状态，通常表示未识别的原生状态、查询失败后的默认值或非 Windows 平台占位。 */
    Unknown = 0,

    /** @brief 用户已经登录到 WinStation，并且当前处于活动连接状态，通常是最接近可交互桌面的状态。 */
    Active = 1,

    /** @brief WinStation 已连接到客户端，但不一定代表用户桌面已经处于可自动化的活动状态。 */
    Connected = 2,

    /** @brief WinStation 正在查询或协商连接。 */
    ConnectQuery = 3,

    /** @brief WinStation 正在影子控制或镜像另一个 WinStation。 */
    Shadow = 4,

    /** @brief 会话仍存在，但客户端已经断开；远程桌面断开、切回锁屏等场景可能出现该状态。 */
    Disconnected = 5,

    /** @brief WinStation 空闲，正在等待客户端连接。 */
    Idle = 6,

    /** @brief WinStation 正在监听连接请求。 */
    Listen = 7,

    /** @brief WinStation 正在重置。 */
    Reset = 8,

    /** @brief WinStation 已关闭、不可用或处于下线状态。 */
    Down = 9,

    /** @brief WinStation 正在初始化。 */
    Init = 10
};

/**
 * @brief 已知锁屏状态。
 *
 * @details
 * Windows 没有为普通桌面进程提供一个总是可靠的“立即查询当前是否锁屏”的简单 API。
 * 本模块中的锁屏状态主要来自进程内接收到的 WTS 锁屏/解锁事件缓存，因此 Unknown 是正常且需要调用方处理的状态。
 */
enum class GB_SystemSessionLockState : uint16_t
{
    /** @brief 未知锁屏状态，通常表示本进程尚未收到对应会话的锁屏/解锁事件，或平台不支持。 */
    Unknown = 0,

    /** @brief 已通过本进程接收到的会话事件确认目标会话处于锁定状态。 */
    Locked = 1,

    /** @brief 已通过本进程接收到的会话事件确认目标会话处于解锁状态。 */
    Unlocked = 2
};

/**
 * @brief 会话事件类型。
 *
 * @details
 * 该枚举对 WM_WTSSESSION_CHANGE 消息中的 WTS_* 原生事件进行语义化封装，
 * 便于业务层通过强类型事件处理会话连接、登录、锁屏、解锁和桌面就绪等变化。
 */
enum class GB_SystemSessionEventType : uint16_t
{
    /** @brief 未知事件，表示未识别的原生 WTS 事件或默认占位。 */
    Unknown = 0,

    /** @brief 会话已连接到物理控制台终端或控制台相关会话。 */
    ConsoleConnect = 1,

    /** @brief 会话已从物理控制台终端或控制台相关会话断开。 */
    ConsoleDisconnect = 2,

    /** @brief 会话已连接到远程终端。 */
    RemoteConnect = 3,

    /** @brief 会话已从远程终端断开。 */
    RemoteDisconnect = 4,

    /** @brief 用户已登录到指定会话。 */
    Logon = 5,

    /** @brief 用户已从指定会话注销。 */
    Logoff = 6,

    /** @brief 指定会话已锁定。 */
    Lock = 7,

    /** @brief 指定会话已解锁。 */
    Unlock = 8,

    /** @brief 指定会话的远程控制状态发生变化。 */
    RemoteControlChanged = 9,

    /** @brief 指定会话已创建；在 Windows 文档中该事件属于保留用途，调用方不应强依赖其稳定业务语义。 */
    SessionCreated = 10,

    /** @brief 指定会话已终止；在 Windows 文档中该事件属于保留用途，调用方不应强依赖其稳定业务语义。 */
    SessionTerminated = 11,

    /** @brief 指定会话已切换到用户桌面，通常可作为桌面环境已就绪的辅助信号。 */
    DesktopReady = 12
};

/**
 * @brief WTS 会话通知范围。
 *
 * @details
 * 用于控制 GB_SystemSessionWatcher 注册 WTS 会话通知时只接收当前会话事件，还是接收全部会话事件。
 */
enum class GB_SystemSessionNotificationScope : uint16_t
{
    /** @brief 仅接收当前进程所在会话相关的通知。 */
    CurrentSession = 0,

    /** @brief 接收本机所有会话相关的通知。 */
    AllSessions = 1
};

/**
 * @brief 当前 UI 自动化可执行性判断结果。
 *
 * @details
 * 该枚举给出 CheckAutomationAvailability 的分类结论，用于在执行鼠标、键盘、窗口等 UI 自动化前进行前置检查。
 */
enum class GB_SystemSessionAutomationAvailability : uint16_t
{
    /** @brief 当前会话状态、锁屏缓存和输入桌面检查均允许执行 UI 自动化。 */
    Available = 0,

    /** @brief 当前会话不是可交互会话，通常不适合直接执行 UI 自动化。 */
    NotInteractiveSession = 1,

    /** @brief 当前进程运行在 Session 0，通常是服务或非用户交互环境，不适合直接执行 UI 自动化。 */
    SessionZero = 2,

    /** @brief 当前会话已知处于锁定状态。 */
    Locked = 3,

    /** @brief 当前会话处于断开、重置或下线等不可交互连接状态。 */
    Disconnected = 4,

    /** @brief 当前线程无法打开输入桌面，说明当前没有可用于 UI 自动化的活动桌面上下文。 */
    NoActiveDesktop = 5,

    /** @brief 无法可靠判断 UI 自动化是否可执行，调用方应保守处理。 */
    Unknown = 6
};

/**
 * @brief Windows 会话信息快照。
 *
 * @remarks
 * - 所有 std::string 均约定为 UTF-8 编码。
 * - lockedState 是进程内事件缓存，不是 Windows 提供的即时绝对真值；只有本进程收到过相关 WTS 事件后才会变为 Locked/Unlocked。
 * - idleMilliseconds 仅对当前进程所在会话可靠。
 */
struct GB_SystemSessionInfo
{
    /** @brief Windows 会话 ID；Session 0 通常表示服务隔离会话，普通用户交互桌面通常不是 0。 */
    uint32_t sessionId = 0;

    /** @brief WinStation 名称，UTF-8 编码；可能为空，例如查询失败、权限不足或该会话没有有效名称时。 */
    std::string winStationNameUtf8 = "";

    /** @brief 当前会话关联的用户名，UTF-8 编码；未登录、权限不足或查询失败时可能为空。 */
    std::string userNameUtf8 = "";

    /** @brief 当前会话关联的域名或计算机名，UTF-8 编码；未登录、权限不足或查询失败时可能为空。 */
    std::string domainNameUtf8 = "";

    /** @brief 会话连接状态，来自 WTSConnectState 查询或 WTS 会话枚举结果。 */
    GB_SystemSessionConnectState connectState = GB_SystemSessionConnectState::Unknown;

    /** @brief 本进程已知的锁屏状态缓存；Unknown 不代表一定未锁屏，只表示本进程尚无可靠事件缓存。 */
    GB_SystemSessionLockState lockedState = GB_SystemSessionLockState::Unknown;

    /** @brief 该会话是否为当前进程所在的会话。 */
    bool isCurrentProcessSession = false;

    /** @brief 该会话是否为当前活动的物理控制台会话；当没有活动控制台会话时该值为 false。 */
    bool isActiveConsoleSession = false;

    /** @brief 该会话是否看起来是远程会话；由 WTSClientProtocolType 和当前会话的 SM_REMOTESESSION 等信息综合判断。 */
    bool isRemoteSession = false;

    /** @brief 该会话是否为 Session 0；Session 0 通常不应直接执行面向用户桌面的 UI 自动化。 */
    bool isSessionZero = false;

    /** @brief 该会话是否大概率具备交互条件；当前实现主要要求 sessionId 非 0 且连接状态为 Active。 */
    bool isProbablyInteractive = false;

    /** @brief 当前进程所在会话的用户输入空闲时长，单位毫秒；仅当 hasIdleMilliseconds 为 true 时有效。 */
    uint64_t idleMilliseconds = 0;

    /** @brief idleMilliseconds 是否有效；只有查询当前进程所在会话空闲时间成功时才会置为 true。 */
    bool hasIdleMilliseconds = false;

    /** @brief 生成该快照的时间戳，单位毫秒；时间基准与 GB_EventDispatcher::GetCurrentTimestampMilliseconds 保持一致。 */
    uint64_t timestampMilliseconds = 0;

    /** @brief 最近一次可记录的原生 Win32 错误码；为 0 表示没有记录到原生错误。 */
    uint64_t nativeErrorCode = 0;

    /** @brief 诊断信息；用于保存可选字段读取失败、空闲时间读取失败等非致命问题的中文说明。 */
    std::string diagnosticMessage = "";
};

/**
 * @brief 会话状态变化事件。
 *
 * @remarks eventName 形如 "SystemSession.Lock"，payload 可通过 GB_Variant::AnyCast<GB_SystemSessionEvent>() 取回。
 */
struct GB_SystemSessionEvent
{
    /** @brief 语义化后的会话事件类型。 */
    GB_SystemSessionEventType eventType = GB_SystemSessionEventType::Unknown;

    /** @brief 事件名称，通常为 "SystemSession." 加事件类型英文名，例如 "SystemSession.Lock"。 */
    std::string eventName = "";

    /** @brief 事件来源名称；当前实现固定表示事件来自 WTSRegisterSessionNotification 注册的会话通知。 */
    std::string sourceName = "WTSRegisterSessionNotification";

    /** @brief 事件入队时的时间戳，单位毫秒；时间基准与 GB_EventDispatcher::GetCurrentTimestampMilliseconds 保持一致。 */
    uint64_t timestampMilliseconds = 0;

    /** @brief 原生 WTS 事件码，对应 WM_WTSSESSION_CHANGE 消息的 wParam。 */
    uint32_t nativeEvent = 0;

    /** @brief 原生窗口消息编号；Windows 下通常为 WM_WTSSESSION_CHANGE。 */
    uint32_t nativeMessage = 0;

    /** @brief 事件对应的 Windows 会话 ID，对应 WM_WTSSESSION_CHANGE 消息的 lParam。 */
    uint32_t sessionId = 0;

    /** @brief 事件发生时补充查询到的会话信息快照；只有 hasSessionInfo 为 true 时才表示查询成功。 */
    GB_SystemSessionInfo sessionInfo;

    /** @brief sessionInfo 是否有效；如果事件发生时目标会话已注销、权限不足或查询失败，则该值可能为 false。 */
    bool hasSessionInfo = false;

    /** @brief 事件对应会话是否为当前进程所在会话；即使 hasSessionInfo 为 false，也会尽量通过会话 ID 进行判断。 */
    bool isCurrentProcessSession = false;

    /** @brief 事件对应会话是否为当前活动控制台会话；即使 hasSessionInfo 为 false，也会尽量通过活动控制台会话 ID 进行判断。 */
    bool isActiveConsoleSession = false;

    /** @brief 事件对应会话是否看起来是远程会话；只有会话信息查询成功时才会被可靠填充。 */
    bool isRemoteSession = false;
};

/** @brief UI 自动化可用性判断快照。 */
struct GB_SystemSessionAvailability
{
    /** @brief 是否允许直接执行 UI 自动化；只有 availability 为 Available 时才会为 true。 */
    bool isAvailable = false;

    /** @brief UI 自动化可用性分类结果，用于说明不可用的主要原因。 */
    GB_SystemSessionAutomationAvailability availability = GB_SystemSessionAutomationAvailability::Unknown;

    /** @brief 当前进程所在的 Windows 会话 ID。 */
    uint32_t currentSessionId = 0;

    /** @brief 当前活动物理控制台会话 ID；只有 hasActiveConsoleSessionId 为 true 时有效。 */
    uint32_t activeConsoleSessionId = 0;

    /** @brief activeConsoleSessionId 是否有效；当系统暂无活动控制台会话时该值为 false。 */
    bool hasActiveConsoleSessionId = false;

    /** @brief 当前进程所在会话的连接状态；查询失败或非 Windows 平台时为 Unknown。 */
    GB_SystemSessionConnectState connectState = GB_SystemSessionConnectState::Unknown;

    /** @brief 当前进程所在会话的已知锁屏状态缓存；Unknown 表示没有可靠缓存，不等同于一定未锁屏。 */
    GB_SystemSessionLockState lockedState = GB_SystemSessionLockState::Unknown;

    /** @brief 可用性诊断说明；不可用时描述主要原因，可用但锁屏状态未知时也可能包含提示信息。 */
    std::string diagnosticMessage = "";
};

/**
 * @brief 等待会话状态变化时使用的选项。
 *
 * @remarks
 * - WaitForLockState 依赖本进程已知的 WTS 锁屏/解锁事件缓存；如果没有运行 GB_SystemSessionWatcher 或目标事件发生在监听器启动前，状态可能一直为 Unknown。
 * - cancellationFlag 不会主动唤醒条件变量，因此 pollIntervalMilliseconds 同时也是取消检查间隔。
 */
struct GB_SystemSessionWaitOptions
{
    /** @brief 最大等待时长，单位毫秒；-1 表示无限等待，0 表示只检查一次当前缓存，正数表示有限等待。 */
    int64_t timeoutMilliseconds = 5000;

    /** @brief 轮询间隔，单位毫秒；必须大于 0，同时决定取消标志的最大响应延迟。 */
    uint32_t pollIntervalMilliseconds = 50;

    /** @brief 可选取消标志指针；调用方必须保证该指针在 WaitForLockState 返回前始终有效，nullptr 表示不支持取消。 */
    const std::atomic<bool>* cancellationFlag = nullptr;
};

/** @brief 会话事件监听器选项。 */
struct GB_SystemSessionWatcherOptions
{
    /** @brief WTS 会话通知范围；默认只监听当前进程所在会话。 */
    GB_SystemSessionNotificationScope notificationScope = GB_SystemSessionNotificationScope::CurrentSession;

    /** @brief 原生 WTS 消息队列最大缓存数量；队列满时当前实现丢弃最早的原生事件并增加丢弃计数。 */
    size_t maxPendingNativeEvents = 1024;

    /** @brief GB_EventDispatcher 分发队列最大缓存数量；用于限制用户回调处理慢时的内存增长。 */
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
    /** @brief 禁止构造；GB_SystemSession 是纯静态工具类。 */
    GB_SystemSession() = delete;

    /** @brief 禁止析构；GB_SystemSession 不应被实例化。 */
    ~GB_SystemSession() = delete;

    /**
     * @brief 获取当前进程所在的 Windows 会话 ID。
     * @param sessionId 输出当前进程会话 ID；失败时会被置为 0。
     * @return 成功时返回 Succeeded；非 Windows 平台或 Win32 调用失败时返回失败结果。
     */
    static GB_SystemResult GetCurrentProcessSessionId(uint32_t& sessionId);

    /**
     * @brief 获取指定进程所在的 Windows 会话 ID。
     * @param processId 输入进程 ID；不能为 0。
     * @param sessionId 输出指定进程会话 ID；失败时会被置为 0。
     * @return 成功时返回 Succeeded；processId 非法、非 Windows 平台或 Win32 调用失败时返回失败结果。
     */
    static GB_SystemResult GetProcessSessionId(uint32_t processId, uint32_t& sessionId);

    /**
     * @brief 获取当前活动物理控制台会话 ID。
     * @param sessionId 输出活动控制台会话 ID；当 available 为 false 时该值不应作为有效会话使用。
     * @param available 输出是否存在有效的活动控制台会话；false 通常表示系统当前没有活动控制台会话。
     * @return 成功读取系统状态时返回 Succeeded；注意返回成功不代表 available 一定为 true。
     */
    static GB_SystemResult GetActiveConsoleSessionId(uint32_t& sessionId, bool& available);

    /**
     * @brief 获取当前进程所在会话的信息快照。
     * @param sessionInfo 输出会话信息快照；失败时会被重置为默认值。
     * @return 成功时返回 Succeeded；读取当前进程会话 ID 或会话连接状态失败时返回失败结果。
     */
    static GB_SystemResult GetCurrentSessionInfo(GB_SystemSessionInfo& sessionInfo);

    /**
     * @brief 获取指定会话的信息快照。
     * @param sessionId 输入目标 Windows 会话 ID。
     * @param sessionInfo 输出会话信息快照；失败时会被重置为默认值。
     * @return 成功时返回 Succeeded；读取关键会话状态失败、权限不足或平台不支持时返回失败结果。
     */
    static GB_SystemResult GetSessionInfo(uint32_t sessionId, GB_SystemSessionInfo& sessionInfo);

    /**
     * @brief 枚举本机 Windows 会话信息。
     * @param sessions 输出会话信息列表；函数入口会先清空该容器。
     * @return 成功时返回 Succeeded；枚举失败、内存不足或平台不支持时返回失败结果。
     */
    static GB_SystemResult EnumerateSessions(std::vector<GB_SystemSessionInfo>& sessions);

    /**
     * @brief 请求锁定当前工作站。
     * @return Win32 请求发起成功时返回 Succeeded；该结果只表示请求已发起，不保证锁屏已经完成。
     *
     * @details
     * Windows 要求调用进程运行在交互式桌面中；已经锁定、没有用户登录、权限或桌面上下文不满足条件时，请求可能失败或不产生实际锁定效果。
     */
    static GB_SystemResult LockWorkstation();

    /**
     * @brief 获取当前进程所在会话的已知锁屏状态。
     * @param lockState 输出已知锁屏状态；没有事件缓存时为 Unknown。
     * @return 成功读取缓存时返回 Succeeded；读取当前进程会话 ID 失败或平台不支持时返回失败结果。
     */
    static GB_SystemResult GetKnownLockState(GB_SystemSessionLockState& lockState);

    /**
     * @brief 获取指定会话的已知锁屏状态。
     * @param sessionId 输入目标 Windows 会话 ID。
     * @param lockState 输出已知锁屏状态；没有事件缓存时为 Unknown。
     * @return 成功读取缓存时返回 Succeeded；非 Windows 平台返回 UnsupportedPlatform。
     */
    static GB_SystemResult GetKnownLockState(uint32_t sessionId, GB_SystemSessionLockState& lockState);

    /**
     * @brief 获取当前进程所在会话的用户输入空闲时间。
     * @param idleMilliseconds 输出空闲时长，单位毫秒。
     * @return 成功时返回 Succeeded；读取最后输入时间失败或平台不支持时返回失败结果。
     *
     * @details
     * 该函数基于当前调用会话的最后输入时间计算，不用于跨会话统计，也不表示整机所有会话的全局空闲时间。
     */
    static GB_SystemResult GetIdleMilliseconds(uint64_t& idleMilliseconds);

    /**
     * @brief 等待当前进程所在会话达到指定锁屏状态。
     * @param targetLockState 目标锁屏状态；只能为 Locked 或 Unlocked。
     * @param waitOptions 等待选项，包括超时时间、轮询间隔和可选取消标志。
     * @return 达到目标状态时返回 Succeeded；参数非法、超时、取消或平台不支持时返回失败结果。
     *
     * @details
     * 该函数等待的是本进程维护的锁屏状态缓存。若未启动 GB_SystemSessionWatcher，或目标状态变化发生在监听启动之前，可能一直等不到目标状态。
     */
    static GB_SystemResult WaitForLockState(GB_SystemSessionLockState targetLockState, const GB_SystemSessionWaitOptions& waitOptions = GB_SystemSessionWaitOptions());

    /**
     * @brief 检查当前进程是否适合执行 UI 自动化操作。
     * @param availability 输出可用性判断快照，包含当前会话 ID、控制台会话 ID、连接状态、锁屏状态和诊断信息。
     * @return 检查流程可完成时通常返回 Succeeded；无法读取关键基础信息或平台不支持时返回失败结果。
     *
     * @details
     * 该函数主要用于鼠标、键盘、窗口等自动化操作前置判断；结果为 Available 只代表基础环境允许，不保证具体 UI 操作一定成功。
     */
    static GB_SystemResult CheckAutomationAvailability(GB_SystemSessionAvailability& availability);

    /**
     * @brief 判断当前进程是否运行在 Session 0。
     * @param isSessionZero 输出判断结果。
     * @return 成功时返回 Succeeded；读取当前进程会话 ID 失败或平台不支持时返回失败结果。
     */
    static GB_SystemResult IsCurrentProcessSessionZero(bool& isSessionZero);

    /**
     * @brief 判断当前进程所在会话是否为当前活动物理控制台会话。
     * @param isActiveConsoleSession 输出判断结果；系统没有活动控制台会话时为 false。
     * @return 成功时返回 Succeeded；读取当前进程会话 ID、活动控制台会话 ID 失败或平台不支持时返回失败结果。
     */
    static GB_SystemResult IsCurrentProcessActiveConsoleSession(bool& isActiveConsoleSession);

    /**
     * @brief 判断当前进程所在会话是否看起来是远程会话。
     * @param isRemoteSession 输出判断结果。
     * @return 成功时返回 Succeeded；读取当前会话信息失败或平台不支持时返回失败结果。
     */
    static GB_SystemResult IsCurrentProcessRemoteSession(bool& isRemoteSession);

    /**
     * @brief 获取会话连接状态的稳定英文名称。
     * @param connectState 输入会话连接状态。
     * @return 返回枚举值对应的英文名称；未知值返回 "Unknown"。
     */
    static std::string GetConnectStateName(GB_SystemSessionConnectState connectState);

    /**
     * @brief 获取锁屏状态的稳定英文名称。
     * @param lockState 输入锁屏状态。
     * @return 返回枚举值对应的英文名称；未知值返回 "Unknown"。
     */
    static std::string GetLockStateName(GB_SystemSessionLockState lockState);

    /**
     * @brief 获取会话事件类型的稳定英文名称。
     * @param eventType 输入会话事件类型。
     * @return 返回枚举值对应的英文名称；未知值返回 "Unknown"。
     */
    static std::string GetSessionEventTypeName(GB_SystemSessionEventType eventType);

    /**
     * @brief 获取 UI 自动化可用性分类的稳定英文名称。
     * @param availability 输入 UI 自动化可用性分类。
     * @return 返回枚举值对应的英文名称；未知值返回 "Unknown"。
     */
    static std::string GetAutomationAvailabilityName(GB_SystemSessionAutomationAvailability availability);
};

/**
 * @brief Windows 会话事件监听器。
 *
 * @remarks
 * - Windows 下使用隐藏消息窗口注册 WTSRegisterSessionNotification。
 * - 原生窗口线程只接收 WM_WTSSESSION_CHANGE 并入队，事件补充和用户回调通过内部工作线程与 GB_EventDispatcher 完成。
 * - Start()/Stop() 可重复调用；析构时自动停止；可以在强类型回调中调用 Stop()。
 * - 本监听器适合普通桌面进程；Windows 服务接收会话变化应使用服务控制 HandlerEx。
 */
class GLOBALBASE_PORT GB_SystemSessionWatcher final
{
public:
    /** @brief 强类型会话事件回调函数；参数为已经补充过基础会话信息的 GB_SystemSessionEvent。 */
    using SessionEventCallback = std::function<void(const GB_SystemSessionEvent& event)>;

    /** @brief 使用默认选项构造会话事件监听器；默认只监听当前会话，使用默认队列容量。 */
    GB_SystemSessionWatcher();

    /**
     * @brief 使用指定选项构造会话事件监听器。
     * @param options 会话事件监听器选项；Start 时会校验通知范围和队列容量是否合法。
     */
    explicit GB_SystemSessionWatcher(const GB_SystemSessionWatcherOptions& options);

    /** @brief 析构监听器并自动停止内部消息线程、事件工作线程和事件分发器。 */
    ~GB_SystemSessionWatcher() noexcept;

    /** @brief 禁止拷贝构造；监听器持有线程、窗口句柄和事件队列，不能安全拷贝。 */
    GB_SystemSessionWatcher(const GB_SystemSessionWatcher&) = delete;

    /** @brief 禁止拷贝赋值；监听器持有线程、窗口句柄和事件队列，不能安全拷贝。 */
    GB_SystemSessionWatcher& operator=(const GB_SystemSessionWatcher&) = delete;

    /**
     * @brief 启动会话事件监听器。
     * @return 启动成功或已经处于运行状态时返回 Succeeded；参数非法、线程创建失败、WTS 注册失败或平台不支持时返回失败结果。
     *
     * @details
     * Windows 下会创建隐藏消息窗口并调用 WTSRegisterSessionNotification 注册会话通知，随后由内部线程完成事件入队、补充查询和分发。
     */
    GB_SystemResult Start();

    /**
     * @brief 停止会话事件监听器。
     * @return 停止成功或本来未运行时返回 Succeeded；停止消息投递失败、事件分发器停止失败等场景返回失败结果。
     *
     * @details
     * Stop 会请求消息线程退出、请求事件工作线程退出，并按当前实现尽量 drain 已分发队列；该函数可以在强类型回调中调用。
     */
    GB_SystemResult Stop();

    /**
     * @brief 判断监听器当前是否处于运行状态。
     * @return 正在运行且没有收到停止请求时返回 true，否则返回 false。
     */
    bool IsRunning() const;

    /**
     * @brief 设置强类型会话事件回调。
     * @param callback 回调函数；传入空 std::function 可清除回调。
     *
     * @details
     * 回调不会在原生窗口消息线程中直接执行，而是通过内部事件分发链路触发；回调中仍应避免长时间阻塞。
     */
    void SetSessionEventCallback(const SessionEventCallback& callback);

    /**
     * @brief 获取内部事件分发器。
     * @return 返回内部 GB_EventDispatcher 引用，调用方可额外订阅原始 GB_Event 事件。
     *
     * @details
     * 分发事件的 payload 可通过 GB_Variant::AnyCast<GB_SystemSessionEvent>() 取回，事件属性中还会包含 eventType、eventTypeName、sessionId、nativeEvent 等信息。
     */
    GB_EventDispatcher& GetEventDispatcher();

    /**
     * @brief 获取已丢弃的原生会话事件数量。
     * @return 返回累计丢弃计数；队列溢出、事件投递失败或事件构建过程发生异常时可能增加。
     */
    uint64_t GetDroppedNativeEventCount() const;

private:
    /** @brief PImpl 实现类声明，用于隐藏 Windows 头文件、线程、窗口句柄和内部队列等实现细节。 */
    class Impl;

    /** @brief 内部实现对象；负责实际的 WTS 注册、消息线程、事件工作线程和事件分发。 */
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_SESSION_H_H
