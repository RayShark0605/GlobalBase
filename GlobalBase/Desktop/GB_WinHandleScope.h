#ifndef GLOBALBASE_WIN_HANDLE_SCOPE_H_H
#define GLOBALBASE_WIN_HANDLE_SCOPE_H_H

#include "GB_SystemResult.h"

#include <cstdint>
#include <string>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief Windows 原生资源句柄关闭方式。
 *
 * 说明：
 * - Windows API 中并不存在一种可以关闭所有句柄的统一函数；
 * - HANDLE、FindFirstFile 返回的搜索句柄、HKEY、HGDIOBJ、HDC、HWND、HMODULE 等资源必须按来源调用对应释放函数；
 * - socket 不是 CloseHandle 管理对象，应由网络模块使用 closesocket 释放，不应传入 FromKernelHandle；
 * - 本枚举用于告诉 GB_WinHandleScope 在析构或 Close() 时应采用哪一种释放策略；
 * - None 表示仅保存借用句柄，不拥有资源，不会自动关闭。
 */
enum class GB_WinHandleCloseMethod : uint16_t
{
    /** @brief 不拥有资源，不执行关闭。 */
    None = 0,

    /** @brief 使用 CloseHandle 关闭内核对象句柄。 */
    CloseHandle = 1,

    /** @brief 使用 FindClose 关闭 FindFirstFile / FindFirstStream 等返回的搜索句柄。 */
    FindClose = 2,

    /** @brief 使用 FindCloseChangeNotification 关闭文件系统变更通知句柄。 */
    FindCloseChangeNotification = 3,

    /** @brief 使用 CloseServiceHandle 关闭 SCM 或 Service 句柄。 */
    CloseServiceHandle = 4,

    /** @brief 使用 RegCloseKey 关闭注册表键句柄。 */
    RegCloseKey = 5,

    /** @brief 使用 FreeLibrary 释放 LoadLibrary 得到的模块句柄。 */
    FreeLibrary = 6,

    /** @brief 使用 LocalFree 释放 LocalAlloc 或部分 Windows API 返回的本地内存。 */
    LocalFree = 7,

    /** @brief 使用 GlobalFree 释放 GlobalAlloc 返回的全局内存。 */
    GlobalFree = 8,

    /** @brief 使用 DeleteObject 删除 GDI 对象，例如画笔、画刷、字体、位图、区域、调色板。 */
    DeleteObject = 9,

    /** @brief 使用 DeleteDC 删除 CreateCompatibleDC / CreateDC 等创建的设备上下文。 */
    DeleteDC = 10,

    /** @brief 使用 ReleaseDC 释放 GetDC / GetWindowDC 获取的窗口设备上下文。 */
    ReleaseDC = 11,

    /** @brief 使用 DestroyWindow 销毁窗口。 */
    DestroyWindow = 12,

    /** @brief 使用 DestroyMenu 销毁菜单。 */
    DestroyMenu = 13,

    /** @brief 使用 DestroyIcon 销毁图标。 */
    DestroyIcon = 14,

    /** @brief 使用 DestroyCursor 销毁光标。 */
    DestroyCursor = 15,

    /** @brief 使用 UnhookWindowsHookEx 卸载 Windows Hook。 */
    UnhookWindowsHookEx = 16,

    /** @brief 使用 UnmapViewOfFile 解除文件映射视图。 */
    UnmapViewOfFile = 17,

    /** @brief 使用 CoTaskMemFree 释放 COM 任务内存。 */
    CoTaskMemFree = 18
};

/**
 * @brief Windows 原生资源句柄 RAII 封装。
 *
 * 说明：
 * - 该类用于系统自动化模块内部管理 Win32 / User32 / GDI / Advapi32 等 API 返回的原生资源句柄；
 * - 析构函数会按 GB_WinHandleCloseMethod 自动释放资源，避免函数多出口、异常或提前 return 导致句柄泄漏；
 * - 析构函数不抛异常，也无法向外返回关闭失败信息；需要诊断关闭失败时，应显式调用 Close()；
 * - Close() 会返回 GB_SystemResult；关闭成功、无需关闭或句柄本身无效时清空当前对象，关闭失败时保留句柄以便调用方诊断或重试；
 * - 本类不可复制，只能移动，保证同一资源在任意时刻最多只有一个所有者；
 * - 本类不会强行接管伪句柄、INVALID_HANDLE_VALUE、预定义注册表根键等不应关闭的句柄；
 * - std::string 均约定为 UTF-8 编码；
 * - 该类不保证线程安全，同一个实例不应被多个线程并发读写。
 */
