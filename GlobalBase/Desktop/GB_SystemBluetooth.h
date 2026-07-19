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
 * - radioAddress 可选，用于多无线电环境下限定查询或配对所使用的本机无线电；为空时由系统或模块自动选择；
 * - 当前 PairDevice / RemoveDevice / IsDeviceConnected 只支持具有 address 的经典蓝牙设备。
 */
struct GB_BluetoothDeviceId
{
    GB_BluetoothDeviceKind deviceKind = GB_BluetoothDeviceKind::Unknown;
    std::string address = "";
    std::string nativeDeviceId = "";
    std::string radioAddress = "";
};

/**
 * @brief 蓝牙设备信息。
 *
 * 说明：
 * - deviceId 对经典蓝牙设备使用标准化 address；对 BLE 设备接口优先使用 PnP deviceInstanceId，缺失时退化为 deviceInterfacePath；
 * - deviceInstanceId 与 deviceInterfacePath 分别表达 Windows PnP 设备实例 ID 和可供 CreateFileW 打开的设备接口路径，不能混为同一概念；
 * - 同一远端经典蓝牙设备可能被多个本机无线电观察到，必要时应结合 radioAddress 区分；
 * - pairStatus、connectionStatus 明确区分“已配对”和“已连接”；
 * - GUID_BLUETOOTHLE_DEVICE_INTERFACE 仅枚举已由 Windows 配对并暴露为设备接口的 BLE 设备，因此 BLE 枚举结果会标记为 Paired/Remembered；
 * - 该接口不提供配对所用的认证方式或链路安全级别，因此 BLE isAuthenticated 保持 false，不把“已配对”误写为“已完成 MITM 认证”；
 * - 单纯的 Win32 PnP 接口仍无法可靠表达实时无线链路连接状态，因此 BLE connectionStatus 保持 Unknown，而不是根据“接口存在”进行猜测；
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
 * @brief 经典蓝牙 SSP 配对认证要求。
 *
 * 说明：
 * - 该枚举映射 Windows AUTHENTICATION_REQUIREMENTS；
 * - 仅在 pinCodeUtf8 为空、使用 BluetoothAuthenticateDeviceEx 时生效；
 * - Required 系列会要求中间人攻击保护，远端设备能力不足时配对会失败。
 */
enum class GB_BluetoothAuthenticationRequirement : uint16_t
{
    MitmProtectionNotRequired = 0,
    MitmProtectionRequired = 1,
    MitmProtectionNotRequiredBonding = 2,
    MitmProtectionRequiredBonding = 3,
    MitmProtectionNotRequiredGeneralBonding = 4,
    MitmProtectionRequiredGeneralBonding = 5
};

/**
 * @brief 经典蓝牙配对选项。
 *
 * 说明：
 * - pinCodeUtf8 非空时使用 BluetoothAuthenticateDevice 的透明 PIN 配对模式，转换后的 PIN 长度不能超过 16 个 UTF-16 字符；内部使用固定长度敏感缓冲区并在退出时清零；
 * - pinCodeUtf8 为空时使用 BluetoothAuthenticateDeviceEx 发起系统配对流程，可能弹出系统 UI；
 * - authenticationRequirement 只用于 BluetoothAuthenticateDeviceEx；
 * - parentWindowHandle 可传入 HWND 的 void* 表达，使系统配对向导归属于指定窗口；
 * - 当前 C++14 实现不承诺超时控制或自定义 SSP 交互。
 */
struct GB_BluetoothPairingOptions
{
    std::string pinCodeUtf8 = "";
    bool allowSystemPairingUi = true;
    GB_BluetoothAuthenticationRequirement authenticationRequirement = GB_BluetoothAuthenticationRequirement::MitmProtectionNotRequiredBonding;
    void* parentWindowHandle = nullptr;
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
 * @brief BLE GATT 会话访问模式。
 */
enum class GB_BluetoothGattSessionAccessMode : uint16_t
{
    /** @brief 只允许服务枚举、特征枚举和读取操作。 */
    ReadOnly = 0,

