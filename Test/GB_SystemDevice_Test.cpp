#include "GB_RunTests.h"
#include "Desktop/GB_SystemDevice.h"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    const std::string kGuidDevinterfaceUsbDevice = "{A5DCBF10-6530-11D2-901F-00C04FB951ED}";
    const std::string kGuidDevinterfaceHid = "{4D1E55B2-F16F-11CF-88CB-001111000030}";
    const std::string kGuidDevinterfaceDisk = "{53F56307-B6BF-11D0-94F2-00A0C91EFB8B}";
    const std::string kGuidDevinterfaceVolume = "{53F5630D-B6BF-11D0-94F2-00A0C91EFB8B}";
    const std::string kGuidDevinterfaceMonitor = "{E6F07B5F-EE97-4A90-B076-33F57BF4EAA7}";

    int totalSystemDeviceCaseCount = 0;

    [[noreturn]] void FailSystemDeviceTest(const std::string& caseName, const std::string& detail)
    {
        std::cerr << "[FAILED] " << caseName << "\n" << detail << std::endl;
        std::exit(1);
    }

    void RequireTrueSystemDevice(const bool condition, const std::string& caseName, const std::string& detail)
    {
        totalSystemDeviceCaseCount++;
        if (!condition)
        {
            FailSystemDeviceTest(caseName, detail);
        }
    }

    void RequireResultSucceeded(const GB_SystemResult& result, const std::string& caseName)
    {
        totalSystemDeviceCaseCount++;
        if (result.IsFailed())
        {
            FailSystemDeviceTest(caseName, result.ToString());
        }
    }

    bool HasAnyIdentity(const GB_SystemDeviceInfo& deviceInfo)
    {
        return !deviceInfo.instanceId.empty() || !deviceInfo.friendlyName.empty() || !deviceInfo.description.empty();
    }

    void TestDeviceEnumeration()
    {
        GB_SystemDeviceQueryOptions options;
        options.presentOnly = true;
        options.readDriverInfo = true;

        std::vector<GB_SystemDeviceInfo> devices;
        const GB_SystemResult devicesResult = GB_SystemDevice::GetDevices(devices, options);
        RequireResultSucceeded(devicesResult, "GB_SystemDevice::GetDevices present");
        RequireTrueSystemDevice(!devices.empty(), "GB_SystemDevice::GetDevices non-empty", "Expected at least one present system device.");

        bool foundDeviceWithIdentity = false;
        bool foundPresentDevice = false;
        bool foundStatusFields = false;
        bool foundClassGuid = false;
        for (size_t index = 0; index < devices.size(); index++)
        {
            foundDeviceWithIdentity = foundDeviceWithIdentity || HasAnyIdentity(devices[index]);
            foundPresentDevice = foundPresentDevice || devices[index].isPresent;
            foundStatusFields = foundStatusFields || devices[index].devNodeStatus != 0 || devices[index].problemCode != 0;
            foundClassGuid = foundClassGuid || !devices[index].classGuid.empty();
            RequireTrueSystemDevice(!(devices[index].isDisabled && !devices[index].hasProblem), "GB_SystemDevice disabled consistency", "Disabled device should also report a problem state.");
        }

        RequireTrueSystemDevice(foundDeviceWithIdentity, "GB_SystemDevice identity fields", "Expected at least one device with instance id, friendly name, or description.");
        RequireTrueSystemDevice(foundPresentDevice, "GB_SystemDevice present flag", "Expected at least one device to be marked present.");
        RequireTrueSystemDevice(foundStatusFields, "GB_SystemDevice status fields", "Expected at least one device to expose devnode status or problem fields.");
        RequireTrueSystemDevice(foundClassGuid, "GB_SystemDevice class guid", "Expected at least one device to expose a setup class GUID.");

        std::vector<GB_SystemDeviceInfo> usbDevices;
        const GB_SystemResult usbResult = GB_SystemDevice::GetDevicesByKind(GB_SystemDeviceKind::Usb, usbDevices, true);
        RequireResultSucceeded(usbResult, "GB_SystemDevice::GetDevicesByKind Usb");
        for (size_t index = 0; index < usbDevices.size(); index++)
        {
            RequireTrueSystemDevice(usbDevices[index].deviceKind == GB_SystemDeviceKind::Usb, "GB_SystemDevice USB kind filter", "GetDevicesByKind(Usb) returned a non-USB device.");
        }
    }

    void TestDeviceLookup()
    {
        std::vector<GB_SystemDeviceInfo> devices;
        const GB_SystemResult devicesResult = GB_SystemDevice::GetDevices(devices, GB_SystemDeviceQueryOptions());
        RequireResultSucceeded(devicesResult, "GB_SystemDevice::GetDevices lookup source");
        RequireTrueSystemDevice(!devices.empty(), "GB_SystemDevice lookup source non-empty", "Expected at least one device for lookup tests.");

        std::string instanceId;
        for (size_t index = 0; index < devices.size(); index++)
        {
            if (!devices[index].instanceId.empty())
            {
                instanceId = devices[index].instanceId;
                break;
            }
        }
        RequireTrueSystemDevice(!instanceId.empty(), "GB_SystemDevice lookup instance id", "Expected at least one device with instance id.");

        GB_SystemDeviceInfo deviceInfo;
        bool found = false;
        const GB_SystemResult lookupResult = GB_SystemDevice::GetDeviceByInstanceId(instanceId, deviceInfo, found);
        RequireResultSucceeded(lookupResult, "GB_SystemDevice::GetDeviceByInstanceId");
        RequireTrueSystemDevice(found, "GB_SystemDevice lookup found", "Expected GetDeviceByInstanceId to find a known present device.");
        RequireTrueSystemDevice(deviceInfo.instanceId == instanceId, "GB_SystemDevice lookup identity", "Lookup should return the requested device instance id.");

        bool exists = false;
        const GB_SystemResult existsResult = GB_SystemDevice::DeviceExists(instanceId, exists);
        RequireResultSucceeded(existsResult, "GB_SystemDevice::DeviceExists");
        RequireTrueSystemDevice(exists, "GB_SystemDevice exists", "Known present device should exist.");
    }

    void TestDeviceInterfaceEnumeration()
    {
        GB_SystemDeviceInterfaceQueryOptions options;
        options.presentOnly = true;
        options.enumerateAllInstalledInterfaceClasses = true;

        std::vector<GB_SystemDeviceInterfaceInfo> deviceInterfaces;
        const GB_SystemResult result = GB_SystemDevice::GetDeviceInterfaces(deviceInterfaces, options);
        RequireResultSucceeded(result, "GB_SystemDevice::GetDeviceInterfaces all");

        GB_SystemDeviceInterfaceQueryOptions invalidOptions;
        invalidOptions.presentOnly = true;
        invalidOptions.enumerateAllInstalledInterfaceClasses = false;
        std::vector<GB_SystemDeviceInterfaceInfo> invalidInterfaces;
        const GB_SystemResult invalidResult = GB_SystemDevice::GetDeviceInterfaces(invalidInterfaces, invalidOptions);
        RequireTrueSystemDevice(invalidResult.IsFailed(), "GB_SystemDevice interface invalid options", "Empty interfaceClassGuid with disabled all-class enumeration should fail.");

        for (size_t index = 0; index < deviceInterfaces.size(); index++)
        {
            RequireTrueSystemDevice(!deviceInterfaces[index].interfacePath.empty(), "GB_SystemDevice interface path", "Device interface path should not be empty.");
            RequireTrueSystemDevice(!deviceInterfaces[index].interfaceClassGuid.empty(), "GB_SystemDevice interface class guid", "Device interface class GUID should not be empty.");
            RequireTrueSystemDevice(deviceInterfaces[index].isPresent, "GB_SystemDevice present interface", "Present-only interface enumeration returned a non-present interface.");
            RequireTrueSystemDevice(deviceInterfaces[index].isEnabled, "GB_SystemDevice enabled interface", "Present-only interface enumeration returned a disabled interface.");
        }

        const std::string interfaceGuids[] = { kGuidDevinterfaceUsbDevice, kGuidDevinterfaceHid, kGuidDevinterfaceDisk, kGuidDevinterfaceVolume, kGuidDevinterfaceMonitor };
        for (size_t index = 0; index < sizeof(interfaceGuids) / sizeof(interfaceGuids[0]); index++)
        {
            std::vector<GB_SystemDeviceInterfaceInfo> typedInterfaces;
            const GB_SystemResult typedResult = GB_SystemDevice::GetDeviceInterfacesByClassGuid(interfaceGuids[index], typedInterfaces, true);
            RequireResultSucceeded(typedResult, "GB_SystemDevice::GetDeviceInterfacesByClassGuid");
            for (size_t interfaceIndex = 0; interfaceIndex < typedInterfaces.size(); interfaceIndex++)
            {
                RequireTrueSystemDevice(!typedInterfaces[interfaceIndex].interfacePath.empty(), "GB_SystemDevice typed interface path", "Typed interface path should not be empty.");
                RequireTrueSystemDevice(typedInterfaces[interfaceIndex].interfaceClassGuid == interfaceGuids[index], "GB_SystemDevice typed interface guid", "Typed interface enumeration returned a different class GUID.");
            }
        }
    }

    void TestWatcherLifecycle()
    {
        GB_SystemDeviceWatcher watcher;
        std::atomic<int> callbackCount(0);
        watcher.SetDeviceEventCallback([&callbackCount](const GB_SystemDeviceEvent&)
            {
                callbackCount++;
            });

        GB_EventSubscriptionToken subscriptionToken;
        const GB_SystemResult subscribeResult = watcher.GetEventDispatcher().SubscribeAll([](const GB_Event&)
            {
            }, subscriptionToken);
        RequireResultSucceeded(subscribeResult, "GB_SystemDeviceWatcher public SubscribeAll");

        GB_SystemResult startResult = watcher.Start();
        RequireResultSucceeded(startResult, "GB_SystemDeviceWatcher Start first");
        RequireTrueSystemDevice(watcher.IsRunning(), "GB_SystemDeviceWatcher running first", "Watcher should be running after Start().");

        startResult = watcher.Start();
        RequireResultSucceeded(startResult, "GB_SystemDeviceWatcher Start already running");
        RequireTrueSystemDevice(watcher.IsRunning(), "GB_SystemDeviceWatcher running duplicate", "Watcher should remain running after duplicate Start().");

        GB_SystemResult stopResult = watcher.Stop();
        RequireResultSucceeded(stopResult, "GB_SystemDeviceWatcher Stop first");
        RequireTrueSystemDevice(!watcher.IsRunning(), "GB_SystemDeviceWatcher stopped first", "Watcher should not be running after Stop().");

        stopResult = watcher.Stop();
        RequireResultSucceeded(stopResult, "GB_SystemDeviceWatcher Stop already stopped");
        RequireTrueSystemDevice(!watcher.IsRunning(), "GB_SystemDeviceWatcher stopped duplicate", "Watcher should remain stopped after duplicate Stop().");

        startResult = watcher.Start();
        RequireResultSucceeded(startResult, "GB_SystemDeviceWatcher Start second");
        RequireTrueSystemDevice(watcher.IsRunning(), "GB_SystemDeviceWatcher running second", "Watcher should support starting again after Stop().");

        stopResult = watcher.Stop();
        RequireResultSucceeded(stopResult, "GB_SystemDeviceWatcher Stop second");
        RequireTrueSystemDevice(!watcher.IsRunning(), "GB_SystemDeviceWatcher stopped second", "Watcher should support stopping again after restart.");

        (void)callbackCount.load();
    }
}

int RunGB_SystemDeviceTests()
{
    try
    {
        TestDeviceEnumeration();
        TestDeviceLookup();
        TestDeviceInterfaceEnumeration();
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

    std::cout << "GB_SystemDevice tests passed. Total checks: " << totalSystemDeviceCaseCount << std::endl;
    return 0;
}
