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
 */
enum class GB_SystemPowerSource : uint16_t
{
    Unknown = 0,
    AC = 1,
    Battery = 2,
    Offline = 3
};

/**
 * @brief Windows 系统睡眠状态摘要。
 */
enum class GB_SystemPowerSleepState : uint16_t
{
    Unknown = 0,
    Working = 1,
    Sleeping1 = 2,
    Sleeping2 = 3,
    Sleeping3 = 4,
    Hibernate = 5,
    Shutdown = 6
};

/**
 * @brief 系统电源动作类型。
 */
enum class GB_SystemPowerActionType : uint16_t
{
    Shutdown = 0,
    PowerOff = 1,
    Reboot = 2,
    HybridShutdown = 3
};

/**
 * @brief 保持唤醒请求类型。
 */
enum class GB_SystemPowerKeepAwakeMode : uint16_t
{
    System = 0,
    Display = 1,
    SystemAndDisplay = 2,
    AwayMode = 3
};

/**
 * @brief 电源方案人格分类。
 */
enum class GB_SystemPowerPlanPersonality : uint16_t
{
    Unknown = 0,
    PowerSaver = 1,
    Balanced = 2,
    HighPerformance = 3
};

/**
 * @brief 电源事件类型。
 */
enum class GB_SystemPowerEventType : uint16_t
{
    Unknown = 0,
    Suspend = 1,
    ResumeAutomatic = 2,
    ResumeUser = 3,
    ResumeCritical = 4,
    PowerStatusChanged = 5,
    PowerSourceChanged = 6,
    BatteryPercentageChanged = 7,
    ActivePowerPlanChanged = 8,
    DisplayStateChanged = 9,
    UserPresenceChanged = 10,
    PowerSettingChanged = 11,
    BatterySaverStatusChanged = 12,
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
    GB_SystemPowerSource powerSource = GB_SystemPowerSource::Unknown;
    bool isAcOnline = false;
    bool isOnBattery = false;
    bool batteryPresent = false;
    bool isCharging = false;
    bool isBatteryCritical = false;
    bool isBatteryLow = false;
    bool isBatterySaverOn = false;
    bool hasBatteryPercent = false;
    uint8_t batteryPercent = 0;
    bool hasBatteryLifeSeconds = false;
    uint32_t batteryLifeSeconds = 0;
    bool hasBatteryFullLifeSeconds = false;
    uint32_t batteryFullLifeSeconds = 0;
    uint8_t rawAcLineStatus = 0;
    uint8_t rawBatteryFlag = 0;
    uint8_t rawBatteryLifePercent = 0;
    uint8_t rawSystemStatusFlag = 0;
    uint64_t timestampMilliseconds = 0;
};

/**
 * @brief 系统电源能力摘要。
 */
struct GB_SystemPowerCapabilities
{
    bool hasPowerButton = false;
    bool hasSleepButton = false;
    bool hasLidSwitch = false;
    bool supportsS1 = false;
    bool supportsS2 = false;
    bool supportsS3 = false;
    bool supportsS4 = false;
    bool supportsS5 = false;
    bool supportsSleep = false;
    bool supportsHibernate = false;
    bool hasHibernationFile = false;
    bool supportsFastS4 = false;
    bool supportsHiberboot = false;
    bool supportsWakeAlarm = false;
    bool supportsAoAc = false;
    bool supportsFullWake = false;
    bool supportsVideoDimming = false;
    bool hasApm = false;
    bool hasUps = false;
    bool hasBattery = false;
    bool batteriesAreShortTerm = false;
    bool hasThermalControl = false;
    bool supportsProcessorThrottle = false;
    bool supportsDiskSpinDown = false;
    GB_SystemPowerSleepState acOnlineWakeState = GB_SystemPowerSleepState::Unknown;
    GB_SystemPowerSleepState lidWakeState = GB_SystemPowerSleepState::Unknown;
    GB_SystemPowerSleepState rtcWakeState = GB_SystemPowerSleepState::Unknown;
    GB_SystemPowerSleepState minDeviceWakeState = GB_SystemPowerSleepState::Unknown;
    GB_SystemPowerSleepState defaultLowLatencyWakeState = GB_SystemPowerSleepState::Unknown;
    uint8_t processorMinThrottle = 0;
    uint8_t processorMaxThrottle = 0;
};

/**
 * @brief 系统关机、断电关机、重启或混合关机选项。
 */
struct GB_SystemPowerActionOptions
{
    GB_SystemPowerActionType actionType = GB_SystemPowerActionType::Shutdown;
    bool forceApplications = false;
    bool forceIfHung = false;
    bool rebootRegisteredApplications = false;
    uint32_t reasonCode = 0;
};

/**
 * @brief 睡眠或休眠选项。
 */
struct GB_SystemSuspendOptions
{
    bool force = false;
    bool disableWakeEvents = false;
    bool checkCapabilityBeforeCall = true;
};

/**
 * @brief 计划关机或计划重启选项。
 */
struct GB_SystemScheduledShutdownOptions
{
    uint32_t delaySeconds = 60;
    std::string messageUtf8 = "";
    bool rebootAfterShutdown = false;
    bool forceApplications = false;
    uint32_t reasonCode = 0;
};

/**
 * @brief 保持系统唤醒请求选项。
 */
