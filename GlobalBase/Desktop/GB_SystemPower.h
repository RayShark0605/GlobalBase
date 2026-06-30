#ifndef GLOBALBASE_SYSTEM_POWER_H_H
#define GLOBALBASE_SYSTEM_POWER_H_H

#include "../GlobalBasePort.h"
#include "GB_EventDispatcher.h"
#include "GB_SystemResult.h"

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
 * @brief 当前系统电源来源。
 *
 * @remarks
 * - 该枚举是对 Windows SYSTEM_POWER_STATUS::ACLineStatus 以及电源设置通知中电源来源信息的归一化表达。
 * - Unknown 表示系统未能提供可信状态，不应按交流电或电池状态做业务决策。
 */
enum class GB_SystemPowerSource : uint16_t
{
    /** @brief 未知电源来源，通常表示系统无法读取或返回了未知状态。 */
    Unknown = 0,

    /** @brief 交流电源、外接电源或等价的持续供电来源。 */
    AC = 1,

    /** @brief 内置电池供电。 */
    Battery = 2,

    /** @brief 离线或短时供电状态，例如没有交流电且没有可识别系统电池的异常状态。 */
    Offline = 3
};

/**
 * @brief Windows 系统睡眠状态摘要。
 *
 * @remarks
 * - 该枚举对应 Win32 SYSTEM_POWER_STATE 的常用值，用于表达系统能力中的唤醒状态门槛。
 * - 这里的 Hibernate 表示 ACPI S4，Shutdown 表示 ACPI S5。
 */
enum class GB_SystemPowerSleepState : uint16_t
{
    /** @brief 未知或无法映射的系统电源状态。 */
    Unknown = 0,

    /** @brief 系统处于工作状态，通常对应 S0。 */
    Working = 1,

    /** @brief 睡眠状态 S1。 */
    Sleeping1 = 2,

    /** @brief 睡眠状态 S2。 */
    Sleeping2 = 3,

    /** @brief 睡眠状态 S3，传统待机通常使用该状态。 */
    Sleeping3 = 4,

    /** @brief 休眠状态 S4。 */
    Hibernate = 5,

    /** @brief 关机状态 S5。 */
    Shutdown = 6
};

/**
 * @brief 系统电源动作类型。
 *
 * @remarks
 * - 该枚举会映射到 ExitWindowsEx 的 EWX_SHUTDOWN、EWX_POWEROFF、EWX_REBOOT 和 EWX_HYBRID_SHUTDOWN 组合。
 * - 执行动作通常要求当前进程能够启用 SeShutdownPrivilege。
 */
enum class GB_SystemPowerActionType : uint16_t
{
    /** @brief 关闭 Windows 会话并关机，但不强制要求物理断电。 */
    Shutdown = 0,

    /** @brief 关机并在硬件支持时关闭电源。 */
    PowerOff = 1,

    /** @brief 重启系统。 */
    Reboot = 2,

    /** @brief 混合关机，用于支持快速启动的 Windows 版本。 */
    HybridShutdown = 3
};

/**
 * @brief 保持唤醒请求类型。
 *
 * @remarks
 * - 该枚举会映射到 PowerSetRequest 的 POWER_REQUEST_TYPE。
 * - Display 与 SystemAndDisplay 都会同时申请 SystemRequired 和 DisplayRequired，因为 Windows 文档明确要求若要显示器持续点亮且系统不进入睡眠，需要同时申请 SystemRequired。
 * - Execution 主要用于进程执行保活；不同 Windows 版本和不同电源策略下持续时间可能不同。
 */
enum class GB_SystemPowerKeepAwakeMode : uint16_t
{
    /** @brief 保持系统运行，阻止因用户空闲超时而自动睡眠。 */
    System = 0,

    /** @brief 保持显示器点亮，同时保持系统运行，避免系统睡眠导致显示器保活失效。 */
    Display = 1,

    /** @brief 保持系统运行并保持显示器点亮；语义上等价于同时申请 SystemRequired 和 DisplayRequired。 */
    SystemAndDisplay = 2,

    /** @brief Away Mode 请求；仅对传统 S3 睡眠系统有意义，系统表现为类似睡眠但仍继续运行。 */
    AwayMode = 3,

    /** @brief 请求当前调用进程在电源策略允许范围内继续执行；主要用于 Modern Standby/进程生命周期管理场景。 */
    Execution = 4,

