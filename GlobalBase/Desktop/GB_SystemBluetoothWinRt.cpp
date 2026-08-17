#include "GB_SystemBluetoothWinRt.h"

#include "../GB_Utf8String.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <map>
#include <mutex>
#include <new>
#include <sstream>
#include <thread>
#include <utility>

#if defined(_WIN32)
#  include <Windows.h>
#  include <objbase.h>
#  include <roapi.h>
#  include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#  include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#  include <winrt/Windows.Devices.Bluetooth.h>
#  include <winrt/Windows.Foundation.Collections.h>
#  include <winrt/Windows.Foundation.h>
#  include <winrt/Windows.Storage.Streams.h>
#  include <winrt/base.h>
#  pragma comment(lib, "runtimeobject.lib")
#  pragma comment(lib, "windowsapp.lib")
#endif

namespace
{
    std::string FormatBluetoothAddress(const uint64_t address)
    {
        std::ostringstream stream;
        stream << std::uppercase << std::hex << std::setfill('0');
        for (int byteIndex = 5; byteIndex >= 0; byteIndex--)
        {
            if (byteIndex != 5)
            {
                stream << ':';
            }
            stream << std::setw(2) << static_cast<unsigned int>((address >> (byteIndex * 8)) & 0xffu);
        }
        return stream.str();
    }

    std::string BuildAdvertisementKey(const uint64_t address, const GB_BluetoothLeAddressType addressType)
    {
        std::ostringstream stream;
        stream << static_cast<unsigned int>(addressType) << ':' << std::hex << address;
        return stream.str();
    }

#if defined(_WIN32)
    class RoApartmentScope final
    {
    public:
        RoApartmentScope()
        {
            initializeResult = ::RoInitialize(RO_INIT_MULTITHREADED);
            ownsInitialization = initializeResult == S_OK || initializeResult == S_FALSE;
        }

        ~RoApartmentScope() noexcept
        {
            if (ownsInitialization)
            {
                ::RoUninitialize();
            }
        }

        bool IsUsable() const
        {
            return SUCCEEDED(initializeResult) || initializeResult == RPC_E_CHANGED_MODE;
        }

        HRESULT GetResult() const
        {
            return initializeResult;
        }

    private:
        HRESULT initializeResult = E_FAIL;
        bool ownsInitialization = false;
    };

    template<typename TResult>
    GB_SystemResult WaitForAsyncOperation(const winrt::Windows::Foundation::IAsyncOperation<TResult>& operation,
                                          const uint32_t timeoutMilliseconds,
                                          TResult& result,
                                          const std::string& operationName)
    {
        struct CompletionState
        {
            std::mutex mutex;
            std::condition_variable condition;
            bool completed = false;
        };

        if (timeoutMilliseconds == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"异步操作超时必须大于零毫秒。");
        }

        const std::shared_ptr<CompletionState> state = std::make_shared<CompletionState>();
        try
        {
            operation.Completed([state](const auto&, const winrt::Windows::Foundation::AsyncStatus)
                {
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->completed = true;
                    }
                    state->condition.notify_all();
                });

            std::unique_lock<std::mutex> lock(state->mutex);
            const bool completed = state->condition.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds), [state]()
                {
                    return state->completed;
                });
            lock.unlock();

            if (!completed)
            {
                operation.Cancel();
                return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, operationName, u8"Windows Runtime BLE 异步操作超时。");
            }

            const winrt::Windows::Foundation::AsyncStatus status = operation.Status();
            if (status == winrt::Windows::Foundation::AsyncStatus::Canceled)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, operationName, u8"Windows Runtime BLE 异步操作已取消。");
            }
            if (status == winrt::Windows::Foundation::AsyncStatus::Error)
            {
                return GB_SystemResult::FromWinRtHResult(operation.ErrorCode().value, operationName, u8"Windows Runtime BLE 异步操作失败。");
            }
            if (status != winrt::Windows::Foundation::AsyncStatus::Completed)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"Windows Runtime BLE 异步操作没有进入完成状态。");
            }

            result = operation.GetResults();
            return GB_SystemResult::Succeeded(operationName);
        }
        catch (const winrt::hresult_error& error)
        {
            return GB_SystemResult::FromWinRtHResult(error.code().value, operationName, GB_WStringToUtf8(error.message().c_str()));
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"等待 Windows Runtime BLE 异步操作时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::UnknownError, operationName, u8"等待 Windows Runtime BLE 异步操作时发生未知异常。");
        }
    }

    GB_SystemResult ParseUuid(const std::string& uuidText, winrt::guid& uuid, const std::string& operationName)
    {
        if (uuidText.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"GATT UUID 不能为空。");
        }

        try
        {
            std::wstring text = GB_Utf8ToWString(uuidText);
            if (text.empty())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"GATT UUID 不是有效 UTF-8 文本。");
            }
            if (text.front() != L'{')
            {
                text.insert(text.begin(), L'{');
            }
            if (text.back() != L'}')
            {
                text.push_back(L'}');
            }

            GUID nativeUuid = GUID();
            const HRESULT hresult = ::CLSIDFromString(text.c_str(), &nativeUuid);
            if (FAILED(hresult))
            {
                return GB_SystemResult::FromHResult(static_cast<int32_t>(hresult), operationName, u8"GATT UUID 格式非法。");
            }
            uuid = winrt::guid(nativeUuid);
            return GB_SystemResult::Succeeded(operationName);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"解析 GATT UUID 时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, operationName, u8"解析 GATT UUID 时文本转换失败。");
        }
    }

    std::string GuidToUtf8(const winrt::guid& uuid)
    {
        return GB_WStringToUtf8(winrt::to_hstring(uuid).c_str());
    }

    GB_SystemResult CheckCommunicationStatus(const winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus status,
                                              const std::string& operationName,
                                              const std::string& message)
    {
        using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
        if (status == GattCommunicationStatus::Success)
        {
            return GB_SystemResult::Succeeded(operationName);
        }
        if (status == GattCommunicationStatus::AccessDenied)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::PermissionDenied, operationName, message);
        }
        if (status == GattCommunicationStatus::Unreachable)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, message);
        }
        return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, message);
    }
