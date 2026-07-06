#ifndef GLOBALBASE_SYSTEM_BLUETOOTH_H_H
#define GLOBALBASE_SYSTEM_BLUETOOTH_H_H

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
 * @brief 蓝牙设备类型。
 *
 * 说明：
 * - Classic 表示通过 Win32 Bluetooth API 可见的 BR/EDR 经典蓝牙设备；
 * - LowEnergy 表示 BLE 设备，完整枚举需要 Windows Runtime Bluetooth API；
 * - DualMode 表示同一物理设备同时暴露经典蓝牙和 BLE 能力；
 * - Unknown 表示当前系统返回的信息不足以可靠归类。
 */
enum class GB_BluetoothDeviceKind : uint16_t
{
    Unknown = 0,
    Classic = 1,
    LowEnergy = 2,
    DualMode = 3
};

/**
 * @brief 蓝牙配对状态。
 */
enum class GB_BluetoothPairStatus : uint16_t
{
    Unknown = 0,
    Unpaired = 1,
    Paired = 2,
    CannotPair = 3
};

/**
 * @brief 蓝牙连接状态。
 */
enum class GB_BluetoothConnectionStatus : uint16_t
{
    Unknown = 0,
    Disconnected = 1,
    Connected = 2
};

/**
 * @brief 蓝牙监听事件类型。
 */
enum class GB_BluetoothEventType : uint16_t
{
    Unknown = 0,
    DeviceAdded = 1,
    DeviceUpdated = 2,
    DeviceRemoved = 3,
    RadioChanged = 4
};

/**
 * @brief 本机蓝牙无线电 / 适配器信息。
 *
 * 说明：
 * - radioId 当前使用蓝牙地址作为稳定 ID；没有地址时为空；
 * - address 为标准冒号分隔十六进制文本，例如 "AA:BB:CC:DD:EE:FF"；
 * - Win32 Bluetooth API 不提供可靠的统一蓝牙电源开关状态，isConnectable / isDiscoverable 仅表示当前可连接/可发现属性；
 * - SetRadioConnectable / SetRadioDiscoverable 控制的是入站连接和可发现性，不等价于启用或禁用蓝牙适配器电源；
 * - isLowEnergySupported 当前为保守默认值，完整 BLE 能力探测需要 WinRT BluetoothAdapter。
 */
struct GB_BluetoothRadioInfo
{
    std::string radioId = "";
    std::string name = "";
    std::string address = "";
    uint32_t classOfDevice = 0;
    uint16_t manufacturer = 0;
    bool isClassicSupported = true;
    bool isLowEnergySupported = false;
    bool isConnectable = false;
    bool isDiscoverable = false;
    std::string nativeDeviceId = "";
};

/**
 * @brief 蓝牙设备 ID。
 *
 * 说明：
 * - address 用于经典蓝牙设备，接受 12 位十六进制、AA:BB:CC:DD:EE:FF 或 AA-BB-CC-DD-EE-FF 形式，允许首尾 ASCII 空白，不接受内嵌空白或混合分隔符，内部会归一化；
 * - nativeDeviceId 预留给 WinRT DeviceInformation ID，不要求等价于 MAC 地址；
 * - 当前 PairDevice / RemoveDevice / IsDeviceConnected 只支持具有 address 的经典蓝牙设备。
 */
struct GB_BluetoothDeviceId
{
    GB_BluetoothDeviceKind deviceKind = GB_BluetoothDeviceKind::Unknown;
    std::string address = "";
    std::string nativeDeviceId = "";
};

/**
 * @brief 蓝牙设备信息。
 *
 * 说明：
 * - deviceId 当前对经典蓝牙设备使用标准化 address；同一远端设备可能被多个本机无线电观察到，必要时应结合 radioAddress 区分；后续 BLE/WinRT 实现可使用 DeviceInformation ID；
 * - pairStatus、connectionStatus 明确区分“已配对”和“已连接”；
 * - installedServiceGuids 仅在查询选项 includeInstalledServices=true 且系统返回成功时填充；系统报告没有服务或缓存记录不存在时保持为空。
 */
