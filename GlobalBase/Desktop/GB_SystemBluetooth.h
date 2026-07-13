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
 * - deviceId 对经典蓝牙设备使用标准化 address；对 BLE 设备接口优先使用 PnP deviceInstanceId，缺失时退化为 deviceInterfacePath；
 * - deviceInstanceId 与 deviceInterfacePath 分别表达 Windows PnP 设备实例 ID 和可供 CreateFileW 打开的设备接口路径，不能混为同一概念；
 * - 同一远端经典蓝牙设备可能被多个本机无线电观察到，必要时应结合 radioAddress 区分；
 * - pairStatus、connectionStatus 明确区分“已配对”和“已连接”；
 * - 对 BLE 设备接口枚举结果，单纯的 Win32 PnP 接口无法可靠表达配对、记忆和实时连接状态，因此相关字段保持 Unknown/false，而不是根据“接口存在”进行猜测；
 * - BLE address 仅在设备实例 ID 或接口路径明确包含地址时尝试解析；随机私有地址不应被当作长期稳定身份；
 * - installedServiceGuids 仅在查询选项 includeInstalledServices=true 且系统返回成功时填充；系统报告没有服务或缓存记录不存在时保持为空。
 */
struct GB_BluetoothDeviceInfo
{
    std::string deviceId = "";
    std::string nativeDeviceId = "";
    std::string deviceInstanceId = "";
    std::string deviceInterfacePath = "";
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
 * - pinCodeUtf8 非空时使用 BluetoothAuthenticateDevice 的透明 PIN 配对模式；
 * - pinCodeUtf8 为空时使用 BluetoothAuthenticateDeviceEx 发起系统配对流程，可能弹出系统 UI；
 * - 当前 C++14 实现不承诺超时控制或自定义 SSP 交互。
 */
struct GB_BluetoothPairingOptions
{
    std::string pinCodeUtf8 = "";
    bool allowSystemPairingUi = true;
};

/**
 * @brief 蓝牙监听器选项。
 *
 * 说明：
 * - maxDispatchQueueSize 限制异步事件队列容量，队列满时丢弃最旧事件；
 * - maxDispatchQueueSize=0 会归一化为默认值 64，避免形成无容量限制或无法入队的歧义配置。
 */
struct GB_SystemBluetoothWatcherOptions
{
    size_t maxDispatchQueueSize = 64;
};

/**
 * @brief 蓝牙相关 PnP 设备变化事件。
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
 * @brief BLE GATT 特征值读取缓存策略。
 */
enum class GB_BluetoothGattCacheMode : uint16_t
{
    /** @brief 使用 Windows GATT API 默认策略：优先使用缓存，缓存不存在时访问设备。 */
    Default = 0,

    /** @brief 强制从远端设备读取，并用读取结果更新系统缓存。 */
    ForceReadFromDevice = 1,

