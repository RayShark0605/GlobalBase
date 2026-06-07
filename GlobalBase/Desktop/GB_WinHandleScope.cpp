#include "GB_WinHandleScope.h"

#include <limits>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#ifdef _MSC_VER
#  pragma comment(lib, "Advapi32.lib")
#  pragma comment(lib, "Gdi32.lib")
#  pragma comment(lib, "User32.lib")
#  pragma comment(lib, "Ole32.lib")
#endif
#endif

namespace
{
    static uintptr_t MakeInvalidHandleValueBits()
    {
        return std::numeric_limits<uintptr_t>::max();
    }

    static GB_WinHandleCloseMethod NormalizeCloseMethod(const GB_WinHandleCloseMethod closeMethod)
    {
        const uint64_t closeMethodValue = static_cast<uint64_t>(closeMethod);
        if (!GB_WinHandleScope::IsValidCloseMethodValue(closeMethodValue))
        {
            return GB_WinHandleCloseMethod::None;
        }

        return closeMethod;
    }

    static std::string BuildCloseOperationName(const GB_WinHandleCloseMethod closeMethod)
    {
        std::string operationName = u8"GB_WinHandleScope::Close";
        const std::string closeMethodName = GB_WinHandleScope::GetCloseMethodName(closeMethod);
        if (!closeMethodName.empty())
        {
            operationName += ".";
            operationName += closeMethodName;
        }

        return operationName;
    }

    static std::string BuildCloseDetailMessage(const GB_WinHandleCloseMethod closeMethod, const std::string& resourceName, const std::string& detail)
    {
        std::string message = detail.empty() ? std::string(u8"关闭 Windows 原生资源失败。") : detail;
        message += u8" closeMethod=";
        message += GB_WinHandleScope::GetCloseMethodName(closeMethod);

        if (!resourceName.empty())
        {
            message += u8", resourceName=";
            message += resourceName;
        }

        return message;
    }

#if defined(_WIN32)
    static GB_SystemResult MakeCloseSucceededResult(const GB_WinHandleCloseMethod closeMethod, const std::string&, const std::string& message)
    {
        return GB_SystemResult::Succeeded(BuildCloseOperationName(closeMethod), message);
    }

    static GB_SystemResult MakeCloseFailedResultWithoutNativeCode(const GB_WinHandleCloseMethod closeMethod, const std::string& resourceName, const std::string& detail)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NativeApiFailed, BuildCloseOperationName(closeMethod), BuildCloseDetailMessage(closeMethod, resourceName, detail));
    }

#endif

