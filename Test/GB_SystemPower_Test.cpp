#include "GB_RunTests.h"
#include "Desktop/GB_SystemPower.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int totalSystemPowerCaseCount = 0;

    [[noreturn]] void FailSystemPowerTest(const std::string& caseName, const std::string& detail)
    {
        std::cerr << "[FAILED] " << caseName << "\n" << detail << std::endl;
        std::exit(1);
    }

    void RequireTrueSystemPower(const bool condition, const std::string& caseName, const std::string& detail)
    {
        totalSystemPowerCaseCount++;
        if (!condition)
        {
            FailSystemPowerTest(caseName, detail);
        }
    }

    void RequirePowerResultSucceeded(const GB_SystemResult& result, const std::string& caseName)
    {
        totalSystemPowerCaseCount++;
        if (result.IsFailed())
        {
            FailSystemPowerTest(caseName, result.ToString());
        }
    }

    bool IsValidPowerSourceForTest(const GB_SystemPowerSource powerSource)
    {
        return powerSource == GB_SystemPowerSource::Unknown || powerSource == GB_SystemPowerSource::AC || powerSource == GB_SystemPowerSource::Battery || powerSource == GB_SystemPowerSource::Offline;
    }

    bool IsValidSleepStateForTest(const GB_SystemPowerSleepState sleepState)
    {
        return sleepState >= GB_SystemPowerSleepState::Unknown && sleepState <= GB_SystemPowerSleepState::Shutdown;
    }

    void TestPowerStatusAndCapabilities()
    {
        GB_SystemPowerStatus powerStatus;
        RequirePowerResultSucceeded(GB_SystemPower::GetPowerStatus(powerStatus), "GB_SystemPower::GetPowerStatus");
        RequireTrueSystemPower(IsValidPowerSourceForTest(powerStatus.powerSource), "Power status source valid", "Invalid power source enum value.");
        RequireTrueSystemPower(!GB_SystemPower::GetPowerSourceName(powerStatus.powerSource).empty(), "Power source name", "Power source name should not be empty.");
        if (powerStatus.hasBatteryPercent)
        {
            RequireTrueSystemPower(powerStatus.batteryPercent <= 100, "Battery percent range", "Battery percent should be in [0, 100].");
        }
        RequireTrueSystemPower(powerStatus.timestampMilliseconds != 0, "Power status timestamp", "Power status timestamp should be filled.");

        GB_SystemPowerCapabilities capabilities;
        RequirePowerResultSucceeded(GB_SystemPower::GetPowerCapabilities(capabilities), "GB_SystemPower::GetPowerCapabilities");
        RequireTrueSystemPower(capabilities.supportsSleep == (capabilities.supportsS1 || capabilities.supportsS2 || capabilities.supportsS3 || capabilities.supportsAoAc), "Sleep capability consistency", "supportsSleep should summarize S1/S2/S3/AoAc.");
        RequireTrueSystemPower(capabilities.supportsHibernate == (capabilities.supportsS4 && capabilities.hasHibernationFile), "Hibernate capability consistency", "supportsHibernate should require S4 and hibernation file.");
        RequireTrueSystemPower(IsValidSleepStateForTest(capabilities.acOnlineWakeState), "AC wake state valid", "Invalid AC wake state.");
        RequireTrueSystemPower(IsValidSleepStateForTest(capabilities.rtcWakeState), "RTC wake state valid", "Invalid RTC wake state.");
        RequireTrueSystemPower(!GB_SystemPower::GetSleepStateName(capabilities.minDeviceWakeState).empty(), "Sleep state name", "Sleep state name should not be empty.");
    }

    void TestPowerPlans()
    {
        std::vector<GB_SystemPowerPlanInfo> powerPlans;
        RequirePowerResultSucceeded(GB_SystemPower::EnumeratePowerPlans(powerPlans), "GB_SystemPower::EnumeratePowerPlans");
        RequireTrueSystemPower(!powerPlans.empty(), "Power plans non-empty", "Expected at least one power plan.");

        GB_SystemPowerPlanInfo activePlan;
        RequirePowerResultSucceeded(GB_SystemPower::GetActivePowerPlan(activePlan), "GB_SystemPower::GetActivePowerPlan");
        RequireTrueSystemPower(!activePlan.schemeGuid.empty(), "Active power plan guid", "Active power plan GUID should not be empty.");
        RequireTrueSystemPower(activePlan.isActive, "Active power plan flag", "GetActivePowerPlan should mark returned plan as active.");
        RequireTrueSystemPower(!GB_SystemPower::GetPowerPlanPersonalityName(activePlan.personality).empty(), "Power plan personality name", "Power plan personality name should not be empty.");

        bool foundActivePlan = false;
        for (size_t index = 0; index < powerPlans.size(); index++)
        {
            RequireTrueSystemPower(!powerPlans[index].schemeGuid.empty(), "Enumerated power plan guid", "Enumerated power plan GUID should not be empty.");
            foundActivePlan = foundActivePlan || powerPlans[index].isActive;
        }
        RequireTrueSystemPower(foundActivePlan, "Enumerated active power plan", "Enumerated power plans should include active plan.");

        uint32_t valueIndex = 0;
        const GB_SystemResult invalidReadResult = GB_SystemPower::ReadPowerSettingIndex(activePlan.schemeGuid, "{00000000-0000-0000-0000-000000000000}", "not-a-guid", true, valueIndex);
        RequireTrueSystemPower(invalidReadResult.IsFailed() && invalidReadResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Read setting rejects invalid GUID", invalidReadResult.ToString());

        const GB_SystemResult invalidSetResult = GB_SystemPower::SetActivePowerPlan("not-a-guid");
        RequireTrueSystemPower(invalidSetResult.IsFailed() && invalidSetResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Set active plan rejects invalid GUID", invalidSetResult.ToString());
    }

    void TestKeepAwakeRequest()
    {
        GB_SystemPowerKeepAwakeRequest request;

        GB_SystemPowerKeepAwakeOptions invalidOptions;
        invalidOptions.reasonUtf8 = "";
        GB_SystemResult result = GB_SystemPower::CreateKeepAwakeRequest(invalidOptions, request);
        RequireTrueSystemPower(result.IsFailed() && result.errorCode == GB_SystemErrorCode::InvalidArgument, "Keep awake rejects empty reason", result.ToString());
        RequireTrueSystemPower(!request.IsActive(), "Invalid keep awake leaves inactive request", "Invalid request should not become active.");

        GB_SystemPowerKeepAwakeOptions options;
        options.mode = GB_SystemPowerKeepAwakeMode::System;
        options.reasonUtf8 = "GB_SystemPower test keep awake request";
        RequirePowerResultSucceeded(GB_SystemPower::CreateKeepAwakeRequest(options, request), "Create keep awake request");
        RequireTrueSystemPower(request.IsActive(), "Keep awake request active", "Request should be active after creation.");
        RequireTrueSystemPower(request.GetOptions().reasonUtf8 == options.reasonUtf8, "Keep awake options preserved", "Request options should be preserved.");

        GB_SystemPowerKeepAwakeRequest movedRequest(std::move(request));
        RequireTrueSystemPower(!request.IsActive(), "Moved-from keep awake inactive", "Moved-from request should be inactive.");
        RequireTrueSystemPower(movedRequest.IsActive(), "Moved keep awake active", "Moved request should remain active.");
        RequirePowerResultSucceeded(movedRequest.Release(), "Release keep awake request");
        RequireTrueSystemPower(!movedRequest.IsActive(), "Released keep awake inactive", "Released request should be inactive.");
        RequirePowerResultSucceeded(movedRequest.Release(), "Release keep awake request twice");
    }

    void TestActionValidation()
    {
        GB_SystemPowerActionOptions invalidActionOptions;
        invalidActionOptions.actionType = static_cast<GB_SystemPowerActionType>(999);
        GB_SystemResult result = GB_SystemPower::RequestPowerAction(invalidActionOptions);
        RequireTrueSystemPower(result.IsFailed() && result.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject invalid power action", result.ToString());

        GB_SystemPowerActionOptions conflictingForceOptions;
        conflictingForceOptions.forceApplications = true;
        conflictingForceOptions.forceIfHung = true;
        result = GB_SystemPower::RequestPowerAction(conflictingForceOptions);
        RequireTrueSystemPower(result.IsFailed() && result.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject conflicting force options", result.ToString());

        GB_SystemPowerActionOptions restartAppsOptions;
        restartAppsOptions.actionType = GB_SystemPowerActionType::Shutdown;
        restartAppsOptions.rebootRegisteredApplications = true;
        result = GB_SystemPower::RequestPowerAction(restartAppsOptions);
        RequireTrueSystemPower(result.IsFailed() && result.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject restart apps without reboot", result.ToString());

        RequireTrueSystemPower(!GB_SystemPower::GetPowerActionTypeName(GB_SystemPowerActionType::Shutdown).empty(), "Power action type name", "Power action type name should not be empty.");
        RequireTrueSystemPower(!GB_SystemPower::GetKeepAwakeModeName(GB_SystemPowerKeepAwakeMode::SystemAndDisplay).empty(), "Keep awake mode name", "Keep awake mode name should not be empty.");
        RequireTrueSystemPower(!GB_SystemPower::GetPowerEventTypeName(GB_SystemPowerEventType::PowerStatusChanged).empty(), "Power event type name", "Power event type name should not be empty.");
    }

    void TestWatcherLifecycle()
    {
        GB_SystemPowerWatcher watcher;
        std::atomic<int> callbackCount(0);
        watcher.SetPowerEventCallback([&callbackCount](const GB_SystemPowerEvent&)
            {
                callbackCount++;
            });

        GB_EventSubscriptionToken subscriptionToken;
        RequirePowerResultSucceeded(watcher.GetEventDispatcher().SubscribeAll([](const GB_Event&)
            {
            }, subscriptionToken), "Power watcher public SubscribeAll");

        RequirePowerResultSucceeded(watcher.Start(), "Power watcher Start first");
        RequireTrueSystemPower(watcher.IsRunning(), "Power watcher running first", "Watcher should be running after Start().");
        RequirePowerResultSucceeded(watcher.Start(), "Power watcher duplicate Start");
        RequireTrueSystemPower(watcher.IsRunning(), "Power watcher running duplicate", "Watcher should remain running after duplicate Start().");

        RequirePowerResultSucceeded(watcher.Stop(), "Power watcher Stop first");
        RequireTrueSystemPower(!watcher.IsRunning(), "Power watcher stopped first", "Watcher should not be running after Stop().");
        RequirePowerResultSucceeded(watcher.Stop(), "Power watcher duplicate Stop");

        RequirePowerResultSucceeded(watcher.Start(), "Power watcher restart");
        RequireTrueSystemPower(watcher.IsRunning(), "Power watcher running after restart", "Watcher should restart after Stop().");
        RequirePowerResultSucceeded(watcher.Stop(), "Power watcher Stop after restart");

        GB_SystemPowerWatcherOptions invalidNativeQueueOptions;
        invalidNativeQueueOptions.maxPendingNativeEvents = 0;
        GB_SystemPowerWatcher invalidNativeQueueWatcher(invalidNativeQueueOptions);
        GB_SystemResult invalidStartResult = invalidNativeQueueWatcher.Start();
        RequireTrueSystemPower(invalidStartResult.IsFailed() && invalidStartResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Power watcher rejects zero native queue", invalidStartResult.ToString());

        GB_SystemPowerWatcherOptions invalidDispatchQueueOptions;
        invalidDispatchQueueOptions.maxDispatchQueueSize = 0;
        GB_SystemPowerWatcher invalidDispatchQueueWatcher(invalidDispatchQueueOptions);
        invalidStartResult = invalidDispatchQueueWatcher.Start();
        RequireTrueSystemPower(invalidStartResult.IsFailed() && invalidStartResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Power watcher rejects zero dispatch queue", invalidStartResult.ToString());

        GB_SystemPowerWatcher callbackStopWatcher;
        std::mutex callbackMutex;
        std::condition_variable callbackCondition;
        int callbackStopState = 0;
        callbackStopWatcher.SetPowerEventCallback([&callbackStopWatcher, &callbackMutex, &callbackCondition, &callbackStopState](const GB_SystemPowerEvent&)
            {
                const GB_SystemResult stopResult = callbackStopWatcher.Stop();
                {
                    std::lock_guard<std::mutex> lock(callbackMutex);
                    callbackStopState = stopResult.IsSucceeded() ? 1 : -1;
                }
                callbackCondition.notify_all();
            });

        RequirePowerResultSucceeded(callbackStopWatcher.Start(), "Power watcher Start for callback Stop");
        GB_SystemPowerEvent syntheticEvent;
        syntheticEvent.eventType = GB_SystemPowerEventType::PowerStatusChanged;
        syntheticEvent.eventName = "SystemPower.PowerStatusChanged";
        syntheticEvent.sourceName = "GB_SystemPower_Test";
        syntheticEvent.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
        RequirePowerResultSucceeded(callbackStopWatcher.GetEventDispatcher().Post(GB_Event(syntheticEvent.eventName, GB_Variant(syntheticEvent), syntheticEvent.sourceName)), "Post synthetic power event");

        {
            std::unique_lock<std::mutex> lock(callbackMutex);
            const bool callbackStopped = callbackCondition.wait_for(lock, std::chrono::seconds(5), [&callbackStopState]() { return callbackStopState != 0; });
            RequireTrueSystemPower(callbackStopped && callbackStopState == 1, "Power watcher Stop from callback", "Stopping watcher from callback did not complete.");
        }
        RequireTrueSystemPower(!callbackStopWatcher.IsRunning(), "Power watcher stopped by callback", "Watcher should not be running after callback Stop().");
        callbackStopWatcher.SetPowerEventCallback(GB_SystemPowerWatcher::PowerEventCallback());

        GB_SystemResult callbackRestartResult = callbackStopWatcher.Start();
        for (int retryIndex = 0; callbackRestartResult.IsFailed() && retryIndex < 20; retryIndex++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            callbackRestartResult = callbackStopWatcher.Start();
        }
        RequirePowerResultSucceeded(callbackRestartResult, "Power watcher restart after callback Stop");
        RequirePowerResultSucceeded(callbackStopWatcher.Stop(), "Power watcher final Stop after callback Stop");

        {
            GB_SystemPowerWatcher destructorWatcher;
            RequirePowerResultSucceeded(destructorWatcher.Start(), "Power watcher Start for destructor Stop");
        }

        (void)callbackCount.load();
    }
}

int RunGB_SystemPowerTests()
{
#if !defined(_WIN32)
    std::cout << "GB_SystemPower tests skipped on non-Windows platform." << std::endl;
    return 0;
#else
    try
    {
        TestPowerStatusAndCapabilities();
        TestPowerPlans();
        TestKeepAwakeRequest();
        TestActionValidation();
        TestWatcherLifecycle();
        std::cout << "GB_SystemPower tests passed. Case count: " << totalSystemPowerCaseCount << std::endl;
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAILED] GB_SystemPower unexpected exception: " << exception.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "[FAILED] GB_SystemPower unknown exception." << std::endl;
        return 1;
    }
#endif
}
