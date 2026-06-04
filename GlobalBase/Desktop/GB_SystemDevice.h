#ifndef GLOBALBASE_SYSTEM_DEVICE_H_H
#define GLOBALBASE_SYSTEM_DEVICE_H_H

#include "GB_EventDispatcher.h"
#include "GB_SystemResult.h"

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
 * @brief 系统设备粗分类。
 *
 * 说明：
 * - 该枚举用于 GlobalBase 上层模块按能力筛选设备，不替代 Windows 设备安装类或设备接口类；
 * - 一个物理设备可能同时暴露多个接口，也可能在 Windows PnP 树中拆成多个设备节点；
 * - Unknown 表示当前信息不足以可靠归类，上层不应把它视为错误。
 */
enum class GB_SystemDeviceKind : uint16_t
{
    Unknown = 0,
    Usb = 1,
    Display = 2,
    Battery = 3,
    Storage = 4,
    Network = 5,
    HumanInterface = 6,
    Audio = 7,
    Bluetooth = 8,
    Camera = 9,
    Keyboard = 10,
    Mouse = 11,
    Printer = 12
};

/**
 * @brief 系统设备查询条件。
 *
 * 说明：
 * - 所有 std::string 均约定为 UTF-8 编码；
 * - classGuid 使用 Windows GUID 字符串形式，例如 "{4D36E972-E325-11CE-BFC1-08002BE10318}"，比较时不区分大小写；
 * - enumeratorName 是 PnP 枚举器名，例如 "USB"、"PCI"、"BTH"；
 * - deviceKind 为 Unknown 时不按粗分类过滤；
 * - readDriverInfo=true 会额外读取驱动提供商、版本和日期，成本略高，默认关闭。
 */
struct GB_SystemDeviceQueryOptions
{
    bool presentOnly = true;
    std::string classGuid = "";
    std::string enumeratorName = "";
    GB_SystemDeviceKind deviceKind = GB_SystemDeviceKind::Unknown;
    bool readDriverInfo = false;
};

/**
 * @brief 设备接口查询条件。
 *
 * 说明：
 * - 设备接口是用户态可打开或可识别的符号链接路径，例如 HID、卷、磁盘、监视器、摄像头等接口；
 * - interfaceClassGuid 为空且 enumerateAllInstalledInterfaceClasses=true 时，会枚举系统已注册的全部设备接口类；
 * - interfaceClassGuid 非空时只枚举指定接口类；
 * - deviceInstanceId 非空时只返回该设备实例暴露的接口；
 * - presentOnly=true 只返回当前存在且启用的接口。
 */
struct GB_SystemDeviceInterfaceQueryOptions
{
    bool presentOnly = true;
    std::string interfaceClassGuid = "";
    std::string deviceInstanceId = "";
    bool enumerateAllInstalledInterfaceClasses = true;
};

/**
 * @brief 单个系统设备实例信息。
 *
 * 说明：
 * - 这里的“设备”对应 Windows PnP 设备节点，而不是某个具体业务模块中的“蓝牙设备”“音频端点”或“显示屏参数”；
 * - instanceId 是设备实例 ID，可用于稳定关联上层模块需要的底层设备；
 * - classGuid 是设备安装类 GUID，不是 device interface class GUID；
 * - hardwareIds / compatibleIds 可能为空，取决于设备和驱动提供的信息；
 * - devNodeStatus / problemCode 保存 Windows 原生状态位和问题码，便于上层做更精细诊断；
 * - removalPolicy 保存 CM_REMOVAL_POLICY_xxx 原始值，0 表示未能读取；
 * - capabilities 保存 CM_DEVCAP_xxx 位集合，0 表示未能读取或设备未报告能力。
 */
struct GB_SystemDeviceInfo
{
    std::string instanceId = "";
    std::string parentInstanceId = "";
    std::string containerId = "";
    std::string className = "";
    std::string classGuid = "";
    std::string enumeratorName = "";
    std::string friendlyName = "";
    std::string description = "";
    std::string manufacturer = "";
    std::string serviceName = "";
    std::string location = "";
    std::vector<std::string> hardwareIds;
    std::vector<std::string> compatibleIds;
    std::string driverProvider = "";
    std::string driverVersion = "";
    std::string driverDate = "";
    uint32_t devNodeStatus = 0;
    uint32_t problemCode = 0;
    uint32_t capabilities = 0;
    uint32_t removalPolicy = 0;
    bool isPresent = false;
    bool isStarted = false;
    bool hasProblem = false;
    bool isDisabled = false;
    bool isRemovable = false;
    GB_SystemDeviceKind deviceKind = GB_SystemDeviceKind::Unknown;
};

/**
 * @brief 单个系统设备接口信息。
 *
 * 说明：
 * - interfacePath 是 Windows 设备接口符号链接路径，通常可作为 CreateFileW 的目标或业务模块的底层关联键；
 * - interfaceClassGuid 是设备接口类 GUID，不等同于设备安装类 GUID；
 * - deviceInstanceId 是该接口所属的 PnP 设备实例 ID；
 * - associatedDeviceName 是便于日志和 UI 展示的简短名称，优先来自设备友好名或设备描述；
 * - isPresent 表示接口对应的设备当前是否存在；
 * - isEnabled 表示接口当前是否被 SetupAPI 标记为 active/enabled。
 */
struct GB_SystemDeviceInterfaceInfo
{
    std::string interfacePath = "";
    std::string interfaceClassGuid = "";
    std::string deviceInstanceId = "";
    std::string associatedDeviceName = "";
    bool isPresent = false;
    bool isEnabled = false;
};