struct GB_BluetoothDeviceInfo
{
    std::string deviceId = "";
    std::string nativeDeviceId = "";
    std::string radioId = "";
    std::string radioAddress = "";
    std::string address = "";
    std::string name = "";
    GB_BluetoothDeviceKind deviceKind = GB_BluetoothDeviceKind::Unknown;
    GB_BluetoothPairStatus pairStatus = GB_BluetoothPairStatus::Unknown;
    GB_BluetoothConnectionStatus connectionStatus = GB_BluetoothConnectionStatus::Unknown;
    bool isRemembered = false;
    bool isAuthenticated = false;
    bool isConnected = false;
    bool isClassicSupported = false;
    bool isLowEnergySupported = false;
    uint32_t classOfDevice = 0;
    uint32_t serviceClass = 0;
    uint32_t majorDeviceClass = 0;
    uint32_t minorDeviceClass = 0;
    std::string lastSeenTimeLocal = "";
    std::string lastUsedTimeLocal = "";
    std::vector<std::string> installedServiceGuids;
    std::string sourceName = "";
};

/**
 * @brief 经典蓝牙设备查询选项。
 *
 * 说明：
 * - requestFreshInquiry=true 会触发经典蓝牙 Inquiry，可能持续数秒；
 * - inquiryTimeoutMultiplier 单位为 1.28 秒，Windows API 最大有效值为 48；
 * - includeUnknown=true 才返回本机未记住/未配对但 Inquiry 发现的设备；
 * - radioAddress 非空时只查询指定本机蓝牙无线电；多无线电环境下，同一远端地址可能返回多条不同 radioAddress 的记录。
 */
struct GB_BluetoothClassicDeviceQueryOptions
{
    bool includeAuthenticated = true;
    bool includeRemembered = true;
    bool includeUnknown = false;
    bool includeConnected = true;
    bool requestFreshInquiry = false;
    uint8_t inquiryTimeoutMultiplier = 4;
    bool includeInstalledServices = false;
    std::string radioAddress = "";
};

/**
 * @brief 经典蓝牙配对选项。
 *
 * 说明：
 * - 当前实现使用 BluetoothAuthenticateDeviceEx 发起基础系统配对，可能弹出系统配对 UI；
 * - pinCodeUtf8 预留给后续自定义认证回调实现，当前非空时会返回 UnsupportedPlatform；
 * - 当前 C++14 实现不承诺超时、PIN 输入或自定义 SSP 交互。
 */
struct GB_BluetoothPairingOptions
{
    std::string pinCodeUtf8 = "";
    bool allowSystemPairingUi = true;
};

/**
 * @brief 蓝牙监听器选项。
 */
struct GB_SystemBluetoothWatcherOptions
{
    size_t maxDispatchQueueSize = 64;
};

/**
 * @brief 蓝牙状态变化事件。
 */
struct GB_BluetoothEvent
{
    GB_BluetoothEventType eventType = GB_BluetoothEventType::Unknown;
    std::string eventName = "";
    std::string sourceName = "";
    uint64_t timestampMilliseconds = 0;
    std::string deviceInstanceId = "";
    std::string deviceInterfacePath = "";
    std::string interfaceClassGuid = "";
    uint32_t nativeAction = 0;
};

/**
 * @brief Windows 系统蓝牙能力入口。
 *
 * 说明：
 * - 所有 std::string 输入输出均约定为 UTF-8；
 * - 当前实现覆盖 Win32 经典蓝牙无线电和设备能力；
 * - BLE / GATT / RFCOMM 完整能力需要 WinRT ABI 或隔离的 C++17 实现单元，本类不会用经典蓝牙枚举伪装 BLE 结果。
 */
class GLOBALBASE_PORT GB_SystemBluetooth final
{
public:
    GB_SystemBluetooth() = delete;
    ~GB_SystemBluetooth() = delete;

    static GB_SystemResult GetRadios(std::vector<GB_BluetoothRadioInfo>& radios);
    static GB_SystemResult GetDefaultRadio(GB_BluetoothRadioInfo& radio, bool& found);
    static GB_SystemResult IsBluetoothAvailable(bool& available);

