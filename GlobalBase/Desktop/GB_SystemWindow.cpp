#include "GB_SystemWindow.h"

#include "GB_WinHandleScope.h"
#include "../GB_Utf8String.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <iterator>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dwmapi.h>
#  pragma comment(lib, "Dwmapi.lib")
#endif

namespace
{
    const char* const GB_WindowOperationGetForeground = "GB_SystemWindow::GetForegroundWindow";
    const char* const GB_WindowOperationGetTopLevel = "GB_SystemWindow::GetTopLevelWindows";
    const char* const GB_WindowOperationGetChildren = "GB_SystemWindow::GetChildWindows";
    const char* const GB_WindowOperationFind = "GB_SystemWindow::FindWindows";
    const char* const GB_WindowOperationGetInfo = "GB_SystemWindow::GetWindowInfo";
    const char* const GB_WindowOperationIsAlive = "GB_SystemWindow::IsWindowAlive";
    const char* const GB_WindowOperationShow = "GB_SystemWindow::ShowWindow";
    const char* const GB_WindowOperationMoveResize = "GB_SystemWindow::MoveResizeWindow";
    const char* const GB_WindowOperationSetTopMost = "GB_SystemWindow::SetTopMost";
    const char* const GB_WindowOperationActivate = "GB_SystemWindow::TryActivateWindow";
    const char* const GB_WindowOperationClose = "GB_SystemWindow::RequestCloseWindow";
    const char* const GB_WindowOperationWait = "GB_SystemWindow::Wait";
    const char* const GB_WindowOperationWatcherStart = "GB_SystemWindowWatcher::Start";
    const char* const GB_WindowOperationWatcherStop = "GB_SystemWindowWatcher::Stop";

#if !defined(_WIN32)
    static GB_SystemResult MakeUnsupportedPlatformResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, "当前平台不支持 Windows 窗口管理。");
    }
#endif

    static GB_SystemResult MakeAllocationFailedResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, "分配窗口模块内部内存失败。");
    }

#if defined(_WIN32)
    static GB_SystemResult MakeLastWin32ErrorOrNativeApiFailedResult(const std::string& operationName, const std::string& message)
    {
        const DWORD lastError = ::GetLastError();
        if (lastError != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(lastError, operationName, message);
        }

        return GB_SystemResult::Failed(GB_SystemErrorCode::NativeApiFailed, operationName, message.empty() ? std::string("Win32 API 调用失败，但系统未提供扩展错误码。") : message);
    }
#endif

    static bool IsValidWaitOptions(const GB_WindowWaitOptions& options)
    {
        return options.timeoutMilliseconds >= -1 && options.pollIntervalMilliseconds > 0;
    }

    static GB_SystemResult ValidateWaitOptions(const GB_WindowWaitOptions& options, const std::string& operationName)
    {
        if (!IsValidWaitOptions(options))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "timeoutMilliseconds 必须大于等于 -1，pollIntervalMilliseconds 必须大于 0。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    static bool IsCancelled(const GB_WindowWaitOptions& options)
    {
        return options.cancellationFlag != nullptr && options.cancellationFlag->load(std::memory_order_acquire);
    }

    template<typename Predicate>
    static GB_SystemResult WaitUntil(const GB_WindowWaitOptions& options, const std::string& operationName, Predicate predicate)
    {
        const GB_SystemResult validationResult = ValidateWaitOptions(options, operationName);
        if (validationResult.IsFailed())
        {
            return validationResult;
        }

        const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
        while (true)
        {
            if (IsCancelled(options))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, operationName, "窗口等待操作已取消。");
            }

            bool completed = false;
            const GB_SystemResult predicateResult = predicate(completed);
            if (predicateResult.IsFailed() || completed)
            {
                return predicateResult;
            }

            if (options.timeoutMilliseconds == 0)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, operationName, "等待窗口状态超时。");
            }

            uint32_t sleepMilliseconds = options.pollIntervalMilliseconds;
            if (options.timeoutMilliseconds > 0)
            {
                const std::chrono::steady_clock::time_point nowTime = std::chrono::steady_clock::now();
                const int64_t elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - startTime).count();
                if (elapsedMilliseconds >= options.timeoutMilliseconds)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, operationName, "等待窗口状态超时。");
                }

                const int64_t remainingMilliseconds = options.timeoutMilliseconds - elapsedMilliseconds;
                if (remainingMilliseconds <= 0)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, operationName, "等待窗口状态超时。");
                }
                if (remainingMilliseconds < static_cast<int64_t>(sleepMilliseconds))
                {
                    sleepMilliseconds = static_cast<uint32_t>(remainingMilliseconds);
                }
            }

            if (sleepMilliseconds > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMilliseconds));
            }
        }
    }

