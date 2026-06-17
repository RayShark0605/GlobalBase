#include "GB_RunTests.h"
#include "Desktop/GB_SystemSession.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace
{
    int totalSystemSessionCaseCount = 0;

    [[noreturn]] void FailSystemSessionTest(const std::string& caseName, const std::string& detail)
    {
        std::cerr << "[FAILED] " << caseName << "\n" << detail << std::endl;
        std::exit(1);
    }

    void RequireTrueSystemSession(const bool condition, const std::string& caseName, const std::string& detail)
    {
        totalSystemSessionCaseCount++;
        if (!condition)
        {
            FailSystemSessionTest(caseName, detail);
        }
    }

    void RequireSessionResultSucceeded(const GB_SystemResult& result, const std::string& caseName)
    {
        totalSystemSessionCaseCount++;
        if (result.IsFailed())
        {
            FailSystemSessionTest(caseName, result.ToString());
        }
    }

    bool IsValidConnectStateForTest(const GB_SystemSessionConnectState connectState)
    {
        return connectState >= GB_SystemSessionConnectState::Unknown && connectState <= GB_SystemSessionConnectState::Init;
    }

    bool IsValidLockStateForTest(const GB_SystemSessionLockState lockState)
    {
        return lockState == GB_SystemSessionLockState::Unknown || lockState == GB_SystemSessionLockState::Locked || lockState == GB_SystemSessionLockState::Unlocked;
    }

    bool IsValidAvailabilityForTest(const GB_SystemSessionAutomationAvailability availability)
    {
        return availability >= GB_SystemSessionAutomationAvailability::Available && availability <= GB_SystemSessionAutomationAvailability::Unknown;
    }

    void TestSessionIds()
    {
#if defined(_WIN32)
        uint32_t currentSessionId = 0;
        RequireSessionResultSucceeded(GB_SystemSession::GetCurrentProcessSessionId(currentSessionId), "Get current process session id");

        DWORD nativeSessionId = 0;
        RequireTrueSystemSession(::ProcessIdToSessionId(::GetCurrentProcessId(), &nativeSessionId) != FALSE, "Native ProcessIdToSessionId", "ProcessIdToSessionId failed.");
        RequireTrueSystemSession(currentSessionId == static_cast<uint32_t>(nativeSessionId), "Current session id matches native", "GB_SystemSession session id mismatch.");

        uint32_t queriedSessionId = 0;
        RequireSessionResultSucceeded(GB_SystemSession::GetProcessSessionId(::GetCurrentProcessId(), queriedSessionId), "Get process session id");
        RequireTrueSystemSession(queriedSessionId == currentSessionId, "Process session id matches current", "GetProcessSessionId returned a different session.");

        const GB_SystemResult invalidProcessResult = GB_SystemSession::GetProcessSessionId(0, queriedSessionId);
        RequireTrueSystemSession(invalidProcessResult.IsFailed() && invalidProcessResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject zero process id", invalidProcessResult.ToString());

        uint32_t consoleSessionId = 0;
        bool consoleAvailable = false;
        RequireSessionResultSucceeded(GB_SystemSession::GetActiveConsoleSessionId(consoleSessionId, consoleAvailable), "Get active console session id");
        RequireTrueSystemSession(consoleAvailable || consoleSessionId == 0xFFFFFFFFu, "Console session unavailable value", "Unavailable console session should be represented by 0xFFFFFFFF.");
#endif
    }

    void TestSessionInfoAndEnumeration()
    {
        uint32_t currentSessionId = 0;
        RequireSessionResultSucceeded(GB_SystemSession::GetCurrentProcessSessionId(currentSessionId), "Get current session id for info");

        GB_SystemSessionInfo currentInfo;
        RequireSessionResultSucceeded(GB_SystemSession::GetCurrentSessionInfo(currentInfo), "Get current session info");
        RequireTrueSystemSession(currentInfo.sessionId == currentSessionId, "Current session info id", "Current session info used a different session id.");
        RequireTrueSystemSession(currentInfo.isCurrentProcessSession, "Current session info current flag", "Current session should be marked as current.");
        RequireTrueSystemSession(currentInfo.isSessionZero == (currentSessionId == 0), "Current session zero flag", "Session zero flag mismatch.");
        RequireTrueSystemSession(IsValidConnectStateForTest(currentInfo.connectState), "Current connect state valid", "Invalid connect state value.");
        RequireTrueSystemSession(IsValidLockStateForTest(currentInfo.lockedState), "Current lock state valid", "Invalid lock state value.");
        RequireTrueSystemSession(currentInfo.isProbablyInteractive == (currentSessionId != 0 && currentInfo.connectState == GB_SystemSessionConnectState::Active), "Current probably interactive flag", "Interactive flag should require a non-zero active session.");
        RequireTrueSystemSession(!GB_SystemSession::GetConnectStateName(currentInfo.connectState).empty(), "Connect state name", "Connect state name should not be empty.");
        RequireTrueSystemSession(!GB_SystemSession::GetLockStateName(currentInfo.lockedState).empty(), "Lock state name", "Lock state name should not be empty.");

        bool isSessionZero = false;
        RequireSessionResultSucceeded(GB_SystemSession::IsCurrentProcessSessionZero(isSessionZero), "Is current session zero");
        RequireTrueSystemSession(isSessionZero == (currentSessionId == 0), "Is session zero matches id", "IsCurrentProcessSessionZero mismatch.");

        bool isActiveConsoleSession = false;
        RequireSessionResultSucceeded(GB_SystemSession::IsCurrentProcessActiveConsoleSession(isActiveConsoleSession), "Is current active console");

        bool isRemoteSession = false;
        RequireSessionResultSucceeded(GB_SystemSession::IsCurrentProcessRemoteSession(isRemoteSession), "Is current remote session");
        RequireTrueSystemSession(isRemoteSession == currentInfo.isRemoteSession, "Remote session helper matches info", "Remote helper mismatch.");

        std::vector<GB_SystemSessionInfo> sessions;
        RequireSessionResultSucceeded(GB_SystemSession::EnumerateSessions(sessions), "Enumerate sessions");
        RequireTrueSystemSession(!sessions.empty(), "Enumerate sessions non-empty", "Expected at least one Windows session.");

        bool foundCurrentSession = false;
        for (size_t index = 0; index < sessions.size(); index++)
        {
            foundCurrentSession = foundCurrentSession || sessions[index].sessionId == currentSessionId;
            RequireTrueSystemSession(IsValidConnectStateForTest(sessions[index].connectState), "Enumerated connect state valid", "Invalid enumerated connect state.");
            RequireTrueSystemSession(IsValidLockStateForTest(sessions[index].lockedState), "Enumerated lock state valid", "Invalid enumerated lock state.");
        }
        RequireTrueSystemSession(foundCurrentSession, "Enumeration contains current session", "Current process session was not found in WTSEnumerateSessions.");
    }

    void TestIdleAndAvailability()
    {
        uint64_t idleMilliseconds = 0;
        RequireSessionResultSucceeded(GB_SystemSession::GetIdleMilliseconds(idleMilliseconds), "Get idle milliseconds");
        RequireTrueSystemSession(idleMilliseconds <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()), "Idle milliseconds bounded", "Idle milliseconds should fit the GetTickCount interval.");

        GB_SystemSessionAvailability availability;
        RequireSessionResultSucceeded(GB_SystemSession::CheckAutomationAvailability(availability), "Check automation availability");
        RequireTrueSystemSession(IsValidAvailabilityForTest(availability.availability), "Availability enum valid", "Invalid availability enum value.");
        RequireTrueSystemSession(availability.isAvailable == (availability.availability == GB_SystemSessionAutomationAvailability::Available), "Availability bool matches reason", "Availability bool and enum disagree.");
        RequireTrueSystemSession(!GB_SystemSession::GetAutomationAvailabilityName(availability.availability).empty(), "Availability name", "Availability name should not be empty.");
    }

    void TestWaitForLockState()
    {
        GB_SystemSessionWaitOptions invalidTimeoutOptions;
        invalidTimeoutOptions.timeoutMilliseconds = -2;
        GB_SystemResult result = GB_SystemSession::WaitForLockState(GB_SystemSessionLockState::Locked, invalidTimeoutOptions);
        RequireTrueSystemSession(result.IsFailed() && result.errorCode == GB_SystemErrorCode::InvalidArgument, "Wait invalid timeout", result.ToString());

        GB_SystemSessionWaitOptions invalidPollOptions;
        invalidPollOptions.pollIntervalMilliseconds = 0;
        result = GB_SystemSession::WaitForLockState(GB_SystemSessionLockState::Locked, invalidPollOptions);
        RequireTrueSystemSession(result.IsFailed() && result.errorCode == GB_SystemErrorCode::InvalidArgument, "Wait invalid poll", result.ToString());

        result = GB_SystemSession::WaitForLockState(GB_SystemSessionLockState::Unknown, GB_SystemSessionWaitOptions());
        RequireTrueSystemSession(result.IsFailed() && result.errorCode == GB_SystemErrorCode::InvalidArgument, "Wait invalid target lock state", result.ToString());

        std::atomic<bool> cancellationFlag(true);
        GB_SystemSessionWaitOptions cancelledOptions;
        cancelledOptions.cancellationFlag = &cancellationFlag;
        result = GB_SystemSession::WaitForLockState(GB_SystemSessionLockState::Locked, cancelledOptions);
        RequireTrueSystemSession(result.IsFailed() && result.errorCode == GB_SystemErrorCode::Cancelled, "Wait cancelled before start", result.ToString());

        GB_SystemSessionLockState knownLockState = GB_SystemSessionLockState::Unknown;
        RequireSessionResultSucceeded(GB_SystemSession::GetKnownLockState(knownLockState), "Get known lock state for timeout");
        const GB_SystemSessionLockState oppositeState = knownLockState == GB_SystemSessionLockState::Locked ? GB_SystemSessionLockState::Unlocked : GB_SystemSessionLockState::Locked;
        GB_SystemSessionWaitOptions timeoutOptions;
        timeoutOptions.timeoutMilliseconds = 0;
        timeoutOptions.pollIntervalMilliseconds = 1;
        result = GB_SystemSession::WaitForLockState(oppositeState, timeoutOptions);
        RequireTrueSystemSession(result.IsFailed() && result.errorCode == GB_SystemErrorCode::Timeout, "Wait immediate timeout", result.ToString());
    }

    void TestWatcherLifecycle()
    {
        GB_SystemSessionWatcher watcher;
        std::atomic<int> callbackCount(0);
        watcher.SetSessionEventCallback([&callbackCount](const GB_SystemSessionEvent&)
            {
                callbackCount++;
            });

        GB_EventSubscriptionToken subscriptionToken;
        RequireSessionResultSucceeded(watcher.GetEventDispatcher().SubscribeAll([](const GB_Event&)
            {
            }, subscriptionToken), "Session watcher public SubscribeAll");

        RequireSessionResultSucceeded(watcher.Start(), "Session watcher Start first");
        RequireTrueSystemSession(watcher.IsRunning(), "Session watcher running first", "Watcher should be running after Start().");
        RequireSessionResultSucceeded(watcher.Start(), "Session watcher duplicate Start");
        RequireTrueSystemSession(watcher.IsRunning(), "Session watcher running duplicate", "Watcher should remain running after duplicate Start().");

        RequireSessionResultSucceeded(watcher.Stop(), "Session watcher Stop first");
        RequireTrueSystemSession(!watcher.IsRunning(), "Session watcher stopped first", "Watcher should not be running after Stop().");
        RequireSessionResultSucceeded(watcher.Stop(), "Session watcher duplicate Stop");

        RequireSessionResultSucceeded(watcher.Start(), "Session watcher restart");
        RequireTrueSystemSession(watcher.IsRunning(), "Session watcher running after restart", "Watcher should restart after Stop().");
        RequireSessionResultSucceeded(watcher.Stop(), "Session watcher Stop after restart");

        GB_SystemSessionWatcherOptions invalidNativeQueueOptions;
        invalidNativeQueueOptions.maxPendingNativeEvents = 0;
        GB_SystemSessionWatcher invalidNativeQueueWatcher(invalidNativeQueueOptions);
        GB_SystemResult invalidStartResult = invalidNativeQueueWatcher.Start();
        RequireTrueSystemSession(invalidStartResult.IsFailed() && invalidStartResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Session watcher rejects zero native queue", invalidStartResult.ToString());

        GB_SystemSessionWatcherOptions invalidDispatchQueueOptions;
        invalidDispatchQueueOptions.maxDispatchQueueSize = 0;
        GB_SystemSessionWatcher invalidDispatchQueueWatcher(invalidDispatchQueueOptions);
        invalidStartResult = invalidDispatchQueueWatcher.Start();
        RequireTrueSystemSession(invalidStartResult.IsFailed() && invalidStartResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Session watcher rejects zero dispatch queue", invalidStartResult.ToString());

        GB_SystemSessionWatcher callbackStopWatcher;
        std::mutex callbackMutex;
        std::condition_variable callbackCondition;
        int callbackStopState = 0;
        callbackStopWatcher.SetSessionEventCallback([&callbackStopWatcher, &callbackMutex, &callbackCondition, &callbackStopState](const GB_SystemSessionEvent&)
            {
                const GB_SystemResult stopResult = callbackStopWatcher.Stop();
                {
                    std::lock_guard<std::mutex> lock(callbackMutex);
                    callbackStopState = stopResult.IsSucceeded() ? 1 : -1;
                }
                callbackCondition.notify_all();
            });

        RequireSessionResultSucceeded(callbackStopWatcher.Start(), "Session watcher Start for callback Stop");
        uint32_t currentSessionId = 0;
        RequireSessionResultSucceeded(GB_SystemSession::GetCurrentProcessSessionId(currentSessionId), "Get current session for synthetic callback Stop event");
        GB_SystemSessionEvent syntheticEvent;
        syntheticEvent.eventType = GB_SystemSessionEventType::DesktopReady;
        syntheticEvent.eventName = "SystemSession.DesktopReady";
        syntheticEvent.sessionId = currentSessionId;
        syntheticEvent.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
        RequireSessionResultSucceeded(callbackStopWatcher.GetEventDispatcher().Post(GB_Event(syntheticEvent.eventName, GB_Variant(syntheticEvent), "GB_SystemSession_Test")), "Post synthetic session event");

        {
            std::unique_lock<std::mutex> lock(callbackMutex);
            const bool callbackStopped = callbackCondition.wait_for(lock, std::chrono::seconds(5), [&callbackStopState]() { return callbackStopState != 0; });
            RequireTrueSystemSession(callbackStopped && callbackStopState == 1, "Session watcher Stop from callback", "Stopping watcher from callback did not complete.");
        }
        RequireTrueSystemSession(!callbackStopWatcher.IsRunning(), "Session watcher stopped by callback", "Watcher should not be running after callback Stop().");
        callbackStopWatcher.SetSessionEventCallback(GB_SystemSessionWatcher::SessionEventCallback());

        GB_SystemResult callbackRestartResult = callbackStopWatcher.Start();
        for (int retryIndex = 0; callbackRestartResult.IsFailed() && retryIndex < 20; retryIndex++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            callbackRestartResult = callbackStopWatcher.Start();
        }
        RequireSessionResultSucceeded(callbackRestartResult, "Session watcher restart after callback Stop");
        RequireSessionResultSucceeded(callbackStopWatcher.Stop(), "Session watcher final Stop after callback Stop");

        {
            GB_SystemSessionWatcher destructorWatcher;
            RequireSessionResultSucceeded(destructorWatcher.Start(), "Session watcher Start for destructor Stop");
        }
    }
}

int RunGB_SystemSessionTests()
{
#if !defined(_WIN32)
    std::cout << "GB_SystemSession tests skipped on non-Windows platform." << std::endl;
    return 0;
#else
    try
    {
        TestSessionIds();
        TestSessionInfoAndEnumeration();
        TestIdleAndAvailability();
        TestWaitForLockState();
        TestWatcherLifecycle();
        std::cout << "GB_SystemSession tests passed. Case count: " << totalSystemSessionCaseCount << std::endl;
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[FAILED] GB_SystemSession unexpected exception: " << exception.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "[FAILED] GB_SystemSession unknown exception." << std::endl;
        return 1;
    }
#endif
}