#endif
}

class GB_BluetoothLeAdvertisementWatcher::Impl final
{
public:
    explicit Impl(const GB_BluetoothLeAdvertisementWatcherOptions& watcherOptions)
        : options(watcherOptions)
    {
    }

    ~Impl() noexcept
    {
        (void)Stop();
    }

    GB_SystemResult Start()
    {
#if !defined(_WIN32)
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_BluetoothLeAdvertisementWatcher::Start", u8"BLE 广播监听仅支持 Windows。");
#else
        std::lock_guard<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (running)
            {
                return GB_SystemResult::Succeeded(u8"GB_BluetoothLeAdvertisementWatcher::Start", u8"BLE 广播监听器已经运行。");
            }
            if (workerThread.joinable())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceBusy, u8"GB_BluetoothLeAdvertisementWatcher::Start", u8"上一次 BLE 广播监听工作线程尚未完成清理。");
            }
            stopRequested = false;
            startCompleted = false;
            trackedDevices.clear();
            startResult = GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_BluetoothLeAdvertisementWatcher::Start", u8"BLE 广播监听工作线程未完成初始化。");
        }

        try
        {
            workerThread = std::thread(&Impl::WorkerLoop, this);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_BluetoothLeAdvertisementWatcher::Start", u8"创建 BLE 广播监听工作线程失败。");
        }

        GB_SystemResult result;
        {
            std::unique_lock<std::mutex> lock(stateMutex);
            startCondition.wait(lock, [this]() { return startCompleted; });
            result = startResult;
        }
        if (result.IsFailed() && workerThread.joinable())
        {
            workerThread.join();
        }
        return result;
#endif
    }

    GB_SystemResult Stop()
    {
#if !defined(_WIN32)
        return GB_SystemResult::Succeeded(u8"GB_BluetoothLeAdvertisementWatcher::Stop");
#else
        std::lock_guard<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            stopRequested = true;
        }
        stopCondition.notify_all();
        if (workerThread.joinable())
        {
            workerThread.join();
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            running = false;
        }
        return GB_SystemResult::Succeeded(u8"GB_BluetoothLeAdvertisementWatcher::Stop");
#endif
    }

    bool IsRunning() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return running;
    }

    void SetCallback(const AdvertisementCallback& newCallback)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        callback = newCallback;
    }

    std::vector<GB_BluetoothLeAdvertisementInfo> GetTrackedDevices() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        std::vector<GB_BluetoothLeAdvertisementInfo> result;
        result.reserve(trackedDevices.size());
        for (const auto& entry : trackedDevices)
        {
            result.push_back(entry.second);
        }
        return result;
    }

