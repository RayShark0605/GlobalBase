#include "GB_RunTests.h"
#include "Desktop/GB_SystemWindow.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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
    int totalSystemWindowCaseCount = 0;

    [[noreturn]] void FailSystemWindowTest(const std::string& caseName, const std::string& detail)
    {
        std::cerr << "[FAILED] " << caseName << "\n" << detail << std::endl;
        std::exit(1);
    }

    void RequireTrueSystemWindow(const bool condition, const std::string& caseName, const std::string& detail)
    {
        totalSystemWindowCaseCount++;
        if (!condition)
        {
            FailSystemWindowTest(caseName, detail);
        }
    }

    void RequireWindowResultSucceeded(const GB_SystemResult& result, const std::string& caseName)
    {
        totalSystemWindowCaseCount++;
        if (result.IsFailed())
        {
            FailSystemWindowTest(caseName, result.ToString());
        }
    }

#if defined(_WIN32)
    const wchar_t* const TestWindowClassName = L"GB_SystemWindow_Deterministic_Test_Class";
    const wchar_t* const InitialWindowTitle = L"GB Window Test \x4E2D\x6587";
    const wchar_t* const ChangedWindowTitle = L"GB Window Changed \x7A97\x53E3";

    class TestWindowHost final
    {
    public:
        TestWindowHost() = default;

        ~TestWindowHost()
        {
            Stop();
        }

        bool Start(const wchar_t* title = InitialWindowTitle)
        {
            Stop();
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                requestedTitle = title == nullptr ? L"" : title;
                startCompleted = false;
                startSucceeded = false;
            }

            windowThread = std::thread(&TestWindowHost::ThreadMain, this);
            std::unique_lock<std::mutex> lock(stateMutex);
            startCondition.wait(lock, [this]() { return startCompleted; });
            return startSucceeded;
        }

        void Stop()
        {
            const HWND windowHandle = GetWindowHandle();
            if (windowHandle != nullptr)
            {
                (void)::PostMessageW(windowHandle, WM_CLOSE, 0, 0);
            }
            if (windowThread.joinable())
            {
                windowThread.join();
            }
        }

        HWND GetWindowHandle() const
        {
            return reinterpret_cast<HWND>(windowHandleValue.load(std::memory_order_acquire));
        }

        HWND GetChildWindowHandle() const
        {
            return reinterpret_cast<HWND>(childWindowHandleValue.load(std::memory_order_acquire));
        }

        bool SetTitle(const wchar_t* title)
        {
            const HWND windowHandle = GetWindowHandle();
            return windowHandle != nullptr && ::SetWindowTextW(windowHandle, title) != FALSE;
        }

    private:
        static LRESULT CALLBACK WindowProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message == WM_CLOSE)
            {
                (void)::DestroyWindow(windowHandle);
                return 0;
            }
            if (message == WM_DESTROY)
            {
                ::PostQuitMessage(0);
                return 0;
            }
            return ::DefWindowProcW(windowHandle, message, wParam, lParam);
        }

        void SignalStart(const bool succeeded)
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            startSucceeded = succeeded;
            startCompleted = true;
            startCondition.notify_all();
        }

        void ThreadMain()
        {
            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = &TestWindowHost::WindowProc;
            windowClass.hInstance = ::GetModuleHandleW(nullptr);
            windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            windowClass.lpszClassName = TestWindowClassName;
            const ATOM classAtom = ::RegisterClassExW(&windowClass);
            if (classAtom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                SignalStart(false);
                return;
            }

            std::wstring title;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                title = requestedTitle;
            }
            const HWND windowHandle = ::CreateWindowExW(WS_EX_APPWINDOW, TestWindowClassName, title.c_str(), WS_OVERLAPPEDWINDOW, 120, 100, 480, 360, nullptr, nullptr, windowClass.hInstance, nullptr);
            if (windowHandle == nullptr)
            {
                SignalStart(false);
                return;
            }
            const HWND childWindowHandle = ::CreateWindowExW(0, L"STATIC", L"GB Child Window", WS_CHILD | WS_VISIBLE, 10, 10, 120, 30, windowHandle, nullptr, windowClass.hInstance, nullptr);
            if (childWindowHandle == nullptr)
            {
                (void)::DestroyWindow(windowHandle);
                SignalStart(false);
                return;
            }

            windowHandleValue.store(reinterpret_cast<uintptr_t>(windowHandle), std::memory_order_release);
            childWindowHandleValue.store(reinterpret_cast<uintptr_t>(childWindowHandle), std::memory_order_release);
            (void)::ShowWindow(windowHandle, SW_SHOWNOACTIVATE);
            (void)::UpdateWindow(windowHandle);
            SignalStart(true);

            MSG message = {};
            while (::GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }

            childWindowHandleValue.store(0, std::memory_order_release);
            windowHandleValue.store(0, std::memory_order_release);
        }

    private:
        mutable std::mutex stateMutex;
        std::condition_variable startCondition;
        std::thread windowThread;
        std::wstring requestedTitle;
        std::atomic<uintptr_t> windowHandleValue{ 0 };
        std::atomic<uintptr_t> childWindowHandleValue{ 0 };
        bool startCompleted = false;
        bool startSucceeded = false;
    };

    bool WaitForWindowState(const GB_WindowId& windowId, const std::function<bool(const GB_WindowInfo&)>& predicate, GB_WindowInfo& windowInfo, const uint32_t timeoutMilliseconds = 5000)
    {
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const GB_SystemResult result = GB_SystemWindow::GetWindowInfo(windowId, windowInfo);
            if (result.IsSucceeded() && predicate(windowInfo))
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    GB_WindowInfo FindTestWindow(const std::string& titleContains)
    {
        GB_WindowFindOptions options;
        options.titleContains = titleContains;
        options.classNameEquals = "GB_SystemWindow_Deterministic_Test_Class";
        options.processId = ::GetCurrentProcessId();

        GB_WindowInfo windowInfo;
        bool found = false;
        RequireWindowResultSucceeded(GB_SystemWindow::FindFirstWindow(options, windowInfo, found), "Find deterministic test window");
        RequireTrueSystemWindow(found, "Find deterministic test window found", "Expected the deterministic test window.");
        return windowInfo;
    }

    void TestWindowIdAndQueries(TestWindowHost& host, GB_WindowInfo& windowInfo)
    {
        windowInfo = FindTestWindow("window test");
        RequireTrueSystemWindow(windowInfo.windowId.IsValid(), "Window id valid", "Expected a valid window id.");
        RequireTrueSystemWindow(windowInfo.windowId.processId == ::GetCurrentProcessId(), "Window process id", "Window process id mismatch.");
        RequireTrueSystemWindow(windowInfo.hasClassName && windowInfo.className == "GB_SystemWindow_Deterministic_Test_Class", "Window class name", windowInfo.className);
        RequireTrueSystemWindow(windowInfo.hasTitle && windowInfo.title.find("GB Window Test") != std::string::npos, "Window Unicode title", windowInfo.title);
        RequireTrueSystemWindow(windowInfo.hasProcessPath && !windowInfo.processPath.empty(), "Window process path", "Expected the current process path.");
        RequireTrueSystemWindow(windowInfo.hasWindowRectangle && windowInfo.windowRectangle.IsValid(), "Window rectangle", "Expected a valid window rectangle.");
        RequireTrueSystemWindow(windowInfo.hasClientRectangle && windowInfo.clientRectangle.IsValid(), "Client rectangle", "Expected a valid client rectangle.");
        RequireTrueSystemWindow(windowInfo.hasStyle && windowInfo.hasExtendedStyle, "Window styles available", "Expected window style snapshots.");
        RequireTrueSystemWindow(windowInfo.hasCloakedState, "Window cloaked state available", "Expected DWM cloaked state on the test window.");
        RequireTrueSystemWindow(windowInfo.isVisible && !windowInfo.isChildWindow, "Top-level visible state", "Expected a visible top-level window.");

        std::vector<GB_WindowInfo> topLevelWindows;
        RequireWindowResultSucceeded(GB_SystemWindow::GetTopLevelWindows(topLevelWindows), "Get top-level windows");
        bool foundTopLevel = false;
        for (size_t index = 0; index < topLevelWindows.size(); index++)
        {
            foundTopLevel = foundTopLevel || topLevelWindows[index].windowId == windowInfo.windowId;
        }
        RequireTrueSystemWindow(foundTopLevel, "Top-level enumeration contains test window", "Test window was not enumerated.");

        std::vector<GB_WindowInfo> childWindows;
        RequireWindowResultSucceeded(GB_SystemWindow::GetChildWindows(windowInfo.windowId, childWindows, true), "Get child windows");
        bool foundChild = false;
        for (size_t index = 0; index < childWindows.size(); index++)
        {
            foundChild = foundChild || childWindows[index].className == "Static" && childWindows[index].isChildWindow;
        }
        RequireTrueSystemWindow(foundChild, "Child enumeration contains static child", "Expected the deterministic child window.");

        GB_WindowFindOptions childOptions;
        childOptions.parentWindowId = windowInfo.windowId;
        childOptions.classNameEquals = "Static";
        childOptions.visibleOnly = true;
        childOptions.includeUntitledWindows = true;
        childOptions.includeToolWindows = true;
        childOptions.applicationWindowsOnly = false;
        std::vector<GB_WindowInfo> matchedChildren;
        RequireWindowResultSucceeded(GB_SystemWindow::FindWindows(childOptions, matchedChildren), "Find child windows");
        RequireTrueSystemWindow(!matchedChildren.empty(), "Find child windows non-empty", "Expected a matching child window.");
        const GB_Rectangle childTargetRectangle(windowInfo.clientRectangle.minX + 20, windowInfo.clientRectangle.minY + 20, windowInfo.clientRectangle.minX + 170, windowInfo.clientRectangle.minY + 60);
        RequireWindowResultSucceeded(GB_SystemWindow::MoveResizeWindow(matchedChildren.front().windowId, childTargetRectangle), "Move child window with screen coordinates");
        GB_WindowInfo movedChildInfo;
        RequireTrueSystemWindow(WaitForWindowState(matchedChildren.front().windowId, [&childTargetRectangle](const GB_WindowInfo& info)
            {
                return info.hasWindowRectangle && info.windowRectangle == childTargetRectangle;
            }, movedChildInfo), "Child screen-coordinate movement observed", movedChildInfo.hasWindowRectangle ? movedChildInfo.windowRectangle.SerializeToString() : "No rectangle");

        GB_WindowFindOptions invalidOptions;
        invalidOptions.titleContains = std::string("\xC3\x28", 2);
        std::vector<GB_WindowInfo> invalidWindows;
        const GB_SystemResult invalidResult = GB_SystemWindow::FindWindows(invalidOptions, invalidWindows);
        RequireTrueSystemWindow(invalidResult.IsFailed() && invalidResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject invalid UTF-8 filter", invalidResult.ToString());

        bool alive = false;
        RequireWindowResultSucceeded(GB_SystemWindow::IsWindowAlive(windowInfo.windowId, alive), "Window alive");
        RequireTrueSystemWindow(alive, "Window alive true", "Expected the test window to be alive.");

        GB_WindowId mismatchedId = windowInfo.windowId;
        mismatchedId.processId++;
        RequireWindowResultSucceeded(GB_SystemWindow::IsWindowAlive(mismatchedId, alive), "Mismatched window id alive query");
        RequireTrueSystemWindow(!alive, "Mismatched window id rejected", "Mismatched process id should not be considered alive.");

        (void)host;
    }

    void TestWindowControlAndWait(TestWindowHost& host, const GB_WindowId& windowId)
    {
        const double intMaximum = static_cast<double>((std::numeric_limits<int>::max)());
        const GB_SystemResult invalidRectangleResult = GB_SystemWindow::MoveResizeWindow(windowId, GB_Rectangle(intMaximum, 0, intMaximum + 1.0, 100));
        RequireTrueSystemWindow(invalidRectangleResult.IsFailed() && invalidRectangleResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject out-of-range rectangle", invalidRectangleResult.ToString());

        const GB_Rectangle targetRectangle(-40, -30, 420, 310);
        RequireWindowResultSucceeded(GB_SystemWindow::MoveResizeWindow(windowId, targetRectangle), "Move and resize window");
        GB_WindowInfo currentInfo;
        RequireTrueSystemWindow(WaitForWindowState(windowId, [](const GB_WindowInfo& info)
            {
                return info.hasWindowRectangle && info.windowRectangle.minX == -40 && info.windowRectangle.minY == -30 && info.windowRectangle.Width() == 460 && info.windowRectangle.Height() == 340;
            }, currentInfo), "Move and resize observed", currentInfo.hasWindowRectangle ? currentInfo.windowRectangle.SerializeToString() : "No rectangle");

        RequireWindowResultSucceeded(GB_SystemWindow::SetTopMost(windowId, true), "Set top-most");
        RequireTrueSystemWindow(WaitForWindowState(windowId, [](const GB_WindowInfo& info) { return info.isTopMost; }, currentInfo), "Top-most observed", "Window did not become top-most.");
        RequireWindowResultSucceeded(GB_SystemWindow::SetTopMost(windowId, false), "Clear top-most");
        RequireTrueSystemWindow(WaitForWindowState(windowId, [](const GB_WindowInfo& info) { return !info.isTopMost; }, currentInfo), "Top-most cleared", "Window remained top-most.");

        RequireWindowResultSucceeded(GB_SystemWindow::HideWindow(windowId), "Hide window");
        RequireTrueSystemWindow(WaitForWindowState(windowId, [](const GB_WindowInfo& info) { return !info.isVisible; }, currentInfo), "Hidden observed", "Window did not become hidden.");
        RequireWindowResultSucceeded(GB_SystemWindow::ShowWindowWithoutActivation(windowId), "Show window without activation");
        GB_WindowWaitOptions visibleWaitOptions;
        visibleWaitOptions.timeoutMilliseconds = 5000;
        visibleWaitOptions.pollIntervalMilliseconds = 10;
        RequireWindowResultSucceeded(GB_SystemWindow::WaitForWindowVisible(windowId, currentInfo, visibleWaitOptions), "Wait for visible window");

        RequireWindowResultSucceeded(GB_SystemWindow::MinimizeWindow(windowId), "Minimize window");
        RequireTrueSystemWindow(WaitForWindowState(windowId, [](const GB_WindowInfo& info) { return info.isMinimized; }, currentInfo), "Minimized observed", "Window did not become minimized.");
        RequireWindowResultSucceeded(GB_SystemWindow::RestoreWindow(windowId), "Restore window");
        RequireTrueSystemWindow(WaitForWindowState(windowId, [](const GB_WindowInfo& info) { return !info.isMinimized; }, currentInfo), "Restored observed", "Window did not restore.");

        const GB_SystemResult activateResult = GB_SystemWindow::TryActivateWindow(windowId);
        RequireTrueSystemWindow(activateResult.IsSucceeded() || activateResult.errorCode == GB_SystemErrorCode::OperationFailed, "TryActivateWindow reasonable result", activateResult.ToString());

        GB_WindowFindOptions missingOptions;
        missingOptions.titleEquals = "GB System Window Missing Title";
        GB_WindowWaitOptions immediateWaitOptions;
        immediateWaitOptions.timeoutMilliseconds = 0;
        immediateWaitOptions.pollIntervalMilliseconds = 1;
        GB_WindowInfo missingWindowInfo;
        const GB_SystemResult timeoutResult = GB_SystemWindow::WaitForWindow(missingOptions, missingWindowInfo, immediateWaitOptions);
        RequireTrueSystemWindow(timeoutResult.IsFailed() && timeoutResult.errorCode == GB_SystemErrorCode::Timeout, "WaitForWindow timeout", timeoutResult.ToString());

        std::atomic<bool> cancellationFlag(true);
        GB_WindowWaitOptions cancelledWaitOptions;
        cancelledWaitOptions.timeoutMilliseconds = 5000;
        cancelledWaitOptions.pollIntervalMilliseconds = 1;
        cancelledWaitOptions.cancellationFlag = &cancellationFlag;
        const GB_SystemResult cancelledResult = GB_SystemWindow::WaitForWindow(missingOptions, missingWindowInfo, cancelledWaitOptions);
        RequireTrueSystemWindow(cancelledResult.IsFailed() && cancelledResult.errorCode == GB_SystemErrorCode::Cancelled, "WaitForWindow cancelled", cancelledResult.ToString());

        std::thread titleThread([&host]()
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                (void)host.SetTitle(ChangedWindowTitle);
            });
        GB_WindowInfo changedInfo;
        RequireWindowResultSucceeded(GB_SystemWindow::WaitForWindowTitleChanged(windowId, currentInfo.title, changedInfo, visibleWaitOptions), "Wait for title changed");
        titleThread.join();
        RequireTrueSystemWindow(changedInfo.title.find("GB Window Changed") != std::string::npos, "Changed title observed", changedInfo.title);
    }

    bool ContainsEventType(const std::vector<GB_SystemWindowEvent>& events, const uint64_t nativeHandle, const GB_SystemWindowEventType eventType)
    {
        for (size_t index = 0; index < events.size(); index++)
        {
            if (events[index].windowId.nativeHandle == nativeHandle && events[index].eventType == eventType)
            {
                return true;
            }
        }
        return false;
    }

    void TestWindowWatcher()
    {
        GB_SystemWindowWatcherOptions watcherOptions;
        watcherOptions.filter.processId = ::GetCurrentProcessId();
        watcherOptions.maxPendingNativeEvents = 64;
        watcherOptions.maxDispatchQueueSize = 128;
        watcherOptions.coalesceLocationChanges = true;

        GB_SystemWindowWatcher watcher(watcherOptions);
        std::mutex eventMutex;
        std::condition_variable eventCondition;
        std::vector<GB_SystemWindowEvent> events;
        watcher.SetWindowEventCallback([&](const GB_SystemWindowEvent& event)
            {
                {
                    std::lock_guard<std::mutex> lock(eventMutex);
                    events.push_back(event);
                }
                eventCondition.notify_all();
            });

        GB_EventSubscriptionToken publicSubscription;
        std::atomic<int> publicEventCount(0);
        RequireWindowResultSucceeded(watcher.GetEventDispatcher().SubscribeAll([&publicEventCount](const GB_Event&)
            {
                publicEventCount++;
            }, publicSubscription), "Subscribe public window events");

        RequireWindowResultSucceeded(watcher.Start(), "Watcher start");
        RequireTrueSystemWindow(watcher.IsRunning(), "Watcher running", "Watcher should be running.");
        RequireWindowResultSucceeded(watcher.Start(), "Watcher duplicate start");

        TestWindowHost host;
        RequireTrueSystemWindow(host.Start(L"GB Watcher Test Window"), "Watcher test window start", "Could not create watcher test window.");
        const GB_WindowInfo windowInfo = FindTestWindow("watcher test");
        const uint64_t nativeHandle = windowInfo.windowId.nativeHandle;

        {
            std::unique_lock<std::mutex> lock(eventMutex);
            const bool initialEventReceived = eventCondition.wait_for(lock, std::chrono::seconds(5), [&]()
                {
                    return ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::Created) || ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::Shown);
                });
            RequireTrueSystemWindow(initialEventReceived, "Watcher initial event", "Expected a create or show event for the test window.");
        }

        RequireTrueSystemWindow(host.SetTitle(L"GB Watcher Renamed Window"), "Watcher title change request", "SetWindowTextW failed.");
        {
            std::unique_lock<std::mutex> lock(eventMutex);
            const bool titleEventReceived = eventCondition.wait_for(lock, std::chrono::seconds(5), [&]()
                {
                    return ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::TitleChanged);
                });
            RequireTrueSystemWindow(titleEventReceived, "Watcher title event", "Expected a title-changed event.");
        }

        RequireWindowResultSucceeded(GB_SystemWindow::MoveResizeWindow(windowInfo.windowId, GB_Rectangle(200, 180, 700, 560)), "Watcher move and resize");
        {
            std::unique_lock<std::mutex> lock(eventMutex);
            const bool locationEventsReceived = eventCondition.wait_for(lock, std::chrono::seconds(5), [&]()
                {
                    return ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::LocationChanged) &&
                        ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::Moved) &&
                        ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::Resized);
                });
            RequireTrueSystemWindow(locationEventsReceived, "Watcher location events", "Expected raw location, moved, and resized events.");
        }

        RequireWindowResultSucceeded(GB_SystemWindow::HideWindow(windowInfo.windowId), "Watcher hide");
        {
            std::unique_lock<std::mutex> lock(eventMutex);
            const bool hiddenEventReceived = eventCondition.wait_for(lock, std::chrono::seconds(5), [&]()
                {
                    return ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::Hidden);
                });
            RequireTrueSystemWindow(hiddenEventReceived, "Watcher hidden event", "Expected a hidden event.");
        }

        RequireWindowResultSucceeded(GB_SystemWindow::ShowWindowWithoutActivation(windowInfo.windowId), "Watcher show");
        {
            std::unique_lock<std::mutex> lock(eventMutex);
            const bool shownEventReceived = eventCondition.wait_for(lock, std::chrono::seconds(5), [&]()
                {
                    return ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::Shown);
                });
            RequireTrueSystemWindow(shownEventReceived, "Watcher shown event", "Expected a shown event.");
        }

        RequireWindowResultSucceeded(GB_SystemWindow::MinimizeWindow(windowInfo.windowId), "Watcher minimize");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        RequireWindowResultSucceeded(GB_SystemWindow::RestoreWindow(windowInfo.windowId), "Watcher restore");

        {
            std::unique_lock<std::mutex> lock(eventMutex);
            const bool coreEventsReceived = eventCondition.wait_for(lock, std::chrono::seconds(10), [&]()
                {
                    return ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::TitleChanged) &&
                        ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::LocationChanged) &&
                        ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::Moved) &&
                        ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::Resized) &&
                        ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::Hidden) &&
                        ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::Shown);
                });
            RequireTrueSystemWindow(coreEventsReceived, "Watcher core events", "Expected title, location, move, resize, hide, and show events.");
        }

        RequireWindowResultSucceeded(GB_SystemWindow::RequestCloseWindow(windowInfo.windowId), "Watcher close request");
        GB_WindowWaitOptions closeWaitOptions;
        closeWaitOptions.timeoutMilliseconds = 5000;
        closeWaitOptions.pollIntervalMilliseconds = 10;
        RequireWindowResultSucceeded(GB_SystemWindow::WaitForWindowClosed(windowInfo.windowId, closeWaitOptions), "Watcher wait closed");
        host.Stop();

        {
            std::unique_lock<std::mutex> lock(eventMutex);
            const bool destroyedReceived = eventCondition.wait_for(lock, std::chrono::seconds(5), [&]()
                {
                    return ContainsEventType(events, nativeHandle, GB_SystemWindowEventType::Destroyed);
                });
            RequireTrueSystemWindow(destroyedReceived, "Watcher destroyed event", "Expected a destroyed event with cached window identity.");
        }
        RequireTrueSystemWindow(publicEventCount.load() > 0, "Watcher public events", "Expected events through the public dispatcher.");

        RequireWindowResultSucceeded(watcher.Stop(), "Watcher stop");
        RequireTrueSystemWindow(!watcher.IsRunning(), "Watcher stopped", "Watcher should be stopped.");
        RequireWindowResultSucceeded(watcher.Stop(), "Watcher duplicate stop");
        RequireWindowResultSucceeded(watcher.Start(), "Watcher restart");
        RequireWindowResultSucceeded(watcher.Stop(), "Watcher stop after restart");

        GB_SystemWindowWatcher callbackStopWatcher(watcherOptions);
        std::atomic<int> callbackStopState(0);
        callbackStopWatcher.SetWindowEventCallback([&](const GB_SystemWindowEvent&)
            {
                int expectedState = 0;
                if (callbackStopState.compare_exchange_strong(expectedState, 2))
                {
                    const GB_SystemResult stopResult = callbackStopWatcher.Stop();
                    callbackStopState.store(stopResult.IsSucceeded() ? 1 : -1);
                }
            });
        RequireWindowResultSucceeded(callbackStopWatcher.Start(), "Callback-stop watcher start");
        TestWindowHost callbackHost;
        RequireTrueSystemWindow(callbackHost.Start(L"GB Callback Stop Window"), "Callback-stop test window start", "Could not create callback-stop window.");
        const std::chrono::steady_clock::time_point callbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (callbackStopState.load() != 1 && callbackStopState.load() != -1 && std::chrono::steady_clock::now() < callbackDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        RequireTrueSystemWindow(callbackStopState.load() == 1 && !callbackStopWatcher.IsRunning(), "Watcher stop from callback", "Stopping from the typed callback failed.");
        callbackHost.Stop();

        GB_SystemWindowWatcherOptions concurrentStopOptions = watcherOptions;
        concurrentStopOptions.filter.titleContains = "GB Concurrent Stop Window";
        GB_SystemWindowWatcher concurrentStopWatcher(concurrentStopOptions);
        std::atomic<bool> concurrentCallbackEntered(false);
        std::atomic<bool> allowNestedStop(false);
        std::atomic<int> nestedStopState(0);
        concurrentStopWatcher.SetWindowEventCallback([&](const GB_SystemWindowEvent&)
            {
                concurrentCallbackEntered.store(true, std::memory_order_release);
                while (!allowNestedStop.load(std::memory_order_acquire))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                const GB_SystemResult nestedStopResult = concurrentStopWatcher.Stop();
                nestedStopState.store(nestedStopResult.IsSucceeded() ? 1 : -1, std::memory_order_release);
            });
        RequireWindowResultSucceeded(concurrentStopWatcher.Start(), "Concurrent-stop watcher start");
        TestWindowHost concurrentStopHost;
        RequireTrueSystemWindow(concurrentStopHost.Start(L"GB Concurrent Stop Window"), "Concurrent-stop test window start", "Could not create concurrent-stop window.");
        const std::chrono::steady_clock::time_point concurrentCallbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!concurrentCallbackEntered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < concurrentCallbackDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        RequireTrueSystemWindow(concurrentCallbackEntered.load(std::memory_order_acquire), "Concurrent-stop callback entered", "Expected a callback before the external Stop call.");
        std::atomic<int> externalStopState(0);
        std::thread externalStopThread([&]()
            {
                const GB_SystemResult externalStopResult = concurrentStopWatcher.Stop();
                externalStopState.store(externalStopResult.IsSucceeded() ? 1 : -1, std::memory_order_release);
            });
        const std::chrono::steady_clock::time_point stoppingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (concurrentStopWatcher.IsRunning() && std::chrono::steady_clock::now() < stoppingDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        allowNestedStop.store(true, std::memory_order_release);
        externalStopThread.join();
        RequireTrueSystemWindow(nestedStopState.load(std::memory_order_acquire) == 1 && externalStopState.load(std::memory_order_acquire) == 1, "Concurrent external and callback Stop", "Concurrent Stop calls should complete without deadlock.");
        concurrentStopHost.Stop();

        {
            GB_SystemWindowWatcher destructorWatcher(watcherOptions);
            RequireWindowResultSucceeded(destructorWatcher.Start(), "Destructor watcher start");
        }
    }

    void TestCloseAndStaleId(TestWindowHost& host, const GB_WindowId& windowId)
    {
        RequireWindowResultSucceeded(GB_SystemWindow::RequestCloseWindow(windowId), "Request close window");
        GB_WindowWaitOptions waitOptions;
        waitOptions.timeoutMilliseconds = 5000;
        waitOptions.pollIntervalMilliseconds = 10;
        RequireWindowResultSucceeded(GB_SystemWindow::WaitForWindowClosed(windowId, waitOptions), "Wait for window closed");
        host.Stop();

        bool alive = true;
        RequireWindowResultSucceeded(GB_SystemWindow::IsWindowAlive(windowId, alive), "Stale window id alive query");
        RequireTrueSystemWindow(!alive, "Stale window id rejected", "Destroyed window should not be alive.");
        GB_WindowInfo staleInfo;
        const GB_SystemResult staleInfoResult = GB_SystemWindow::GetWindowInfo(windowId, staleInfo);
        RequireTrueSystemWindow(staleInfoResult.IsFailed() && staleInfoResult.errorCode == GB_SystemErrorCode::NotFound, "Stale window info rejected", staleInfoResult.ToString());
    }
#endif
}

int RunGB_SystemWindowTests()
{
#if !defined(_WIN32)
    std::cout << "GB_SystemWindow tests skipped on non-Windows." << std::endl;
    return 0;
#else
    try
    {
        TestWindowHost host;
        RequireTrueSystemWindow(host.Start(), "Deterministic test window start", "Could not create the deterministic window.");
        GB_WindowInfo windowInfo;
        TestWindowIdAndQueries(host, windowInfo);
        TestWindowControlAndWait(host, windowInfo.windowId);
        TestWindowWatcher();
        TestCloseAndStaleId(host, windowInfo.windowId);
    }
    catch (const std::exception& exceptionObject)
    {
        std::cerr << "[FAILED] Unexpected system window test exception\n" << exceptionObject.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "[FAILED] Unknown system window test exception" << std::endl;
        return 1;
    }

    std::cout << "GB_SystemWindow tests passed. Total checks: " << totalSystemWindowCaseCount << std::endl;
    return 0;
#endif
}
