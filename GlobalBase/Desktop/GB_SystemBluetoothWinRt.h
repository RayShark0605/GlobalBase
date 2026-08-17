#ifndef GLOBALBASE_SYSTEM_BLUETOOTH_WINRT_H_H
#define GLOBALBASE_SYSTEM_BLUETOOTH_WINRT_H_H

#include "../GlobalBasePort.h"
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
 * @brief BLE 广播地址类型。
 */
enum class GB_BluetoothLeAddressType : uint16_t
{
    Unknown = 0,
    Public = 1,
    Random = 2
};

/**
 * @brief 一次 BLE 广播观察结果。
 *
 * 所有字符串均为 UTF-8。bluetoothAddress 保存 Windows Runtime 返回的 48 位地址值；
 * 对随机地址，调用方不得把该值当作跨会话永久身份。
 */
struct GB_BluetoothLeAdvertisementInfo
{
    uint64_t bluetoothAddress = 0;
    GB_BluetoothLeAddressType addressType = GB_BluetoothLeAddressType::Unknown;
    std::string address = "";
    std::string localName = "";
    int16_t rawSignalStrengthDbm = 0;
    bool isConnectable = false;
    std::vector<std::string> serviceUuids;
};

/**
 * @brief BLE 广播监听选项。
 *
 * maxTrackedDevices=0 表示监听器本身不限制去重集合容量；调用方仍应在业务层设置结果上限。
 */
struct GB_BluetoothLeAdvertisementWatcherOptions
{
    bool activeScanning = true;
    bool allowDuplicateAddresses = true;
    size_t maxTrackedDevices = 0;
};

/**
 * @brief 基于 Windows Runtime BluetoothLEAdvertisementWatcher 的通用 BLE 广播监听器。
 *
 * 实现持有独立的 MTA 工作线程，公开头文件不暴露任何 C++/WinRT 类型。
 */
class GLOBALBASE_PORT GB_BluetoothLeAdvertisementWatcher final
{
public:
    using AdvertisementCallback = std::function<void(const GB_BluetoothLeAdvertisementInfo& advertisement)>;

    GB_BluetoothLeAdvertisementWatcher();
    explicit GB_BluetoothLeAdvertisementWatcher(const GB_BluetoothLeAdvertisementWatcherOptions& options);
    ~GB_BluetoothLeAdvertisementWatcher() noexcept;

    GB_BluetoothLeAdvertisementWatcher(const GB_BluetoothLeAdvertisementWatcher&) = delete;
    GB_BluetoothLeAdvertisementWatcher& operator=(const GB_BluetoothLeAdvertisementWatcher&) = delete;

    bool IsValid() const;
    GB_SystemResult Start();
    GB_SystemResult Stop();
    bool IsRunning() const;
    void SetAdvertisementCallback(const AdvertisementCallback& callback);
    std::vector<GB_BluetoothLeAdvertisementInfo> GetTrackedDevices() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

/**
 * @brief BLE GATT 写入模式。
 */
enum class GB_BluetoothLeGattWriteMode : uint16_t
{
    Unknown = 0,
    WithResponse = 1,
    WithoutResponse = 2
};

/**
 * @brief 由调用方选择的 GATT 服务、通知特征和写特征。
 */
struct GB_BluetoothLeGattProfile
{
    std::string serviceUuid = "";
    std::string notifyCharacteristicUuid = "";
    std::string writeCharacteristicUuid = "";
};

/**
 * @brief GATT 客户端超时选项；两个值都必须由调用方提供为非零毫秒数。
 */
struct GB_BluetoothLeGattClientOptions
{
    uint32_t connectTimeoutMilliseconds = 0;
    uint32_t operationTimeoutMilliseconds = 0;
};

/**
 * @brief 使用 C++/WinRT 的 BLE GATT 客户端。
 *
 * Connect() 只有在服务和特征发现、属性校验以及 CCCD 写入全部成功后才返回成功。
 * 连续调用应由上层串行化；通知和连接状态回调可能来自 Windows Runtime 线程池。
 */
class GLOBALBASE_PORT GB_BluetoothLeGattClient final
{
public:
    using NotificationCallback = std::function<void(const std::vector<uint8_t>& value)>;
    using ConnectionCallback = std::function<void(bool connected)>;

    GB_BluetoothLeGattClient();
    ~GB_BluetoothLeGattClient() noexcept;

    GB_BluetoothLeGattClient(const GB_BluetoothLeGattClient&) = delete;
    GB_BluetoothLeGattClient& operator=(const GB_BluetoothLeGattClient&) = delete;

    bool IsValid() const;
    GB_SystemResult Connect(uint64_t bluetoothAddress,
                            GB_BluetoothLeAddressType addressType,
                            const GB_BluetoothLeGattProfile& profile,
                            const GB_BluetoothLeGattClientOptions& options);
    GB_SystemResult Disconnect();
    bool IsConnected() const;
    bool IsReady() const;
    std::string GetDeviceName() const;
    GB_BluetoothLeGattWriteMode GetWriteMode() const;
    GB_SystemResult Write(const std::vector<uint8_t>& value);
    void SetNotificationCallback(const NotificationCallback& callback);
    void SetConnectionCallback(const ConnectionCallback& callback);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_BLUETOOTH_WINRT_H_H