#if defined(_WIN32)
    static GB_SystemResult MakeCloseFailedResultFromLastError(const GB_WinHandleCloseMethod closeMethod, const std::string& resourceName, const std::string& detail)
    {
        const DWORD lastError = ::GetLastError();
        if (lastError != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(static_cast<uint32_t>(lastError), BuildCloseOperationName(closeMethod), BuildCloseDetailMessage(closeMethod, resourceName, detail));
        }

        return MakeCloseFailedResultWithoutNativeCode(closeMethod, resourceName, detail);
    }

    class Win32LastErrorValueScope
    {
    public:
        Win32LastErrorValueScope() noexcept
            : lastErrorCode(::GetLastError())
        {
        }

        ~Win32LastErrorValueScope() noexcept
        {
            ::SetLastError(lastErrorCode);
        }

        Win32LastErrorValueScope(const Win32LastErrorValueScope&) = delete;
        Win32LastErrorValueScope& operator=(const Win32LastErrorValueScope&) = delete;

    private:
        DWORD lastErrorCode = ERROR_SUCCESS;
    };

    static bool IsPredefinedRegistryKey(const void* handle)
    {
        const HKEY registryKey = reinterpret_cast<HKEY>(const_cast<void*>(handle));

#if defined(HKEY_CLASSES_ROOT)
        if (registryKey == HKEY_CLASSES_ROOT)
        {
            return true;
        }
#endif
#if defined(HKEY_CURRENT_USER)
        if (registryKey == HKEY_CURRENT_USER)
        {
            return true;
        }
#endif
#if defined(HKEY_LOCAL_MACHINE)
        if (registryKey == HKEY_LOCAL_MACHINE)
        {
            return true;
        }
#endif
#if defined(HKEY_USERS)
        if (registryKey == HKEY_USERS)
        {
            return true;
        }
#endif
#if defined(HKEY_PERFORMANCE_DATA)
        if (registryKey == HKEY_PERFORMANCE_DATA)
        {
            return true;
        }
#endif
#if defined(HKEY_PERFORMANCE_TEXT)
        if (registryKey == HKEY_PERFORMANCE_TEXT)
        {
            return true;
        }
#endif
#if defined(HKEY_PERFORMANCE_NLSTEXT)
        if (registryKey == HKEY_PERFORMANCE_NLSTEXT)
        {
            return true;
        }
#endif
#if defined(HKEY_CURRENT_CONFIG)
        if (registryKey == HKEY_CURRENT_CONFIG)
        {
            return true;
        }
#endif
#if defined(HKEY_DYN_DATA)
        if (registryKey == HKEY_DYN_DATA)
        {
            return true;
        }
#endif
#if defined(HKEY_CURRENT_USER_LOCAL_SETTINGS)
        if (registryKey == HKEY_CURRENT_USER_LOCAL_SETTINGS)
        {
            return true;
        }
#endif

        return false;
    }

    static bool IsPseudoCloseHandleValue(const void* handle)
    {
        const HANDLE nativeHandle = static_cast<HANDLE>(const_cast<void*>(handle));

        if (nativeHandle == ::GetCurrentProcess())
        {
            return true;
        }

        if (nativeHandle == ::GetCurrentThread())
        {
            return true;
        }

        const intptr_t handleValue = reinterpret_cast<intptr_t>(handle);
        if (handleValue == static_cast<intptr_t>(-4) || handleValue == static_cast<intptr_t>(-5) || handleValue == static_cast<intptr_t>(-6))
        {
            return true;
        }

        return false;
    }

    static bool TryGetCloseHandleFlags(const void* handle, DWORD& flags)
    {
        const Win32LastErrorValueScope lastErrorValueScope;
        flags = 0;
        return ::GetHandleInformation(static_cast<HANDLE>(const_cast<void*>(handle)), &flags) != FALSE;
    }

    static bool IsValidCloseHandleValue(const void* handle)
    {
        if (IsPseudoCloseHandleValue(handle))
        {
            return false;
        }

        DWORD flags = 0;
        return TryGetCloseHandleFlags(handle, flags);
    }

    static bool IsProtectedCloseHandleValue(const void* handle)
    {
        DWORD flags = 0;
        if (!TryGetCloseHandleFlags(handle, flags))
        {
            return false;
        }

        return (flags & HANDLE_FLAG_PROTECT_FROM_CLOSE) != 0;
    }

    static GB_SystemResult CloseRawHandle(void* handle, void* contextHandle, const GB_WinHandleCloseMethod closeMethod, const std::string& resourceName)
    {
        switch (closeMethod)
        {
        case GB_WinHandleCloseMethod::CloseHandle:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::CloseHandle(static_cast<HANDLE>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"CloseHandle 调用失败。");
        }

        case GB_WinHandleCloseMethod::FindClose:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::FindClose(static_cast<HANDLE>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"FindClose 调用失败。");
        }

        case GB_WinHandleCloseMethod::FindCloseChangeNotification:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::FindCloseChangeNotification(static_cast<HANDLE>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"FindCloseChangeNotification 调用失败。");
        }

        case GB_WinHandleCloseMethod::CloseServiceHandle:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::CloseServiceHandle(static_cast<SC_HANDLE>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"CloseServiceHandle 调用失败。");
        }

        case GB_WinHandleCloseMethod::RegCloseKey:
        {
            const LSTATUS closeResult = ::RegCloseKey(reinterpret_cast<HKEY>(handle));
            if (closeResult == ERROR_SUCCESS)
            {
                return MakeCloseSucceededResult(closeMethod, resourceName, std::string());
            }

            return GB_SystemResult::FromWin32Error(static_cast<uint32_t>(closeResult), BuildCloseOperationName(closeMethod), BuildCloseDetailMessage(closeMethod, resourceName, u8"RegCloseKey 调用失败。"));
        }

        case GB_WinHandleCloseMethod::FreeLibrary:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::FreeLibrary(static_cast<HMODULE>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"FreeLibrary 调用失败。");
        }

        case GB_WinHandleCloseMethod::LocalFree:
        {
            ::SetLastError(ERROR_SUCCESS);
            const HLOCAL freeResult = ::LocalFree(static_cast<HLOCAL>(handle));
            return freeResult == nullptr ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"LocalFree 调用失败。");
        }

        case GB_WinHandleCloseMethod::GlobalFree:
        {
            ::SetLastError(ERROR_SUCCESS);
            const HGLOBAL freeResult = ::GlobalFree(static_cast<HGLOBAL>(handle));
            return freeResult == nullptr ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"GlobalFree 调用失败。");
        }

        case GB_WinHandleCloseMethod::DeleteObject:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::DeleteObject(static_cast<HGDIOBJ>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"DeleteObject 调用失败。请确认 GDI 对象未被选入任何 DC。");
        }

        case GB_WinHandleCloseMethod::DeleteDC:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::DeleteDC(static_cast<HDC>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"DeleteDC 调用失败。");
        }

        case GB_WinHandleCloseMethod::ReleaseDC:
        {
            ::SetLastError(ERROR_SUCCESS);
            const int closeResult = ::ReleaseDC(static_cast<HWND>(contextHandle), static_cast<HDC>(handle));
            return closeResult != 0 ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"ReleaseDC 调用失败。请确认该 HDC 来自 GetDC 或 GetWindowDC，并且由同一线程释放。");
        }

        case GB_WinHandleCloseMethod::DestroyWindow:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::DestroyWindow(static_cast<HWND>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"DestroyWindow 调用失败。");
        }

        case GB_WinHandleCloseMethod::DestroyMenu:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::DestroyMenu(static_cast<HMENU>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"DestroyMenu 调用失败。");
        }

        case GB_WinHandleCloseMethod::DestroyIcon:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::DestroyIcon(static_cast<HICON>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"DestroyIcon 调用失败。");
        }

        case GB_WinHandleCloseMethod::DestroyCursor:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::DestroyCursor(static_cast<HCURSOR>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"DestroyCursor 调用失败。");
        }

        case GB_WinHandleCloseMethod::UnhookWindowsHookEx:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::UnhookWindowsHookEx(static_cast<HHOOK>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"UnhookWindowsHookEx 调用失败。");
        }

        case GB_WinHandleCloseMethod::UnhookWinEvent:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::UnhookWinEvent(static_cast<HWINEVENTHOOK>(handle));
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"UnhookWinEvent 调用失败。");
        }

        case GB_WinHandleCloseMethod::UnmapViewOfFile:
        {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL closeResult = ::UnmapViewOfFile(handle);
            return closeResult != FALSE ? MakeCloseSucceededResult(closeMethod, resourceName, std::string()) : MakeCloseFailedResultFromLastError(closeMethod, resourceName, u8"UnmapViewOfFile 调用失败。");
        }

        case GB_WinHandleCloseMethod::CoTaskMemFree:
        {
            ::CoTaskMemFree(handle);
            return MakeCloseSucceededResult(closeMethod, resourceName, std::string());
        }

        case GB_WinHandleCloseMethod::None:
            return MakeCloseSucceededResult(closeMethod, resourceName, std::string());

        default:
            break;
        }

        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, BuildCloseOperationName(closeMethod), u8"未知的 Windows 句柄关闭方式。");
    }

    static void CloseRawHandleSilently(void* handle, void* contextHandle, const GB_WinHandleCloseMethod closeMethod) noexcept
    {
        const Win32LastErrorValueScope lastErrorValueScope;

        switch (closeMethod)
        {
        case GB_WinHandleCloseMethod::CloseHandle:
            (void)::CloseHandle(static_cast<HANDLE>(handle));
            return;

        case GB_WinHandleCloseMethod::FindClose:
            (void)::FindClose(static_cast<HANDLE>(handle));
            return;

        case GB_WinHandleCloseMethod::FindCloseChangeNotification:
            (void)::FindCloseChangeNotification(static_cast<HANDLE>(handle));
            return;

        case GB_WinHandleCloseMethod::CloseServiceHandle:
            (void)::CloseServiceHandle(static_cast<SC_HANDLE>(handle));
            return;

        case GB_WinHandleCloseMethod::RegCloseKey:
            (void)::RegCloseKey(reinterpret_cast<HKEY>(handle));
            return;

        case GB_WinHandleCloseMethod::FreeLibrary:
            (void)::FreeLibrary(static_cast<HMODULE>(handle));
            return;

        case GB_WinHandleCloseMethod::LocalFree:
            (void)::LocalFree(static_cast<HLOCAL>(handle));
            return;

        case GB_WinHandleCloseMethod::GlobalFree:
            (void)::GlobalFree(static_cast<HGLOBAL>(handle));
            return;

        case GB_WinHandleCloseMethod::DeleteObject:
            (void)::DeleteObject(static_cast<HGDIOBJ>(handle));
            return;

        case GB_WinHandleCloseMethod::DeleteDC:
            (void)::DeleteDC(static_cast<HDC>(handle));
            return;

        case GB_WinHandleCloseMethod::ReleaseDC:
            (void)::ReleaseDC(static_cast<HWND>(contextHandle), static_cast<HDC>(handle));
            return;

        case GB_WinHandleCloseMethod::DestroyWindow:
            (void)::DestroyWindow(static_cast<HWND>(handle));
            return;

        case GB_WinHandleCloseMethod::DestroyMenu:
            (void)::DestroyMenu(static_cast<HMENU>(handle));
            return;

        case GB_WinHandleCloseMethod::DestroyIcon:
            (void)::DestroyIcon(static_cast<HICON>(handle));
            return;

        case GB_WinHandleCloseMethod::DestroyCursor:
            (void)::DestroyCursor(static_cast<HCURSOR>(handle));
            return;

        case GB_WinHandleCloseMethod::UnhookWindowsHookEx:
            (void)::UnhookWindowsHookEx(static_cast<HHOOK>(handle));
            return;

        case GB_WinHandleCloseMethod::UnhookWinEvent:
            (void)::UnhookWinEvent(static_cast<HWINEVENTHOOK>(handle));
            return;

        case GB_WinHandleCloseMethod::UnmapViewOfFile:
            (void)::UnmapViewOfFile(handle);
            return;

        case GB_WinHandleCloseMethod::CoTaskMemFree:
            ::CoTaskMemFree(handle);
            return;

        case GB_WinHandleCloseMethod::None:
        default:
            return;
        }
    }