class GLOBALBASE_PORT GB_WinHandleScope final
{
public:
    /** @brief 原生句柄存储类型。Windows 下各类指针型句柄均可隐式转换为 void*。 */
    using NativeHandle = void*;

public:
    /**
     * @brief 构造空句柄对象。
     */
    GB_WinHandleScope();

    /**
     * @brief 接管指定句柄。
     *
     * @param handle 原生句柄。
     * @param closeMethod 关闭方式。
     * @param contextHandle 上下文句柄。当前主要用于 ReleaseDC，此时 contextHandle 表示 HWND。
     * @param resourceName 资源名或业务名，用于错误诊断。
     */
    GB_WinHandleScope(NativeHandle handle, GB_WinHandleCloseMethod closeMethod, NativeHandle contextHandle = nullptr, const std::string& resourceName = std::string());

    /**
     * @brief 析构并自动释放当前拥有的资源。
     */
    ~GB_WinHandleScope() noexcept;

    GB_WinHandleScope(const GB_WinHandleScope&) = delete;
    GB_WinHandleScope& operator=(const GB_WinHandleScope&) = delete;

    /**
     * @brief 移动构造，转移句柄所有权。
     */
    GB_WinHandleScope(GB_WinHandleScope&& other);

    /**
     * @brief 移动赋值，先释放当前资源，再接管 @p other 的资源。
     */
    GB_WinHandleScope& operator=(GB_WinHandleScope&& other);