    /** @brief 允许读取和写入操作；服务枚举允许保留只读服务，写入时会对目标服务严格申请写权限。 */
    ReadWrite = 1
};

/**
 * @brief BLE GATT 服务信息。
 *
 * 说明：
 * - deviceInterfacePath 是 GUID_BLUETOOTHLE_DEVICE_INTERFACE 枚举得到的 BLE 设备接口路径，用于标识所属远端设备；
 * - serviceInterfacePath 是 GUID_BLUETOOTH_GATT_SERVICE_DEVICE_INTERFACE 枚举得到的服务接口路径；特征枚举和特征值读写必须打开该路径，而不能继续复用设备接口句柄；
 * - serviceDeviceInstanceId 是服务接口所属的 Windows PnP 设备实例 ID，用于诊断和设备关联；
 * - uuid 为 "0x180D" 这类 16 位短 UUID 或 "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" 形式 128 位 UUID；
 * - attributeHandle 是 Windows GATT API 返回的服务属性句柄，用于继续枚举特征。
 */
struct GB_BluetoothGattServiceInfo
{
    std::string deviceId = "";
    std::string deviceInterfacePath = "";
    std::string serviceDeviceInstanceId = "";
    std::string serviceInterfacePath = "";
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
 * - 静态 GATT 接口会创建临时会话；GB_BluetoothGattSession 会缓存 Windows GATT API 原样返回的原生服务和特征结构，并按 UUID 与句柄定位；
 * - 调用方不应手工伪造 attributeHandle / characteristicValueHandle，建议始终使用同一会话或静态 GetGattCharacteristics 的返回值。
 */
struct GB_BluetoothGattCharacteristicInfo
{
    std::string deviceId = "";
    std::string deviceInterfacePath = "";
    std::string serviceDeviceInstanceId = "";
    std::string serviceInterfacePath = "";
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
 * @brief 可复用的 BLE GATT 设备会话。
 *
 * 说明：
 * - 会话在 Open() 后持有 BLE 设备接口句柄，并在加载服务缓存时持续持有该设备对应的 GATT 服务接口句柄；
 * - ReadWrite 会话会优先以读写权限打开每个服务接口；不能取得写权限的服务仍会以只读方式加入缓存，只有写入该服务时才会返回权限错误或再次申请写权限；
 * - 服务枚举使用 BLE 设备接口句柄；特征枚举和特征值读写使用对应的 GATT 服务接口句柄，严格遵循 Windows GATT 句柄层级；
 * - Windows 返回主服务但对应服务接口暂时不可打开或无法验证层级时，会跳过该不可访问服务并保留其它已经严格验证的可用服务；只有一个可用服务都无法建立时才返回失败；
 * - UUID 比较遵循 Windows IsBthLEUuidMatch 语义，16 位短 UUID 与等价 Bluetooth Base UUID 形式的 128 位 UUID 可正确匹配；
 * - 连续读写同一设备时，可避免每次操作都重新枚举设备/服务接口、重复 CreateFileW 和重建 GATT 层次缓存；
 * - RefreshCache() 用于服务变更、设备重连或系统缓存变化后重新获取服务层次；
 * - 所有公开方法会串行化同一会话上的访问，同一对象可由多个线程调用，但耗时 GATT 操作仍会互斥执行；
 * - 移动后的源对象为空会话，可再次调用 Open()；
 * - 非 Windows 平台下实际操作返回 UnsupportedPlatform。
 */
class GLOBALBASE_PORT GB_BluetoothGattSession final
{
public:
    GB_BluetoothGattSession();
    ~GB_BluetoothGattSession() noexcept;

    GB_BluetoothGattSession(const GB_BluetoothGattSession&) = delete;
    GB_BluetoothGattSession& operator=(const GB_BluetoothGattSession&) = delete;