#if defined(_WIN32)
    class DpiAwarenessScope final
    {
    public:
        DpiAwarenessScope()
        {
            static const SetThreadDpiAwarenessContextFunction function = []() -> SetThreadDpiAwarenessContextFunction
                {
                    const HMODULE user32Module = ::GetModuleHandleW(L"user32.dll");
                    return user32Module == nullptr ? nullptr : reinterpret_cast<SetThreadDpiAwarenessContextFunction>(::GetProcAddress(user32Module, "SetThreadDpiAwarenessContext"));
                }();
            if (function == nullptr)
            {
                return;
            }

            setThreadDpiAwarenessContextFunction = function;
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
            previousContext = setThreadDpiAwarenessContextFunction(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#else
            previousContext = setThreadDpiAwarenessContextFunction(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4));
#endif
            if (previousContext == nullptr)
            {
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)
                previousContext = setThreadDpiAwarenessContextFunction(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
#else
                previousContext = setThreadDpiAwarenessContextFunction(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-3));
#endif
            }
            isActive = previousContext != nullptr;
        }

        ~DpiAwarenessScope()
        {
            if (isActive && setThreadDpiAwarenessContextFunction != nullptr)
            {
                (void)setThreadDpiAwarenessContextFunction(previousContext);
            }
        }

    private:
        using SetThreadDpiAwarenessContextFunction = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT dpiContext);

        SetThreadDpiAwarenessContextFunction setThreadDpiAwarenessContextFunction = nullptr;
        DPI_AWARENESS_CONTEXT previousContext = nullptr;
        bool isActive = false;
    };

    struct ProcessIdentity
    {
        std::string processName;
        std::string processPath;
        uint64_t processCreationTime = 0;
        bool hasProcessName = false;
        bool hasProcessPath = false;
        bool hasProcessCreationTime = false;
    };

    struct ProcessIdentityCacheEntry
    {
        ProcessIdentity identity;
        uint64_t queryTickMilliseconds = 0;
    };

    using ProcessIdentityCache = std::unordered_map<DWORD, ProcessIdentityCacheEntry>;

    const uint64_t GB_ProcessIdentityCacheTtlMilliseconds = 5000;
    const uint64_t GB_ProcessIdentityNegativeCacheTtlMilliseconds = 1000;
    const size_t GB_ProcessIdentityCacheMaxSize = 4096;

    static uint64_t GetSteadyTickMilliseconds()
    {
        const std::chrono::steady_clock::time_point nowTime = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(nowTime.time_since_epoch()).count());
    }

    static uint64_t FileTimeToUInt64(const FILETIME& fileTime)
    {
        ULARGE_INTEGER value = {};
        value.LowPart = fileTime.dwLowDateTime;
        value.HighPart = fileTime.dwHighDateTime;
        return value.QuadPart;
    }

    static bool IsCacheEntryFresh(const ProcessIdentityCacheEntry& cacheEntry, const uint64_t nowMilliseconds)
    {
        if (nowMilliseconds < cacheEntry.queryTickMilliseconds)
        {
            return false;
        }

        const uint64_t cacheTtlMilliseconds = cacheEntry.identity.hasProcessPath ? GB_ProcessIdentityCacheTtlMilliseconds : GB_ProcessIdentityNegativeCacheTtlMilliseconds;
        return nowMilliseconds - cacheEntry.queryTickMilliseconds <= cacheTtlMilliseconds;
    }

    static bool HasAnyWindowIdField(const GB_WindowId& windowId)
    {
        return windowId.nativeHandle != 0 || windowId.processId != 0 || windowId.threadId != 0;
    }

    static HWND WindowIdToHandle(const GB_WindowId& windowId)
    {
        return reinterpret_cast<HWND>(static_cast<uintptr_t>(windowId.nativeHandle));
    }

    static uint64_t WindowHandleToValue(HWND windowHandle)
    {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(windowHandle));
    }

    static GB_WindowId MakeWindowId(HWND windowHandle)
    {
        GB_WindowId windowId;
        if (windowHandle == nullptr)
        {
            return windowId;
        }

        DWORD processId = 0;
        const DWORD threadId = ::GetWindowThreadProcessId(windowHandle, &processId);
        windowId.nativeHandle = WindowHandleToValue(windowHandle);
        windowId.processId = static_cast<uint32_t>(processId);
        windowId.threadId = static_cast<uint32_t>(threadId);
        return windowId;
    }

    static GB_SystemResult ValidateWindowId(const GB_WindowId& windowId, HWND& windowHandle, const std::string& operationName)
    {
        windowHandle = nullptr;
        if (!windowId.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "窗口标识无效。");
        }

        if (windowId.nativeHandle > static_cast<uint64_t>((std::numeric_limits<uintptr_t>::max)()))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "窗口原生句柄值超出当前进程可表示范围。");
        }

        HWND candidateHandle = WindowIdToHandle(windowId);
        if (candidateHandle == nullptr || ::IsWindow(candidateHandle) == FALSE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, "目标窗口已不存在。");
        }

        DWORD currentProcessId = 0;
        const DWORD currentThreadId = ::GetWindowThreadProcessId(candidateHandle, &currentProcessId);
        if (currentThreadId == 0 || currentProcessId == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, "目标窗口身份无法确认，窗口可能已经销毁。");
        }
        if (windowId.processId != 0 && windowId.processId != currentProcessId)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, "窗口句柄已被其他进程窗口复用。");
        }
        if (windowId.threadId != 0 && windowId.threadId != currentThreadId)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, "窗口句柄已被其他线程窗口复用。");
        }

        windowHandle = candidateHandle;
        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_Rectangle RectToRectangle(const RECT& rectangle)
    {
        return GB_Rectangle(static_cast<double>(rectangle.left), static_cast<double>(rectangle.top), static_cast<double>(rectangle.right), static_cast<double>(rectangle.bottom));
    }

    static bool ReadWindowTitleWide(HWND windowHandle, std::wstring& title)
    {
        title.clear();
        ::SetLastError(ERROR_SUCCESS);
        const int textLength = ::GetWindowTextLengthW(windowHandle);
        if (textLength < 0)
        {
            return false;
        }

        int capacity = textLength + 1;
        capacity = (std::max)(capacity, 2);
        const int maxCapacity = 1024 * 1024;

        while (capacity <= maxCapacity)
        {
            std::vector<wchar_t> buffer(static_cast<size_t>(capacity), L'\0');
            ::SetLastError(ERROR_SUCCESS);
            const int copiedLength = ::GetWindowTextW(windowHandle, buffer.data(), capacity);
            if (copiedLength == 0)
            {
                const DWORD lastError = ::GetLastError();
                if (lastError != ERROR_SUCCESS)
                {
                    return false;
                }
                title.clear();
                return true;
            }

            if (copiedLength < capacity - 1 || capacity >= maxCapacity)
            {
                title.assign(buffer.data(), static_cast<size_t>(copiedLength));
                return true;
            }
            if (capacity > maxCapacity / 2)
            {
                capacity = maxCapacity;
            }
            else
            {
                capacity *= 2;
            }
        }
        return false;
    }

    static bool ReadWindowTitle(HWND windowHandle, std::string& title)
    {
        title.clear();
        std::wstring titleWide;
        if (!ReadWindowTitleWide(windowHandle, titleWide))
        {
            return false;
        }
        title = GB_WStringToUtf8(titleWide);
        return true;
    }

    static bool ReadWindowClassNameWide(HWND windowHandle, std::wstring& className)
    {
        className.clear();
        int capacity = 256;
        const int maxCapacity = 65536;
        while (capacity <= maxCapacity)
        {
            std::vector<wchar_t> buffer(static_cast<size_t>(capacity), L'\0');
            ::SetLastError(ERROR_SUCCESS);
            const int copiedLength = ::GetClassNameW(windowHandle, buffer.data(), capacity);
            if (copiedLength == 0)
            {
                return false;
            }
            if (copiedLength < capacity - 1 || capacity >= maxCapacity)
            {
                className.assign(buffer.data(), static_cast<size_t>(copiedLength));
                return true;
            }
            if (capacity > maxCapacity / 2)
            {
                capacity = maxCapacity;
            }
            else
            {
                capacity *= 2;
            }
        }
        return false;
    }

    static bool ReadWindowClassName(HWND windowHandle, std::string& className)
    {
        className.clear();
        std::wstring classNameWide;
        if (!ReadWindowClassNameWide(windowHandle, classNameWide))
        {
            return false;
        }
        className = GB_WStringToUtf8(classNameWide);
        return true;
    }

    static bool QueryProcessCreationTime(const DWORD processId, uint64_t& processCreationTime)
    {
        processCreationTime = 0;
        const HANDLE processHandle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (processHandle == nullptr)
        {
            return false;
        }

        GB_WinHandleScope processScope = GB_WinHandleScope::FromKernelHandle(processHandle, "SystemWindow.ProcessCreationTime");
        FILETIME creationTime = {};
        FILETIME exitTime = {};
        FILETIME kernelTime = {};
        FILETIME userTime = {};
        if (::GetProcessTimes(processHandle, &creationTime, &exitTime, &kernelTime, &userTime) == FALSE)
        {
            return false;
        }

        processCreationTime = FileTimeToUInt64(creationTime);
        return true;
    }

    static ProcessIdentity QueryProcessIdentity(const DWORD processId)
    {
        ProcessIdentity identity;
        const HANDLE processHandle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (processHandle == nullptr)
        {
            return identity;
        }

        GB_WinHandleScope processScope = GB_WinHandleScope::FromKernelHandle(processHandle, "SystemWindow.Process");

        FILETIME creationTime = {};
        FILETIME exitTime = {};
        FILETIME kernelTime = {};
        FILETIME userTime = {};
        if (::GetProcessTimes(processHandle, &creationTime, &exitTime, &kernelTime, &userTime) != FALSE)
        {
            identity.processCreationTime = FileTimeToUInt64(creationTime);
            identity.hasProcessCreationTime = true;
        }

        std::vector<wchar_t> buffer(32768, L'\0');
        DWORD pathLength = static_cast<DWORD>(buffer.size());
        if (::QueryFullProcessImageNameW(processHandle, 0, buffer.data(), &pathLength) == FALSE || pathLength == 0)
        {
            return identity;
        }

        try
        {
            identity.processPath = GB_WStringToUtf8(std::wstring(buffer.data(), static_cast<size_t>(pathLength)));
            identity.hasProcessPath = true;
            const size_t separatorIndex = identity.processPath.find_last_of("\\/");
            identity.processName = separatorIndex == std::string::npos ? identity.processPath : identity.processPath.substr(separatorIndex + 1);
            identity.hasProcessName = !identity.processName.empty();
        }
        catch (...)
        {
            identity = ProcessIdentity();
        }
        return identity;
    }

    static bool IsProcessIdentityCacheReusable(const ProcessIdentityCacheEntry& cacheEntry, const uint64_t nowMilliseconds, const DWORD processId)
    {
        if (!IsCacheEntryFresh(cacheEntry, nowMilliseconds))
        {
            return false;
        }

        if (!cacheEntry.identity.hasProcessCreationTime)
        {
            return nowMilliseconds >= cacheEntry.queryTickMilliseconds && nowMilliseconds - cacheEntry.queryTickMilliseconds <= GB_ProcessIdentityNegativeCacheTtlMilliseconds;
        }

        uint64_t currentProcessCreationTime = 0;
        if (!QueryProcessCreationTime(processId, currentProcessCreationTime))
        {
            return false;
        }
        return currentProcessCreationTime == cacheEntry.identity.processCreationTime;
    }

    static ProcessIdentity GetProcessIdentity(const DWORD processId, ProcessIdentityCache* cache)
    {
        if (cache == nullptr)
        {
            return QueryProcessIdentity(processId);
        }

        const uint64_t nowMilliseconds = GetSteadyTickMilliseconds();
        const ProcessIdentityCache::const_iterator existing = cache->find(processId);
        if (existing != cache->end() && IsProcessIdentityCacheReusable(existing->second, nowMilliseconds, processId))
        {
            return existing->second.identity;
        }

        const ProcessIdentity identity = QueryProcessIdentity(processId);
        try
        {
            if (existing == cache->end() && cache->size() >= GB_ProcessIdentityCacheMaxSize)
            {
                cache->clear();
            }
            ProcessIdentityCacheEntry cacheEntry;
            cacheEntry.identity = identity;
            cacheEntry.queryTickMilliseconds = nowMilliseconds;
            (*cache)[processId] = std::move(cacheEntry);
        }
        catch (...)
        {
        }
        return identity;
    }

    static GB_SystemResult BuildWindowInfo(HWND windowHandle, GB_WindowInfo& windowInfo, ProcessIdentityCache* processCache, const std::string& operationName)
    {
        windowInfo = GB_WindowInfo();
        if (windowHandle == nullptr || ::IsWindow(windowHandle) == FALSE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, "目标窗口已不存在。");
        }

        try
        {
            const DpiAwarenessScope dpiAwarenessScope;
            windowInfo.windowId = MakeWindowId(windowHandle);
            if (!windowInfo.windowId.IsValid())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, "无法读取目标窗口身份。");
            }

            windowInfo.hasTitle = ReadWindowTitle(windowHandle, windowInfo.title);
            windowInfo.hasClassName = ReadWindowClassName(windowHandle, windowInfo.className);

            const ProcessIdentity processIdentity = GetProcessIdentity(static_cast<DWORD>(windowInfo.windowId.processId), processCache);
            windowInfo.processName = processIdentity.processName;
            windowInfo.processPath = processIdentity.processPath;
            windowInfo.hasProcessName = processIdentity.hasProcessName;
            windowInfo.hasProcessPath = processIdentity.hasProcessPath;

            RECT rectangle = {};
            if (::GetWindowRect(windowHandle, &rectangle) != FALSE)
            {
                windowInfo.windowRectangle = RectToRectangle(rectangle);
                windowInfo.hasWindowRectangle = true;
            }

            RECT clientRectangle = {};
            if (::GetClientRect(windowHandle, &clientRectangle) != FALSE)
            {
                POINT topLeft = { clientRectangle.left, clientRectangle.top };
                POINT bottomRight = { clientRectangle.right, clientRectangle.bottom };
                if (::ClientToScreen(windowHandle, &topLeft) != FALSE && ::ClientToScreen(windowHandle, &bottomRight) != FALSE)
                {
                    clientRectangle.left = topLeft.x;
                    clientRectangle.top = topLeft.y;
                    clientRectangle.right = bottomRight.x;
                    clientRectangle.bottom = bottomRight.y;
                    windowInfo.clientRectangle = RectToRectangle(clientRectangle);
                    windowInfo.hasClientRectangle = true;
                }
            }

            RECT visibleFrameRectangle = {};
            if (SUCCEEDED(::DwmGetWindowAttribute(windowHandle, DWMWA_EXTENDED_FRAME_BOUNDS, &visibleFrameRectangle, sizeof(visibleFrameRectangle))))
            {
                windowInfo.visibleFrameRectangle = RectToRectangle(visibleFrameRectangle);
                windowInfo.hasVisibleFrameRectangle = true;
            }

            ::SetLastError(ERROR_SUCCESS);
            const LONG_PTR style = ::GetWindowLongPtrW(windowHandle, GWL_STYLE);
            windowInfo.hasStyle = style != 0 || ::GetLastError() == ERROR_SUCCESS;
            ::SetLastError(ERROR_SUCCESS);
            const LONG_PTR extendedStyle = ::GetWindowLongPtrW(windowHandle, GWL_EXSTYLE);
            windowInfo.hasExtendedStyle = extendedStyle != 0 || ::GetLastError() == ERROR_SUCCESS;
            windowInfo.style = windowInfo.hasStyle ? static_cast<uint64_t>(static_cast<uint32_t>(style)) : 0;
            windowInfo.extendedStyle = windowInfo.hasExtendedStyle ? static_cast<uint64_t>(static_cast<uint32_t>(extendedStyle)) : 0;
            windowInfo.isVisible = ::IsWindowVisible(windowHandle) != FALSE;
            windowInfo.isEnabled = ::IsWindowEnabled(windowHandle) != FALSE;
            windowInfo.isMinimized = ::IsIconic(windowHandle) != FALSE;
            windowInfo.isMaximized = ::IsZoomed(windowHandle) != FALSE;
            windowInfo.isTopMost = windowInfo.hasExtendedStyle && (extendedStyle & WS_EX_TOPMOST) != 0;
            windowInfo.isForeground = ::GetForegroundWindow() == windowHandle;
            windowInfo.isToolWindow = windowInfo.hasExtendedStyle && (extendedStyle & WS_EX_TOOLWINDOW) != 0;
            windowInfo.isChildWindow = windowInfo.hasStyle && (style & WS_CHILD) != 0;
            const HWND ownerWindow = ::GetWindow(windowHandle, GW_OWNER);
            windowInfo.isAppWindow = windowInfo.hasStyle && windowInfo.hasExtendedStyle && ((extendedStyle & WS_EX_APPWINDOW) != 0 || (!windowInfo.isToolWindow && !windowInfo.isChildWindow && ownerWindow == nullptr));

            DWORD cloaked = 0;
            if (SUCCEEDED(::DwmGetWindowAttribute(windowHandle, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
            {
                windowInfo.hasCloakedState = true;
                windowInfo.isCloaked = cloaked != 0;
            }

            if (::IsWindow(windowHandle) == FALSE)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, "读取窗口信息期间目标窗口已销毁。");
            }

            DWORD finalProcessId = 0;
            const DWORD finalThreadId = ::GetWindowThreadProcessId(windowHandle, &finalProcessId);
            if (finalProcessId != windowInfo.windowId.processId || finalThreadId != windowInfo.windowId.threadId)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, "读取窗口信息期间窗口句柄发生复用。");
            }
            return GB_SystemResult::Succeeded(operationName);
        }
        catch (const std::bad_alloc&)
        {
            return MakeAllocationFailedResult(operationName);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, operationName, "读取窗口文本或进程路径时编码转换失败。");
        }
    }

    static bool WideEquals(const std::wstring& left, const std::wstring& right, const bool caseSensitive)
    {
        if (left.empty() || right.empty())
        {
            return left == right;
        }
        const int compareResult = ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), caseSensitive ? FALSE : TRUE);
        return compareResult == CSTR_EQUAL;
    }

    static bool WideContains(const std::wstring& text, const std::wstring& target, const bool caseSensitive)
    {
        if (target.empty())
        {
            return true;
        }
        if (text.empty())
        {
            return false;
        }
        const DWORD flags = FIND_FROMSTART | (caseSensitive ? 0 : NORM_IGNORECASE);
        return ::FindNLSStringEx(LOCALE_NAME_INVARIANT, flags, text.data(), static_cast<int>(text.size()), target.data(), static_cast<int>(target.size()), nullptr, nullptr, nullptr, 0) >= 0;
    }

    static bool Utf8Equals(const std::string& left, const std::string& right, const bool caseSensitive)
    {
        if (left.empty() || right.empty())
        {
            return left == right;
        }
        const std::wstring leftWide = GB_Utf8ToWString(left);
        const std::wstring rightWide = GB_Utf8ToWString(right);
        const int compareResult = ::CompareStringOrdinal(leftWide.data(), static_cast<int>(leftWide.size()), rightWide.data(), static_cast<int>(rightWide.size()), caseSensitive ? FALSE : TRUE);
        return compareResult == CSTR_EQUAL;
    }

    static bool Utf8Contains(const std::string& text, const std::string& target, const bool caseSensitive)
    {
        if (target.empty())
        {
            return true;
        }
        if (text.empty())
        {
            return false;
        }
        const std::wstring textWide = GB_Utf8ToWString(text);
        const std::wstring targetWide = GB_Utf8ToWString(target);
        const DWORD flags = FIND_FROMSTART | (caseSensitive ? 0 : NORM_IGNORECASE);
        return ::FindNLSStringEx(LOCALE_NAME_INVARIANT, flags, textWide.data(), static_cast<int>(textWide.size()), targetWide.data(), static_cast<int>(targetWide.size()), nullptr, nullptr, nullptr, 0) >= 0;
    }

    struct PreparedFindOptions
    {
        const GB_WindowFindOptions* options = nullptr;
        std::wstring titleEquals;
        std::wstring titleContains;
        std::wstring classNameEquals;
        std::wstring classNameContains;
        std::wstring processNameEquals;
        std::wstring processPathEquals;
    };

    static GB_SystemResult PrepareFindOptions(const GB_WindowFindOptions& options, PreparedFindOptions& preparedOptions)
    {
        preparedOptions = PreparedFindOptions();
        const std::string* strings[] =
        {
            &options.titleEquals,
            &options.titleContains,
            &options.classNameEquals,
            &options.classNameContains,
            &options.processNameEquals,
            &options.processPathEquals
        };
        for (size_t index = 0; index < sizeof(strings) / sizeof(strings[0]); index++)
        {
            if (!GB_IsUtf8(*strings[index]))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_WindowOperationFind, "窗口查找条件包含非法 UTF-8 字符串。");
            }
        }
        if (HasAnyWindowIdField(options.parentWindowId) && !options.parentWindowId.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_WindowOperationFind, "父窗口标识无效。");
        }

        try
        {
            preparedOptions.options = &options;
            if (!options.titleEquals.empty())
            {
                preparedOptions.titleEquals = GB_Utf8ToWString(options.titleEquals);
            }
            if (!options.titleContains.empty())
            {
                preparedOptions.titleContains = GB_Utf8ToWString(options.titleContains);
            }
            if (!options.classNameEquals.empty())
            {
                preparedOptions.classNameEquals = GB_Utf8ToWString(options.classNameEquals);
            }
            if (!options.classNameContains.empty())
            {
                preparedOptions.classNameContains = GB_Utf8ToWString(options.classNameContains);
            }
            if (!options.processNameEquals.empty())
            {
                preparedOptions.processNameEquals = GB_Utf8ToWString(options.processNameEquals);
            }
            if (!options.processPathEquals.empty())
            {
                preparedOptions.processPathEquals = GB_Utf8ToWString(options.processPathEquals);
            }
        }
        catch (const std::bad_alloc&)
        {
            return MakeAllocationFailedResult(GB_WindowOperationFind);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_WindowOperationFind, "转换窗口查找条件时编码转换失败。");
        }
        return GB_SystemResult::Succeeded(GB_WindowOperationFind);
    }

    static bool Utf8EqualsWideTarget(const std::string& text, const std::wstring& target, const bool caseSensitive)
    {
        if (text.empty() || target.empty())
        {
            return text.empty() && target.empty();
        }
        const std::wstring textWide = GB_Utf8ToWString(text);
        const int compareResult = ::CompareStringOrdinal(textWide.data(), static_cast<int>(textWide.size()), target.data(), static_cast<int>(target.size()), caseSensitive ? FALSE : TRUE);
        return compareResult == CSTR_EQUAL;
    }

    static bool Utf8ContainsWideTarget(const std::string& text, const std::wstring& target, const bool caseSensitive)
    {
        if (target.empty())
        {
            return true;
        }
        if (text.empty())
        {
            return false;
        }
        const std::wstring textWide = GB_Utf8ToWString(text);
        const DWORD flags = FIND_FROMSTART | (caseSensitive ? 0 : NORM_IGNORECASE);
        return ::FindNLSStringEx(LOCALE_NAME_INVARIANT, flags, textWide.data(), static_cast<int>(textWide.size()), target.data(), static_cast<int>(target.size()), nullptr, nullptr, nullptr, 0) >= 0;
    }

    static bool ShouldSkipWindowByCheapFindOptions(HWND windowHandle, const PreparedFindOptions& preparedOptions)
    {
        const GB_WindowFindOptions& options = *preparedOptions.options;
        if (options.processId != 0 || options.threadId != 0)
        {
            DWORD processId = 0;
            const DWORD threadId = ::GetWindowThreadProcessId(windowHandle, &processId);
            if (threadId == 0)
            {
                return true;
            }
            if (options.processId != 0 && processId != options.processId)
            {
                return true;
            }
            if (options.threadId != 0 && threadId != options.threadId)
            {
                return true;
            }
        }
        if (options.visibleOnly && ::IsWindowVisible(windowHandle) == FALSE)
        {
            return true;
        }
        if (!options.includeToolWindows)
        {
            ::SetLastError(ERROR_SUCCESS);
            const LONG_PTR extendedStyle = ::GetWindowLongPtrW(windowHandle, GWL_EXSTYLE);
            if ((extendedStyle != 0 || ::GetLastError() == ERROR_SUCCESS) && (extendedStyle & WS_EX_TOOLWINDOW) != 0)
            {
                return true;
            }
        }
        if (!options.includeCloakedWindows)
        {
            DWORD cloaked = 0;
            if (SUCCEEDED(::DwmGetWindowAttribute(windowHandle, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
            {
                return true;
            }
        }
        if (options.applicationWindowsOnly)
        {
            ::SetLastError(ERROR_SUCCESS);
            const LONG_PTR style = ::GetWindowLongPtrW(windowHandle, GWL_STYLE);
            const bool hasStyle = style != 0 || ::GetLastError() == ERROR_SUCCESS;
            ::SetLastError(ERROR_SUCCESS);
            const LONG_PTR extendedStyle = ::GetWindowLongPtrW(windowHandle, GWL_EXSTYLE);
            const bool hasExtendedStyle = extendedStyle != 0 || ::GetLastError() == ERROR_SUCCESS;
            if (hasStyle && hasExtendedStyle)
            {
                const bool isChildWindow = (style & WS_CHILD) != 0;
                const bool isToolWindow = (extendedStyle & WS_EX_TOOLWINDOW) != 0;
                const HWND ownerWindow = ::GetWindow(windowHandle, GW_OWNER);
                const bool isAppWindow = (extendedStyle & WS_EX_APPWINDOW) != 0 || (!isToolWindow && !isChildWindow && ownerWindow == nullptr);
                if (!isChildWindow && !isAppWindow)
                {
                    return true;
                }
            }
        }
        if (!options.includeUntitledWindows && preparedOptions.titleEquals.empty() && preparedOptions.titleContains.empty())
        {
            ::SetLastError(ERROR_SUCCESS);
            const int textLength = ::GetWindowTextLengthW(windowHandle);
            if (textLength == 0 && ::GetLastError() == ERROR_SUCCESS)
            {
                return true;
            }
        }
        if (!preparedOptions.titleEquals.empty() || !preparedOptions.titleContains.empty())
        {
            std::wstring title;
            if (!ReadWindowTitleWide(windowHandle, title))
            {
                return true;
            }
            if (!preparedOptions.titleEquals.empty() && !WideEquals(title, preparedOptions.titleEquals, options.caseSensitive))
            {
                return true;
            }
            if (!preparedOptions.titleContains.empty() && !WideContains(title, preparedOptions.titleContains, options.caseSensitive))
            {
                return true;
            }
        }
        if (!preparedOptions.classNameEquals.empty() || !preparedOptions.classNameContains.empty())
        {
            std::wstring className;
            if (!ReadWindowClassNameWide(windowHandle, className))
            {
                return true;
            }
            if (!preparedOptions.classNameEquals.empty() && !WideEquals(className, preparedOptions.classNameEquals, options.caseSensitive))
            {
                return true;
            }
            if (!preparedOptions.classNameContains.empty() && !WideContains(className, preparedOptions.classNameContains, options.caseSensitive))
            {
                return true;
            }
        }
        return false;
    }

    static bool MatchesFindOptions(const GB_WindowInfo& windowInfo, const PreparedFindOptions& preparedOptions)
    {
        const GB_WindowFindOptions& options = *preparedOptions.options;
        if (options.processId != 0 && windowInfo.windowId.processId != options.processId)
        {
            return false;
        }
        if (options.threadId != 0 && windowInfo.windowId.threadId != options.threadId)
        {
            return false;
        }
        if (options.visibleOnly && !windowInfo.isVisible)
        {
            return false;
        }
        if (!options.includeToolWindows && windowInfo.isToolWindow)
        {
            return false;
        }
        if (!options.includeCloakedWindows && windowInfo.isCloaked)
        {
            return false;
        }
        if (!options.includeUntitledWindows && windowInfo.title.empty())
        {
            return false;
        }
        if (options.applicationWindowsOnly && !windowInfo.isChildWindow && !windowInfo.isAppWindow)
        {
            return false;
        }
        if (!preparedOptions.titleEquals.empty() && !Utf8EqualsWideTarget(windowInfo.title, preparedOptions.titleEquals, options.caseSensitive))
        {
            return false;
        }
        if (!preparedOptions.titleContains.empty() && !Utf8ContainsWideTarget(windowInfo.title, preparedOptions.titleContains, options.caseSensitive))
        {
            return false;
        }
        if (!preparedOptions.classNameEquals.empty() && !Utf8EqualsWideTarget(windowInfo.className, preparedOptions.classNameEquals, options.caseSensitive))
        {
            return false;
        }
        if (!preparedOptions.classNameContains.empty() && !Utf8ContainsWideTarget(windowInfo.className, preparedOptions.classNameContains, options.caseSensitive))
        {
            return false;
        }
        if (!preparedOptions.processNameEquals.empty() && (!windowInfo.hasProcessName || !Utf8EqualsWideTarget(windowInfo.processName, preparedOptions.processNameEquals, options.caseSensitive)))
        {
            return false;
        }
        if (!preparedOptions.processPathEquals.empty() && (!windowInfo.hasProcessPath || !Utf8EqualsWideTarget(windowInfo.processPath, preparedOptions.processPathEquals, options.caseSensitive)))
        {
            return false;
        }
        return true;
    }

    struct EnumerationContext
    {
        std::vector<GB_WindowInfo>* windows = nullptr;
        const PreparedFindOptions* findOptions = nullptr;
        ProcessIdentityCache* processCache = nullptr;
        GB_SystemResult result = GB_SystemResult::Succeeded(GB_WindowOperationFind);
        bool stopAfterFirst = false;
        bool stoppedByRequest = false;
    };

    static BOOL CALLBACK EnumerateWindowCallback(HWND windowHandle, LPARAM parameter)
    {
        EnumerationContext* context = reinterpret_cast<EnumerationContext*>(parameter);
        if (context == nullptr || context->windows == nullptr)
        {
            return FALSE;
        }

        if (context->findOptions != nullptr && ShouldSkipWindowByCheapFindOptions(windowHandle, *context->findOptions))
        {
            return TRUE;
        }

        GB_WindowInfo windowInfo;
        const GB_SystemResult infoResult = BuildWindowInfo(windowHandle, windowInfo, context->processCache, GB_WindowOperationFind);
        if (infoResult.IsFailed())
        {
            if (infoResult.errorCode == GB_SystemErrorCode::NotFound)
            {
                return TRUE;
            }
            context->result = infoResult;
            return FALSE;
        }

        try
        {
            if (context->findOptions == nullptr || MatchesFindOptions(windowInfo, *context->findOptions))
            {
                context->windows->push_back(std::move(windowInfo));
                if (context->stopAfterFirst)
                {
                    context->stoppedByRequest = true;
                    return FALSE;
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            context->result = MakeAllocationFailedResult(GB_WindowOperationFind);
            return FALSE;
        }
        catch (...)
        {
            context->result = GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_WindowOperationFind, "匹配窗口文本时编码转换失败。");
            return FALSE;
        }
        return TRUE;
    }

    struct WindowHandleEnumerationContext
    {
        std::vector<HWND>* windowHandles = nullptr;
        GB_SystemResult result = GB_SystemResult::Succeeded(GB_WindowOperationFind);
    };

    static BOOL CALLBACK EnumerateWindowHandleCallback(HWND windowHandle, LPARAM parameter)
    {
        WindowHandleEnumerationContext* context = reinterpret_cast<WindowHandleEnumerationContext*>(parameter);
        if (context == nullptr || context->windowHandles == nullptr)
        {
            return FALSE;
        }

        try
        {
            context->windowHandles->push_back(windowHandle);
            return TRUE;
        }
        catch (const std::bad_alloc&)
        {
            context->result = MakeAllocationFailedResult(GB_WindowOperationFind);
            return FALSE;
        }
        catch (...)
        {
            context->result = GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, GB_WindowOperationFind, "缓存顶层窗口句柄时发生内部异常。");
            return FALSE;
        }
    }

    static GB_SystemResult EnumerateTopLevelWindowHandles(std::vector<HWND>& windowHandles, const std::string& operationName)
    {
        windowHandles.clear();
        WindowHandleEnumerationContext context;
        context.windowHandles = &windowHandles;
        context.result = GB_SystemResult::Succeeded(operationName);

        ::SetLastError(ERROR_SUCCESS);
        const BOOL enumerationResult = ::EnumWindows(&EnumerateWindowHandleCallback, reinterpret_cast<LPARAM>(&context));
        if (context.result.IsFailed())
        {
            return context.result.WithOperationName(operationName);
        }
        if (enumerationResult == FALSE)
        {
            const DWORD lastError = ::GetLastError();
            if (lastError != ERROR_SUCCESS)
            {
                return GB_SystemResult::FromWin32Error(lastError, operationName, "EnumWindows 枚举顶层窗口句柄失败。");
            }
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult EnumerateSingleWindow(HWND windowHandle, std::vector<GB_WindowInfo>& windows, const PreparedFindOptions* options, ProcessIdentityCache& processCache, const bool stopAfterFirst)
    {
        EnumerationContext context;
        context.windows = &windows;
        context.findOptions = options;
        context.processCache = &processCache;
        context.stopAfterFirst = stopAfterFirst;
        (void)EnumerateWindowCallback(windowHandle, reinterpret_cast<LPARAM>(&context));
        return context.result;
    }

    static GB_SystemResult EnumerateTopLevelWindows(std::vector<GB_WindowInfo>& windows, const PreparedFindOptions* options, ProcessIdentityCache& processCache, const bool stopAfterFirst)
    {
        EnumerationContext context;
        context.windows = &windows;
        context.findOptions = options;
        context.processCache = &processCache;
        context.stopAfterFirst = stopAfterFirst;
        ::SetLastError(ERROR_SUCCESS);
        const BOOL enumerationResult = ::EnumWindows(&EnumerateWindowCallback, reinterpret_cast<LPARAM>(&context));
        if (context.result.IsFailed())
        {
            return context.result;
        }
        if (enumerationResult == FALSE && !context.stoppedByRequest)
        {
            const DWORD lastError = ::GetLastError();
            if (lastError != ERROR_SUCCESS)
            {
                return GB_SystemResult::FromWin32Error(lastError, GB_WindowOperationFind, "EnumWindows 枚举顶层窗口失败。");
            }
        }
        return GB_SystemResult::Succeeded(GB_WindowOperationFind);
    }

    static GB_SystemResult EnumerateDescendantWindows(HWND parentWindow, std::vector<GB_WindowInfo>& windows, const PreparedFindOptions* options, ProcessIdentityCache& processCache, const bool recursive, const bool stopAfterFirst)
    {
        EnumerationContext context;
        context.windows = &windows;
        context.findOptions = options;
        context.processCache = &processCache;
        context.stopAfterFirst = stopAfterFirst;

        if (recursive)
        {
            (void)::EnumChildWindows(parentWindow, &EnumerateWindowCallback, reinterpret_cast<LPARAM>(&context));
            if (context.result.IsFailed())
            {
                return context.result;
            }
            return GB_SystemResult::Succeeded(GB_WindowOperationGetChildren);
        }

        for (HWND childWindow = ::GetWindow(parentWindow, GW_CHILD); childWindow != nullptr; childWindow = ::GetWindow(childWindow, GW_HWNDNEXT))
        {
            if (EnumerateWindowCallback(childWindow, reinterpret_cast<LPARAM>(&context)) == FALSE)
            {
                break;
            }
        }
        return context.result;
    }

    static bool TryConvertRectangle(const GB_Rectangle& rectangle, int& x, int& y, int& width, int& height)
    {
        if (!rectangle.IsValid() || !std::isfinite(rectangle.minX) || !std::isfinite(rectangle.minY) || !std::isfinite(rectangle.maxX) || !std::isfinite(rectangle.maxY))
        {
            return false;
        }

        const double left = std::floor(rectangle.minX);
        const double top = std::floor(rectangle.minY);
        const double right = std::ceil(rectangle.maxX);
        const double bottom = std::ceil(rectangle.maxY);
        const double rectangleWidth = right - left;
        const double rectangleHeight = bottom - top;
        const double intMinimum = static_cast<double>((std::numeric_limits<int>::min)());
        const double intMaximum = static_cast<double>((std::numeric_limits<int>::max)());
        if (left < intMinimum || left > intMaximum || top < intMinimum || top > intMaximum || right < intMinimum || right > intMaximum || bottom < intMinimum || bottom > intMaximum || rectangleWidth < 0 || rectangleWidth > intMaximum || rectangleHeight < 0 || rectangleHeight > intMaximum)
        {
            return false;
        }

        x = static_cast<int>(left);
        y = static_cast<int>(top);
        width = static_cast<int>(rectangleWidth);
        height = static_cast<int>(rectangleHeight);
        return true;
    }

    static GB_SystemResult RequestShowState(const GB_WindowId& windowId, const int showCommand, const std::string& operationName)
    {
        HWND windowHandle = nullptr;
        const GB_SystemResult validationResult = ValidateWindowId(windowId, windowHandle, operationName);
        if (validationResult.IsFailed())
        {
            return validationResult;
        }

        ::SetLastError(ERROR_SUCCESS);
        if (::ShowWindowAsync(windowHandle, showCommand) == FALSE)
        {
            return MakeLastWin32ErrorOrNativeApiFailedResult(operationName, "ShowWindowAsync 提交窗口显示状态请求失败。");
        }
        return GB_SystemResult::Succeeded(operationName, "窗口显示状态请求已提交；目标线程会异步处理该请求。");
    }

    static bool RectanglesHaveSamePosition(const GB_Rectangle& left, const GB_Rectangle& right)
    {
        return left.IsValid() && right.IsValid() && left.minX == right.minX && left.minY == right.minY;
    }

    static bool RectanglesHaveSameSize(const GB_Rectangle& left, const GB_Rectangle& right)
    {
        return left.IsValid() && right.IsValid() && left.Width() == right.Width() && left.Height() == right.Height();
    }


    static bool IsTopLevelWindowHandle(HWND windowHandle)
    {
        if (windowHandle == nullptr)
        {
            return false;
        }

        const HWND rootWindow = ::GetAncestor(windowHandle, GA_ROOT);
        return rootWindow == windowHandle;
    }
#endif
}

bool GB_WindowId::IsValid() const
{
    return nativeHandle != 0 && processId != 0 && threadId != 0;
}

void GB_WindowId::Reset()
{
    nativeHandle = 0;
    processId = 0;
    threadId = 0;
}

GB_WindowId::operator bool() const
{
    return IsValid();
}

bool GB_WindowId::operator==(const GB_WindowId& other) const
{
    return nativeHandle == other.nativeHandle && processId == other.processId && threadId == other.threadId;
}

bool GB_WindowId::operator!=(const GB_WindowId& other) const
{
    return !(*this == other);
}

GB_SystemResult GB_SystemWindow::GetForegroundWindow(GB_WindowInfo& windowInfo, bool& found)
{
    windowInfo = GB_WindowInfo();
    found = false;
#if defined(_WIN32)
    const HWND windowHandle = ::GetForegroundWindow();
    if (windowHandle == nullptr)
    {
        return GB_SystemResult::Succeeded(GB_WindowOperationGetForeground, "当前没有前台窗口。");
    }

    ProcessIdentityCache processCache;
    const GB_SystemResult result = BuildWindowInfo(windowHandle, windowInfo, &processCache, GB_WindowOperationGetForeground);
    if (result.IsSucceeded())
    {
        found = true;
    }
    return result;
#else
    return MakeUnsupportedPlatformResult(GB_WindowOperationGetForeground);
#endif
}

GB_SystemResult GB_SystemWindow::GetTopLevelWindows(std::vector<GB_WindowInfo>& windows)
{
    windows.clear();
#if defined(_WIN32)
    try
    {
        ProcessIdentityCache processCache;
        return EnumerateTopLevelWindows(windows, nullptr, processCache, false).WithOperationName(GB_WindowOperationGetTopLevel);
    }
    catch (const std::bad_alloc&)
    {
        return MakeAllocationFailedResult(GB_WindowOperationGetTopLevel);
    }
#else
    return MakeUnsupportedPlatformResult(GB_WindowOperationGetTopLevel);
#endif
}

GB_SystemResult GB_SystemWindow::GetChildWindows(const GB_WindowId& parentWindowId, std::vector<GB_WindowInfo>& windows, const bool recursive)
{
    windows.clear();
#if defined(_WIN32)
    HWND parentWindow = nullptr;
    const GB_SystemResult validationResult = ValidateWindowId(parentWindowId, parentWindow, GB_WindowOperationGetChildren);
    if (validationResult.IsFailed())
    {
        return validationResult;
    }

    try
    {
        ProcessIdentityCache processCache;
        return EnumerateDescendantWindows(parentWindow, windows, nullptr, processCache, recursive, false).WithOperationName(GB_WindowOperationGetChildren);
    }
    catch (const std::bad_alloc&)
    {
        return MakeAllocationFailedResult(GB_WindowOperationGetChildren);
    }
#else
    (void)parentWindowId;
    (void)recursive;
    return MakeUnsupportedPlatformResult(GB_WindowOperationGetChildren);
#endif
}

GB_SystemResult GB_SystemWindow::FindWindows(const GB_WindowFindOptions& options, std::vector<GB_WindowInfo>& windows)
{
    windows.clear();
#if defined(_WIN32)
    PreparedFindOptions preparedOptions;
    const GB_SystemResult optionsResult = PrepareFindOptions(options, preparedOptions);
    if (optionsResult.IsFailed())
    {
        return optionsResult;
    }

    try
    {
        ProcessIdentityCache processCache;
        if (options.parentWindowId.IsValid())
        {
            HWND parentWindow = nullptr;
            const GB_SystemResult parentResult = ValidateWindowId(options.parentWindowId, parentWindow, GB_WindowOperationFind);
            if (parentResult.IsFailed())
            {
                return parentResult;
            }
            return EnumerateDescendantWindows(parentWindow, windows, &preparedOptions, processCache, true, false).WithOperationName(GB_WindowOperationFind);
        }

        if (!options.includeChildWindows)
        {
            return EnumerateTopLevelWindows(windows, &preparedOptions, processCache, false).WithOperationName(GB_WindowOperationFind);
        }

        std::vector<HWND> topLevelHandles;
        GB_SystemResult result = EnumerateTopLevelWindowHandles(topLevelHandles, GB_WindowOperationFind);
        if (result.IsFailed())
        {
            return result.WithOperationName(GB_WindowOperationFind);
        }
        for (size_t index = 0; index < topLevelHandles.size(); index++)
        {
            HWND topLevelHandle = topLevelHandles[index];
            if (topLevelHandle == nullptr || ::IsWindow(topLevelHandle) == FALSE)
            {
                continue;
            }

            result = EnumerateSingleWindow(topLevelHandle, windows, &preparedOptions, processCache, false);
            if (result.IsFailed())
            {
                return result.WithOperationName(GB_WindowOperationFind);
            }
            if (::IsWindow(topLevelHandle) == FALSE)
            {
                continue;
            }

            result = EnumerateDescendantWindows(topLevelHandle, windows, &preparedOptions, processCache, true, false);
            if (result.IsFailed())
            {
                return result.WithOperationName(GB_WindowOperationFind);
            }
        }
        return GB_SystemResult::Succeeded(GB_WindowOperationFind);
    }
    catch (const std::bad_alloc&)
    {
        return MakeAllocationFailedResult(GB_WindowOperationFind);
    }
    catch (...)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_WindowOperationFind, "匹配窗口查找条件时编码转换失败。");
    }
#else
    (void)options;
    return MakeUnsupportedPlatformResult(GB_WindowOperationFind);
#endif
}

GB_SystemResult GB_SystemWindow::FindFirstWindow(const GB_WindowFindOptions& options, GB_WindowInfo& windowInfo, bool& found)
{
    windowInfo = GB_WindowInfo();
    found = false;
#if defined(_WIN32)
    PreparedFindOptions preparedOptions;
    const GB_SystemResult optionsResult = PrepareFindOptions(options, preparedOptions);
    if (optionsResult.IsFailed())
    {
        return optionsResult;
    }

    try
    {
        std::vector<GB_WindowInfo> windows;
        ProcessIdentityCache processCache;
        GB_SystemResult result;
        if (options.parentWindowId.IsValid())
        {
            HWND parentWindow = nullptr;
            result = ValidateWindowId(options.parentWindowId, parentWindow, GB_WindowOperationFind);
            if (result.IsFailed())
            {
                return result;
            }
            result = EnumerateDescendantWindows(parentWindow, windows, &preparedOptions, processCache, true, true);
        }
        else
        {
            result = EnumerateTopLevelWindows(windows, &preparedOptions, processCache, true);
            if (result.IsSucceeded() && windows.empty() && options.includeChildWindows)
            {
                std::vector<HWND> topLevelHandles;
                result = EnumerateTopLevelWindowHandles(topLevelHandles, GB_WindowOperationFind);
                for (size_t index = 0; result.IsSucceeded() && index < topLevelHandles.size() && windows.empty(); index++)
                {
                    HWND topLevelHandle = topLevelHandles[index];
                    if (topLevelHandle == nullptr || ::IsWindow(topLevelHandle) == FALSE)
                    {
                        continue;
                    }
                    result = EnumerateDescendantWindows(topLevelHandle, windows, &preparedOptions, processCache, true, true);
                }
            }
        }

        if (result.IsFailed())
        {
            return result.WithOperationName(GB_WindowOperationFind);
        }
        if (!windows.empty())
        {
            windowInfo = std::move(windows.front());
            found = true;
        }
        return GB_SystemResult::Succeeded(GB_WindowOperationFind);
    }
    catch (const std::bad_alloc&)
    {
        return MakeAllocationFailedResult(GB_WindowOperationFind);
    }
    catch (...)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_WindowOperationFind, "匹配窗口查找条件时编码转换失败。");
    }
#else
    (void)options;
    return MakeUnsupportedPlatformResult(GB_WindowOperationFind);
#endif
}