    /** @brief 同时请求系统保持运行以及当前调用进程继续执行。 */
    SystemAndExecution = 5
};

/**
 * @brief 电源方案人格分类。
 *
 * @remarks
 * - Windows 内置电源方案通常具有省电、平衡、高性能三类 personality GUID。
 * - 第三方或厂商自定义电源方案可能无法归类，此时返回 Unknown。
 */
enum class GB_SystemPowerPlanPersonality : uint16_t
{
    /** @brief 未知或无法归类的电源方案。 */
    Unknown = 0,

    /** @brief 省电方案。 */
    PowerSaver = 1,

    /** @brief 平衡方案。 */
    Balanced = 2,

    /** @brief 高性能方案。 */
    HighPerformance = 3
};

/**
 * @brief 电源事件类型。
 *
 * @remarks
 * - Suspend/Resume* 来自 WM_POWERBROADCAST 的系统电源广播。
 * - Power*Changed、DisplayStateChanged、UserPresenceChanged 等通常来自 RegisterPowerSettingNotification 注册的电源设置通知。
 */
enum class GB_SystemPowerEventType : uint16_t
{
    /** @brief 未知、未处理或无法映射的电源事件。 */
    Unknown = 0,

    /** @brief 系统即将进入挂起状态。 */
    Suspend = 1,

    /** @brief 系统从低功耗状态自动恢复。 */
    ResumeAutomatic = 2,

    /** @brief 系统从低功耗状态恢复，且恢复由用户输入触发。 */
    ResumeUser = 3,

    /** @brief 系统因关键原因从低功耗状态恢复。 */
    ResumeCritical = 4,

    /** @brief 系统电源状态发生变化，例如 AC/DC 切换、电量阈值变化等。 */
    PowerStatusChanged = 5,

    /** @brief 电源来源发生变化。 */
    PowerSourceChanged = 6,

    /** @brief 电池剩余百分比发生变化。 */
    BatteryPercentageChanged = 7,

    /** @brief 当前活动电源方案发生变化。 */
    ActivePowerPlanChanged = 8,

    /** @brief 显示器、控制台显示或会话显示状态发生变化。 */
    DisplayStateChanged = 9,

    /** @brief 全局或当前会话用户存在状态发生变化。 */
    UserPresenceChanged = 10,

    /** @brief 其他已注册但未进一步细分的电源设置发生变化。 */
    PowerSettingChanged = 11,

    /** @brief Windows 节电模式状态发生变化。 */
    BatterySaverStatusChanged = 12,

    /** @brief 电源方案 personality 发生变化。 */
    PowerPlanPersonalityChanged = 13
};

/**
 * @brief 当前系统电源状态快照。
 *
 * @remarks
 * - Windows 可能无法提供电池百分比、剩余时间或完整续航时间，调用方应先检查对应 has* 字段。
 * - raw* 字段保留 Windows 原始值，便于日志诊断和兼容性判断。
 */
struct GB_SystemPowerStatus
{
    /** @brief 归一化后的当前电源来源。 */
    GB_SystemPowerSource powerSource = GB_SystemPowerSource::Unknown;

    /** @brief 当前是否处于交流电源或外接电源在线状态。 */
    bool isAcOnline = false;

    /** @brief 当前是否处于电池供电状态。 */
    bool isOnBattery = false;

    /** @brief 系统是否报告存在主系统电池。 */
    bool batteryPresent = false;

    /** @brief 电池当前是否处于充电状态。 */
    bool isCharging = false;

    /** @brief 电池电量是否处于严重不足状态。 */
    bool isBatteryCritical = false;

    /** @brief 电池电量是否处于低电量状态；严重不足也会视为低电量。 */
    bool isBatteryLow = false;

    /** @brief Windows 节电模式是否开启。 */
    bool isBatterySaverOn = false;

    /** @brief batteryPercent 字段是否有效。 */
    bool hasBatteryPercent = false;

    /** @brief 电池剩余百分比，范围通常为 0 到 100；仅当 hasBatteryPercent=true 时有效。 */
    uint8_t batteryPercent = 0;

    /** @brief batteryLifeSeconds 字段是否有效。 */
    bool hasBatteryLifeSeconds = false;

    /** @brief 估计剩余续航秒数；仅当 hasBatteryLifeSeconds=true 时有效。 */
    uint32_t batteryLifeSeconds = 0;