    /**
     * @brief 设置本机蓝牙无线电是否接受入站连接。
     *
     * 说明：
     * - radioAddress 为空时作用于当前发现的全部本机蓝牙无线电；非空时只作用于指定地址的无线电；
     * - enabled=false 时会先关闭可发现性，再关闭入站连接，以满足 Windows Bluetooth API 的状态前置条件；
     * - 本接口不等价于系统蓝牙总开关，不会启用或禁用蓝牙适配器设备。
     */
    static GB_SystemResult SetRadioConnectable(const std::string& radioAddress, bool enabled);

    /**
     * @brief 设置本机蓝牙无线电是否可被其他设备发现。
     *
     * 说明：
     * - radioAddress 为空时作用于当前发现的全部本机蓝牙无线电；非空时只作用于指定地址的无线电；
     * - enabled=true 时会先打开入站连接，再打开可发现性，以满足 Windows Bluetooth API 的状态前置条件；
     * - Windows 对可发现性的改变只保证在调用进程生命周期内有效，进程结束后系统会恢复原状态。
     */
    static GB_SystemResult SetRadioDiscoverable(const std::string& radioAddress, bool enabled);

    static GB_SystemResult GetClassicDevices(std::vector<GB_BluetoothDeviceInfo>& devices, const GB_BluetoothClassicDeviceQueryOptions& options = GB_BluetoothClassicDeviceQueryOptions());
    static GB_SystemResult GetLowEnergyDevices(std::vector<GB_BluetoothDeviceInfo>& devices);
    static GB_SystemResult GetClassicDeviceByAddress(const std::string& address, GB_BluetoothDeviceInfo& device, bool& found, const GB_BluetoothClassicDeviceQueryOptions& options = GB_BluetoothClassicDeviceQueryOptions());

    static GB_SystemResult IsDeviceConnected(const GB_BluetoothDeviceId& deviceId, bool& connected);
    static GB_SystemResult PairDevice(const GB_BluetoothDeviceId& deviceId, const GB_BluetoothPairingOptions& options = GB_BluetoothPairingOptions());
    static GB_SystemResult RemoveDevice(const GB_BluetoothDeviceId& deviceId);

    static bool IsValidAddress(const std::string& address);
    static std::string NormalizeAddress(const std::string& address);
    static std::string GetDeviceKindName(GB_BluetoothDeviceKind deviceKind);
    static std::string GetPairStatusName(GB_BluetoothPairStatus pairStatus);
    static std::string GetConnectionStatusName(GB_BluetoothConnectionStatus connectionStatus);
    static std::string GetEventTypeName(GB_BluetoothEventType eventType);
};

/**
 * @brief 蓝牙设备变化监听器。
 *
 * 说明：
 * - 当前监听器复用 GB_SystemDeviceWatcher 的 PnP 事件，只转发带有蓝牙语义的设备实例或接口变化；
 * - 回调通过 GB_EventDispatcher 异步分发，避免阻塞底层系统设备通知线程；
 * - 它不替代 BLE AdvertisementWatcher 或 WinRT DeviceWatcher，后续 BLE 扫描应作为独立能力补充。
 */
class GLOBALBASE_PORT GB_SystemBluetoothWatcher final
{
public:
    using BluetoothEventCallback = std::function<void(const GB_BluetoothEvent& event)>;

    GB_SystemBluetoothWatcher();
    explicit GB_SystemBluetoothWatcher(const GB_SystemBluetoothWatcherOptions& options);
    ~GB_SystemBluetoothWatcher() noexcept;

    GB_SystemBluetoothWatcher(const GB_SystemBluetoothWatcher&) = delete;
    GB_SystemBluetoothWatcher& operator=(const GB_SystemBluetoothWatcher&) = delete;

    GB_SystemResult Start();
    GB_SystemResult Stop();
    bool IsRunning() const;

    void SetBluetoothEventCallback(const BluetoothEventCallback& callback);
    GB_EventDispatcher& GetEventDispatcher();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_BLUETOOTH_H_H