GB_SystemResult GB_SystemWindow::GetWindowInfo(const GB_WindowId& windowId, GB_WindowInfo& windowInfo)
{
    windowInfo = GB_WindowInfo();
#if defined(_WIN32)
    HWND windowHandle = nullptr;
    const GB_SystemResult validationResult = ValidateWindowId(windowId, windowHandle, GB_WindowOperationGetInfo);
    if (validationResult.IsFailed())
    {
        return validationResult;
    }
    ProcessIdentityCache processCache;
    return BuildWindowInfo(windowHandle, windowInfo, &processCache, GB_WindowOperationGetInfo);
#else
    (void)windowId;
    return MakeUnsupportedPlatformResult(GB_WindowOperationGetInfo);
#endif
}

GB_SystemResult GB_SystemWindow::IsWindowAlive(const GB_WindowId& windowId, bool& alive)
{
    alive = false;
#if defined(_WIN32)
    if (!windowId.IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_WindowOperationIsAlive, "窗口标识无效。");
    }
    HWND windowHandle = nullptr;
    const GB_SystemResult validationResult = ValidateWindowId(windowId, windowHandle, GB_WindowOperationIsAlive);
    if (validationResult.IsSucceeded())
    {
        alive = true;
        return validationResult;
    }
    if (validationResult.errorCode == GB_SystemErrorCode::NotFound)
    {
        return GB_SystemResult::Succeeded(GB_WindowOperationIsAlive, "目标窗口已不存在或窗口句柄已被复用。");
    }
    return validationResult;