private:
#if defined(_WIN32)
    void WorkerLoop() noexcept
    {
        RoApartmentScope apartment;
        if (!apartment.IsUsable())
        {
            SignalStart(GB_SystemResult::FromWinRtHResult(static_cast<int32_t>(apartment.GetResult()), u8"GB_BluetoothLeAdvertisementWatcher::Start", u8"初始化 Windows Runtime 单元失败。"));
            return;
        }

        try
        {
            namespace Advertisement = winrt::Windows::Devices::Bluetooth::Advertisement;
            Advertisement::BluetoothLEAdvertisementWatcher watcher;
            watcher.ScanningMode(options.activeScanning ? Advertisement::BluetoothLEScanningMode::Active : Advertisement::BluetoothLEScanningMode::Passive);
            const winrt::event_token receivedToken = watcher.Received([this](const Advertisement::BluetoothLEAdvertisementWatcher&, const Advertisement::BluetoothLEAdvertisementReceivedEventArgs& args)
                {
                    HandleAdvertisement(args);
                });
            watcher.Start();

            {
                std::lock_guard<std::mutex> lock(stateMutex);
                running = true;
            }
            SignalStart(GB_SystemResult::Succeeded(u8"GB_BluetoothLeAdvertisementWatcher::Start"));

            {
                std::unique_lock<std::mutex> lock(stateMutex);
                stopCondition.wait(lock, [this]() { return stopRequested; });
                running = false;
            }

            watcher.Stop();
            watcher.Received(receivedToken);
        }
        catch (const winrt::hresult_error& error)
        {
            SignalStart(GB_SystemResult::FromWinRtHResult(error.code().value, u8"GB_BluetoothLeAdvertisementWatcher::Start", GB_WStringToUtf8(error.message().c_str())));
        }
        catch (const std::bad_alloc&)
        {
            SignalStart(GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_BluetoothLeAdvertisementWatcher::Start", u8"BLE 广播监听器内存不足。"));
        }
        catch (...)
        {
            SignalStart(GB_SystemResult::Failed(GB_SystemErrorCode::UnknownError, u8"GB_BluetoothLeAdvertisementWatcher::Start", u8"BLE 广播监听器发生未知异常。"));
        }

        std::lock_guard<std::mutex> lock(stateMutex);
        running = false;
    }

    void SignalStart(const GB_SystemResult& result)
    {
        bool shouldSignal = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!startCompleted)
            {
                startResult = result;
                startCompleted = true;
                shouldSignal = true;
            }
        }
        if (shouldSignal)
        {
            startCondition.notify_all();
        }
    }

    void HandleAdvertisement(const winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs& args) noexcept
    {
        try
        {
            GB_BluetoothLeAdvertisementInfo info;
            info.bluetoothAddress = args.BluetoothAddress();
            info.address = FormatBluetoothAddress(info.bluetoothAddress);
            info.rawSignalStrengthDbm = args.RawSignalStrengthInDBm();
            info.isConnectable = args.IsConnectable();
            const auto nativeAddressType = args.BluetoothAddressType();
            if (nativeAddressType == winrt::Windows::Devices::Bluetooth::BluetoothAddressType::Public)
            {
                info.addressType = GB_BluetoothLeAddressType::Public;
            }
            else if (nativeAddressType == winrt::Windows::Devices::Bluetooth::BluetoothAddressType::Random)
            {
                info.addressType = GB_BluetoothLeAddressType::Random;
            }
            info.localName = GB_WStringToUtf8(args.Advertisement().LocalName().c_str());
            for (const winrt::guid& uuid : args.Advertisement().ServiceUuids())
            {
                info.serviceUuids.push_back(GuidToUtf8(uuid));
            }

            AdvertisementCallback callbackSnapshot;
            bool dispatch = false;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                if (!running || stopRequested)
                {
                    return;
                }
                const std::string key = BuildAdvertisementKey(info.bluetoothAddress, info.addressType);
                const auto existing = trackedDevices.find(key);
                const bool isNew = existing == trackedDevices.end();
                if (isNew && options.maxTrackedDevices != 0 && trackedDevices.size() >= options.maxTrackedDevices)
                {
                    return;
                }
                trackedDevices[key] = info;
                dispatch = options.allowDuplicateAddresses || isNew;
                callbackSnapshot = callback;
            }
            if (dispatch && callbackSnapshot)
            {
                callbackSnapshot(info);
            }
        }
        catch (...)
        {
            // Windows Runtime 事件线程不得因用户数据或分配失败退出。
        }
    }
#endif

private:
    GB_BluetoothLeAdvertisementWatcherOptions options;
    mutable std::mutex operationMutex;
    mutable std::mutex stateMutex;
    std::condition_variable startCondition;
    std::condition_variable stopCondition;
    std::thread workerThread;
    bool running = false;
    bool stopRequested = false;
    bool startCompleted = false;
    GB_SystemResult startResult;
    AdvertisementCallback callback;
    std::map<std::string, GB_BluetoothLeAdvertisementInfo> trackedDevices;
};

GB_BluetoothLeAdvertisementWatcher::GB_BluetoothLeAdvertisementWatcher()
    : GB_BluetoothLeAdvertisementWatcher(GB_BluetoothLeAdvertisementWatcherOptions())
{
}

GB_BluetoothLeAdvertisementWatcher::GB_BluetoothLeAdvertisementWatcher(const GB_BluetoothLeAdvertisementWatcherOptions& options)
    : impl(new (std::nothrow) Impl(options))
{
}