    /** @brief batteryFullLifeSeconds 字段是否有效。 */
    bool hasBatteryFullLifeSeconds = false;

    /** @brief 满电时估计续航秒数；仅当 hasBatteryFullLifeSeconds=true 时有效。 */
    uint32_t batteryFullLifeSeconds = 0;

    /** @brief SYSTEM_POWER_STATUS::ACLineStatus 原始值。 */
    uint8_t rawAcLineStatus = 0;

    /** @brief SYSTEM_POWER_STATUS::BatteryFlag 原始值。 */
    uint8_t rawBatteryFlag = 0;

    /** @brief SYSTEM_POWER_STATUS::BatteryLifePercent 原始值。 */
    uint8_t rawBatteryLifePercent = 0;

    /** @brief SYSTEM_POWER_STATUS::SystemStatusFlag 原始值。 */
    uint8_t rawSystemStatusFlag = 0;

    /** @brief 状态采样时刻，单位为毫秒，来源为 GB_EventDispatcher::GetCurrentTimestampMilliseconds()。 */
    uint64_t timestampMilliseconds = 0;
};

/**
 * @brief 系统电源能力摘要。
 *
 * @remarks
 * - 该结构由 GetPwrCapabilities 返回的 SYSTEM_POWER_CAPABILITIES 归一化得到。
 * - supports* 字段表达当前硬件、驱动和系统策略综合报告的能力，可能随驱动、BIOS/UEFI 设置或休眠文件状态变化。
 */
struct GB_SystemPowerCapabilities
{
    /** @brief 系统是否报告存在物理电源按钮。 */
    bool hasPowerButton = false;

    /** @brief 系统是否报告存在物理睡眠按钮。 */
    bool hasSleepButton = false;

    /** @brief 系统是否报告存在合盖开关。 */
    bool hasLidSwitch = false;

    /** @brief 系统是否支持 S1 睡眠状态。 */
    bool supportsS1 = false;

    /** @brief 系统是否支持 S2 睡眠状态。 */
    bool supportsS2 = false;

    /** @brief 系统是否支持 S3 睡眠状态。 */
    bool supportsS3 = false;

    /** @brief 系统是否支持 S4 休眠状态。 */
    bool supportsS4 = false;

    /** @brief 系统是否支持 S5 软关机状态。 */
    bool supportsS5 = false;

    /** @brief 系统是否报告存在可用睡眠能力，包括传统睡眠状态或 Modern Standby/AoAc。 */
    bool supportsSleep = false;

    /** @brief 系统是否报告可用休眠能力；通常要求支持 S4 且休眠文件存在。 */
    bool supportsHibernate = false;

    /** @brief 系统休眠文件是否存在。 */
    bool hasHibernationFile = false;

    /** @brief 系统是否支持 Fast S4。 */
    bool supportsFastS4 = false;

    /** @brief 系统是否支持 hiberboot/快速启动。 */
    bool supportsHiberboot = false;

    /** @brief 系统是否支持唤醒定时器。 */
    bool supportsWakeAlarm = false;

    /** @brief 系统是否支持 Always On Always Connected/Modern Standby 相关能力。 */
    bool supportsAoAc = false;

    /** @brief 系统是否支持 full wake。 */
    bool supportsFullWake = false;

    /** @brief 系统是否支持视频调暗。 */
    bool supportsVideoDimming = false;

    /** @brief 系统是否存在 APM 支持。 */
    bool hasApm = false;

    /** @brief 系统是否报告存在 UPS。 */
    bool hasUps = false;

    /** @brief 系统是否报告存在系统电池。 */
    bool hasBattery = false;

    /** @brief 系统电池是否为短时供电电池。 */
    bool batteriesAreShortTerm = false;

    /** @brief 系统是否支持热控能力。 */
    bool hasThermalControl = false;

    /** @brief 系统是否支持处理器节流。 */
    bool supportsProcessorThrottle = false;

    /** @brief 系统是否支持磁盘降速/停转。 */
    bool supportsDiskSpinDown = false;

    /** @brief 接入交流电源时可唤醒系统的最低电源状态。 */
    GB_SystemPowerSleepState acOnlineWakeState = GB_SystemPowerSleepState::Unknown;

    /** @brief 合盖相关唤醒的最低电源状态。 */
    GB_SystemPowerSleepState lidWakeState = GB_SystemPowerSleepState::Unknown;