#else
    (void)windowId;
    return MakeUnsupportedPlatformResult(GB_WindowOperationIsAlive);
#endif
}

GB_SystemResult GB_SystemWindow::ShowWindowNormal(const GB_WindowId& windowId, const bool activate)
{
#if defined(_WIN32)
    return RequestShowState(windowId, activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE, GB_WindowOperationShow);
#else
    (void)windowId;
    (void)activate;
    return MakeUnsupportedPlatformResult(GB_WindowOperationShow);
#endif
}

GB_SystemResult GB_SystemWindow::ShowWindowWithoutActivation(const GB_WindowId& windowId)
{
#if defined(_WIN32)
    return RequestShowState(windowId, SW_SHOWNOACTIVATE, GB_WindowOperationShow);
#else
    (void)windowId;
    return MakeUnsupportedPlatformResult(GB_WindowOperationShow);
#endif
}

GB_SystemResult GB_SystemWindow::HideWindow(const GB_WindowId& windowId)
{
#if defined(_WIN32)
    return RequestShowState(windowId, SW_HIDE, GB_WindowOperationShow);
#else
    (void)windowId;
    return MakeUnsupportedPlatformResult(GB_WindowOperationShow);
#endif
}

GB_SystemResult GB_SystemWindow::MinimizeWindow(const GB_WindowId& windowId)
{
#if defined(_WIN32)
    return RequestShowState(windowId, SW_MINIMIZE, GB_WindowOperationShow);
#else
    (void)windowId;
    return MakeUnsupportedPlatformResult(GB_WindowOperationShow);
#endif
}

