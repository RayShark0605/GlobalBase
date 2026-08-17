#include "GB_RunTests.h"
#include "Desktop/GB_SystemBluetooth.h"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int totalSystemBluetoothCaseCount = 0;

    [[noreturn]] void FailSystemBluetoothTest(const std::string& caseName, const std::string& detail)
    {
        std::cerr << "[FAILED] " << caseName << "\n" << detail << std::endl;
        std::exit(1);
    }

    void RequireTrueSystemBluetooth(const bool condition, const std::string& caseName, const std::string& detail)
    {
        totalSystemBluetoothCaseCount++;
        if (!condition)
        {
            FailSystemBluetoothTest(caseName, detail);
        }
    }

    void RequireResultSucceeded(const GB_SystemResult& result, const std::string& caseName)
    {
        totalSystemBluetoothCaseCount++;
        if (result.IsFailed())
        {
            FailSystemBluetoothTest(caseName, result.ToString());
        }
    }

    void RequireResultFailed(const GB_SystemResult& result, const std::string& caseName)
    {
        totalSystemBluetoothCaseCount++;
        if (result.IsSucceeded())
        {
            FailSystemBluetoothTest(caseName, "Expected a failed GB_SystemResult.");
        }
    }

    void TestAddressHelpers()
    {
        RequireTrueSystemBluetooth(GB_SystemBluetooth::IsValidAddress("01:23:45:67:89:AB"), "GB_SystemBluetooth valid colon address", "Expected colon separated Bluetooth address to be valid.");
        RequireTrueSystemBluetooth(GB_SystemBluetooth::IsValidAddress("01-23-45-67-89-ab"), "GB_SystemBluetooth valid hyphen address", "Expected hyphen separated Bluetooth address to be valid.");
        RequireTrueSystemBluetooth(GB_SystemBluetooth::IsValidAddress("0123456789ab"), "GB_SystemBluetooth valid compact address", "Expected compact Bluetooth address to be valid.");
        RequireTrueSystemBluetooth(GB_SystemBluetooth::NormalizeAddress("01-23-45-67-89-ab") == "01:23:45:67:89:AB", "GB_SystemBluetooth normalize address", "Bluetooth address should normalize to uppercase colon separated text.");
        RequireTrueSystemBluetooth(!GB_SystemBluetooth::IsValidAddress("01:23:45:67:89"), "GB_SystemBluetooth invalid short address", "Short Bluetooth address should be rejected.");
        RequireTrueSystemBluetooth(GB_SystemBluetooth::NormalizeAddress("not-a-bluetooth-address").empty(), "GB_SystemBluetooth invalid normalize empty", "Invalid Bluetooth address should normalize to empty text.");
    }

    void TestNameHelpers()
    {
        RequireTrueSystemBluetooth(GB_SystemBluetooth::GetDeviceKindName(GB_BluetoothDeviceKind::Classic) == "Classic", "GB_SystemBluetooth device kind name", "Classic kind should map to Classic.");
        RequireTrueSystemBluetooth(GB_SystemBluetooth::GetPairStatusName(GB_BluetoothPairStatus::Paired) == "Paired", "GB_SystemBluetooth pair status name", "Paired status should map to Paired.");
        RequireTrueSystemBluetooth(GB_SystemBluetooth::GetConnectionStatusName(GB_BluetoothConnectionStatus::Connected) == "Connected", "GB_SystemBluetooth connection status name", "Connected status should map to Connected.");
        RequireTrueSystemBluetooth(GB_SystemBluetooth::GetEventTypeName(GB_BluetoothEventType::DeviceAdded) == "DeviceAdded", "GB_SystemBluetooth event type name", "DeviceAdded event should map to DeviceAdded.");
    }

    void TestRadioAndClassicEnumeration()
    {
        std::vector<GB_BluetoothRadioInfo> radios;
        const GB_SystemResult radioResult = GB_SystemBluetooth::GetRadios(radios);
        RequireResultSucceeded(radioResult, "GB_SystemBluetooth::GetRadios");

        bool available = false;
        const GB_SystemResult availableResult = GB_SystemBluetooth::IsBluetoothAvailable(available);
        RequireResultSucceeded(availableResult, "GB_SystemBluetooth::IsBluetoothAvailable");
        RequireTrueSystemBluetooth(available == !radios.empty(), "GB_SystemBluetooth availability consistency", "Availability should match whether any Bluetooth radio was enumerated.");

        GB_BluetoothRadioInfo defaultRadio;
        bool foundDefaultRadio = false;
        const GB_SystemResult defaultRadioResult = GB_SystemBluetooth::GetDefaultRadio(defaultRadio, foundDefaultRadio);
        RequireResultSucceeded(defaultRadioResult, "GB_SystemBluetooth::GetDefaultRadio");
        RequireTrueSystemBluetooth(foundDefaultRadio == !radios.empty(), "GB_SystemBluetooth default radio consistency", "Default radio found flag should match radio enumeration.");

        for (size_t index = 0; index < radios.size(); index++)
        {
            RequireTrueSystemBluetooth(radios[index].address.empty() || GB_SystemBluetooth::IsValidAddress(radios[index].address), "GB_SystemBluetooth radio address", "Radio address should be empty or a valid Bluetooth address.");
            RequireTrueSystemBluetooth(radios[index].isClassicSupported, "GB_SystemBluetooth radio classic support", "Win32 enumerated Bluetooth radios should report classic support.");
        }

        GB_BluetoothClassicDeviceQueryOptions options;
        options.requestFreshInquiry = false;
        std::vector<GB_BluetoothDeviceInfo> devices;
        const GB_SystemResult devicesResult = GB_SystemBluetooth::GetClassicDevices(devices, options);
        RequireResultSucceeded(devicesResult, "GB_SystemBluetooth::GetClassicDevices cached");

        for (size_t index = 0; index < devices.size(); index++)
        {
            RequireTrueSystemBluetooth(GB_SystemBluetooth::IsValidAddress(devices[index].address), "GB_SystemBluetooth device address", "Classic Bluetooth devices should expose a valid address.");
            RequireTrueSystemBluetooth(devices[index].deviceId == devices[index].address, "GB_SystemBluetooth device id", "Classic Bluetooth deviceId should match normalized address.");
            RequireTrueSystemBluetooth(devices[index].deviceKind == GB_BluetoothDeviceKind::Classic, "GB_SystemBluetooth device kind", "Win32 enumerated devices should be marked Classic.");
            RequireTrueSystemBluetooth(devices[index].isClassicSupported, "GB_SystemBluetooth device classic support", "Win32 enumerated devices should report classic support.");
            RequireTrueSystemBluetooth(devices[index].isConnected == (devices[index].connectionStatus == GB_BluetoothConnectionStatus::Connected), "GB_SystemBluetooth connection consistency", "isConnected and connectionStatus should agree.");
            RequireTrueSystemBluetooth(devices[index].isAuthenticated == (devices[index].pairStatus == GB_BluetoothPairStatus::Paired), "GB_SystemBluetooth pair consistency", "isAuthenticated and pairStatus should agree.");
        }
    }

    void TestUnsupportedAndInvalidOperations()
    {
        std::vector<GB_BluetoothDeviceInfo> lowEnergyDevices;
        const GB_SystemResult lowEnergyResult = GB_SystemBluetooth::GetLowEnergyDevices(lowEnergyDevices);
#if defined(_WIN32)
        RequireResultSucceeded(lowEnergyResult, "GB_SystemBluetooth::GetLowEnergyDevices Windows");
        for (size_t index = 0; index < lowEnergyDevices.size(); index++)
        {
            RequireTrueSystemBluetooth(lowEnergyDevices[index].deviceKind == GB_BluetoothDeviceKind::LowEnergy
                                           || lowEnergyDevices[index].deviceKind == GB_BluetoothDeviceKind::DualMode,
                                       "GB_SystemBluetooth BLE kind",
                                       "Windows BLE interface enumeration should identify low-energy capability.");
        }
#else
        RequireResultFailed(lowEnergyResult, "GB_SystemBluetooth::GetLowEnergyDevices unsupported");
        RequireTrueSystemBluetooth(lowEnergyResult.errorCode == GB_SystemErrorCode::UnsupportedPlatform, "GB_SystemBluetooth BLE unsupported code", "BLE enumeration should explicitly report UnsupportedPlatform until WinRT implementation exists.");
#endif

        GB_BluetoothDeviceId invalidDeviceId;
        invalidDeviceId.deviceKind = GB_BluetoothDeviceKind::Classic;
        invalidDeviceId.address = "invalid-address";

        bool connected = true;
        const GB_SystemResult connectedResult = GB_SystemBluetooth::IsDeviceConnected(invalidDeviceId, connected);
        RequireResultFailed(connectedResult, "GB_SystemBluetooth::IsDeviceConnected invalid address");
        RequireTrueSystemBluetooth(!connected, "GB_SystemBluetooth invalid connected output", "Invalid connection query should leave connected as false.");

        const GB_SystemResult pairResult = GB_SystemBluetooth::PairDevice(invalidDeviceId, GB_BluetoothPairingOptions());
        RequireResultFailed(pairResult, "GB_SystemBluetooth::PairDevice invalid address");

        const GB_SystemResult removeResult = GB_SystemBluetooth::RemoveDevice(invalidDeviceId);
        RequireResultFailed(removeResult, "GB_SystemBluetooth::RemoveDevice invalid address");

        GB_BluetoothDeviceId validFormatDeviceId;
        validFormatDeviceId.deviceKind = GB_BluetoothDeviceKind::Classic;
        validFormatDeviceId.address = "01:23:45:67:89:AB";

        GB_BluetoothPairingOptions pinOptions;
        pinOptions.pinCodeUtf8 = "0000";
        const GB_SystemResult pinPairResult = GB_SystemBluetooth::PairDevice(validFormatDeviceId, pinOptions);
        RequireResultFailed(pinPairResult, "GB_SystemBluetooth::PairDevice nonexistent PIN target");

        GB_BluetoothPairingOptions noUiOptions;
        noUiOptions.allowSystemPairingUi = false;
        const GB_SystemResult noUiPairResult = GB_SystemBluetooth::PairDevice(validFormatDeviceId, noUiOptions);
        RequireResultFailed(noUiPairResult, "GB_SystemBluetooth::PairDevice no UI without PIN");
        RequireTrueSystemBluetooth(noUiPairResult.errorCode == GB_SystemErrorCode::InvalidArgument,
                                   "GB_SystemBluetooth no UI without PIN code",
                                   "Non-UI pairing should require an explicit PIN before enumerating hardware.");
    }

    void TestWinRtBlePrimitiveLifecycle()
    {
        GB_BluetoothLeAdvertisementWatcherOptions watcherOptions;
        watcherOptions.activeScanning = true;
        watcherOptions.allowDuplicateAddresses = false;
        watcherOptions.maxTrackedDevices = 8;
        GB_BluetoothLeAdvertisementWatcher watcher(watcherOptions);
        RequireTrueSystemBluetooth(watcher.IsValid(), "GB_BluetoothLeAdvertisementWatcher valid", "Advertisement watcher should create its PIMPL state.");
        RequireTrueSystemBluetooth(!watcher.IsRunning(), "GB_BluetoothLeAdvertisementWatcher initially stopped", "Advertisement watcher should initially be stopped.");
        RequireTrueSystemBluetooth(watcher.GetTrackedDevices().empty(), "GB_BluetoothLeAdvertisementWatcher initially empty", "Advertisement watcher should initially have no tracked devices.");
        RequireResultSucceeded(watcher.Start(), "GB_BluetoothLeAdvertisementWatcher Start");
        RequireTrueSystemBluetooth(watcher.IsRunning(), "GB_BluetoothLeAdvertisementWatcher running", "Advertisement watcher should report running after Start().");
        RequireResultSucceeded(watcher.Stop(), "GB_BluetoothLeAdvertisementWatcher Stop");
        RequireTrueSystemBluetooth(!watcher.IsRunning(), "GB_BluetoothLeAdvertisementWatcher stopped", "Advertisement watcher should report stopped after Stop().");
        RequireResultSucceeded(watcher.Stop(), "GB_BluetoothLeAdvertisementWatcher Stop idempotent");

        GB_BluetoothLeGattClient client;
        RequireTrueSystemBluetooth(client.IsValid(), "GB_BluetoothLeGattClient valid", "GATT client should create its PIMPL state.");
        RequireTrueSystemBluetooth(!client.IsConnected() && !client.IsReady(), "GB_BluetoothLeGattClient initially disconnected", "GATT client should initially be disconnected and not ready.");

        GB_BluetoothLeGattProfile profile;
        profile.serviceUuid = "0000FFF0-0000-1000-8000-00805F9B34FB";
        profile.notifyCharacteristicUuid = "0000FFF1-0000-1000-8000-00805F9B34FB";
        profile.writeCharacteristicUuid = "0000FFF2-0000-1000-8000-00805F9B34FB";
        GB_BluetoothLeGattClientOptions clientOptions;
        clientOptions.connectTimeoutMilliseconds = 1;
        clientOptions.operationTimeoutMilliseconds = 1;
        const GB_SystemResult invalidAddressResult = client.Connect(0, GB_BluetoothLeAddressType::Unknown, profile, clientOptions);
        RequireResultFailed(invalidAddressResult, "GB_BluetoothLeGattClient zero address");
        RequireTrueSystemBluetooth(invalidAddressResult.errorCode == GB_SystemErrorCode::InvalidArgument,
                                   "GB_BluetoothLeGattClient zero address code",
                                   "A zero BLE address should be rejected before touching Windows Runtime.");
        RequireResultFailed(client.Write(std::vector<uint8_t>()), "GB_BluetoothLeGattClient empty write");
        RequireResultSucceeded(client.Disconnect(), "GB_BluetoothLeGattClient Disconnect idempotent");
    }

    void TestWatcherLifecycle()
    {
        GB_SystemBluetoothWatcher watcher;
        std::atomic<int> callbackCount(0);
        watcher.SetBluetoothEventCallback([&callbackCount](const GB_BluetoothEvent&)
            {
                callbackCount++;
            });

        GB_SystemResult startResult = watcher.Start();
        RequireResultSucceeded(startResult, "GB_SystemBluetoothWatcher Start first");
        RequireTrueSystemBluetooth(watcher.IsRunning(), "GB_SystemBluetoothWatcher running first", "Watcher should be running after Start().");

        startResult = watcher.Start();
        RequireResultSucceeded(startResult, "GB_SystemBluetoothWatcher Start already running");
        RequireTrueSystemBluetooth(watcher.IsRunning(), "GB_SystemBluetoothWatcher running duplicate", "Watcher should remain running after duplicate Start().");

        GB_SystemResult stopResult = watcher.Stop();
        RequireResultSucceeded(stopResult, "GB_SystemBluetoothWatcher Stop first");
        RequireTrueSystemBluetooth(!watcher.IsRunning(), "GB_SystemBluetoothWatcher stopped first", "Watcher should not be running after Stop().");

        stopResult = watcher.Stop();
        RequireResultSucceeded(stopResult, "GB_SystemBluetoothWatcher Stop already stopped");
        RequireTrueSystemBluetooth(!watcher.IsRunning(), "GB_SystemBluetoothWatcher stopped duplicate", "Watcher should remain stopped after duplicate Stop().");

        startResult = watcher.Start();
        RequireResultSucceeded(startResult, "GB_SystemBluetoothWatcher Start second");
        RequireTrueSystemBluetooth(watcher.IsRunning(), "GB_SystemBluetoothWatcher running second", "Watcher should support starting again after Stop().");

        stopResult = watcher.Stop();
        RequireResultSucceeded(stopResult, "GB_SystemBluetoothWatcher Stop second");
        RequireTrueSystemBluetooth(!watcher.IsRunning(), "GB_SystemBluetoothWatcher stopped second", "Watcher should support stopping again after restart.");

        (void)callbackCount.load();
    }
}

int RunGB_SystemBluetoothTests()
{
    try
    {
        TestAddressHelpers();
        TestNameHelpers();
        TestRadioAndClassicEnumeration();
        TestUnsupportedAndInvalidOperations();
        TestWinRtBlePrimitiveLifecycle();
        TestWatcherLifecycle();
    }
    catch (const std::exception& exceptionObject)
    {
        std::cerr << "[FAILED] Unexpected exception\n" << exceptionObject.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "[FAILED] Unknown unexpected exception" << std::endl;
        return 1;
    }

    std::cout << "GB_SystemBluetooth tests passed. Total checks: " << totalSystemBluetoothCaseCount << std::endl;
    return 0;
}