    /** @brief RTC 唤醒的最低电源状态。 */
    GB_SystemPowerSleepState rtcWakeState = GB_SystemPowerSleepState::Unknown;

    /** @brief 设备唤醒支持的最低系统电源状态。 */
    GB_SystemPowerSleepState minDeviceWakeState = GB_SystemPowerSleepState::Unknown;

    /** @brief 低延迟唤醒请求默认使用的系统电源状态。 */
    GB_SystemPowerSleepState defaultLowLatencyWakeState = GB_SystemPowerSleepState::Unknown;

    /** @brief 处理器最小节流百分比或系统报告的原始节流下限。 */
    uint8_t processorMinThrottle = 0;

    /** @brief 处理器最大节流百分比或系统报告的原始节流上限。 */
    uint8_t processorMaxThrottle = 0;
};

/**
 * @brief 系统关机、断电关机、重启或混合关机选项。
 */
struct GB_SystemPowerActionOptions
{
    /** @brief 要执行的电源动作类型。 */
    GB_SystemPowerActionType actionType = GB_SystemPowerActionType::Shutdown;

    /** @brief 是否强制关闭应用程序；可能导致应用程序丢失未保存数据，不建议默认开启。 */
    bool forceApplications = false;

    /** @brief 是否仅强制关闭无响应应用程序；不能与 forceApplications 同时开启。 */
    bool forceIfHung = false;

    /** @brief 重启后是否尝试重启已注册应用程序；仅对 Reboot 有效。 */
    bool rebootRegisteredApplications = false;

    /** @brief Windows 关机原因码；为 0 时使用模块默认的计划维护原因码。 */
    uint32_t reasonCode = 0;
};

/**
 * @brief 睡眠或休眠选项。
 */
struct GB_SystemSuspendOptions
{
    /** @brief 保留参数；现代 Windows 中 SetSuspendState 的 bForce 参数无实际效果。 */
    bool force = false;

    /** @brief 是否禁用唤醒事件；true 表示本次睡眠/休眠期间禁用系统唤醒事件。 */
    bool disableWakeEvents = false;

    /** @brief 调用 SetSuspendState 前是否先读取系统能力并做前置校验。 */
    bool checkCapabilityBeforeCall = true;
};

/**
 * @brief 计划关机或计划重启选项。
 */
struct GB_SystemScheduledShutdownOptions
{
    /** @brief 延迟秒数；为 0 时会立即关机且通常无法再通过 AbortScheduledShutdown 取消。 */
    uint32_t delaySeconds = 60;

    /** @brief 展示给用户的计划关机提示消息，UTF-8 编码；为空时使用模块默认提示。 */
    std::string messageUtf8 = "";

    /** @brief 关机完成后是否重启系统。 */
    bool rebootAfterShutdown = false;

    /** @brief 是否强制关闭应用程序；可能导致未保存数据丢失。 */
    bool forceApplications = false;

    /** @brief Windows 关机原因码；为 0 时使用模块默认的计划维护原因码。 */
    uint32_t reasonCode = 0;
};

/**
 * @brief 保持系统唤醒请求选项。
 */
struct GB_SystemPowerKeepAwakeOptions
{
    /** @brief 保持唤醒模式。 */
    GB_SystemPowerKeepAwakeMode mode = GB_SystemPowerKeepAwakeMode::System;

    /** @brief 保持唤醒原因，UTF-8 编码；Windows 会记录该文本用于诊断，不能为空且不能包含 NUL 字符。 */
    std::string reasonUtf8 = "";
};

/**
 * @brief 电源方案摘要。
 */
struct GB_SystemPowerPlanInfo
{
    /** @brief 电源方案 GUID 字符串，格式为带花括号的大写 GUID。 */
    std::string schemeGuid = "";

    /** @brief 电源方案友好名称，UTF-8 编码。 */
    std::string friendlyNameUtf8 = "";

    /** @brief 电源方案描述，UTF-8 编码。 */
    std::string descriptionUtf8 = "";

    /** @brief 当前方案是否为活动电源方案。 */
    bool isActive = false;

    /** @brief 电源方案人格分类。 */
    GB_SystemPowerPlanPersonality personality = GB_SystemPowerPlanPersonality::Unknown;
};