GB_BluetoothLeAdvertisementWatcher::~GB_BluetoothLeAdvertisementWatcher() noexcept = default;

bool GB_BluetoothLeAdvertisementWatcher::IsValid() const
{
    return static_cast<bool>(impl);
}

GB_SystemResult GB_BluetoothLeAdvertisementWatcher::Start()
{
    return impl ? impl->Start() : GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_BluetoothLeAdvertisementWatcher::Start", u8"BLE 广播监听器内部状态创建失败。");
}

GB_SystemResult GB_BluetoothLeAdvertisementWatcher::Stop()
{
    return impl ? impl->Stop() : GB_SystemResult::Succeeded(u8"GB_BluetoothLeAdvertisementWatcher::Stop");
}

bool GB_BluetoothLeAdvertisementWatcher::IsRunning() const
{
    return impl && impl->IsRunning();
}

void GB_BluetoothLeAdvertisementWatcher::SetAdvertisementCallback(const AdvertisementCallback& callback)
{
    if (impl)
    {
        impl->SetCallback(callback);
    }
}

std::vector<GB_BluetoothLeAdvertisementInfo> GB_BluetoothLeAdvertisementWatcher::GetTrackedDevices() const
{
    return impl ? impl->GetTrackedDevices() : std::vector<GB_BluetoothLeAdvertisementInfo>();
}

class GB_BluetoothLeGattClient::Impl final
{
public:
    ~Impl() noexcept
    {
        (void)Disconnect();
    }