#else
    static bool IsPredefinedRegistryKey(const void*)
    {
        return false;
    }

    static bool IsPseudoCloseHandleValue(const void*)
    {
        return false;
    }

    static GB_SystemResult CloseRawHandle(void*, void*, const GB_WinHandleCloseMethod closeMethod, const std::string& resourceName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, BuildCloseOperationName(closeMethod), BuildCloseDetailMessage(closeMethod, resourceName, u8"当前平台不支持 Windows 原生句柄释放。"));
    }

    static void CloseRawHandleSilently(void*, void*, const GB_WinHandleCloseMethod) noexcept
    {
    }
#endif
}

GB_WinHandleScope::GB_WinHandleScope()
    : lastCloseResult(GB_SystemResult::Succeeded(u8"GB_WinHandleScope"))
{
}

GB_WinHandleScope::GB_WinHandleScope(NativeHandle handle, const GB_WinHandleCloseMethod closeMethod, NativeHandle contextHandle, const std::string& resourceName)
    : handle(handle)
    , contextHandle(contextHandle)
    , closeMethod(closeMethod)
    , resourceName(resourceName)
    , lastCloseResult(GB_SystemResult::Succeeded(u8"GB_WinHandleScope"))
{
}

GB_WinHandleScope::~GB_WinHandleScope() noexcept
{
    CloseSilently();
}