/**
 * @brief 电源事件数据。
 *
 * @remarks
 * - eventName 形如 "SystemPower.PowerSourceChanged"，payload 可通过 GB_Variant::AnyCast<GB_SystemPowerEvent>() 取回。
 * - settingDataUInt32 仅在原始电源设置数据长度恰好为 4 字节时有效。
 * - settingDataGuid 仅在原始电源设置数据长度恰好为 GUID 长度时有效。
 */
struct GB_SystemPowerEvent
{
    /** @brief 归一化后的电源事件类型。 */
    GB_SystemPowerEventType eventType = GB_SystemPowerEventType::Unknown;

    /** @brief 事件名称，通常为 SystemPower. 加事件类型英文名。 */
    std::string eventName = "";

    /** @brief 事件来源名称，例如 WM_POWERBROADCAST 或 RegisterPowerSettingNotification。 */
    std::string sourceName = "";

    /** @brief 事件捕获时间戳，单位为毫秒。 */
    uint64_t timestampMilliseconds = 0;

    /** @brief 事件发生时采样的系统电源状态快照。 */
    GB_SystemPowerStatus powerStatus;

    /** @brief powerStatus 字段是否有效。 */
    bool hasPowerStatus = false;

    /** @brief 原生 Windows 消息 ID，通常为 WM_POWERBROADCAST。 */
    uint32_t nativeMessage = 0;

    /** @brief 原生 Windows wParam 值。 */
    uint64_t nativeWParam = 0;

    /** @brief 原生电源设置 GUID 字符串；仅电源设置通知通常有效。 */
    std::string settingGuid = "";

    /** @brief 原生电源设置通知携带的数据字节。 */
    std::vector<uint8_t> settingData;

    /** @brief settingDataUInt32 字段是否有效。 */
    bool hasSettingDataUInt32 = false;

    /** @brief 当 settingData 长度为 4 字节时解析出的 uint32_t 值。 */
    uint32_t settingDataUInt32 = 0;

    /** @brief settingDataGuid 字段是否有效。 */
    bool hasSettingDataGuid = false;

    /** @brief 当 settingData 长度为 GUID 大小时解析出的 GUID 字符串。 */
    std::string settingDataGuid = "";
};

/**
 * @brief 电源事件监听器选项。
 */
struct GB_SystemPowerWatcherOptions
{
    /** @brief 原生消息线程到事件工作线程之间的最大待处理事件数；达到上限时丢弃最旧事件。 */
    size_t maxPendingNativeEvents = 1024;

    /** @brief GB_EventDispatcher 内部事件队列最大长度；达到上限时由 dispatcher 的溢出策略处理。 */
    size_t maxDispatchQueueSize = 1024;

    /** @brief 构造强类型事件时是否同步采样一份 GB_SystemPowerStatus。 */
    bool capturePowerStatusSnapshot = true;
};

/**
 * @brief 保持系统唤醒请求 RAII 对象。
 *
 * @remarks
 * - 该对象不可复制，只能移动；移动后由新对象负责清理 Windows Power Request。
 * - 析构会自动释放请求；也可提前调用 Release()。
 */
class GLOBALBASE_PORT GB_SystemPowerKeepAwakeRequest final
{
public:
    /** @brief 构造空的保持唤醒请求对象，不持有任何原生句柄。 */
    GB_SystemPowerKeepAwakeRequest();

    /** @brief 析构并自动释放仍处于活动状态的 Power Request。 */
    ~GB_SystemPowerKeepAwakeRequest() noexcept;

    /** @brief 禁止复制构造，避免多个对象重复释放同一个 Power Request 句柄。 */
    GB_SystemPowerKeepAwakeRequest(const GB_SystemPowerKeepAwakeRequest&) = delete;

    /** @brief 禁止复制赋值，避免多个对象重复释放同一个 Power Request 句柄。 */
    GB_SystemPowerKeepAwakeRequest& operator=(const GB_SystemPowerKeepAwakeRequest&) = delete;

    /** @brief 移动构造，接管 other 持有的 Power Request 句柄和请求状态。 */
    GB_SystemPowerKeepAwakeRequest(GB_SystemPowerKeepAwakeRequest&& other) noexcept;

    /** @brief 移动赋值；先释放当前请求，再接管 other 持有的请求。 */
    GB_SystemPowerKeepAwakeRequest& operator=(GB_SystemPowerKeepAwakeRequest&& other) noexcept;

    /** @brief 主动释放当前保持唤醒请求并关闭原生句柄。 */
    GB_SystemResult Release();