    GB_SystemResult Connect(const uint64_t bluetoothAddress,
                            const GB_BluetoothLeAddressType addressType,
                            const GB_BluetoothLeGattProfile& requestedProfile,
                            const GB_BluetoothLeGattClientOptions& requestedOptions)
    {
#if !defined(_WIN32)
        (void)bluetoothAddress;
        (void)addressType;
        (void)requestedProfile;
        (void)requestedOptions;
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_BluetoothLeGattClient::Connect", u8"BLE GATT 客户端仅支持 Windows。");
#else
        std::lock_guard<std::mutex> operationLock(operationMutex);
        if (bluetoothAddress == 0 || requestedOptions.connectTimeoutMilliseconds == 0 || requestedOptions.operationTimeoutMilliseconds == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_BluetoothLeGattClient::Connect", u8"蓝牙地址和两个超时值都必须有效且非零。");
        }

        winrt::guid serviceUuid;
        winrt::guid notifyUuid;
        winrt::guid writeUuid;
        GB_SystemResult result = ParseUuid(requestedProfile.serviceUuid, serviceUuid, u8"GB_BluetoothLeGattClient::Connect");
        if (result.IsFailed()) return result;
        result = ParseUuid(requestedProfile.notifyCharacteristicUuid, notifyUuid, u8"GB_BluetoothLeGattClient::Connect");
        if (result.IsFailed()) return result;
        result = ParseUuid(requestedProfile.writeCharacteristicUuid, writeUuid, u8"GB_BluetoothLeGattClient::Connect");
        if (result.IsFailed()) return result;

        (void)DisconnectLocked();
        RoApartmentScope apartment;
        if (!apartment.IsUsable())
        {
            return GB_SystemResult::FromWinRtHResult(static_cast<int32_t>(apartment.GetResult()), u8"GB_BluetoothLeGattClient::Connect", u8"初始化 Windows Runtime 单元失败。");
        }

        try
        {
            using namespace winrt::Windows::Devices::Bluetooth;
            using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
            options = requestedOptions;
            profile = requestedProfile;

            winrt::Windows::Foundation::IAsyncOperation<BluetoothLEDevice> deviceOperation = nullptr;
            if (addressType == GB_BluetoothLeAddressType::Public)
            {
                deviceOperation = BluetoothLEDevice::FromBluetoothAddressAsync(bluetoothAddress, BluetoothAddressType::Public);
            }
            else if (addressType == GB_BluetoothLeAddressType::Random)
            {
                deviceOperation = BluetoothLEDevice::FromBluetoothAddressAsync(bluetoothAddress, BluetoothAddressType::Random);
            }
            else
            {
                deviceOperation = BluetoothLEDevice::FromBluetoothAddressAsync(bluetoothAddress);
            }

            BluetoothLEDevice connectedDevice = nullptr;
            result = WaitForAsyncOperation(deviceOperation, options.connectTimeoutMilliseconds, connectedDevice, u8"GB_BluetoothLeGattClient::Connect.Device");
            if (result.IsFailed()) return result;
            if (!connectedDevice)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_BluetoothLeGattClient::Connect", u8"Windows 未能从广播地址创建 BLE 设备对象。");
            }

            const winrt::event_token newConnectionToken = connectedDevice.ConnectionStatusChanged([this](const BluetoothLEDevice& sender, const winrt::Windows::Foundation::IInspectable&)
                {
                    HandleConnectionChanged(sender.ConnectionStatus() == BluetoothConnectionStatus::Connected);
                });

            GattDeviceServicesResult servicesResult = nullptr;
            result = WaitForAsyncOperation(connectedDevice.GetGattServicesForUuidAsync(serviceUuid, BluetoothCacheMode::Uncached), options.connectTimeoutMilliseconds, servicesResult, u8"GB_BluetoothLeGattClient::Connect.Services");
            if (result.IsFailed())
            {
                connectedDevice.ConnectionStatusChanged(newConnectionToken);
                connectedDevice.Close();
                return result;
            }
            result = CheckCommunicationStatus(servicesResult.Status(), u8"GB_BluetoothLeGattClient::Connect.Services", u8"无法访问目标 BLE GATT 服务。");
            if (result.IsFailed() || servicesResult.Services().Size() == 0)
            {
                connectedDevice.ConnectionStatusChanged(newConnectionToken);
                connectedDevice.Close();
                return result.IsFailed() ? result : GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_BluetoothLeGattClient::Connect.Services", u8"未找到目标 BLE GATT 服务。");
            }
            GattDeviceService connectedService = servicesResult.Services().GetAt(0);

            GattCharacteristicsResult notifyResult = nullptr;
            result = WaitForAsyncOperation(connectedService.GetCharacteristicsForUuidAsync(notifyUuid, BluetoothCacheMode::Uncached), options.operationTimeoutMilliseconds, notifyResult, u8"GB_BluetoothLeGattClient::Connect.NotifyCharacteristic");
            if (result.IsFailed())
            {
                connectedService.Close();
                connectedDevice.ConnectionStatusChanged(newConnectionToken);
                connectedDevice.Close();
                return result;
            }
            result = CheckCommunicationStatus(notifyResult.Status(), u8"GB_BluetoothLeGattClient::Connect.NotifyCharacteristic", u8"无法访问目标通知特征。");
            if (result.IsFailed() || notifyResult.Characteristics().Size() == 0)
            {
                connectedService.Close();
                connectedDevice.ConnectionStatusChanged(newConnectionToken);
                connectedDevice.Close();
                return result.IsFailed() ? result : GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_BluetoothLeGattClient::Connect.NotifyCharacteristic", u8"未找到目标通知特征。");
            }
            GattCharacteristic connectedNotifyCharacteristic = notifyResult.Characteristics().GetAt(0);

            GattCharacteristicsResult writeResult = nullptr;
            result = WaitForAsyncOperation(connectedService.GetCharacteristicsForUuidAsync(writeUuid, BluetoothCacheMode::Uncached), options.operationTimeoutMilliseconds, writeResult, u8"GB_BluetoothLeGattClient::Connect.WriteCharacteristic");
            if (result.IsFailed())
            {
                connectedService.Close();
                connectedDevice.ConnectionStatusChanged(newConnectionToken);
                connectedDevice.Close();
                return result;
            }
            result = CheckCommunicationStatus(writeResult.Status(), u8"GB_BluetoothLeGattClient::Connect.WriteCharacteristic", u8"无法访问目标写特征。");
            if (result.IsFailed() || writeResult.Characteristics().Size() == 0)
            {
                connectedService.Close();
                connectedDevice.ConnectionStatusChanged(newConnectionToken);
                connectedDevice.Close();
                return result.IsFailed() ? result : GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_BluetoothLeGattClient::Connect.WriteCharacteristic", u8"未找到目标写特征。");
            }
            GattCharacteristic connectedWriteCharacteristic = writeResult.Characteristics().GetAt(0);

            const uint32_t notifyProperties = static_cast<uint32_t>(connectedNotifyCharacteristic.CharacteristicProperties());
            const uint32_t notifyFlag = static_cast<uint32_t>(GattCharacteristicProperties::Notify);
            const uint32_t indicateFlag = static_cast<uint32_t>(GattCharacteristicProperties::Indicate);
            GattClientCharacteristicConfigurationDescriptorValue cccdValue = GattClientCharacteristicConfigurationDescriptorValue::None;
            if ((notifyProperties & notifyFlag) != 0)
            {
                cccdValue = GattClientCharacteristicConfigurationDescriptorValue::Notify;
            }
            else if ((notifyProperties & indicateFlag) != 0)
            {
                cccdValue = GattClientCharacteristicConfigurationDescriptorValue::Indicate;
            }
            else
            {
                connectedService.Close();
                connectedDevice.ConnectionStatusChanged(newConnectionToken);
                connectedDevice.Close();
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_BluetoothLeGattClient::Connect", u8"目标接收特征既不支持 Notify 也不支持 Indicate。");
            }

            const uint32_t writeProperties = static_cast<uint32_t>(connectedWriteCharacteristic.CharacteristicProperties());
            const uint32_t writeFlag = static_cast<uint32_t>(GattCharacteristicProperties::Write);
            const uint32_t writeWithoutResponseFlag = static_cast<uint32_t>(GattCharacteristicProperties::WriteWithoutResponse);
            GB_BluetoothLeGattWriteMode selectedWriteMode = GB_BluetoothLeGattWriteMode::Unknown;
            if ((writeProperties & writeFlag) != 0)
            {
                selectedWriteMode = GB_BluetoothLeGattWriteMode::WithResponse;
            }
            else if ((writeProperties & writeWithoutResponseFlag) != 0)
            {
                selectedWriteMode = GB_BluetoothLeGattWriteMode::WithoutResponse;
            }
            else
            {
                connectedService.Close();
                connectedDevice.ConnectionStatusChanged(newConnectionToken);
                connectedDevice.Close();
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_BluetoothLeGattClient::Connect", u8"目标写特征不支持可用的 GATT 写入模式。");
            }

            const winrt::event_token newValueToken = connectedNotifyCharacteristic.ValueChanged([this](const GattCharacteristic&, const GattValueChangedEventArgs& args)
                {
                    HandleNotification(args);
                });
            GattCommunicationStatus cccdStatus = GattCommunicationStatus::Unreachable;
            result = WaitForAsyncOperation(connectedNotifyCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(cccdValue), options.operationTimeoutMilliseconds, cccdStatus, u8"GB_BluetoothLeGattClient::Connect.CCCD");
            if (result.IsFailed() || cccdStatus != GattCommunicationStatus::Success)
            {
                connectedNotifyCharacteristic.ValueChanged(newValueToken);
                connectedService.Close();
                connectedDevice.ConnectionStatusChanged(newConnectionToken);
                connectedDevice.Close();
                return result.IsFailed() ? result : CheckCommunicationStatus(cccdStatus, u8"GB_BluetoothLeGattClient::Connect.CCCD", u8"订阅目标通知特征失败。");
            }

            {
                std::lock_guard<std::mutex> lock(stateMutex);
                device = connectedDevice;
                service = connectedService;
                notifyCharacteristic = connectedNotifyCharacteristic;
                writeCharacteristic = connectedWriteCharacteristic;
                connectionToken = newConnectionToken;
                valueToken = newValueToken;
                hasConnectionToken = true;
                hasValueToken = true;
                cccdEnabled = true;
                connected = true;
                ready = true;
                writeMode = selectedWriteMode;
                deviceName = GB_WStringToUtf8(connectedDevice.Name().c_str());
            }
            DispatchConnection(true);
            return GB_SystemResult::Succeeded(u8"GB_BluetoothLeGattClient::Connect");
        }
        catch (const winrt::hresult_error& error)
        {
            (void)DisconnectLocked();
            return GB_SystemResult::FromWinRtHResult(error.code().value, u8"GB_BluetoothLeGattClient::Connect", GB_WStringToUtf8(error.message().c_str()));
        }
        catch (const std::bad_alloc&)
        {
            (void)DisconnectLocked();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_BluetoothLeGattClient::Connect", u8"连接 BLE GATT 设备时内存不足。");
        }
        catch (...)
        {
            (void)DisconnectLocked();
            return GB_SystemResult::Failed(GB_SystemErrorCode::UnknownError, u8"GB_BluetoothLeGattClient::Connect", u8"连接 BLE GATT 设备时发生未知异常。");
        }
#endif
    }

    GB_SystemResult Disconnect()
    {
        std::lock_guard<std::mutex> operationLock(operationMutex);
        return DisconnectLocked();
    }

    bool IsConnected() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return connected;
    }

    bool IsReady() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return ready;
    }

    std::string GetDeviceName() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return deviceName;
    }

    GB_BluetoothLeGattWriteMode GetWriteMode() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return writeMode;
    }

    GB_SystemResult Write(const std::vector<uint8_t>& value)
    {
#if !defined(_WIN32)
        (void)value;
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_BluetoothLeGattClient::Write", u8"BLE GATT 客户端仅支持 Windows。");
#else
        std::lock_guard<std::mutex> operationLock(operationMutex);
        if (value.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_BluetoothLeGattClient::Write", u8"写入值不能为空。");
        }

        RoApartmentScope apartment;
        if (!apartment.IsUsable())
        {
            return GB_SystemResult::FromWinRtHResult(static_cast<int32_t>(apartment.GetResult()), u8"GB_BluetoothLeGattClient::Write", u8"初始化 Windows Runtime 单元失败。");
        }

        try
        {
            winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic characteristic = nullptr;
            GB_BluetoothLeGattWriteMode selectedMode = GB_BluetoothLeGattWriteMode::Unknown;
            uint32_t timeoutMilliseconds = 0;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                if (!ready || !writeCharacteristic)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::NotInitialized, u8"GB_BluetoothLeGattClient::Write", u8"BLE GATT 客户端尚未完成连接和订阅。");
                }
                characteristic = writeCharacteristic;
                selectedMode = writeMode;
                timeoutMilliseconds = options.operationTimeoutMilliseconds;
            }

            winrt::Windows::Storage::Streams::DataWriter writer;
            writer.WriteBytes(winrt::array_view<const uint8_t>(value.data(), value.data() + value.size()));
            const winrt::Windows::Storage::Streams::IBuffer buffer = writer.DetachBuffer();
            const auto nativeWriteMode = selectedMode == GB_BluetoothLeGattWriteMode::WithResponse
                ? winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattWriteOption::WriteWithResponse
                : winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattWriteOption::WriteWithoutResponse;
            winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus status = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus::Unreachable;
            GB_SystemResult result = WaitForAsyncOperation(characteristic.WriteValueAsync(buffer, nativeWriteMode), timeoutMilliseconds, status, u8"GB_BluetoothLeGattClient::Write");
            if (result.IsFailed())
            {
                return result;
            }
            return CheckCommunicationStatus(status, u8"GB_BluetoothLeGattClient::Write", u8"Windows BLE 栈未接受 GATT 特征写入。");
        }
        catch (const winrt::hresult_error& error)
        {
            return GB_SystemResult::FromWinRtHResult(error.code().value, u8"GB_BluetoothLeGattClient::Write", GB_WStringToUtf8(error.message().c_str()));
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_BluetoothLeGattClient::Write", u8"准备 GATT 写入缓冲区时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::UnknownError, u8"GB_BluetoothLeGattClient::Write", u8"执行 GATT 写入时发生未知异常。");
        }