    /** @brief 强制只从系统缓存读取。 */
    ForceReadFromCache = 2
};

/**
 * @brief BLE GATT 特征属性位。
 */
enum class GB_BluetoothGattCharacteristicProperty : uint32_t
{
    None = 0,
    Broadcast = 1u << 0,
    Read = 1u << 1,
    Write = 1u << 2,
    WriteWithoutResponse = 1u << 3,
    SignedWrite = 1u << 4,
    Notify = 1u << 5,
    Indicate = 1u << 6,
    ExtendedProperties = 1u << 7
};

/**
 * @brief BLE GATT 服务信息。
 *
 * 说明：
 * - deviceInterfacePath 是 GUID_BLUETOOTHLE_DEVICE_INTERFACE 枚举得到的 BLE 设备接口路径，可直接用于底层 CreateFileW；
 * - uuid 为 "0x180D" 这类 16 位短 UUID 或 "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" 形式 128 位 UUID；
 * - attributeHandle 是 Windows GATT API 返回的服务属性句柄，用于继续枚举特征。
 */
struct GB_BluetoothGattServiceInfo
{
    std::string deviceId = "";
    std::string deviceInterfacePath = "";
    std::string uuid = "";
    uint16_t shortUuid = 0;
    bool isShortUuid = false;
    uint16_t attributeHandle = 0;
};

/**
 * @brief BLE GATT 特征信息。
 *
 * 说明：
 * - 本结构保存从 BluetoothGATTGetCharacteristics 返回的全部关键定位字段和属性；
 * - ReadGattCharacteristic / WriteGattCharacteristic 不会把本结构直接伪装成原生对象，而会重新枚举当前设备缓存，并按服务、特征 UUID 与句柄定位系统返回的未修改原生结构；
 * - 调用方不应手工伪造 attributeHandle / characteristicValueHandle，建议始终使用 GetGattCharacteristics 的返回值。
 */
struct GB_BluetoothGattCharacteristicInfo
{
    std::string deviceId = "";
    std::string deviceInterfacePath = "";
    std::string serviceUuid = "";
    uint16_t serviceShortUuid = 0;
    bool isServiceShortUuid = false;
    uint16_t serviceAttributeHandle = 0;
    std::string characteristicUuid = "";
    uint16_t characteristicShortUuid = 0;
    bool isCharacteristicShortUuid = false;
    uint16_t attributeHandle = 0;
    uint16_t characteristicValueHandle = 0;
    uint32_t propertyFlags = 0;
    bool isBroadcastable = false;
    bool isReadable = false;
    bool isWritable = false;
    bool isWritableWithoutResponse = false;
    bool isSignedWritable = false;
    bool isNotifiable = false;
    bool isIndicatable = false;
    bool hasExtendedProperties = false;
};

/**
 * @brief BLE GATT 特征值读取选项。
 *
 * 说明：
 * - cacheMode 控制读取缓存策略；
 * - requireEncryptedConnection / requireAuthenticatedConnection 会把对应链路安全要求传递给 Windows GATT API；
 * - ForceReadFromDevice 与 ForceReadFromCache 通过枚举互斥表达，避免同时设置两个冲突标志。
 */
struct GB_BluetoothGattReadOptions
{
    GB_BluetoothGattCacheMode cacheMode = GB_BluetoothGattCacheMode::Default;
    bool requireEncryptedConnection = false;
    bool requireAuthenticatedConnection = false;
};

/**
 * @brief BLE GATT 特征值写入选项。
 *
 * 说明：
 * - 普通 writeWithoutResponse 写入要求特征声明 WriteWithoutResponse；
 * - signedWrite=true 时要求特征声明 SignedWrite，并且必须同时设置 writeWithoutResponse=true，但不要求特征另外声明普通 WriteWithoutResponse；
 * - signedWrite 不能同时要求加密或认证链路；
 * - requireEncryptedConnection / requireAuthenticatedConnection 只是向 Windows GATT API 传递链路安全要求，最终是否满足由系统蓝牙栈和远端设备共同决定。
 */
struct GB_BluetoothGattWriteOptions
{
    bool writeWithoutResponse = false;
    bool requireEncryptedConnection = false;
    bool requireAuthenticatedConnection = false;
    bool signedWrite = false;
};

/**
 * @brief Windows 系统蓝牙能力入口。
 *
 * 说明：
 * - 所有 std::string 输入输出均约定为 UTF-8；
 * - 当前实现覆盖 Win32 经典蓝牙无线电、经典蓝牙设备、BLE 设备接口枚举和基础 BLE GATT 读写能力；
 * - BLE GATT 桌面 API 需要 Windows 8 或更高版本；单个 ATT 属性值最大为 512 字节；
 * - BLE 广播扫描、BLE 主动配对、GATT 通知订阅和 RFCOMM Socket 仍应使用 WinRT 或专用通信模块补充；
 * - 本类不会用经典蓝牙枚举伪装 BLE 结果。
 */
class GLOBALBASE_PORT GB_SystemBluetooth final
{
public:
    GB_SystemBluetooth() = delete;
    ~GB_SystemBluetooth() = delete;

    static GB_SystemResult GetRadios(std::vector<GB_BluetoothRadioInfo>& radios);

    /** @brief 获取 BluetoothFindFirstRadio 返回的第一块本机蓝牙无线电；Windows API 不保证它具有独立的“系统默认无线电”语义。 */
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