GB_SystemResult GB_SystemWindow::MaximizeWindow(const GB_WindowId& windowId)
{
#if defined(_WIN32)
    return RequestShowState(windowId, SW_MAXIMIZE, GB_WindowOperationShow);
#else
    (void)windowId;
    return MakeUnsupportedPlatformResult(GB_WindowOperationShow);
#endif
}

GB_SystemResult GB_SystemWindow::RestoreWindow(const GB_WindowId& windowId)
{
#if defined(_WIN32)
    return RequestShowState(windowId, SW_RESTORE, GB_WindowOperationShow);
#else
    (void)windowId;
    return MakeUnsupportedPlatformResult(GB_WindowOperationShow);
#endif
}

GB_SystemResult GB_SystemWindow::MoveResizeWindow(const GB_WindowId& windowId, const GB_Rectangle& rectangle, const bool repaint, const bool activate)
{
#if defined(_WIN32)
    HWND windowHandle = nullptr;
    const GB_SystemResult validationResult = ValidateWindowId(windowId, windowHandle, GB_WindowOperationMoveResize);
    if (validationResult.IsFailed())
    {
        return validationResult;
    }

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (!TryConvertRectangle(rectangle, x, y, width, height))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_WindowOperationMoveResize, "窗口矩形无效、包含非有限值或超出 Win32 整数范围。");
    }

    const DpiAwarenessScope dpiAwarenessScope;
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR style = ::GetWindowLongPtrW(windowHandle, GWL_STYLE);
    const bool hasStyle = style != 0 || ::GetLastError() == ERROR_SUCCESS;
    if (hasStyle && (style & WS_CHILD) != 0)
    {
        const HWND parentWindow = ::GetParent(windowHandle);
        if (parentWindow == nullptr)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_WindowOperationMoveResize, "目标子窗口的父窗口已不存在。");
        }

        POINT parentClientPoint = { x, y };
        ::SetLastError(ERROR_SUCCESS);
        const int mapResult = ::MapWindowPoints(nullptr, parentWindow, &parentClientPoint, 1);
        if (mapResult == 0 && ::GetLastError() != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromLastWin32Error(GB_WindowOperationMoveResize, "将子窗口屏幕坐标转换为父客户区坐标失败。");
        }
        x = parentClientPoint.x;
        y = parentClientPoint.y;
    }

    UINT flags = SWP_NOOWNERZORDER | SWP_NOZORDER;
    if (!repaint)
    {
        flags |= SWP_NOREDRAW;
    }
    if (!activate)
    {
        flags |= SWP_NOACTIVATE;
    }
    if (::GetWindowThreadProcessId(windowHandle, nullptr) != ::GetCurrentThreadId())
    {
        flags |= SWP_ASYNCWINDOWPOS;
    }

    ::SetLastError(ERROR_SUCCESS);
    if (::SetWindowPos(windowHandle, nullptr, x, y, width, height, flags) == FALSE)
    {
        return MakeLastWin32ErrorOrNativeApiFailedResult(GB_WindowOperationMoveResize, "SetWindowPos 移动或缩放窗口失败。");
    }
    return GB_SystemResult::Succeeded(GB_WindowOperationMoveResize, (flags & SWP_ASYNCWINDOWPOS) != 0 ? "窗口位置请求已异步提交。" : "窗口位置已更新。");
#else
    (void)windowId;
    (void)rectangle;
    (void)repaint;
    (void)activate;
    return MakeUnsupportedPlatformResult(GB_WindowOperationMoveResize);
#endif
}

GB_SystemResult GB_SystemWindow::SetTopMost(const GB_WindowId& windowId, const bool topMost)
{
#if defined(_WIN32)
    HWND windowHandle = nullptr;
    const GB_SystemResult validationResult = ValidateWindowId(windowId, windowHandle, GB_WindowOperationSetTopMost);
    if (validationResult.IsFailed())
    {
        return validationResult;
    }

    UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (::GetWindowThreadProcessId(windowHandle, nullptr) != ::GetCurrentThreadId())
    {
        flags |= SWP_ASYNCWINDOWPOS;
    }
    ::SetLastError(ERROR_SUCCESS);
    if (::SetWindowPos(windowHandle, topMost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags) == FALSE)
    {
        return MakeLastWin32ErrorOrNativeApiFailedResult(GB_WindowOperationSetTopMost, "SetWindowPos 修改窗口置顶状态失败。");
    }
    return GB_SystemResult::Succeeded(GB_WindowOperationSetTopMost, (flags & SWP_ASYNCWINDOWPOS) != 0 ? "窗口置顶状态请求已异步提交。" : "窗口置顶状态已更新。");
#else
    (void)windowId;
    (void)topMost;
    return MakeUnsupportedPlatformResult(GB_WindowOperationSetTopMost);
#endif
}

GB_SystemResult GB_SystemWindow::TryActivateWindow(const GB_WindowId& windowId, const bool restoreIfMinimized)
{
#if defined(_WIN32)
    HWND windowHandle = nullptr;
    const GB_SystemResult validationResult = ValidateWindowId(windowId, windowHandle, GB_WindowOperationActivate);
    if (validationResult.IsFailed())
    {
        return validationResult;
    }
    if (::GetForegroundWindow() == windowHandle)
    {
        return GB_SystemResult::Succeeded(GB_WindowOperationActivate, "目标窗口已经是前台窗口。");
    }

    if (restoreIfMinimized && ::IsIconic(windowHandle) != FALSE)
    {
        (void)::ShowWindowAsync(windowHandle, SW_RESTORE);
    }
    else if (::IsWindowVisible(windowHandle) == FALSE)
    {
        (void)::ShowWindowAsync(windowHandle, SW_SHOWNORMAL);
    }

    UINT setPositionFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER;
    if (::GetWindowThreadProcessId(windowHandle, nullptr) != ::GetCurrentThreadId())
    {
        setPositionFlags |= SWP_ASYNCWINDOWPOS;
    }
    (void)::SetWindowPos(windowHandle, HWND_TOP, 0, 0, 0, 0, setPositionFlags);
    (void)::BringWindowToTop(windowHandle);
    const BOOL foregroundResult = ::SetForegroundWindow(windowHandle);
    if (foregroundResult != FALSE)
    {
        return GB_SystemResult::Succeeded(GB_WindowOperationActivate, "SetForegroundWindow 已接受目标窗口激活请求。");
    }
    if (::GetForegroundWindow() == windowHandle)
    {
        return GB_SystemResult::Succeeded(GB_WindowOperationActivate, "目标窗口已经成为前台窗口。");
    }
    return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, GB_WindowOperationActivate, "Windows 前台窗口保护策略或目标窗口状态阻止了激活；模块未尝试绕过系统限制。");
#else
    (void)windowId;
    (void)restoreIfMinimized;
    return MakeUnsupportedPlatformResult(GB_WindowOperationActivate);
#endif
}

GB_SystemResult GB_SystemWindow::RequestCloseWindow(const GB_WindowId& windowId, const uint32_t sendTimeoutMilliseconds)
{
#if defined(_WIN32)
    if (sendTimeoutMilliseconds == 0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_WindowOperationClose, "sendTimeoutMilliseconds 必须大于 0。");
    }

    HWND windowHandle = nullptr;
    const GB_SystemResult validationResult = ValidateWindowId(windowId, windowHandle, GB_WindowOperationClose);
    if (validationResult.IsFailed())
    {
        return validationResult;
    }

    const DWORD targetThreadId = ::GetWindowThreadProcessId(windowHandle, nullptr);
    if (targetThreadId == ::GetCurrentThreadId())
    {
        ::SetLastError(ERROR_SUCCESS);
        if (::PostMessageW(windowHandle, WM_CLOSE, 0, 0) == FALSE)
        {
            return MakeLastWin32ErrorOrNativeApiFailedResult(GB_WindowOperationClose, "向当前线程目标窗口投递 WM_CLOSE 失败。");
        }
        return GB_SystemResult::Succeeded(GB_WindowOperationClose, "目标窗口与调用方位于同一线程，WM_CLOSE 已异步投递；该结果不保证窗口已经销毁。");
    }

    DWORD_PTR messageResult = 0;
    ::SetLastError(ERROR_SUCCESS);
    const LRESULT sendResult = ::SendMessageTimeoutW(windowHandle, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, static_cast<UINT>(sendTimeoutMilliseconds), &messageResult);
    if (sendResult == 0)
    {
        const DWORD lastError = ::GetLastError();
        if (::IsWindow(windowHandle) == FALSE)
        {
            return GB_SystemResult::Succeeded(GB_WindowOperationClose, "目标窗口在 WM_CLOSE 处理期间已销毁。");
        }
        if (lastError == ERROR_TIMEOUT)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, GB_WindowOperationClose, "目标窗口未在限定时间内处理 WM_CLOSE。");
        }
        if (lastError == ERROR_SUCCESS)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, GB_WindowOperationClose, "目标窗口未明确完成 WM_CLOSE 处理，系统未提供扩展错误码。");
        }
        return GB_SystemResult::FromWin32Error(lastError, GB_WindowOperationClose, "向目标窗口发送 WM_CLOSE 失败。");
    }
    return GB_SystemResult::Succeeded(GB_WindowOperationClose, "目标窗口已处理 WM_CLOSE 请求；该结果不保证窗口已经销毁。");
#else
    (void)windowId;
    (void)sendTimeoutMilliseconds;
    return MakeUnsupportedPlatformResult(GB_WindowOperationClose);
#endif
}

GB_SystemResult GB_SystemWindow::WaitForWindow(const GB_WindowFindOptions& findOptions, GB_WindowInfo& windowInfo, const GB_WindowWaitOptions& waitOptions)
{
    windowInfo = GB_WindowInfo();
    return WaitUntil(waitOptions, "GB_SystemWindow::WaitForWindow", [&findOptions, &windowInfo](bool& completed)
        {
            bool found = false;
            const GB_SystemResult result = GB_SystemWindow::FindFirstWindow(findOptions, windowInfo, found);
            completed = found;
            return result;
        });
}

GB_SystemResult GB_SystemWindow::WaitForWindowClosed(const GB_WindowId& windowId, const GB_WindowWaitOptions& waitOptions)
{
    if (!windowId.IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemWindow::WaitForWindowClosed", "窗口标识无效。");
    }
    return WaitUntil(waitOptions, "GB_SystemWindow::WaitForWindowClosed", [&windowId](bool& completed)
        {
            bool alive = false;
            const GB_SystemResult result = GB_SystemWindow::IsWindowAlive(windowId, alive);
            completed = result.IsSucceeded() && !alive;
            return result;
        });
}

GB_SystemResult GB_SystemWindow::WaitForWindowVisible(const GB_WindowId& windowId, GB_WindowInfo& windowInfo, const GB_WindowWaitOptions& waitOptions)
{
    windowInfo = GB_WindowInfo();
    return WaitUntil(waitOptions, "GB_SystemWindow::WaitForWindowVisible", [&windowId, &windowInfo](bool& completed)
        {
            const GB_SystemResult result = GB_SystemWindow::GetWindowInfo(windowId, windowInfo);
            completed = result.IsSucceeded() && windowInfo.isVisible;
            return result;
        });
}

GB_SystemResult GB_SystemWindow::WaitForWindowTitleChanged(const GB_WindowId& windowId, const std::string& originalTitle, GB_WindowInfo& windowInfo, const GB_WindowWaitOptions& waitOptions)
{
    windowInfo = GB_WindowInfo();
    if (!GB_IsUtf8(originalTitle))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemWindow::WaitForWindowTitleChanged", "originalTitle 不是合法 UTF-8。");
    }
    return WaitUntil(waitOptions, "GB_SystemWindow::WaitForWindowTitleChanged", [&windowId, &originalTitle, &windowInfo](bool& completed)
        {
            const GB_SystemResult result = GB_SystemWindow::GetWindowInfo(windowId, windowInfo);
            completed = result.IsSucceeded() && windowInfo.hasTitle && windowInfo.title != originalTitle;
            return result;
        });
}