    /** @brief 当前对象是否持有有效且尚未释放的保持唤醒请求。 */
    bool IsActive() const;

    /** @brief bool 转换；true 表示当前对象持有有效保持唤醒请求。 */
    explicit operator bool() const;

    /** @brief 获取创建该请求时保存的选项副本。 */
    GB_SystemPowerKeepAwakeOptions GetOptions() const;

private:
    friend class GB_SystemPower;

    /** @brief 从另一个请求对象移动原生句柄和状态；调用后 other 会被清空。 */
    void MoveFrom(GB_SystemPowerKeepAwakeRequest& other) noexcept;

    /** @brief 清空内部句柄、请求标志和保存的选项。 */
    void ClearState() noexcept;

    /** @brief 析构路径使用的无异常释放逻辑；忽略底层 Win32 清理错误。 */
    void ReleaseNoThrow() noexcept;

private:
    /** @brief 原生 Power Request 句柄；Windows 下为 HANDLE，非 Windows 下保持为空。 */
    void* requestHandle = nullptr;

    /** @brief 已成功申请的 Power Request 类型位标记，用于 Release 时逐项清理。 */
    uint32_t appliedRequestFlags = 0;

    /** @brief 创建请求时的选项副本。 */
    GB_SystemPowerKeepAwakeOptions options;
};

/**
 * @brief Windows 系统电源状态、动作、计划关机、保持唤醒和电源方案管理入口。
 *
 * @remarks
 * - 本类是静态工具类，不持有状态。
 * - 关机、重启、睡眠、休眠等动作会尝试启用 SeShutdownPrivilege。
 * - 非 Windows 平台下需要调用 Win32 电源管理能力的接口会返回 UnsupportedPlatform。
 */
class GLOBALBASE_PORT GB_SystemPower final
{
public:
    /** @brief 静态工具类，不允许构造实例。 */
    GB_SystemPower() = delete;

    /** @brief 静态工具类，不允许析构实例。 */
    ~GB_SystemPower() = delete;

    /** @brief 读取当前系统电源状态快照。 */
    static GB_SystemResult GetPowerStatus(GB_SystemPowerStatus& powerStatus);

    /** @brief 读取当前系统电源管理硬件和策略能力摘要。 */
    static GB_SystemResult GetPowerCapabilities(GB_SystemPowerCapabilities& powerCapabilities);

    /** @brief 按指定选项请求关机、断电关机、重启或混合关机。 */
    static GB_SystemResult RequestPowerAction(const GB_SystemPowerActionOptions& options);

    /** @brief 请求系统关机。 */
    static GB_SystemResult Shutdown(bool forceApplications = false);

    /** @brief 请求系统关机并在硬件支持时关闭电源。 */
    static GB_SystemResult PowerOff(bool forceApplications = false);

    /** @brief 请求系统重启。 */
    static GB_SystemResult Reboot(bool forceApplications = false);

    /** @brief 请求系统混合关机，用于支持快速启动的 Windows 版本。 */
    static GB_SystemResult HybridShutdown(bool forceApplications = false);

    /** @brief 请求系统进入睡眠状态。 */
    static GB_SystemResult Sleep(const GB_SystemSuspendOptions& options = GB_SystemSuspendOptions());

    /** @brief 请求系统进入休眠状态。 */
    static GB_SystemResult Hibernate(const GB_SystemSuspendOptions& options = GB_SystemSuspendOptions());

    /** @brief 发起计划关机或计划重启。 */
    static GB_SystemResult ScheduleShutdown(const GB_SystemScheduledShutdownOptions& options);

    /** @brief 取消尚未到期的计划关机或计划重启。 */
    static GB_SystemResult AbortScheduledShutdown();

    /** @brief 创建保持唤醒请求；成功后 request 持有原生 Power Request 句柄。 */
    static GB_SystemResult CreateKeepAwakeRequest(const GB_SystemPowerKeepAwakeOptions& options, GB_SystemPowerKeepAwakeRequest& request);

    /** @brief 枚举当前用户可用的电源方案列表。 */
    static GB_SystemResult EnumeratePowerPlans(std::vector<GB_SystemPowerPlanInfo>& powerPlans);

    /** @brief 读取当前活动电源方案摘要。 */
    static GB_SystemResult GetActivePowerPlan(GB_SystemPowerPlanInfo& powerPlan);