GB_WinHandleScope::GB_WinHandleScope(GB_WinHandleScope&& other)
    : lastCloseResult(GB_SystemResult::Succeeded(u8"GB_WinHandleScope"))
{
    MoveFrom(other);
}

GB_WinHandleScope& GB_WinHandleScope::operator=(GB_WinHandleScope&& other)
{
    if (this == &other)
    {
        return *this;
    }

    CloseSilently();
    MoveFrom(other);
    return *this;
}

GB_WinHandleScope GB_WinHandleScope::FromBorrowedHandle(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::None, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromKernelHandle(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::CloseHandle, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromFindHandle(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::FindClose, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromChangeNotificationHandle(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::FindCloseChangeNotification, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromServiceHandle(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::CloseServiceHandle, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromRegistryKey(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::RegCloseKey, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromLibraryModule(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::FreeLibrary, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromLocalMemory(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::LocalFree, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromGlobalMemory(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::GlobalFree, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromGdiObject(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::DeleteObject, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromCreatedDc(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::DeleteDC, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromWindowDc(NativeHandle windowHandle, NativeHandle deviceContextHandle, const std::string& resourceName)
{
    return GB_WinHandleScope(deviceContextHandle, GB_WinHandleCloseMethod::ReleaseDC, windowHandle, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromWindow(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::DestroyWindow, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromMenu(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::DestroyMenu, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromIcon(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::DestroyIcon, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromCursor(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::DestroyCursor, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromHook(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::UnhookWindowsHookEx, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromWinEventHook(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::UnhookWinEvent, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromMappedView(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::UnmapViewOfFile, nullptr, resourceName);
}

GB_WinHandleScope GB_WinHandleScope::FromComTaskMemory(NativeHandle handle, const std::string& resourceName)
{
    return GB_WinHandleScope(handle, GB_WinHandleCloseMethod::CoTaskMemFree, nullptr, resourceName);
}

bool GB_WinHandleScope::IsEmpty() const
{
    return IsNullHandle(handle);
}

bool GB_WinHandleScope::IsValid() const
{
    return IsValidHandleForCloseMethod(handle, closeMethod);
}

bool GB_WinHandleScope::HasOwnership() const
{
    return IsClosableHandleForCloseMethod(handle, closeMethod);
}

GB_WinHandleScope::operator bool() const
{
    return IsValid();
}

GB_WinHandleScope::NativeHandle GB_WinHandleScope::GetHandle() const
{
    return handle;
}

GB_WinHandleScope::NativeHandle GB_WinHandleScope::GetContextHandle() const
{
    return contextHandle;
}

GB_WinHandleCloseMethod GB_WinHandleScope::GetCloseMethod() const
{
    return closeMethod;
}

std::string GB_WinHandleScope::GetResourceName() const
{
    return resourceName;
}

GB_SystemResult GB_WinHandleScope::GetLastCloseResult() const
{
    return lastCloseResult;
}

GB_SystemResult GB_WinHandleScope::Close()
{
#if defined(_WIN32)
    const Win32LastErrorValueScope lastErrorValueScope;
#endif

    const GB_WinHandleCloseMethod currentCloseMethod = closeMethod;
    if (!IsValidCloseMethodValue(static_cast<uint64_t>(currentCloseMethod)))
    {
        lastCloseResult = GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_WinHandleScope::Close", u8"未知的 Windows 句柄关闭方式，未执行关闭；当前对象继续保留原句柄，避免错误清空导致资源泄漏。请调用 Detach() 后由外部接管。");
        return lastCloseResult;
    }

    if (IsNullHandle(handle))
    {
        ClearHandleState();
        lastCloseResult = GB_SystemResult::Succeeded(BuildCloseOperationName(currentCloseMethod));
        return lastCloseResult;
    }

    if (!IsValidHandleForCloseMethod(handle, closeMethod))
    {
        ClearHandleState();
        lastCloseResult = GB_SystemResult::Succeeded(BuildCloseOperationName(currentCloseMethod), u8"句柄值无效，未执行关闭。此情况通常来自返回 INVALID_HANDLE_VALUE 的 Windows API 失败分支。");
        return lastCloseResult;
    }

    if (!IsClosableHandleForCloseMethod(handle, closeMethod))
    {
        ClearHandleState();
        lastCloseResult = GB_SystemResult::Succeeded(BuildCloseOperationName(currentCloseMethod), u8"当前对象不拥有该句柄，未执行关闭。");
        return lastCloseResult;
    }

    const GB_SystemResult closeResult = CloseRawHandle(handle, contextHandle, closeMethod, resourceName);
    lastCloseResult = closeResult;
    if (closeResult.IsSucceeded())
    {
        ClearHandleState();
    }

    return lastCloseResult;
}

GB_WinHandleScope::NativeHandle GB_WinHandleScope::Detach()
{
    const NativeHandle detachedHandle = handle;
    ClearHandleState();
    lastCloseResult = GB_SystemResult::Succeeded(u8"GB_WinHandleScope::Detach");
    return detachedHandle;
}

GB_SystemResult GB_WinHandleScope::Reset(NativeHandle newHandle, const GB_WinHandleCloseMethod newCloseMethod, NativeHandle newContextHandle, const std::string& newResourceName)
{
    const GB_SystemResult closeResult = Close();
    if (closeResult.IsFailed())
    {
        return closeResult;
    }

    handle = newHandle;
    contextHandle = newContextHandle;
    closeMethod = newCloseMethod;
    resourceName = newResourceName;
    lastCloseResult = closeResult;
    return closeResult;
}

void GB_WinHandleScope::Swap(GB_WinHandleScope& other)
{
    using std::swap;
    swap(handle, other.handle);
    swap(contextHandle, other.contextHandle);
    swap(closeMethod, other.closeMethod);
    swap(resourceName, other.resourceName);
    swap(lastCloseResult, other.lastCloseResult);
}

GB_WinHandleScope::NativeHandle GB_WinHandleScope::GetInvalidHandleValue()
{
#if defined(_WIN32)
    return INVALID_HANDLE_VALUE;
#else
    return reinterpret_cast<NativeHandle>(MakeInvalidHandleValueBits());
#endif
}

bool GB_WinHandleScope::IsNullHandle(NativeHandle handle)
{
    return handle == nullptr;
}

bool GB_WinHandleScope::IsInvalidHandleValue(NativeHandle handle)
{
    return reinterpret_cast<uintptr_t>(handle) == MakeInvalidHandleValueBits();
}

bool GB_WinHandleScope::IsValidCloseMethodValue(const uint64_t closeMethodValue)
{
    switch (closeMethodValue)
    {
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::None):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::CloseHandle):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::FindClose):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::FindCloseChangeNotification):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::CloseServiceHandle):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::RegCloseKey):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::FreeLibrary):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::LocalFree):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::GlobalFree):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::DeleteObject):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::DeleteDC):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::ReleaseDC):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::DestroyWindow):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::DestroyMenu):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::DestroyIcon):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::DestroyCursor):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::UnhookWindowsHookEx):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::UnhookWinEvent):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::UnmapViewOfFile):
    case static_cast<uint64_t>(GB_WinHandleCloseMethod::CoTaskMemFree):
        return true;

    default:
        break;
    }

    return false;
}