GB_SystemResult GB_SystemWindow::WaitForForegroundWindow(const GB_WindowId& windowId, const GB_WindowWaitOptions& waitOptions)
{
    if (!windowId.IsValid())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemWindow::WaitForForegroundWindow", "窗口标识无效。");
    }
    return WaitUntil(waitOptions, "GB_SystemWindow::WaitForForegroundWindow", [&windowId](bool& completed)
        {
            bool alive = false;
            const GB_SystemResult aliveResult = GB_SystemWindow::IsWindowAlive(windowId, alive);
            if (aliveResult.IsFailed() || !alive)
            {
                completed = false;
                return aliveResult.IsFailed() ? aliveResult : GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, "GB_SystemWindow::WaitForForegroundWindow", "目标窗口在等待期间已销毁。");
            }
#if defined(_WIN32)
            completed = ::GetForegroundWindow() == WindowIdToHandle(windowId);
#else
            completed = false;
#endif
            return GB_SystemResult::Succeeded("GB_SystemWindow::WaitForForegroundWindow");
        });
}

std::string GB_SystemWindow::GetWindowEventTypeName(const GB_SystemWindowEventType eventType)
{
    switch (eventType)
    {
    case GB_SystemWindowEventType::Created:
        return "Created";
    case GB_SystemWindowEventType::Destroyed:
        return "Destroyed";
    case GB_SystemWindowEventType::Shown:
        return "Shown";
    case GB_SystemWindowEventType::Hidden:
        return "Hidden";
    case GB_SystemWindowEventType::LocationChanged:
        return "LocationChanged";
    case GB_SystemWindowEventType::Moved:
        return "Moved";
    case GB_SystemWindowEventType::Resized:
        return "Resized";
    case GB_SystemWindowEventType::Minimized:
        return "Minimized";
    case GB_SystemWindowEventType::Restored:
        return "Restored";
    case GB_SystemWindowEventType::TitleChanged:
        return "TitleChanged";
    case GB_SystemWindowEventType::ForegroundChanged:
        return "ForegroundChanged";
    case GB_SystemWindowEventType::Focused:
        return "Focused";
    case GB_SystemWindowEventType::Unknown:
    default:
        return "Unknown";
    }
}

#if defined(_WIN32)
class GB_SystemWindowWatcher::Impl final
{
public:
    explicit Impl(const GB_SystemWindowWatcherOptions& inputOptions)
        : options(inputOptions),
        typedDispatcher(GB_EventDispatcher::MakeQueuedOptions(inputOptions.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropOldest, "GB_SystemWindowWatcher.Typed")),
        publicDispatcher(GB_EventDispatcher::MakeQueuedOptions(inputOptions.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropOldest, "GB_SystemWindowWatcher.Public"))
    {
        callbackSetupResult = typedDispatcher.SubscribeAll([this](const GB_Event& event)
            {
                DispatchTypedCallback(event);
            }, typedSubscriptionToken);
    }

    ~Impl() noexcept
    {
        (void)Stop();
    }

    GB_SystemResult Start()
    {
#if defined(_WIN32)
        std::unique_lock<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (running)
            {
                return GB_SystemResult::Succeeded(GB_WindowOperationWatcherStart, "窗口监听器已经启动。");
            }
        }

        if (callbackSetupResult.IsFailed())
        {
            return callbackSetupResult.WithOperationName(GB_WindowOperationWatcherStart);
        }
        const GB_SystemResult optionsResult = ValidateOptions();
        if (optionsResult.IsFailed())
        {
            return optionsResult;
        }

        GB_SystemResult result = JoinPreviousThreadsBeforeStart();
        if (result.IsFailed())
        {
            return result;
        }

        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            stopping = false;
        }
        {
            std::lock_guard<std::mutex> queueLock(nativeQueueMutex);
            pendingNativeEvents.clear();
            nativeWorkerStopRequested = false;
            nativeWorkerProcessingEnabled = false;
        }
        {
            std::lock_guard<std::mutex> cacheLock(cacheMutex);
            windowCache.clear();
        }
        acceptingNativeEvents.store(false, std::memory_order_release);
        droppedNativeEventCount.store(0, std::memory_order_release);

        result = typedDispatcher.Start();
        if (result.IsFailed())
        {
            return result.WithOperationName(GB_WindowOperationWatcherStart);
        }
        result = publicDispatcher.Start();
        if (result.IsFailed())
        {
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return result.WithOperationName(GB_WindowOperationWatcherStart);
        }

        try
        {
            nativeWorkerThread = std::thread(&Impl::NativeWorkerMain, this);
        }
        catch (const std::system_error& exception)
        {
            (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_WindowOperationWatcherStart, std::string("创建窗口事件工作线程失败：") + exception.what());
        }
        catch (const std::bad_alloc&)
        {
            (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return MakeAllocationFailedResult(GB_WindowOperationWatcherStart);
        }

        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            startCompleted = false;
            hookStartSucceeded = false;
            startResult = GB_SystemResult::Succeeded(GB_WindowOperationWatcherStart);
            hookThreadId = 0;
        }
        try
        {
            hookThread = std::thread(&Impl::HookThreadMain, this);
        }
        catch (const std::system_error& exception)
        {
            StopNativeWorker(false);
            (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_WindowOperationWatcherStart, std::string("创建窗口 Hook 消息线程失败：") + exception.what());
        }
        catch (const std::bad_alloc&)
        {
            StopNativeWorker(false);
            (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return MakeAllocationFailedResult(GB_WindowOperationWatcherStart);
        }

        {
            std::unique_lock<std::mutex> stateLock(stateMutex);
            startCondition.wait(stateLock, [this]() { return startCompleted; });
            result = std::move(startResult);
            if (!hookStartSucceeded && result.IsSucceeded())
            {
                result = GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, GB_WindowOperationWatcherStart, "窗口 Hook 消息线程未返回明确启动结果。");
            }
        }

        if (result.IsFailed())
        {
            if (hookThread.joinable())
            {
                hookThread.join();
            }
            StopNativeWorker(false);
            (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return result;
        }

        result = SeedWindowCache();
        if (result.IsFailed())
        {
            acceptingNativeEvents.store(false, std::memory_order_release);
            DWORD localHookThreadId = 0;
            {
                std::lock_guard<std::mutex> stateLock(stateMutex);
                localHookThreadId = hookThreadId;
            }
            if (localHookThreadId != 0)
            {
                (void)::PostThreadMessageW(localHookThreadId, WM_QUIT, 0, 0);
            }
            if (hookThread.joinable())
            {
                hookThread.join();
            }
            StopNativeWorker(false);
            (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return result.WithOperationName(GB_WindowOperationWatcherStart);
        }

        {
            std::lock_guard<std::mutex> queueLock(nativeQueueMutex);
            nativeWorkerProcessingEnabled = true;
        }
        nativeQueueCondition.notify_all();
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            running = true;
        }
        return GB_SystemResult::Succeeded(GB_WindowOperationWatcherStart);
#else
        return MakeUnsupportedPlatformResult(GB_WindowOperationWatcherStart);
#endif
    }

    GB_SystemResult Stop()
    {
#if defined(_WIN32)
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (stopping)
            {
                return GB_SystemResult::Succeeded(GB_WindowOperationWatcherStop, "窗口监听器已经在停止过程中。");
            }
            stopping = true;
        }

    auto finishStop = [this](GB_SystemResult result) -> GB_SystemResult
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            stopping = false;
            return result;
        };

    std::unique_lock<std::mutex> operationLock(operationMutex);

    DWORD localHookThreadId = 0;
    bool hasWorkerThreads = false;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        localHookThreadId = hookThreadId;
        hasWorkerThreads = running || hookThread.joinable() || nativeWorkerThread.joinable();
    }
    if (!hasWorkerThreads)
    {
        (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
        (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
        return finishStop(GB_SystemResult::Succeeded(GB_WindowOperationWatcherStop));
    }

    acceptingNativeEvents.store(false, std::memory_order_release);

    if (localHookThreadId != 0 && ::PostThreadMessageW(localHookThreadId, WM_QUIT, 0, 0) == FALSE)
    {
        const DWORD postThreadMessageError = ::GetLastError();
        if (postThreadMessageError != ERROR_INVALID_THREAD_ID)
        {
            acceptingNativeEvents.store(true, std::memory_order_release);
            return finishStop(GB_SystemResult::FromWin32Error(postThreadMessageError, GB_WindowOperationWatcherStop, "向窗口 Hook 消息线程投递 WM_QUIT 失败，监听器仍保持原状态。"));
        }
    }

    if (hookThread.joinable())
    {
        if (hookThread.get_id() == std::this_thread::get_id())
        {
            hookThread.detach();
        }
        else
        {
            hookThread.join();
        }
    }

    StopNativeWorker(true);
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        running = false;
        hookThreadId = 0;
    }

    GB_SystemResult publicStopResult = publicDispatcher.Stop(GB_EventDispatcherStopMode::Drain);
    GB_SystemResult typedStopResult = typedDispatcher.Stop(GB_EventDispatcherStopMode::Drain);
    if (publicStopResult.IsFailed())
    {
        return finishStop(publicStopResult.WithOperationName(GB_WindowOperationWatcherStop));
    }
    if (typedStopResult.IsFailed())
    {
        return finishStop(typedStopResult.WithOperationName(GB_WindowOperationWatcherStop));
    }
    return finishStop(GB_SystemResult::Succeeded(GB_WindowOperationWatcherStop));
#else
        return MakeUnsupportedPlatformResult(GB_WindowOperationWatcherStop);
#endif
    }

    bool IsRunning() const
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        return running;
    }

    void SetWindowEventCallback(const GB_SystemWindowWatcher::WindowEventCallback& inputCallback)
    {
        std::lock_guard<std::mutex> callbackLock(callbackMutex);
        callback = inputCallback;
    }

    GB_EventDispatcher& GetEventDispatcher()
    {
        return publicDispatcher;
    }

    uint64_t GetDroppedNativeEventCount() const
    {
        return droppedNativeEventCount.load(std::memory_order_acquire);
    }

private:
    struct NativeWindowEvent
    {
        DWORD nativeEvent = 0;
        HWND windowHandle = nullptr;
        LONG objectId = 0;
        LONG childId = 0;
        DWORD eventThreadId = 0;
        DWORD eventTime = 0;
        uint64_t timestampMilliseconds = 0;
        GB_WindowId windowId;
    };

    GB_SystemResult ValidateOptions()
    {
        if (!GB_IsUtf8(options.filter.titleContains) || !GB_IsUtf8(options.filter.classNameContains))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_WindowOperationWatcherStart, "窗口事件过滤条件包含非法 UTF-8 字符串。");
        }
        if (HasAnyWindowIdField(options.filter.windowId) && !options.filter.windowId.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_WindowOperationWatcherStart, "窗口事件过滤器中的窗口标识无效。");
        }

        try
        {
            filterTitleContainsWide = options.filter.titleContains.empty() ? std::wstring() : GB_Utf8ToWString(options.filter.titleContains);
            filterClassNameContainsWide = options.filter.classNameContains.empty() ? std::wstring() : GB_Utf8ToWString(options.filter.classNameContains);
        }
        catch (const std::bad_alloc&)
        {
            return MakeAllocationFailedResult(GB_WindowOperationWatcherStart);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_WindowOperationWatcherStart, "转换窗口事件过滤条件时编码转换失败。");
        }

        nativeHookProcessId = options.filter.processId;
        nativeHookFilterThreadId = options.filter.threadId;
        if (options.filter.windowId.IsValid())
        {
            HWND filterWindowHandle = nullptr;
            GB_SystemResult windowResult = ValidateWindowId(options.filter.windowId, filterWindowHandle, GB_WindowOperationWatcherStart);
            if (windowResult.IsFailed())
            {
                return windowResult.WithOperationName(GB_WindowOperationWatcherStart);
            }

            DWORD processId = 0;
            const DWORD threadId = ::GetWindowThreadProcessId(filterWindowHandle, &processId);
            if (processId == 0 || threadId == 0)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_WindowOperationWatcherStart, "窗口事件过滤器中的目标窗口身份无法确认。");
            }
            if (nativeHookProcessId != 0 && nativeHookProcessId != processId)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_WindowOperationWatcherStart, "窗口事件过滤器中的 windowId 与 processId 不一致。");
            }
            if (nativeHookFilterThreadId != 0 && nativeHookFilterThreadId != threadId)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_WindowOperationWatcherStart, "窗口事件过滤器中的 windowId 与 threadId 不一致。");
            }
            nativeHookProcessId = processId;
            nativeHookFilterThreadId = threadId;
        }
        return GB_SystemResult::Succeeded(GB_WindowOperationWatcherStart);
    }

    GB_SystemResult SeedWindowCache()
    {
#if defined(_WIN32)
        try
        {
            std::vector<GB_WindowInfo> windows;
            GB_SystemResult result = GB_SystemWindow::GetTopLevelWindows(windows);
            if (result.IsFailed())
            {
                return result;
            }
            if (options.filter.includeChildWindows)
            {
                const size_t topLevelWindowCount = windows.size();
                for (size_t index = 0; index < topLevelWindowCount; index++)
                {
                    std::vector<GB_WindowInfo> childWindows;
                    result = GB_SystemWindow::GetChildWindows(windows[index].windowId, childWindows, true);
                    if (result.IsSucceeded())
                    {
                        windows.insert(windows.end(), std::make_move_iterator(childWindows.begin()), std::make_move_iterator(childWindows.end()));
                    }
                    else if (result.errorCode != GB_SystemErrorCode::NotFound)
                    {
                        return result;
                    }
                }
            }

            std::lock_guard<std::mutex> cacheLock(cacheMutex);
            windowCache.reserve(windows.size());
            for (size_t index = 0; index < windows.size(); index++)
            {
                const uint64_t cacheKey = windows[index].windowId.nativeHandle;
                windowCache.emplace(cacheKey, std::move(windows[index]));
            }
        }
        catch (const std::bad_alloc&)
        {
            return MakeAllocationFailedResult(GB_WindowOperationWatcherStart);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, GB_WindowOperationWatcherStart, "建立窗口监听器初始缓存时发生内部异常。");
        }
        return GB_SystemResult::Succeeded(GB_WindowOperationWatcherStart);