struct GB_SystemPowerKeepAwakeOptions
{
    GB_SystemPowerKeepAwakeMode mode = GB_SystemPowerKeepAwakeMode::System;
    std::string reasonUtf8 = "";
};

/**
 * @brief 电源方案摘要。
 */
struct GB_SystemPowerPlanInfo
{
    std::string schemeGuid = "";
    std::string friendlyNameUtf8 = "";
    std::string descriptionUtf8 = "";
    bool isActive = false;
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
    GB_SystemPowerEventType eventType = GB_SystemPowerEventType::Unknown;
    std::string eventName = "";
    std::string sourceName = "";
    uint64_t timestampMilliseconds = 0;
    GB_SystemPowerStatus powerStatus;
    bool hasPowerStatus = false;
    uint32_t nativeMessage = 0;
    uint64_t nativeWParam = 0;
    std::string settingGuid = "";
    std::vector<uint8_t> settingData;
    bool hasSettingDataUInt32 = false;
    uint32_t settingDataUInt32 = 0;
    bool hasSettingDataGuid = false;
    std::string settingDataGuid = "";
};

/**
 * @brief 电源事件监听器选项。
 */
struct GB_SystemPowerWatcherOptions
{
    size_t maxPendingNativeEvents = 1024;
    size_t maxDispatchQueueSize = 1024;
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
    GB_SystemPowerKeepAwakeRequest();
    ~GB_SystemPowerKeepAwakeRequest() noexcept;

    GB_SystemPowerKeepAwakeRequest(const GB_SystemPowerKeepAwakeRequest&) = delete;
    GB_SystemPowerKeepAwakeRequest& operator=(const GB_SystemPowerKeepAwakeRequest&) = delete;

    GB_SystemPowerKeepAwakeRequest(GB_SystemPowerKeepAwakeRequest&& other) noexcept;
    GB_SystemPowerKeepAwakeRequest& operator=(GB_SystemPowerKeepAwakeRequest&& other) noexcept;

    GB_SystemResult Release();
    bool IsActive() const;
    explicit operator bool() const;
    GB_SystemPowerKeepAwakeOptions GetOptions() const;

private:
    friend class GB_SystemPower;
    void MoveFrom(GB_SystemPowerKeepAwakeRequest& other) noexcept;
    void ClearState() noexcept;
    void ReleaseNoThrow() noexcept;

private:
    void* requestHandle = nullptr;
    uint32_t appliedRequestFlags = 0;
    GB_SystemPowerKeepAwakeOptions options;
};

/**
 * @brief Windows 系统电源状态、动作、计划关机、保持唤醒和电源方案管理入口。
 */
class GLOBALBASE_PORT GB_SystemPower final
{
public:
    GB_SystemPower() = delete;
    ~GB_SystemPower() = delete;

    static GB_SystemResult GetPowerStatus(GB_SystemPowerStatus& powerStatus);
    static GB_SystemResult GetPowerCapabilities(GB_SystemPowerCapabilities& powerCapabilities);

    static GB_SystemResult RequestPowerAction(const GB_SystemPowerActionOptions& options);
    static GB_SystemResult Shutdown(bool forceApplications = false);
    static GB_SystemResult PowerOff(bool forceApplications = false);
    static GB_SystemResult Reboot(bool forceApplications = false);
    static GB_SystemResult HybridShutdown(bool forceApplications = false);

    static GB_SystemResult Sleep(const GB_SystemSuspendOptions& options = GB_SystemSuspendOptions());
    static GB_SystemResult Hibernate(const GB_SystemSuspendOptions& options = GB_SystemSuspendOptions());

    static GB_SystemResult ScheduleShutdown(const GB_SystemScheduledShutdownOptions& options);
    static GB_SystemResult AbortScheduledShutdown();

    static GB_SystemResult CreateKeepAwakeRequest(const GB_SystemPowerKeepAwakeOptions& options, GB_SystemPowerKeepAwakeRequest& request);

    static GB_SystemResult EnumeratePowerPlans(std::vector<GB_SystemPowerPlanInfo>& powerPlans);
    static GB_SystemResult GetActivePowerPlan(GB_SystemPowerPlanInfo& powerPlan);
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

    static std::string GetPowerSourceName(GB_SystemPowerSource powerSource);
    static std::string GetSleepStateName(GB_SystemPowerSleepState sleepState);
    static std::string GetPowerActionTypeName(GB_SystemPowerActionType actionType);
    static std::string GetKeepAwakeModeName(GB_SystemPowerKeepAwakeMode keepAwakeMode);
    static std::string GetPowerPlanPersonalityName(GB_SystemPowerPlanPersonality personality);
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
    using PowerEventCallback = std::function<void(const GB_SystemPowerEvent& event)>;

    GB_SystemPowerWatcher();
    explicit GB_SystemPowerWatcher(const GB_SystemPowerWatcherOptions& options);
    ~GB_SystemPowerWatcher() noexcept;

    GB_SystemPowerWatcher(const GB_SystemPowerWatcher&) = delete;
    GB_SystemPowerWatcher& operator=(const GB_SystemPowerWatcher&) = delete;

    GB_SystemResult Start();
    GB_SystemResult Stop();
    bool IsRunning() const;

    void SetPowerEventCallback(const PowerEventCallback& callback);
    GB_EventDispatcher& GetEventDispatcher();
    uint64_t GetDroppedNativeEventCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_POWER_H_H