#endif
    }

    void SetNotificationCallback(const NotificationCallback& newCallback)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        notificationCallback = newCallback;
    }

    void SetConnectionCallback(const ConnectionCallback& newCallback)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        connectionCallback = newCallback;
    }

private:
    GB_SystemResult DisconnectLocked() noexcept
    {
#if !defined(_WIN32)
        return GB_SystemResult::Succeeded(u8"GB_BluetoothLeGattClient::Disconnect");
#else
        try
        {
            RoApartmentScope apartment;
            winrt::Windows::Devices::Bluetooth::BluetoothLEDevice oldDevice = nullptr;
            winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceService oldService = nullptr;
            winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic oldNotify = nullptr;
            winrt::event_token oldConnectionToken{};
            winrt::event_token oldValueToken{};
            bool removeConnectionToken = false;
            bool removeValueToken = false;
            bool disableCccd = false;
            uint32_t timeoutMilliseconds = 0;
            bool wasConnected = false;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                oldDevice = device;
                oldService = service;
                oldNotify = notifyCharacteristic;
                oldConnectionToken = connectionToken;
                oldValueToken = valueToken;
                removeConnectionToken = hasConnectionToken;
                removeValueToken = hasValueToken;
                disableCccd = cccdEnabled;
                timeoutMilliseconds = options.operationTimeoutMilliseconds;
                wasConnected = connected || ready;
                ClearStateLocked();
            }

            if (oldNotify && removeValueToken)
            {
                oldNotify.ValueChanged(oldValueToken);
            }
            if (oldNotify && disableCccd && apartment.IsUsable() && timeoutMilliseconds != 0)
            {
                winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus ignoredStatus = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus::Unreachable;
                (void)WaitForAsyncOperation(oldNotify.WriteClientCharacteristicConfigurationDescriptorAsync(winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattClientCharacteristicConfigurationDescriptorValue::None), timeoutMilliseconds, ignoredStatus, u8"GB_BluetoothLeGattClient::Disconnect.CCCD");
            }
            if (oldDevice && removeConnectionToken)
            {
                oldDevice.ConnectionStatusChanged(oldConnectionToken);
            }
            if (oldService)
            {
                oldService.Close();
            }
            if (oldDevice)
            {
                oldDevice.Close();
            }
            if (wasConnected)
            {
                DispatchConnection(false);
            }
            return GB_SystemResult::Succeeded(u8"GB_BluetoothLeGattClient::Disconnect");
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            ClearStateLocked();
            return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, u8"GB_BluetoothLeGattClient::Disconnect", u8"释放 BLE GATT 资源时发生异常，内部状态已清空。");
        }