bool GB_WinHandleScope::IsValidHandleForCloseMethod(NativeHandle handle, const GB_WinHandleCloseMethod closeMethod)
{
    if (IsNullHandle(handle) || IsInvalidHandleValue(handle))
    {
        return false;
    }

    const uint64_t closeMethodValue = static_cast<uint64_t>(closeMethod);
    if (!IsValidCloseMethodValue(closeMethodValue))
    {
        return false;
    }

    const GB_WinHandleCloseMethod normalizedCloseMethod = closeMethod;
    if (normalizedCloseMethod == GB_WinHandleCloseMethod::None)
    {
        return true;
    }

#if defined(_WIN32)
    if (normalizedCloseMethod == GB_WinHandleCloseMethod::CloseHandle && !IsValidCloseHandleValue(handle))
    {
        return false;
    }
#endif

    return true;
}

bool GB_WinHandleScope::IsClosableHandleForCloseMethod(NativeHandle handle, const GB_WinHandleCloseMethod closeMethod)
{
    if (!IsValidHandleForCloseMethod(handle, closeMethod))
    {
        return false;
    }

    const GB_WinHandleCloseMethod normalizedCloseMethod = NormalizeCloseMethod(closeMethod);
    if (normalizedCloseMethod == GB_WinHandleCloseMethod::None)
    {
        return false;
    }

    if (normalizedCloseMethod == GB_WinHandleCloseMethod::CloseHandle)
    {
#if defined(_WIN32)
        if (IsPseudoCloseHandleValue(handle) || IsProtectedCloseHandleValue(handle))
        {
            return false;
        }
#else
        if (IsPseudoCloseHandleValue(handle))
        {
            return false;
        }
#endif
    }

    if (normalizedCloseMethod == GB_WinHandleCloseMethod::RegCloseKey && IsPredefinedRegistryKey(handle))
    {
        return false;
    }

    return true;
}