/**
 * @brief 系统设备事件类型。
 *
 * 说明：
 * - 事件只表达 PnP 设备节点和设备接口变化，不包含电源、屏幕模式、音量、Wi-Fi、蓝牙配对等业务事件；
 * - CM_Register_Notification 可提供更精细的设备实例事件；
 * - 降级到 RegisterDeviceNotification + 隐藏窗口时，部分事件只能退化为 DeviceNodesChanged 或接口到达/移除。
 */
enum class GB_SystemDeviceEventType : uint16_t
{
    Unknown = 0,
    DeviceNodesChanged = 1,
    DeviceInstanceEnumerated = 2,
    DeviceInstanceStarted = 3,
    DeviceInstanceRemoved = 4,
    DeviceInterfaceArrived = 5,
    DeviceInterfaceRemoved = 6,
    DeviceQueryRemove = 7,
    DeviceQueryRemoveFailed = 8,
    DeviceRemovePending = 9,
    DeviceRemoveComplete = 10,
    DeviceCustomEvent = 11
};

/**
 * @brief 系统设备变化事件。
 *
 * 说明：
 * - eventName 形如 "SystemDevice.DeviceInterfaceArrived"，可直接用于 GB_EventDispatcher 订阅；
 * - deviceInstanceId 对设备实例事件有意义；
 * - deviceInterfacePath / interfaceClassGuid 对设备接口事件有意义；
 * - nativeAction 保存 CM_NOTIFY_ACTION 或 DBT_xxx 原始动作值；
 * - nativeMessage / nativeWParam 仅在隐藏窗口降级路径中有意义；
 * - sourceName 标识事件来源，例如 "CM_Register_Notification" 或 "RegisterDeviceNotification"。
 */
struct GB_SystemDeviceEvent
{
    GB_SystemDeviceEventType eventType = GB_SystemDeviceEventType::Unknown;
    std::string eventName = "";
    std::string sourceName = "";
    uint64_t timestampMilliseconds = 0;
    std::string deviceInterfacePath = "";
    std::string deviceInstanceId = "";
    std::string interfaceClassGuid = "";
    uint32_t nativeAction = 0;
    uint32_t nativeMessage = 0;
    uint64_t nativeWParam = 0;
};

/**
 * @brief Windows 系统设备信息查询入口。
 *
 * 说明：
 * - 本类只负责设备枚举、设备元信息读取、设备接口枚举和设备存在性判断；
 * - 不负责操作具体业务设备，例如调音量、连 Wi-Fi、蓝牙配对、屏幕 DPI、截图或文件夹监听；
 * - 所有 std::string 入参和返回字段均约定为 UTF-8 编码；
 * - 非 Windows 平台下接口返回 UnsupportedPlatform。
 */
class GLOBALBASE_PORT GB_SystemDevice final
{
public:
    GB_SystemDevice() = delete;
    ~GB_SystemDevice() = delete;

    static GB_SystemResult GetDevices(std::vector<GB_SystemDeviceInfo>& devices, const GB_SystemDeviceQueryOptions& options = GB_SystemDeviceQueryOptions());
    static GB_SystemResult GetDevicesByKind(GB_SystemDeviceKind deviceKind, std::vector<GB_SystemDeviceInfo>& devices, bool presentOnly = true);
    static GB_SystemResult GetDeviceByInstanceId(const std::string& instanceId, GB_SystemDeviceInfo& deviceInfo, bool& found);
    static GB_SystemResult DeviceExists(const std::string& instanceId, bool& exists);

    static GB_SystemResult GetDeviceInterfaces(std::vector<GB_SystemDeviceInterfaceInfo>& deviceInterfaces, const GB_SystemDeviceInterfaceQueryOptions& options = GB_SystemDeviceInterfaceQueryOptions());
    static GB_SystemResult GetDeviceInterfacesByClassGuid(const std::string& interfaceClassGuid, std::vector<GB_SystemDeviceInterfaceInfo>& deviceInterfaces, bool presentOnly = true);

    static std::string GetDeviceKindName(GB_SystemDeviceKind deviceKind);
    static std::string GetDeviceEventTypeName(GB_SystemDeviceEventType eventType);
};

/**
 * @brief 系统设备变化监听器。
 *
 * 说明：
 * - Windows 8 及以上优先使用 CM_Register_Notification，无需隐藏窗口；
 * - 当编译环境或运行时不支持 CM_Register_Notification 时，自动退回 RegisterDeviceNotification + 隐藏消息窗口；
 * - 系统回调线程只做轻量事件构造和投递，用户回调通过 GB_EventDispatcher 异步分发；
 * - GetEventDispatcher() 暴露通用事件出口，SetDeviceEventCallback() 暴露强类型回调；
 * - Start()/Stop() 可重复调用，析构时会自动停止监听。
 */
class GLOBALBASE_PORT GB_SystemDeviceWatcher final
{
public:
    using DeviceEventCallback = std::function<void(const GB_SystemDeviceEvent& event)>;

    GB_SystemDeviceWatcher();
    ~GB_SystemDeviceWatcher() noexcept;

    GB_SystemDeviceWatcher(const GB_SystemDeviceWatcher&) = delete;
    GB_SystemDeviceWatcher& operator=(const GB_SystemDeviceWatcher&) = delete;

    GB_SystemResult Start();
    GB_SystemResult Stop();
    bool IsRunning() const;

    void SetDeviceEventCallback(const DeviceEventCallback& callback);
    GB_EventDispatcher& GetEventDispatcher();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_DEVICE_H_H