    /**
     * @brief 枚举当前由 Windows PnP 暴露的 BLE 设备接口。
     *
     * 说明：
     * - 本接口不是主动 BLE 广播扫描，不保证返回附近所有正在广播的 BLE 设备；
     * - 返回项来自 GUID_BLUETOOTHLE_DEVICE_INTERFACE 对应的当前 present 设备接口；
     * - deviceInterfacePath 可继续用于 GetGattServices / GetGattCharacteristics / GATT 读写。
     */
    static GB_SystemResult GetLowEnergyDevices(std::vector<GB_BluetoothDeviceInfo>& devices);

    /**
     * @brief 枚举 BLE 设备的 GATT 服务。
     *
     * 说明：读取类操作会优先以读写权限打开设备接口，以兼容部分桌面蓝牙栈；若失败则自动降级为只读权限。
     */
    static GB_SystemResult GetGattServices(const std::string& deviceInterfacePath, std::vector<GB_BluetoothGattServiceInfo>& services);

    /** @brief 枚举指定 GATT 服务的特征；内部会重新定位系统返回的未修改原生服务结构。 */
    static GB_SystemResult GetGattCharacteristics(const std::string& deviceInterfacePath, const GB_BluetoothGattServiceInfo& service, std::vector<GB_BluetoothGattCharacteristicInfo>& characteristics);

    /** @brief 按指定缓存与链路安全选项读取 BLE GATT 特征值；内部会重新定位当前设备缓存中的原生服务和特征结构。 */
    static GB_SystemResult ReadGattCharacteristic(const std::string& deviceInterfacePath, const GB_BluetoothGattCharacteristicInfo& characteristic, std::vector<uint8_t>& value, const GB_BluetoothGattReadOptions& options = GB_BluetoothGattReadOptions());

    /** @brief 按指定写入模式与链路安全选项写入 BLE GATT 特征值；需要以读写权限打开设备接口，value 最大为 512 字节。 */
    static GB_SystemResult WriteGattCharacteristic(const std::string& deviceInterfacePath, const GB_BluetoothGattCharacteristicInfo& characteristic, const std::vector<uint8_t>& value, const GB_BluetoothGattWriteOptions& options = GB_BluetoothGattWriteOptions());
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
    static bool IsValidGattCacheModeValue(uint64_t cacheModeValue);
    static std::string GetGattCacheModeName(GB_BluetoothGattCacheMode cacheMode);
};

/**
 * @brief 蓝牙相关 PnP 设备变化监听器。
 *
 * 说明：
 * - 当前监听器复用 GB_SystemDeviceWatcher 的 PnP 事件，只转发带有蓝牙语义的设备实例或接口变化；它不表示实时无线链路连接状态；
 * - 回调通过单个 GB_EventDispatcher 异步分发，避免阻塞底层系统设备通知线程，同时避免为同一批蓝牙事件额外维护两条工作线程和两份队列；
 * - GetEventDispatcher() 发布的事件 payload 为 GB_BluetoothEvent，attributes 同时保留常用字段，便于通用事件订阅者使用；内部强类型订阅会在启动和事件投递前自检并在被清除后自动重建；
 * - Start()/Stop() 使用显式生命周期状态串行化转换；并发启动、启动期间停止或重复等待同一次停止会返回 ResourceBusy，停止失败后可再次调用 Stop() 重试清理；
 * - 从蓝牙事件回调内部调用 Stop() 时，事件分发线程不能等待自身退出，因此停止会在当前回调返回后完成；
 * - 它不替代 BLE AdvertisementWatcher 或 WinRT DeviceWatcher，后续 BLE 广播扫描应作为独立能力补充；
 * - 不应在监听回调执行期间销毁当前监听器；如需销毁，应先让回调返回，再从其它线程或外层控制流释放对象。
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

    /** @brief 启动监听；已经运行时幂等成功，生命周期转换进行中或上次停止未清理完成时返回对应失败结果。 */
    GB_SystemResult Start();

    /** @brief 停止监听；首次外部调用会等待已入队回调排空，停止失败时可再次调用本函数重试。 */
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