    /** @brief 将指定 GUID 对应的电源方案设置为当前活动方案。 */
    static GB_SystemResult SetActivePowerPlan(const std::string& schemeGuid);

    /**
     * @brief 读取电源方案中指定设置的 AC 或 DC 索引值。
     *
     * @param schemeGuid 电源方案 GUID；为空时读取当前活动电源方案。
     * @param subgroupGuid 电源设置子组 GUID；为空时使用 Windows NO_SUBGROUP_GUID。
     * @param settingGuid 电源设置 GUID。
     * @param readAcValue true 表示读取 AC 值，false 表示读取 DC 值。
     * @param valueIndex 输出设置索引值。
     */
    static GB_SystemResult ReadPowerSettingIndex(const std::string& schemeGuid, const std::string& subgroupGuid, const std::string& settingGuid, bool readAcValue, uint32_t& valueIndex);

    /** @brief 获取电源来源枚举的英文名称。 */
    static std::string GetPowerSourceName(GB_SystemPowerSource powerSource);

    /** @brief 获取睡眠状态枚举的英文名称。 */
    static std::string GetSleepStateName(GB_SystemPowerSleepState sleepState);

    /** @brief 获取电源动作类型枚举的英文名称。 */
    static std::string GetPowerActionTypeName(GB_SystemPowerActionType actionType);

    /** @brief 获取保持唤醒模式枚举的英文名称。 */
    static std::string GetKeepAwakeModeName(GB_SystemPowerKeepAwakeMode keepAwakeMode);

    /** @brief 获取电源方案人格枚举的英文名称。 */
    static std::string GetPowerPlanPersonalityName(GB_SystemPowerPlanPersonality personality);

    /** @brief 获取电源事件类型枚举的英文名称。 */
    static std::string GetPowerEventTypeName(GB_SystemPowerEventType eventType);
};

/**
 * @brief Windows 电源事件监听器。
 *
 * @remarks
 * - Windows 下使用隐藏消息窗口接收 WM_POWERBROADCAST 和 RegisterPowerSettingNotification 通知。
 * - 原生窗口消息线程只复制原生事件，强类型事件构造和用户回调经内部队列与 GB_EventDispatcher 完成。
 * - Start()/Stop() 可重复调用；析构自动停止；可以在强类型回调中调用 Stop()。
 */
class GLOBALBASE_PORT GB_SystemPowerWatcher final
{
public:
    /** @brief 强类型电源事件回调函数类型。 */
    using PowerEventCallback = std::function<void(const GB_SystemPowerEvent& event)>;

    /** @brief 使用默认选项构造电源事件监听器。 */
    GB_SystemPowerWatcher();

    /** @brief 使用指定选项构造电源事件监听器。 */
    explicit GB_SystemPowerWatcher(const GB_SystemPowerWatcherOptions& options);

    /** @brief 析构并自动停止监听器。 */
    ~GB_SystemPowerWatcher() noexcept;

    /** @brief 禁止复制构造，避免隐藏窗口和线程状态被重复管理。 */
    GB_SystemPowerWatcher(const GB_SystemPowerWatcher&) = delete;

    /** @brief 禁止复制赋值，避免隐藏窗口和线程状态被重复管理。 */
    GB_SystemPowerWatcher& operator=(const GB_SystemPowerWatcher&) = delete;

    /** @brief 启动监听器，创建隐藏消息窗口、注册电源设置通知并启动事件分发线程。 */
    GB_SystemResult Start();

    /** @brief 停止监听器，注销电源设置通知、销毁隐藏窗口并停止事件分发线程。 */
    GB_SystemResult Stop();

    /** @brief 当前监听器是否处于运行状态。 */
    bool IsRunning() const;

    /** @brief 设置或替换强类型电源事件回调；传入空回调可取消回调。 */
    void SetPowerEventCallback(const PowerEventCallback& callback);

    /** @brief 获取内部事件分发器，便于订阅通用 GB_Event。 */
    GB_EventDispatcher& GetEventDispatcher();

    /** @brief 获取因队列溢出、事件转换失败或事件投递失败而丢弃的原生事件数量。 */
    uint64_t GetDroppedNativeEventCount() const;

private:
    /** @brief PImpl 实现类型，隐藏 Windows 头文件和线程/窗口细节。 */
    class Impl;

    /** @brief 监听器实现对象。 */
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_POWER_H_H