    GB_BluetoothGattSession(GB_BluetoothGattSession&& other) noexcept;
    GB_BluetoothGattSession& operator=(GB_BluetoothGattSession&& other) noexcept;

    GB_SystemResult Open(const std::string& deviceInterfacePath, GB_BluetoothGattSessionAccessMode accessMode = GB_BluetoothGattSessionAccessMode::ReadWrite);
    GB_SystemResult Close();
    bool IsOpen() const;

    /** @brief 判断当前会话是否允许执行写操作；该值只表达会话模式，实际写入仍要求目标服务接口可取得写权限且特征声明相应写属性。 */
    bool IsWriteEnabled() const;

    std::string GetDeviceInterfacePath() const;

    GB_SystemResult RefreshCache();
    GB_SystemResult GetServices(std::vector<GB_BluetoothGattServiceInfo>& services);
    GB_SystemResult GetCharacteristics(const GB_BluetoothGattServiceInfo& service, std::vector<GB_BluetoothGattCharacteristicInfo>& characteristics);
    GB_SystemResult ReadCharacteristic(const GB_BluetoothGattCharacteristicInfo& characteristic, std::vector<uint8_t>& value, const GB_BluetoothGattReadOptions& options = GB_BluetoothGattReadOptions());
    GB_SystemResult WriteCharacteristic(const GB_BluetoothGattCharacteristicInfo& characteristic, const std::vector<uint8_t>& value, const GB_BluetoothGattWriteOptions& options = GB_BluetoothGattWriteOptions());

    static bool IsValidAccessModeValue(uint64_t accessModeValue);
    static std::string GetAccessModeName(GB_BluetoothGattSessionAccessMode accessMode);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

/**
 * @brief Windows 系统蓝牙能力入口。
 *
 * 说明：
 * - 所有 std::string 输入输出均约定为 UTF-8；
 * - 当前实现覆盖 Win32 经典蓝牙无线电、经典蓝牙设备、BLE 设备接口枚举和基础 BLE GATT 读写能力；
 * - 静态 GATT 接口适合一次性调用；连续通信应复用 GB_BluetoothGattSession，以避免重复打开设备和重建 GATT 层次缓存；
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
     * - deviceInterfacePath 用于打开 GATT 会话并枚举服务；后续特征枚举和读写会自动使用服务结果中的 serviceInterfacePath。
     */
    static GB_SystemResult GetLowEnergyDevices(std::vector<GB_BluetoothDeviceInfo>& devices);

    /**
     * @brief 枚举 BLE 设备的 GATT 服务。
     *
     * 说明：
     * - deviceInterfacePath 必须来自 GetLowEnergyDevices()；
     * - 内部先通过 BLE 设备接口刷新主服务缓存，再枚举并关联 GUID_BLUETOOTH_GATT_SERVICE_DEVICE_INTERFACE；
     * - 返回的每个服务都已经通过对应服务接口句柄完成层级验证，并包含可用于后续特征枚举和读写的 serviceInterfacePath；
     * - 个别主服务接口暂时不可访问时会跳过该服务并保留其它可用服务；一个可用服务都无法建立时返回失败。
     */
    static GB_SystemResult GetGattServices(const std::string& deviceInterfacePath, std::vector<GB_BluetoothGattServiceInfo>& services);

    /** @brief 枚举指定 GATT 服务的特征；一次性调用使用临时会话，连续操作应复用 GB_BluetoothGattSession。 */
    static GB_SystemResult GetGattCharacteristics(const std::string& deviceInterfacePath, const GB_BluetoothGattServiceInfo& service, std::vector<GB_BluetoothGattCharacteristicInfo>& characteristics);

    /** @brief 按指定缓存与链路安全选项读取 BLE GATT 特征值；一次性调用使用临时会话，连续读取应复用 GB_BluetoothGattSession。 */
    static GB_SystemResult ReadGattCharacteristic(const std::string& deviceInterfacePath, const GB_BluetoothGattCharacteristicInfo& characteristic, std::vector<uint8_t>& value, const GB_BluetoothGattReadOptions& options = GB_BluetoothGattReadOptions());