    /** @brief 创建借用句柄对象，不自动关闭。 */
    static GB_WinHandleScope FromBorrowedHandle(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 CloseHandle 关闭的内核对象句柄对象。 */
    static GB_WinHandleScope FromKernelHandle(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 FindClose 关闭的文件搜索句柄对象。 */
    static GB_WinHandleScope FromFindHandle(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 FindCloseChangeNotification 关闭的文件系统变更通知句柄对象。 */
    static GB_WinHandleScope FromChangeNotificationHandle(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 CloseServiceHandle 关闭的服务管理句柄对象。 */
    static GB_WinHandleScope FromServiceHandle(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 RegCloseKey 关闭的注册表键句柄对象。 */
    static GB_WinHandleScope FromRegistryKey(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 FreeLibrary 释放的模块句柄对象。 */
    static GB_WinHandleScope FromLibraryModule(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 LocalFree 释放的本地内存句柄对象。 */
    static GB_WinHandleScope FromLocalMemory(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 GlobalFree 释放的全局内存句柄对象。 */
    static GB_WinHandleScope FromGlobalMemory(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 DeleteObject 删除的 GDI 对象句柄对象。 */
    static GB_WinHandleScope FromGdiObject(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 DeleteDC 删除的内存 DC 或自建 DC 句柄对象。 */
    static GB_WinHandleScope FromCreatedDc(NativeHandle handle, const std::string& resourceName = std::string());

    /**
     * @brief 创建使用 ReleaseDC 释放的窗口 DC 句柄对象。
     *
     * @param windowHandle GetDC / GetWindowDC 时使用的 HWND；GetDC(nullptr) 获取屏幕 DC 时可传 nullptr。
     * @param deviceContextHandle HDC。
     * @param resourceName 资源名或业务名，用于错误诊断。
     */
    static GB_WinHandleScope FromWindowDc(NativeHandle windowHandle, NativeHandle deviceContextHandle, const std::string& resourceName = std::string());

    /** @brief 创建使用 DestroyWindow 销毁的窗口句柄对象。 */
    static GB_WinHandleScope FromWindow(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 DestroyMenu 销毁的菜单句柄对象。 */
    static GB_WinHandleScope FromMenu(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 DestroyIcon 销毁的图标句柄对象。仅用于 DestroyIcon 文档允许释放的自有图标。 */
    static GB_WinHandleScope FromIcon(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 DestroyCursor 销毁的光标句柄对象。仅用于 DestroyCursor 文档允许释放的自有光标。 */
    static GB_WinHandleScope FromCursor(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 UnhookWindowsHookEx 卸载的 Hook 句柄对象。 */
    static GB_WinHandleScope FromHook(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 UnmapViewOfFile 解除映射的文件映射视图对象。 */
    static GB_WinHandleScope FromMappedView(NativeHandle handle, const std::string& resourceName = std::string());

    /** @brief 创建使用 CoTaskMemFree 释放的 COM 任务内存对象。 */
    static GB_WinHandleScope FromComTaskMemory(NativeHandle handle, const std::string& resourceName = std::string());

    /**
     * @brief 判断当前是否为空句柄。
     */
    bool IsEmpty() const;

    /**
     * @brief 判断当前句柄值对当前关闭方式而言是否有效。
     *
     * 说明：
     * - 空句柄和 INVALID_HANDLE_VALUE 返回 false；
     * - 借用句柄 closeMethod=None 只表示不拥有资源，不表示可以把失败 API 返回值视为有效句柄；
     * - 预定义注册表根键是有效句柄值，但不是本类应关闭的自有资源。
     */
    bool IsValid() const;

    /**
     * @brief 判断当前对象是否拥有需要自动释放的资源。
     */
    bool HasOwnership() const;

    /**
     * @brief 显式 bool 转换。true 表示当前句柄值有效。
     */
    explicit operator bool() const;

    /**
     * @brief 获取原生句柄。
     */
    NativeHandle GetHandle() const;

    /**
     * @brief 按指定类型获取原生句柄。
     */
    template<typename HandleType>
    HandleType GetHandleAs() const
    {
        return reinterpret_cast<HandleType>(handle);
    }

    /**
     * @brief 获取上下文句柄。
     */
    NativeHandle GetContextHandle() const;

    /**
     * @brief 获取关闭方式。
     */
    GB_WinHandleCloseMethod GetCloseMethod() const;

    /**
     * @brief 获取资源名或业务名。
     */
    std::string GetResourceName() const;

    /**
     * @brief 获取上一次 Close() 的结果。
     */
    GB_SystemResult GetLastCloseResult() const;

    /**
     * @brief 显式关闭当前资源。
     *
     * 说明：
     * - 若当前对象为空、句柄值无效或不拥有资源，返回成功结果；
     * - 若关闭失败，返回包含 Win32 原生错误信息的 GB_SystemResult，并保留句柄以便调用方诊断或重试；
     * - 关闭成功、无需关闭或句柄本身无效时会清空当前对象，避免析构时重复关闭同一资源。
     */
    GB_SystemResult Close();

    /**
     * @brief 释放所有权并返回原生句柄，不执行关闭。
     */
    NativeHandle Detach();

    /**
     * @brief 释放所有权并按指定类型返回原生句柄，不执行关闭。
     */
    template<typename HandleType>
    HandleType DetachAs()
    {
        return reinterpret_cast<HandleType>(Detach());
    }

    /**
     * @brief 重置为新句柄，并释放原有资源。
     *
     * @return 原有资源的关闭结果；若原本没有资源，则返回成功。
     */
    GB_SystemResult Reset(NativeHandle newHandle = nullptr, GB_WinHandleCloseMethod newCloseMethod = GB_WinHandleCloseMethod::None, NativeHandle newContextHandle = nullptr, const std::string& newResourceName = std::string());

    /**
     * @brief 交换两个对象。
     */
    void Swap(GB_WinHandleScope& other);

    /**
     * @brief 获取 INVALID_HANDLE_VALUE 等价值。
     */
    static NativeHandle GetInvalidHandleValue();

    /**
     * @brief 判断指定句柄是否为空。
     */
    static bool IsNullHandle(NativeHandle handle);

    /**
     * @brief 判断指定句柄是否等于 INVALID_HANDLE_VALUE。
     */
    static bool IsInvalidHandleValue(NativeHandle handle);

    /**
     * @brief 判断指定关闭方式是否为当前已定义的有效值。
     */
    static bool IsValidCloseMethodValue(uint64_t closeMethodValue);

    /**
     * @brief 判断指定句柄在指定关闭方式下是否为有效句柄值。
     */
    static bool IsValidHandleForCloseMethod(NativeHandle handle, GB_WinHandleCloseMethod closeMethod);

    /**
     * @brief 判断指定句柄在指定关闭方式下是否应由本类关闭。
     */
    static bool IsClosableHandleForCloseMethod(NativeHandle handle, GB_WinHandleCloseMethod closeMethod);

    /**
     * @brief 获取关闭方式英文名称。
     *
     * @return UTF-8 编码文本。
     */
    static std::string GetCloseMethodName(GB_WinHandleCloseMethod closeMethod);

    /**
     * @brief 获取关闭方式中文描述。
     *
     * @return UTF-8 编码文本。
     */
    static std::string GetCloseMethodDescription(GB_WinHandleCloseMethod closeMethod);

private:
    void CloseSilently() noexcept;
    void ClearHandleState() noexcept;
    void MoveFrom(GB_WinHandleScope& other);

private:
    NativeHandle handle = nullptr;
    NativeHandle contextHandle = nullptr;
    GB_WinHandleCloseMethod closeMethod = GB_WinHandleCloseMethod::None;
    std::string resourceName = "";
    GB_SystemResult lastCloseResult;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_WIN_HANDLE_SCOPE_H_H