#endif
    }

    void ClearStateLocked() noexcept
    {
#if defined(_WIN32)
        device = nullptr;
        service = nullptr;
        notifyCharacteristic = nullptr;
        writeCharacteristic = nullptr;
        connectionToken = {};
        valueToken = {};
#endif
        hasConnectionToken = false;
        hasValueToken = false;
        cccdEnabled = false;
        connected = false;
        ready = false;
        deviceName.clear();
        writeMode = GB_BluetoothLeGattWriteMode::Unknown;
    }

#if defined(_WIN32)
    void HandleNotification(const winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattValueChangedEventArgs& args) noexcept
    {
        try
        {
            winrt::Windows::Storage::Streams::DataReader reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(args.CharacteristicValue());
            std::vector<uint8_t> value(reader.UnconsumedBufferLength());
            if (!value.empty())
            {
                reader.ReadBytes(winrt::array_view<uint8_t>(value.data(), value.data() + value.size()));
            }
            NotificationCallback callbackSnapshot;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                callbackSnapshot = notificationCallback;
            }
            if (callbackSnapshot)
            {
                callbackSnapshot(value);
            }
        }
        catch (...)
        {
            // 通知线程不得向 Windows Runtime 传播异常。
        }
    }

    void HandleConnectionChanged(const bool isConnected) noexcept
    {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            changed = connected != isConnected;
            connected = isConnected;
            if (!isConnected)
            {
                ready = false;
            }
        }
        if (changed)
        {
            DispatchConnection(isConnected);
        }
    }