#else
        return MakeUnsupportedPlatformResult(GB_WindowOperationWatcherStart);
#endif
    }

    GB_SystemResult JoinPreviousThreadsBeforeStart()
    {
        if (hookThread.joinable())
        {
            if (hookThread.get_id() == std::this_thread::get_id())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, GB_WindowOperationWatcherStart, "不能在窗口 Hook 消息线程内部重新启动监听器。");
            }
            hookThread.join();
        }
        if (nativeWorkerThread.joinable())
        {
            if (nativeWorkerThread.get_id() == std::this_thread::get_id())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, GB_WindowOperationWatcherStart, "不能在窗口原生事件工作线程内部重新启动监听器。");
            }
            StopNativeWorker(false);
        }
        return GB_SystemResult::Succeeded(GB_WindowOperationWatcherStart);
    }

    void StopNativeWorker(const bool drain)
    {
        {
            std::lock_guard<std::mutex> queueLock(nativeQueueMutex);
            nativeWorkerStopRequested = true;
            if (!drain)
            {
                pendingNativeEvents.clear();
            }
        }
        nativeQueueCondition.notify_all();
        if (nativeWorkerThread.joinable())
        {
            if (nativeWorkerThread.get_id() == std::this_thread::get_id())
            {
                nativeWorkerThread.detach();
            }
            else
            {
                nativeWorkerThread.join();
            }
        }
    }

    void SignalStartResult(const GB_SystemResult& result) noexcept
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        hookStartSucceeded = result.IsSucceeded();
        try
        {
            startResult = result;
        }
        catch (...)
        {
            hookStartSucceeded = false;
            startResult.errorCode = GB_SystemErrorCode::ResourceAllocationFailed;
            startResult.errorSource = GB_NativeErrorSource::None;
            startResult.nativeErrorCode = 0;
            startResult.hresult = 0;
            startResult.operationName.clear();
            startResult.message.clear();
            startResult.nativeMessage.clear();
        }
        startCompleted = true;
        startCondition.notify_all();
    }