std::string GB_WinHandleScope::GetCloseMethodName(const GB_WinHandleCloseMethod closeMethod)
{
    if (!IsValidCloseMethodValue(static_cast<uint64_t>(closeMethod)))
    {
        return u8"Unknown";
    }

    switch (closeMethod)
    {
    case GB_WinHandleCloseMethod::None:
        return u8"None";

    case GB_WinHandleCloseMethod::CloseHandle:
        return u8"CloseHandle";

    case GB_WinHandleCloseMethod::FindClose:
        return u8"FindClose";

    case GB_WinHandleCloseMethod::FindCloseChangeNotification:
        return u8"FindCloseChangeNotification";

    case GB_WinHandleCloseMethod::CloseServiceHandle:
        return u8"CloseServiceHandle";

    case GB_WinHandleCloseMethod::RegCloseKey:
        return u8"RegCloseKey";

    case GB_WinHandleCloseMethod::FreeLibrary:
        return u8"FreeLibrary";

    case GB_WinHandleCloseMethod::LocalFree:
        return u8"LocalFree";

    case GB_WinHandleCloseMethod::GlobalFree:
        return u8"GlobalFree";

    case GB_WinHandleCloseMethod::DeleteObject:
        return u8"DeleteObject";

    case GB_WinHandleCloseMethod::DeleteDC:
        return u8"DeleteDC";

    case GB_WinHandleCloseMethod::ReleaseDC:
        return u8"ReleaseDC";

    case GB_WinHandleCloseMethod::DestroyWindow:
        return u8"DestroyWindow";

    case GB_WinHandleCloseMethod::DestroyMenu:
        return u8"DestroyMenu";

    case GB_WinHandleCloseMethod::DestroyIcon:
        return u8"DestroyIcon";

    case GB_WinHandleCloseMethod::DestroyCursor:
        return u8"DestroyCursor";

    case GB_WinHandleCloseMethod::UnhookWindowsHookEx:
        return u8"UnhookWindowsHookEx";

    case GB_WinHandleCloseMethod::UnhookWinEvent:
        return u8"UnhookWinEvent";

    case GB_WinHandleCloseMethod::UnmapViewOfFile:
        return u8"UnmapViewOfFile";

    case GB_WinHandleCloseMethod::CoTaskMemFree:
        return u8"CoTaskMemFree";

    default:
        break;
    }

    return u8"None";
}