    /** @brief 按指定写入模式与链路安全选项写入 BLE GATT 特征值；一次性调用使用临时读写会话，value 最大为 512 字节。 */
    static GB_SystemResult WriteGattCharacteristic(const std::string& deviceInterfacePath, const GB_BluetoothGattCharacteristicInfo& characteristic, const std::vector<uint8_t>& value, const GB_BluetoothGattWriteOptions& options = GB_BluetoothGattWriteOptions());

    /**
     * @brief 按地址查找经典蓝牙设备。
     *
     * 说明：
     * - options 中的 includeAuthenticated / includeRemembered / includeUnknown / includeConnected 仍作为返回类别过滤条件；
     * - requestFreshInquiry=false 时优先使用 BluetoothGetDeviceInfo 快速读取系统缓存，但不会绕过上述过滤条件；
     * - found=false 表示未找到符合地址、无线电和返回类别条件的设备，不表示调用失败。
     */
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
    static bool IsValidAuthenticationRequirementValue(uint64_t authenticationRequirementValue);
    static std::string GetAuthenticationRequirementName(GB_BluetoothAuthenticationRequirement authenticationRequirement);
    static bool IsValidGattCacheModeValue(uint64_t cacheModeValue);
    static std::string GetGattCacheModeName(GB_BluetoothGattCacheMode cacheMode);
};

/**
 * @brief 蓝牙相关 PnP 设备变化监听器。
 *
 * 说明：
 * - 当前监听器复用 GB_SystemDeviceWatcher 的 PnP 事件，只转发带有蓝牙语义的设备实例或接口变化；它不表示实时无线链路连接状态；
 * - 回调通过单个 GB_EventDispatcher 异步分发，避免阻塞底层系统设备通知线程，同时避免为同一批蓝牙事件额外维护两条工作线程和两份队列；
 * - SetBluetoothEventCallback() 提供单个强类型回调，Subscribe()/SubscribeAll() 提供可并存的强类型订阅；模块不再暴露可被外部任意启动、停止或清空的原始事件分发器；
 * - Start()/Stop() 使用显式生命周期状态串行化转换；并发启动、启动期间停止或重复等待同一次停止会返回 ResourceBusy，停止失败后可再次调用 Stop() 重试清理；
 * - 所有公开回调均经过统一执行作用域保护；从任意蓝牙事件回调内部调用 Stop() 时会返回 InvalidState，避免事件分发线程等待或分离自身；
 * - ClearSubscriptions() 只清除 Subscribe()/SubscribeAll() 创建的外部订阅，不影响 SetBluetoothEventCallback() 的单回调通道；
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

    /** @brief 设置或清除单个蓝牙事件回调；传入空回调表示清除。 */
    void SetBluetoothEventCallback(const BluetoothEventCallback& callback);

    /** @brief 订阅指定类型的蓝牙事件；Unknown 和非法枚举值会返回 InvalidArgument。 */
    GB_SystemResult Subscribe(GB_BluetoothEventType eventType, const BluetoothEventCallback& callback, GB_EventSubscriptionToken& subscriptionToken);

    /** @brief 订阅全部蓝牙事件。 */
    GB_SystemResult SubscribeAll(const BluetoothEventCallback& callback, GB_EventSubscriptionToken& subscriptionToken);

    /** @brief 取消由当前监听器 Subscribe()/SubscribeAll() 创建的订阅。 */
    GB_SystemResult Unsubscribe(const GB_EventSubscriptionToken& subscriptionToken);

    /** @brief 清除由 Subscribe()/SubscribeAll() 创建的全部外部订阅，不影响 SetBluetoothEventCallback()。 */
    GB_SystemResult ClearSubscriptions();

    /** @brief 获取当前外部强类型订阅数量，不包含 SetBluetoothEventCallback() 通道。 */
    size_t GetSubscriptionCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_BLUETOOTH_H_H