#if defined(_WIN32)
    static void CALLBACK WinEventCallback(HWINEVENTHOOK, DWORD event, HWND windowHandle, LONG objectId, LONG childId, DWORD eventThreadId, DWORD eventTime)
    {
        Impl* currentImpl = currentHookThreadImpl;
        if (currentImpl != nullptr)
        {
            currentImpl->QueueNativeEvent(event, windowHandle, objectId, childId, eventThreadId, eventTime);
        }
    }

    static bool IsSupportedNativeEvent(const DWORD event)
    {
        switch (event)
        {
        case EVENT_SYSTEM_FOREGROUND:
        case EVENT_SYSTEM_MINIMIZESTART:
        case EVENT_SYSTEM_MINIMIZEEND:
        case EVENT_OBJECT_CREATE:
        case EVENT_OBJECT_DESTROY:
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_HIDE:
        case EVENT_OBJECT_FOCUS:
        case EVENT_OBJECT_LOCATIONCHANGE:
        case EVENT_OBJECT_NAMECHANGE:
            return true;
        default:
            return false;
        }
    }

    static bool IsObjectEvent(const DWORD event)
    {
        return event >= EVENT_OBJECT_CREATE && event <= EVENT_OBJECT_NAMECHANGE;
    }

    bool ShouldDropNativeEventBeforeQueue(const DWORD event, HWND windowHandle, const LONG objectId, const LONG childId, const DWORD eventThreadId) const
    {
        if (!acceptingNativeEvents.load(std::memory_order_acquire) || windowHandle == nullptr || !IsSupportedNativeEvent(event))
        {
            return true;
        }
        if (IsObjectEvent(event) && (objectId != OBJID_WINDOW || childId != CHILDID_SELF))
        {
            return true;
        }
        if (options.filter.windowId.IsValid() && WindowHandleToValue(windowHandle) != options.filter.windowId.nativeHandle)
        {
            return true;
        }
        if (options.filter.threadId != 0 && eventThreadId != 0 && eventThreadId != options.filter.threadId)
        {
            return true;
        }
        if (!options.filter.includeChildWindows && event != EVENT_OBJECT_DESTROY && !IsTopLevelWindowHandle(windowHandle))
        {
            return true;
        }
        return false;
    }

    void QueueNativeEvent(const DWORD event, HWND windowHandle, const LONG objectId, const LONG childId, const DWORD eventThreadId, const DWORD eventTime) noexcept
    {
        if (ShouldDropNativeEventBeforeQueue(event, windowHandle, objectId, childId, eventThreadId))
        {
            return;
        }

        try
        {
            NativeWindowEvent nativeEvent;
            nativeEvent.nativeEvent = event;
            nativeEvent.windowHandle = windowHandle;
            nativeEvent.objectId = objectId;
            nativeEvent.childId = childId;
            nativeEvent.eventThreadId = eventThreadId;
            nativeEvent.eventTime = eventTime;
            nativeEvent.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
            nativeEvent.windowId = MakeWindowId(windowHandle);
            if (nativeEvent.windowId.nativeHandle == 0)
            {
                nativeEvent.windowId.nativeHandle = WindowHandleToValue(windowHandle);
            }

            {
                std::lock_guard<std::mutex> queueLock(nativeQueueMutex);
                if (options.coalesceLocationChanges && event == EVENT_OBJECT_LOCATIONCHANGE && !pendingNativeEvents.empty())
                {
                    for (std::deque<NativeWindowEvent>::reverse_iterator iter = pendingNativeEvents.rbegin(); iter != pendingNativeEvents.rend(); iter++)
                    {
                        if (iter->windowHandle != windowHandle)
                        {
                            continue;
                        }
                        if (iter->nativeEvent == EVENT_OBJECT_LOCATIONCHANGE)
                        {
                            *iter = nativeEvent;
                            return;
                        }
                        break;
                    }
                }
                if (options.maxPendingNativeEvents != 0 && pendingNativeEvents.size() >= options.maxPendingNativeEvents)
                {
                    pendingNativeEvents.pop_front();
                    droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
                }
                pendingNativeEvents.push_back(nativeEvent);
            }
            nativeQueueCondition.notify_one();
        }
        catch (...)
        {
            droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    GB_SystemResult InstallHook(const DWORD eventMinimum, const DWORD eventMaximum, const std::string& resourceName, std::vector<GB_WinHandleScope>& hooks)
    {
        ::SetLastError(ERROR_SUCCESS);
        const HWINEVENTHOOK hook = ::SetWinEventHook(eventMinimum, eventMaximum, nullptr, &Impl::WinEventCallback, nativeHookProcessId, nativeHookFilterThreadId, WINEVENT_OUTOFCONTEXT);
        if (hook == nullptr)
        {
            const DWORD lastError = ::GetLastError();
            return lastError != ERROR_SUCCESS ? GB_SystemResult::FromWin32Error(lastError, GB_WindowOperationWatcherStart, "SetWinEventHook 注册窗口事件失败。") : GB_SystemResult::Failed(GB_SystemErrorCode::NativeApiFailed, GB_WindowOperationWatcherStart, "SetWinEventHook 注册窗口事件失败，但系统未提供扩展错误码。");
        }

        try
        {
            hooks.push_back(GB_WinHandleScope::FromWinEventHook(hook, resourceName));
        }
        catch (...)
        {
            (void)::UnhookWinEvent(hook);
            return MakeAllocationFailedResult(GB_WindowOperationWatcherStart);
        }
        return GB_SystemResult::Succeeded(GB_WindowOperationWatcherStart);
    }

    void HookThreadMain()
    {
        currentHookThreadImpl = this;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            hookThreadId = ::GetCurrentThreadId();
        }

        MSG message = {};
        (void)::PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        std::vector<GB_WinHandleScope> hooks;
        try
        {
            hooks.reserve(5);
        }
        catch (...)
        {
            SignalStartResult(MakeAllocationFailedResult(GB_WindowOperationWatcherStart));
            currentHookThreadImpl = nullptr;
            return;
        }

        GB_SystemResult result = InstallHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, "SystemWindow.ForegroundHook", hooks);
        if (result.IsSucceeded())
        {
            result = InstallHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, "SystemWindow.MinimizeHook", hooks);
        }
        if (result.IsSucceeded())
        {
            result = InstallHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, "SystemWindow.ObjectLifecycleHook", hooks);
        }
        if (result.IsSucceeded())
        {
            result = InstallHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, "SystemWindow.ObjectFocusHook", hooks);
        }
        if (result.IsSucceeded())
        {
            result = InstallHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE, "SystemWindow.ObjectChangeHook", hooks);
        }
        if (result.IsFailed())
        {
            SignalStartResult(result);
            currentHookThreadImpl = nullptr;
            return;
        }

        acceptingNativeEvents.store(true, std::memory_order_release);
        SignalStartResult(GB_SystemResult::Succeeded(GB_WindowOperationWatcherStart));

        while (true)
        {
            const BOOL messageResult = ::GetMessageW(&message, nullptr, 0, 0);
            if (messageResult <= 0)
            {
                break;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        acceptingNativeEvents.store(false, std::memory_order_release);
        hooks.clear();
        currentHookThreadImpl = nullptr;
        std::lock_guard<std::mutex> stateLock(stateMutex);
        hookThreadId = 0;
        running = false;
    }
#endif

    void NativeWorkerMain()
    {
        ProcessIdentityCache processCache;
        while (true)
        {
            NativeWindowEvent nativeEvent;
            {
                std::unique_lock<std::mutex> queueLock(nativeQueueMutex);
                nativeQueueCondition.wait(queueLock, [this]() { return nativeWorkerStopRequested || (nativeWorkerProcessingEnabled && !pendingNativeEvents.empty()); });
                if (pendingNativeEvents.empty())
                {
                    if (nativeWorkerStopRequested)
                    {
                        break;
                    }
                    continue;
                }
                nativeEvent = pendingNativeEvents.front();
                pendingNativeEvents.pop_front();
            }
            ProcessNativeEvent(nativeEvent, processCache);
        }
    }

    static GB_SystemWindowEventType MapNativeEventType(const DWORD nativeEvent)
    {
#if defined(_WIN32)
        switch (nativeEvent)
        {
        case EVENT_OBJECT_CREATE:
            return GB_SystemWindowEventType::Created;
        case EVENT_OBJECT_DESTROY:
            return GB_SystemWindowEventType::Destroyed;
        case EVENT_OBJECT_SHOW:
            return GB_SystemWindowEventType::Shown;
        case EVENT_OBJECT_HIDE:
            return GB_SystemWindowEventType::Hidden;
        case EVENT_OBJECT_LOCATIONCHANGE:
            return GB_SystemWindowEventType::LocationChanged;
        case EVENT_OBJECT_NAMECHANGE:
            return GB_SystemWindowEventType::TitleChanged;
        case EVENT_SYSTEM_FOREGROUND:
            return GB_SystemWindowEventType::ForegroundChanged;
        case EVENT_OBJECT_FOCUS:
            return GB_SystemWindowEventType::Focused;
        case EVENT_SYSTEM_MINIMIZESTART:
            return GB_SystemWindowEventType::Minimized;
        case EVENT_SYSTEM_MINIMIZEEND:
            return GB_SystemWindowEventType::Restored;
        default:
            break;
        }
#else
        (void)nativeEvent;
#endif
        return GB_SystemWindowEventType::Unknown;
    }

    bool MatchesFilter(const GB_SystemWindowEvent& event) const
    {
        const GB_SystemWindowEventFilter& filter = options.filter;
        if (filter.windowId.IsValid() && event.windowId != filter.windowId)
        {
            return false;
        }
        if (filter.processId != 0 && event.windowId.processId != filter.processId)
        {
            return false;
        }
        if (filter.threadId != 0 && event.windowId.threadId != filter.threadId)
        {
            return false;
        }
        if (event.hasWindowInfo && event.windowInfo.isChildWindow && !filter.includeChildWindows)
        {
            return false;
        }
        if (!event.hasWindowInfo && !filter.includeChildWindows)
        {
#if defined(_WIN32)
            const HWND windowHandle = WindowIdToHandle(event.windowId);
            if (windowHandle != nullptr && !IsTopLevelWindowHandle(windowHandle))
            {
                return false;
            }
#endif
        }

        try
        {
            if (!filterTitleContainsWide.empty() && (!event.hasWindowInfo || !Utf8ContainsWideTarget(event.windowInfo.title, filterTitleContainsWide, filter.caseSensitive)))
            {
                return false;
            }
            if (!filterClassNameContainsWide.empty() && (!event.hasWindowInfo || !Utf8ContainsWideTarget(event.windowInfo.className, filterClassNameContainsWide, filter.caseSensitive)))
            {
                return false;
            }
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    static bool IsCachedWindowInfoCompatibleWithNativeEvent(const GB_WindowInfo& cachedWindowInfo, const NativeWindowEvent& nativeEvent)
    {
        if (cachedWindowInfo.windowId.nativeHandle == 0 || cachedWindowInfo.windowId.nativeHandle != WindowHandleToValue(nativeEvent.windowHandle))
        {
            return false;
        }
        if (nativeEvent.windowId.processId != 0 && cachedWindowInfo.windowId.processId != nativeEvent.windowId.processId)
        {
            return false;
        }
        if (nativeEvent.windowId.threadId != 0 && cachedWindowInfo.windowId.threadId != nativeEvent.windowId.threadId)
        {
            return false;
        }
        return true;
    }

    void ProcessNativeEvent(const NativeWindowEvent& nativeEvent, ProcessIdentityCache& processCache) noexcept
    {
        try
        {
            GB_SystemWindowEvent event;
            event.eventType = MapNativeEventType(nativeEvent.nativeEvent);
            if (event.eventType == GB_SystemWindowEventType::Unknown)
            {
                return;
            }
            event.eventName = "SystemWindow." + GB_SystemWindow::GetWindowEventTypeName(event.eventType);
            event.timestampMilliseconds = nativeEvent.timestampMilliseconds;
            event.nativeEvent = nativeEvent.nativeEvent;
            event.nativeObjectId = nativeEvent.objectId;
            event.nativeChildId = nativeEvent.childId;
            event.windowId = nativeEvent.windowId;

            GB_WindowInfo previousWindowInfo;
            bool hasPreviousWindowInfo = false;
            {
                std::lock_guard<std::mutex> cacheLock(cacheMutex);
                const std::unordered_map<uint64_t, GB_WindowInfo>::const_iterator existing = windowCache.find(WindowHandleToValue(nativeEvent.windowHandle));
                if (existing != windowCache.end() && IsCachedWindowInfoCompatibleWithNativeEvent(existing->second, nativeEvent))
                {
                    previousWindowInfo = existing->second;
                    hasPreviousWindowInfo = true;
                }
            }

            if (event.eventType == GB_SystemWindowEventType::Destroyed)
            {
                if (hasPreviousWindowInfo)
                {
                    event.windowInfo = previousWindowInfo;
                    event.windowId = previousWindowInfo.windowId;
                    event.hasWindowInfo = true;
                }
            }
            else
            {
                GB_SystemResult infoResult = BuildWindowInfo(nativeEvent.windowHandle, event.windowInfo, &processCache, "GB_SystemWindowWatcher::ProcessNativeEvent");
                if (infoResult.IsFailed() && (event.eventType == GB_SystemWindowEventType::Minimized || event.eventType == GB_SystemWindowEventType::Restored))
                {
                    return;
                }
                if (infoResult.IsSucceeded() && (event.eventType == GB_SystemWindowEventType::Minimized || event.eventType == GB_SystemWindowEventType::Restored))
                {
                    const bool expectedMinimized = event.eventType == GB_SystemWindowEventType::Minimized;
                    for (size_t retryIndex = 0; event.windowInfo.isMinimized != expectedMinimized && retryIndex < 5; retryIndex++)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        infoResult = BuildWindowInfo(nativeEvent.windowHandle, event.windowInfo, &processCache, "GB_SystemWindowWatcher::ProcessNativeEvent");
                        if (infoResult.IsFailed())
                        {
                            break;
                        }
                    }
                    if (infoResult.IsSucceeded() && event.windowInfo.isMinimized != expectedMinimized)
                    {
                        return;
                    }
                }
                if (infoResult.IsSucceeded())
                {
                    event.windowId = event.windowInfo.windowId;
                    event.hasWindowInfo = true;
                    if (hasPreviousWindowInfo && previousWindowInfo.windowId != event.windowId)
                    {
                        previousWindowInfo = GB_WindowInfo();
                        hasPreviousWindowInfo = false;
                    }
                }
            }

            if (!event.windowId.IsValid() && hasPreviousWindowInfo)
            {
                event.windowId = previousWindowInfo.windowId;
            }
            if (!event.windowId.IsValid())
            {
                return;
            }
            if (hasPreviousWindowInfo)
            {
                event.previousWindowInfo = previousWindowInfo;
                event.hasPreviousWindowInfo = true;
            }

            if (MatchesFilter(event))
            {
                PublishEvent(event);
                if (event.eventType == GB_SystemWindowEventType::LocationChanged && event.hasWindowInfo && event.hasPreviousWindowInfo)
                {
                    if (event.windowInfo.hasWindowRectangle && event.previousWindowInfo.hasWindowRectangle)
                    {
                        if (!RectanglesHaveSamePosition(event.windowInfo.windowRectangle, event.previousWindowInfo.windowRectangle))
                        {
                            GB_SystemWindowEvent movedEvent = event;
                            movedEvent.eventType = GB_SystemWindowEventType::Moved;
                            movedEvent.eventName = "SystemWindow.Moved";
                            movedEvent.isDerived = true;
                            PublishEvent(movedEvent);
                        }
                        if (!RectanglesHaveSameSize(event.windowInfo.windowRectangle, event.previousWindowInfo.windowRectangle))
                        {
                            GB_SystemWindowEvent resizedEvent = event;
                            resizedEvent.eventType = GB_SystemWindowEventType::Resized;
                            resizedEvent.eventName = "SystemWindow.Resized";
                            resizedEvent.isDerived = true;
                            PublishEvent(resizedEvent);
                        }
                    }
                    if (event.windowInfo.isMinimized != event.previousWindowInfo.isMinimized)
                    {
                        GB_SystemWindowEvent stateEvent = event;
                        stateEvent.eventType = event.windowInfo.isMinimized ? GB_SystemWindowEventType::Minimized : GB_SystemWindowEventType::Restored;
                        stateEvent.eventName = event.windowInfo.isMinimized ? "SystemWindow.Minimized" : "SystemWindow.Restored";
                        stateEvent.isDerived = true;
                        PublishEvent(stateEvent);
                    }
                }
            }

            {
                std::lock_guard<std::mutex> cacheLock(cacheMutex);
                const uint64_t cacheKey = WindowHandleToValue(nativeEvent.windowHandle);
                if (event.eventType == GB_SystemWindowEventType::Destroyed)
                {
                    windowCache.erase(cacheKey);
                }
                else if (event.hasWindowInfo)
                {
                    windowCache[cacheKey] = event.windowInfo;
                }
            }
        }
        catch (...)
        {
            droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    void PublishEvent(const GB_SystemWindowEvent& windowEvent) noexcept
    {
        try
        {
            GB_Event typedEvent(windowEvent.eventName, GB_Variant(windowEvent), "SetWinEventHook");
            typedEvent.timestampMilliseconds = windowEvent.timestampMilliseconds;
            (void)typedDispatcher.Post(typedEvent);

            GB_Event publicEvent(windowEvent.eventName, GB_SystemWindow::GetWindowEventTypeName(windowEvent.eventType), "SetWinEventHook");
            publicEvent.timestampMilliseconds = windowEvent.timestampMilliseconds;
            publicEvent.SetAttribute("eventType", GB_Variant(static_cast<unsigned int>(windowEvent.eventType)));
            publicEvent.SetAttribute("eventTypeName", GB_Variant(GB_SystemWindow::GetWindowEventTypeName(windowEvent.eventType)));
            publicEvent.SetAttribute("nativeHandle", GB_Variant(static_cast<unsigned long long>(windowEvent.windowId.nativeHandle)));
            publicEvent.SetAttribute("processId", GB_Variant(static_cast<unsigned int>(windowEvent.windowId.processId)));
            publicEvent.SetAttribute("threadId", GB_Variant(static_cast<unsigned int>(windowEvent.windowId.threadId)));
            publicEvent.SetAttribute("nativeEvent", GB_Variant(static_cast<unsigned int>(windowEvent.nativeEvent)));
            publicEvent.SetAttribute("isDerived", GB_Variant(windowEvent.isDerived));
            (void)publicDispatcher.Post(publicEvent);
        }
        catch (...)
        {
        }
    }

    void DispatchTypedCallback(const GB_Event& event)
    {
        const GB_SystemWindowEvent* windowEvent = event.payload.AnyCast<GB_SystemWindowEvent>();
        if (windowEvent == nullptr)
        {
            return;
        }

        GB_SystemWindowWatcher::WindowEventCallback callbackCopy;
        {
            std::lock_guard<std::mutex> callbackLock(callbackMutex);
            callbackCopy = callback;
        }
        if (callbackCopy)
        {
            callbackCopy(*windowEvent);
        }
    }

private:
    GB_SystemWindowWatcherOptions options;
    mutable std::mutex operationMutex;
    mutable std::mutex stateMutex;
    mutable std::mutex callbackMutex;
    mutable std::mutex nativeQueueMutex;
    mutable std::mutex cacheMutex;
    std::condition_variable startCondition;
    std::condition_variable nativeQueueCondition;
    std::thread hookThread;
    std::thread nativeWorkerThread;
    std::deque<NativeWindowEvent> pendingNativeEvents;
    std::unordered_map<uint64_t, GB_WindowInfo> windowCache;
    std::atomic<bool> acceptingNativeEvents{ false };
    std::atomic<uint64_t> droppedNativeEventCount{ 0 };
    bool nativeWorkerStopRequested = false;
    bool nativeWorkerProcessingEnabled = false;
    bool running = false;
    bool stopping = false;
    bool startCompleted = false;
    bool hookStartSucceeded = false;
    GB_SystemResult startResult;
    DWORD hookThreadId = 0;
    DWORD nativeHookProcessId = 0;
    DWORD nativeHookFilterThreadId = 0;
    std::wstring filterTitleContainsWide;
    std::wstring filterClassNameContainsWide;
    GB_SystemWindowWatcher::WindowEventCallback callback;
    GB_EventDispatcher typedDispatcher;
    GB_EventDispatcher publicDispatcher;
    GB_EventSubscriptionToken typedSubscriptionToken;
    GB_SystemResult callbackSetupResult;

#if defined(_WIN32)
    static thread_local Impl* currentHookThreadImpl;
#endif
};

#if defined(_WIN32)
thread_local GB_SystemWindowWatcher::Impl* GB_SystemWindowWatcher::Impl::currentHookThreadImpl = nullptr;
#endif

#else

class GB_SystemWindowWatcher::Impl final
{
public:
    explicit Impl(const GB_SystemWindowWatcherOptions& inputOptions)
        : options(inputOptions),
        publicDispatcher(GB_EventDispatcher::MakeDirectOptions("GB_SystemWindowWatcher.Public"))
    {
    }

    ~Impl() noexcept = default;

    GB_SystemResult Start()
    {
        return MakeUnsupportedPlatformResult(GB_WindowOperationWatcherStart);
    }

    GB_SystemResult Stop()
    {
        return GB_SystemResult::Succeeded(GB_WindowOperationWatcherStop);
    }

    bool IsRunning() const
    {
        return false;
    }

    void SetWindowEventCallback(const GB_SystemWindowWatcher::WindowEventCallback& inputCallback)
    {
        callback = inputCallback;
    }

    GB_EventDispatcher& GetEventDispatcher()
    {
        return publicDispatcher;
    }

    uint64_t GetDroppedNativeEventCount() const
    {
        return 0;
    }

private:
    GB_SystemWindowWatcherOptions options;
    GB_SystemWindowWatcher::WindowEventCallback callback;
    GB_EventDispatcher publicDispatcher;
};

#endif

GB_SystemWindowWatcher::GB_SystemWindowWatcher()
    : impl(new Impl(GB_SystemWindowWatcherOptions()))
{
}

GB_SystemWindowWatcher::GB_SystemWindowWatcher(const GB_SystemWindowWatcherOptions& options)
    : impl(new Impl(options))
{
}

GB_SystemWindowWatcher::~GB_SystemWindowWatcher() noexcept = default;

GB_SystemResult GB_SystemWindowWatcher::Start()
{
    return impl->Start();
}

GB_SystemResult GB_SystemWindowWatcher::Stop()
{
    return impl->Stop();
}

bool GB_SystemWindowWatcher::IsRunning() const
{
    return impl->IsRunning();
}

void GB_SystemWindowWatcher::SetWindowEventCallback(const WindowEventCallback& callback)
{
    impl->SetWindowEventCallback(callback);
}

GB_EventDispatcher& GB_SystemWindowWatcher::GetEventDispatcher()
{
    return impl->GetEventDispatcher();
}

uint64_t GB_SystemWindowWatcher::GetDroppedNativeEventCount() const
{
    return impl->GetDroppedNativeEventCount();
}