std::string GB_WinHandleScope::GetCloseMethodDescription(const GB_WinHandleCloseMethod closeMethod)
{
    if (!IsValidCloseMethodValue(static_cast<uint64_t>(closeMethod)))
    {
        return u8"未知的 Windows 句柄关闭方式。";
    }

    switch (closeMethod)
    {
    case GB_WinHandleCloseMethod::None:
        return u8"不拥有资源，不自动关闭。";

    case GB_WinHandleCloseMethod::CloseHandle:
        return u8"使用 CloseHandle 关闭内核对象句柄。";

    case GB_WinHandleCloseMethod::FindClose:
        return u8"使用 FindClose 关闭文件搜索句柄。";

    case GB_WinHandleCloseMethod::FindCloseChangeNotification:
        return u8"使用 FindCloseChangeNotification 关闭文件系统变更通知句柄。";

    case GB_WinHandleCloseMethod::CloseServiceHandle:
        return u8"使用 CloseServiceHandle 关闭服务管理句柄。";

    case GB_WinHandleCloseMethod::RegCloseKey:
        return u8"使用 RegCloseKey 关闭注册表键句柄。";

    case GB_WinHandleCloseMethod::FreeLibrary:
        return u8"使用 FreeLibrary 释放动态库模块句柄。";

    case GB_WinHandleCloseMethod::LocalFree:
        return u8"使用 LocalFree 释放本地内存句柄。";

    case GB_WinHandleCloseMethod::GlobalFree:
        return u8"使用 GlobalFree 释放全局内存句柄。";

    case GB_WinHandleCloseMethod::DeleteObject:
        return u8"使用 DeleteObject 删除 GDI 对象。";

    case GB_WinHandleCloseMethod::DeleteDC:
        return u8"使用 DeleteDC 删除自建设备上下文。";

    case GB_WinHandleCloseMethod::ReleaseDC:
        return u8"使用 ReleaseDC 释放 GetDC 或 GetWindowDC 获取的窗口设备上下文。";

    case GB_WinHandleCloseMethod::DestroyWindow:
        return u8"使用 DestroyWindow 销毁窗口。";

    case GB_WinHandleCloseMethod::DestroyMenu:
        return u8"使用 DestroyMenu 销毁菜单。";

    case GB_WinHandleCloseMethod::DestroyIcon:
        return u8"使用 DestroyIcon 销毁图标。";

    case GB_WinHandleCloseMethod::DestroyCursor:
        return u8"使用 DestroyCursor 销毁光标。";

    case GB_WinHandleCloseMethod::UnhookWindowsHookEx:
        return u8"使用 UnhookWindowsHookEx 卸载 Windows Hook。";

    case GB_WinHandleCloseMethod::UnhookWinEvent:
        return u8"使用 UnhookWinEvent 卸载辅助功能 WinEvent Hook。";

    case GB_WinHandleCloseMethod::UnmapViewOfFile:
        return u8"使用 UnmapViewOfFile 解除文件映射视图。";

    case GB_WinHandleCloseMethod::CoTaskMemFree:
        return u8"使用 CoTaskMemFree 释放 COM 任务内存。";

    default:
        break;
    }

    return u8"未知的 Windows 句柄关闭方式。";
}

void GB_WinHandleScope::CloseSilently() noexcept
{
    if (IsClosableHandleForCloseMethod(handle, closeMethod))
    {
        CloseRawHandleSilently(handle, contextHandle, closeMethod);
    }

    ClearHandleState();
}

void GB_WinHandleScope::ClearHandleState() noexcept
{
    handle = nullptr;
    contextHandle = nullptr;
    closeMethod = GB_WinHandleCloseMethod::None;
    resourceName.clear();
}

void GB_WinHandleScope::MoveFrom(GB_WinHandleScope& other)
{
    handle = other.handle;
    contextHandle = other.contextHandle;
    closeMethod = other.closeMethod;
    resourceName = std::move(other.resourceName);
    lastCloseResult = other.lastCloseResult;
    other.ClearHandleState();
    other.lastCloseResult = GB_SystemResult::Succeeded(u8"GB_WinHandleScope::MoveFrom");
}