#endif

    void DispatchConnection(const bool isConnected) noexcept
    {
        try
        {
            ConnectionCallback callbackSnapshot;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                callbackSnapshot = connectionCallback;
            }
            if (callbackSnapshot)
            {
                callbackSnapshot(isConnected);
            }
        }
        catch (...)
        {
        }
    }

private:
    mutable std::mutex operationMutex;
    mutable std::mutex stateMutex;
    GB_BluetoothLeGattProfile profile;
    GB_BluetoothLeGattClientOptions options;
    bool connected = false;
    bool ready = false;
    bool hasConnectionToken = false;
    bool hasValueToken = false;
    bool cccdEnabled = false;
    std::string deviceName;
    GB_BluetoothLeGattWriteMode writeMode = GB_BluetoothLeGattWriteMode::Unknown;
    NotificationCallback notificationCallback;
    ConnectionCallback connectionCallback;
#if defined(_WIN32)
    winrt::Windows::Devices::Bluetooth::BluetoothLEDevice device = nullptr;
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceService service = nullptr;
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic notifyCharacteristic = nullptr;
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic writeCharacteristic = nullptr;
    winrt::event_token connectionToken{};
    winrt::event_token valueToken{};
#endif
};

GB_BluetoothLeGattClient::GB_BluetoothLeGattClient()
    : impl(new (std::nothrow) Impl())
{
}

GB_BluetoothLeGattClient::~GB_BluetoothLeGattClient() noexcept = default;

bool GB_BluetoothLeGattClient::IsValid() const
{
    return static_cast<bool>(impl);
}

GB_SystemResult GB_BluetoothLeGattClient::Connect(const uint64_t bluetoothAddress,
                                                   const GB_BluetoothLeAddressType addressType,
                                                   const GB_BluetoothLeGattProfile& profile,
                                                   const GB_BluetoothLeGattClientOptions& options)
{
    return impl ? impl->Connect(bluetoothAddress, addressType, profile, options) : GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_BluetoothLeGattClient::Connect", u8"BLE GATT 客户端内部状态创建失败。");
}

GB_SystemResult GB_BluetoothLeGattClient::Disconnect()
{
    return impl ? impl->Disconnect() : GB_SystemResult::Succeeded(u8"GB_BluetoothLeGattClient::Disconnect");
}

bool GB_BluetoothLeGattClient::IsConnected() const
{
    return impl && impl->IsConnected();
}

bool GB_BluetoothLeGattClient::IsReady() const
{
    return impl && impl->IsReady();
}

std::string GB_BluetoothLeGattClient::GetDeviceName() const
{
    return impl ? impl->GetDeviceName() : std::string();
}

GB_BluetoothLeGattWriteMode GB_BluetoothLeGattClient::GetWriteMode() const
{
    return impl ? impl->GetWriteMode() : GB_BluetoothLeGattWriteMode::Unknown;
}

GB_SystemResult GB_BluetoothLeGattClient::Write(const std::vector<uint8_t>& value)
{
    return impl ? impl->Write(value) : GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_BluetoothLeGattClient::Write", u8"BLE GATT 客户端内部状态创建失败。");
}

void GB_BluetoothLeGattClient::SetNotificationCallback(const NotificationCallback& callback)
{
    if (impl)
    {
        impl->SetNotificationCallback(callback);
    }
}

void GB_BluetoothLeGattClient::SetConnectionCallback(const ConnectionCallback& callback)
{
    if (impl)
    {
        impl->SetConnectionCallback(callback);
    }
}
