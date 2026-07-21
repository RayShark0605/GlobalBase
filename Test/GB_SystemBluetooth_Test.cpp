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
        RequireResultFailed(lowEnergyResult, "GB_SystemBluetooth::GetLowEnergyDevices unsupported");
        RequireTrueSystemBluetooth(lowEnergyResult.errorCode == GB_SystemErrorCode::UnsupportedPlatform, "GB_SystemBluetooth BLE unsupported code", "BLE enumeration should explicitly report UnsupportedPlatform until WinRT implementation exists.");

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
        RequireResultFailed(pinPairResult, "GB_SystemBluetooth::PairDevice pin unsupported");
        RequireTrueSystemBluetooth(pinPairResult.errorCode == GB_SystemErrorCode::UnsupportedPlatform, "GB_SystemBluetooth pin unsupported code", "PIN pairing should explicitly report UnsupportedPlatform until custom authentication exists.");

        GB_BluetoothPairingOptions noUiOptions;
        noUiOptions.allowSystemPairingUi = false;
        const GB_SystemResult noUiPairResult = GB_SystemBluetooth::PairDevice(validFormatDeviceId, noUiOptions);
        RequireResultFailed(noUiPairResult, "GB_SystemBluetooth::PairDevice no UI unsupported");
        RequireTrueSystemBluetooth(noUiPairResult.errorCode == GB_SystemErrorCode::UnsupportedPlatform, "GB_SystemBluetooth no UI unsupported code", "Non-UI pairing should explicitly report UnsupportedPlatform until custom authentication exists.");
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
