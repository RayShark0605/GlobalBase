#ifndef GLOBALBASE_SYSTEM_WINDOW_H_H
#define GLOBALBASE_SYSTEM_WINDOW_H_H

#include "../GlobalBasePort.h"
#include "GB_EventDispatcher.h"
#include "GB_SystemResult.h"
#include "../Geometry/GB_Rectangle.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief Windows 窗口的瞬时标识。
 *
 * @remarks
 * - nativeHandle 保存 HWND 的无符号整数表示，使公共头文件不依赖 Windows.h。
 * - HWND 只在窗口生命周期内有效，窗口销毁后该值可能被系统复用。
 * - processId / threadId 是创建标识时的快照；模块在每次操作前会重新核对它们，以降低句柄复用导致误操作的风险。
 * - 该类型不拥有窗口，也不会销毁窗口。
 */
struct GLOBALBASE_PORT GB_WindowId
{
    uint64_t nativeHandle = 0;                  ///< HWND 的无符号整数表示；仅作为瞬时引用，不拥有窗口。
    uint32_t processId = 0;                     ///< 创建该窗口的进程 ID 快照，用于句柄复用校验。
    uint32_t threadId = 0;                      ///< 创建该窗口的 GUI 线程 ID 快照，用于句柄复用校验。

    /**
     * @brief 判断当前窗口标识是否具备基本有效性。
     *
     * @return nativeHandle、processId、threadId 均非 0 时返回 true。
     *
     * @remarks
     * 该函数只检查结构体自身字段，不调用 IsWindow，也不保证 HWND 当前仍然存在。
     */
    bool IsValid() const;

    /**
     * @brief 清空窗口标识。
     *
     * @remarks
     * 调用后 nativeHandle、processId、threadId 都会被重置为 0。
     */
    void Reset();

    /**
     * @brief bool 语义转换。
     *
     * @return 与 IsValid() 相同。
     */
    explicit operator bool() const;

    /**
     * @brief 判断两个窗口标识是否完全相同。
     *
     * @param other 待比较的窗口标识。
     * @return nativeHandle、processId、threadId 全部相等时返回 true。
     */
    bool operator==(const GB_WindowId& other) const;

    /**
     * @brief 判断两个窗口标识是否不相同。
     *
     * @param other 待比较的窗口标识。
     * @return 与 operator== 相反。
     */
    bool operator!=(const GB_WindowId& other) const;
};

/**
 * @brief 窗口状态快照。
 *
 * @remarks
 * - 所有 std::string 均为 UTF-8 编码。
 * - 所有矩形均使用虚拟桌面屏幕坐标，采用 GB_Rectangle 的 min/max 表达。
 * - windowRectangle 是 GetWindowRect 的结果：在调用线程 DPI 上下文可提升时尽量返回物理像素；若系统不支持相关 DPI API，则遵循当前进程 DPI 感知模式。
 * - visibleFrameRectangle 是 DWM 可见边界：不包含不可见缩放边框，且按 Win32/DWM 语义不做 DPI 虚拟化。
 * - clientRectangle 已从客户区局部坐标转换为虚拟桌面屏幕坐标。
 * - 快照返回后不会随目标窗口变化；调用方需要实时状态时应重新查询。
 * - 进程路径、DWM 边界等字段可能因权限、系统版本或窗口瞬时销毁而不可用，对应 hasXxx 字段会为 false。
 */
struct GB_WindowInfo
{
    GB_WindowId windowId;                       ///< 窗口瞬时标识，包含 HWND、进程 ID 和线程 ID。
    std::string title = "";                   ///< 窗口标题，UTF-8 编码；读取失败或无标题时为空。
    std::string className = "";               ///< 窗口类名，UTF-8 编码；读取失败时为空。
    std::string processName = "";             ///< 所属进程可执行文件名，UTF-8 编码；读取失败时为空。
    std::string processPath = "";             ///< 所属进程完整路径，UTF-8 编码；读取失败时为空。
    GB_Rectangle windowRectangle;               ///< GetWindowRect 得到的窗口外接矩形，使用屏幕坐标。
    GB_Rectangle clientRectangle;               ///< 客户区矩形转换到屏幕坐标后的范围。
    GB_Rectangle visibleFrameRectangle;         ///< DWM 扩展边界矩形，通常不包含不可见缩放边框。
    uint64_t style = 0;                         ///< GWL_STYLE / GWLP_STYLE 对应的窗口样式位。
    uint64_t extendedStyle = 0;                 ///< GWL_EXSTYLE / GWLP_EXSTYLE 对应的扩展窗口样式位。
    bool hasTitle = false;                      ///< title 字段是否成功读取。
    bool hasClassName = false;                  ///< className 字段是否成功读取。
    bool hasProcessName = false;                ///< processName 字段是否成功读取。
    bool hasProcessPath = false;                ///< processPath 字段是否成功读取。
    bool hasWindowRectangle = false;            ///< windowRectangle 字段是否有效。
    bool hasClientRectangle = false;            ///< clientRectangle 字段是否有效。
    bool hasVisibleFrameRectangle = false;      ///< visibleFrameRectangle 字段是否有效。
    bool hasStyle = false;                      ///< style 字段是否成功读取。
    bool hasExtendedStyle = false;              ///< extendedStyle 字段是否成功读取。
    bool hasCloakedState = false;               ///< isCloaked 字段是否通过 DWM 成功读取。
    bool isVisible = false;                     ///< IsWindowVisible 的结果；不等价于实际无遮挡可见。
    bool isEnabled = false;                     ///< IsWindowEnabled 的结果。
    bool isMinimized = false;                   ///< 窗口当前是否最小化。
    bool isMaximized = false;                   ///< 窗口当前是否最大化。
    bool isTopMost = false;                     ///< 窗口是否具有 WS_EX_TOPMOST 扩展样式。
    bool isForeground = false;                  ///< 窗口当前是否为前台窗口。
    bool isToolWindow = false;                  ///< 窗口是否具有 WS_EX_TOOLWINDOW 扩展样式。
    bool isAppWindow = false;                   ///< 窗口是否具有 WS_EX_APPWINDOW 扩展样式。
    bool isCloaked = false;                     ///< DWM cloaked 状态；例如 UWP/虚拟桌面隐藏窗口可能为 true。
    bool isChildWindow = false;                 ///< 窗口是否具有 WS_CHILD 样式。
};

/**
 * @brief 窗口查找条件。
 *
 * @remarks
 * - 默认条件面向用户可感知的顶层应用窗口：可见、非工具窗口、非 DWM cloaked、标题非空。
 * - includeChildWindows=true 时，会在顶层窗口之外递归枚举子窗口；parentWindowId 有效时只枚举该父窗口的后代。
 * - applicationWindowsOnly 只约束顶层窗口；子窗口是否参与匹配由 includeChildWindows / parentWindowId 决定。
 * - 字符串匹配默认不区分大小写，并使用 Windows Unicode 序数比较语义。
 * - processNameEquals 匹配可执行文件名，processPathEquals 匹配完整路径；进程信息因权限不可读时不会命中对应条件。
 */
struct GB_WindowFindOptions
{
    std::string titleEquals = "";             ///< 标题精确匹配条件；为空表示不限制。
    std::string titleContains = "";           ///< 标题包含匹配条件；为空表示不限制。
    std::string classNameEquals = "";         ///< 窗口类名精确匹配条件；为空表示不限制。
    std::string classNameContains = "";       ///< 窗口类名包含匹配条件；为空表示不限制。
    std::string processNameEquals = "";       ///< 进程文件名精确匹配条件；为空表示不限制。
    std::string processPathEquals = "";       ///< 进程完整路径精确匹配条件；为空表示不限制。
    uint32_t processId = 0;                     ///< 目标进程 ID；为 0 表示不按进程 ID 过滤。
    uint32_t threadId = 0;                      ///< 目标 GUI 线程 ID；为 0 表示不按线程 ID 过滤。
    GB_WindowId parentWindowId;                 ///< 父窗口标识；有效时只在该窗口后代中查找。
    bool caseSensitive = false;                 ///< 字符串匹配是否区分大小写。
    bool visibleOnly = true;                    ///< 是否仅返回 IsWindowVisible 为 true 的窗口。
    bool includeToolWindows = false;            ///< 是否允许工具窗口参与匹配。
    bool includeCloakedWindows = false;         ///< 是否允许 DWM cloaked 窗口参与匹配。
    bool includeUntitledWindows = false;        ///< 是否允许标题为空的窗口参与匹配。
    bool includeChildWindows = false;           ///< 是否递归枚举并匹配子窗口。
    bool applicationWindowsOnly = true;         ///< 是否按用户应用窗口启发式规则过滤顶层窗口。
};

/**
 * @brief 等待窗口状态时使用的通用选项。
 *
 * @remarks
 * - timeoutMilliseconds=-1 表示无限等待，其他负值非法；0 表示只检查一次。
 * - pollIntervalMilliseconds 必须大于 0。
 * - cancellationFlag 不转移所有权；调用方必须保证等待期间该原子对象仍然存活。
 */
struct GB_WindowWaitOptions
{
    int64_t timeoutMilliseconds = 5000;         ///< 最大等待时间，单位毫秒；-1 表示无限等待，0 表示只检查一次。
    uint32_t pollIntervalMilliseconds = 50;     ///< 轮询间隔，单位毫秒；必须大于 0。
    const std::atomic<bool>* cancellationFlag = nullptr; ///< 外部取消标志；非空且被置为 true 时等待立即取消。
};

/** @brief 窗口事件类型。 */
enum class GB_SystemWindowEventType : uint16_t
{
    Unknown = 0,            ///< 未知或无法映射的窗口事件。
    Created = 1,            ///< 窗口对象创建事件。
    Destroyed = 2,          ///< 窗口对象销毁事件。
    Shown = 3,              ///< 窗口显示事件。
    Hidden = 4,             ///< 窗口隐藏事件。
    LocationChanged = 5,    ///< 原始位置变化事件，可能包含移动、缩放或布局变化。
    Moved = 6,              ///< 根据前后矩形派生出的移动事件。
    Resized = 7,            ///< 根据前后矩形派生出的尺寸变化事件。
    Minimized = 8,          ///< 窗口开始最小化或已最小化事件。
    Restored = 9,           ///< 窗口从最小化状态还原事件。
    TitleChanged = 10,      ///< 窗口标题变化事件。
    ForegroundChanged = 11, ///< 前台窗口变化事件。
    Focused = 12            ///< 焦点窗口变化事件。
};

/**
 * @brief 窗口事件过滤条件。
 *
 * @remarks
 * - 默认只监听顶层窗口；includeChildWindows=true 时允许子窗口事件。
 * - titleContains / classNameContains 为空时不限制对应字段。
 * - Destroyed 事件依赖监听器缓存的最后快照进行过滤；若窗口在监听启动前已存在，Start() 会预先建立缓存。
 */
struct GB_SystemWindowEventFilter
{
    GB_WindowId windowId;                       ///< 指定监听的窗口标识；无效时不按具体窗口过滤。
    uint32_t processId = 0;                     ///< 指定监听的进程 ID；为 0 时不按进程过滤。
    uint32_t threadId = 0;                      ///< 指定监听的 GUI 线程 ID；为 0 时不按线程过滤。
    std::string titleContains = "";           ///< 标题包含过滤条件；为空表示不限制。
    std::string classNameContains = "";       ///< 窗口类名包含过滤条件；为空表示不限制。
    bool caseSensitive = false;                 ///< 标题和类名过滤是否区分大小写。
    bool includeChildWindows = false;           ///< 是否允许子窗口事件通过过滤。
};

/**
 * @brief 窗口监听器选项。
 *
 * @remarks
 * - maxPendingNativeEvents 为原生事件工作队列上限；队列满时丢弃最旧事件。
 * - maxDispatchQueueSize 为强类型和通用事件派发器各自的队列上限；队列满时丢弃最旧事件。
 * - coalesceLocationChanges=true 时，同一窗口尚未处理的连续位置事件只保留最新一条；若中间穿插同一窗口的其他事件，则不跨过该事件合并，避免破坏事件因果顺序。
 */
struct GB_SystemWindowWatcherOptions
{
    GB_SystemWindowEventFilter filter;          ///< 窗口事件过滤条件。
    size_t maxPendingNativeEvents = 1024;       ///< 原生 WinEvent 待处理队列容量上限；0 表示不限制。
    size_t maxDispatchQueueSize = 1024;         ///< 强类型和通用事件分发队列容量上限；0 表示不限制。
    bool coalesceLocationChanges = true;        ///< 是否合并同一窗口连续待处理的位置变化事件。
};

/**
 * @brief 系统窗口事件。
 *
 * @remarks
 * - eventName 形如 "SystemWindow.LocationChanged"。
 * - LocationChanged 始终对应原始 EVENT_OBJECT_LOCATIONCHANGE；Moved / Resized 是根据前后快照派生的便利事件。
 * - Minimized / Restored 优先来自 EVENT_SYSTEM_MINIMIZESTART / EVENT_SYSTEM_MINIMIZEEND。
 * - Destroyed 事件中的 windowInfo 可能是销毁前最后一次缓存快照；事件被上层处理时 windowId 可能已经失效。
 */
struct GB_SystemWindowEvent
{
    GB_SystemWindowEventType eventType = GB_SystemWindowEventType::Unknown; ///< 归一化后的窗口事件类型。
    std::string eventName = "";               ///< 通用事件名称，例如 "SystemWindow.Resized"。
    std::string sourceName = "SetWinEventHook"; ///< 事件来源名称，默认表示来自 WinEvent Hook。
    uint64_t timestampMilliseconds = 0;         ///< 事件生成时间戳，单位毫秒。
    uint32_t nativeEvent = 0;                   ///< Win32 原生事件常量，例如 EVENT_OBJECT_LOCATIONCHANGE。
    int32_t nativeObjectId = 0;                 ///< WinEventProc 收到的 idObject 参数。
    int32_t nativeChildId = 0;                  ///< WinEventProc 收到的 idChild 参数。
    GB_WindowId windowId;                       ///< 事件关联窗口的瞬时标识。
    GB_WindowInfo windowInfo;                   ///< 事件发生后或当前可读取到的窗口快照。
    GB_WindowInfo previousWindowInfo;           ///< 监听器缓存的上一次窗口快照。
    bool hasWindowInfo = false;                 ///< windowInfo 字段是否有效。
    bool hasPreviousWindowInfo = false;         ///< previousWindowInfo 字段是否有效。
    bool isDerived = false;                     ///< 是否为监听器根据原始事件和快照差异派生出的事件。
};

/**
 * @brief Windows 顶层窗口查询、状态读取、状态控制和等待入口。
 *
 * @remarks
 * - 本类只负责 Win32 窗口级能力，不负责鼠标、键盘、截图、OCR、控件级 UI Automation、进程强杀或安全策略绕过。
 * - 非 Windows 平台下返回 UnsupportedPlatform。
 * - 普通完整性级别进程可能无法控制高完整性窗口；前台窗口保护也可能使 TryActivateWindow 失败。
 */
class GLOBALBASE_PORT GB_SystemWindow final
{
public:
    /**
     * @brief 禁止实例化，所有能力均通过静态函数提供。
     */
    GB_SystemWindow() = delete;

    /**
     * @brief 禁止析构实例，所有能力均通过静态函数提供。
     */
    ~GB_SystemWindow() = delete;

    /**
     * @brief 获取当前前台窗口的信息。
     *
     * @param windowInfo 输出窗口信息；当 found=false 时会被重置为空快照。
     * @param found 输出是否存在可读取的前台窗口。
     * @return 操作结果。前台窗口不存在时返回成功且 found=false；平台不支持或 Win32 调用失败时返回失败结果。
     *
     * @remarks
     * 该函数会校验窗口句柄仍然有效，并会尽量读取标题、类名、进程路径、窗口矩形、客户区矩形和 DWM 可见边界。
     */
    static GB_SystemResult GetForegroundWindow(GB_WindowInfo& windowInfo, bool& found);

    /**
     * @brief 枚举当前桌面上的顶层窗口。
     *
     * @param windows 输出窗口信息列表；函数开始时会清空该列表。
     * @return 操作结果。
     *
     * @remarks
     * 该函数按 Win32 EnumWindows 语义枚举顶层窗口，不递归普通子窗口。读取单个窗口信息失败时会跳过该窗口，不中断整体枚举。
     */
    static GB_SystemResult GetTopLevelWindows(std::vector<GB_WindowInfo>& windows);

    /**
     * @brief 枚举指定父窗口的子窗口。
     *
     * @param parentWindowId 父窗口标识。
     * @param windows 输出子窗口信息列表；函数开始时会清空该列表。
     * @param recursive 为 true 时递归枚举全部后代窗口；为 false 时只枚举直接子窗口。
     * @return 操作结果。
     *
     * @remarks
     * 调用前会重新校验 parentWindowId，避免对已销毁或被复用的 HWND 执行枚举。
     */
    static GB_SystemResult GetChildWindows(const GB_WindowId& parentWindowId, std::vector<GB_WindowInfo>& windows, bool recursive = true);

    /**
     * @brief 按条件查找窗口。
     *
     * @param options 查找条件。
     * @param windows 输出匹配窗口信息列表；函数开始时会清空该列表。
     * @return 操作结果。
     *
     * @remarks
     * 默认查找用户可见的顶层应用窗口；如需查找控件或嵌套窗口，应设置 includeChildWindows 或 parentWindowId。
     */
    static GB_SystemResult FindWindows(const GB_WindowFindOptions& options, std::vector<GB_WindowInfo>& windows);

    /**
     * @brief 查找第一个满足条件的窗口。
     *
     * @param options 查找条件。
     * @param windowInfo 输出第一个匹配窗口的信息；未找到时会被重置为空快照。
     * @param found 输出是否找到窗口。
     * @return 操作结果。未找到不是失败，会返回成功且 found=false。
     */
    static GB_SystemResult FindFirstWindow(const GB_WindowFindOptions& options, GB_WindowInfo& windowInfo, bool& found);

    /**
     * @brief 读取指定窗口的当前状态快照。
     *
     * @param windowId 窗口标识。
     * @param windowInfo 输出窗口信息。
     * @return 操作结果。
     *
     * @remarks
     * 调用前会校验 HWND、进程 ID、线程 ID 三元组，降低 HWND 被系统复用后误读其它窗口的风险。
     */
    static GB_SystemResult GetWindowInfo(const GB_WindowId& windowId, GB_WindowInfo& windowInfo);

    /**
     * @brief 判断指定窗口当前是否仍然存活。
     *
     * @param windowId 窗口标识。
     * @param alive 输出是否存活。
     * @return 操作结果。windowId 字段格式无效时返回 InvalidArgument；目标已不存在时返回成功且 alive=false。
     */
    static GB_SystemResult IsWindowAlive(const GB_WindowId& windowId, bool& alive);

    /**
     * @brief 将窗口切换为普通显示状态。
     *
     * @param windowId 窗口标识。
     * @param activate 为 true 时请求激活窗口；为 false 时只请求恢复普通显示状态。
     * @return 操作结果。
     */
    static GB_SystemResult ShowWindowNormal(const GB_WindowId& windowId, bool activate = false);

    /**
     * @brief 显示窗口但不主动激活窗口。
     *
     * @param windowId 窗口标识。
     * @return 操作结果。
     */
    static GB_SystemResult ShowWindowWithoutActivation(const GB_WindowId& windowId);

    /**
     * @brief 隐藏窗口。
     *
     * @param windowId 窗口标识。
     * @return 操作结果。
     */
    static GB_SystemResult HideWindow(const GB_WindowId& windowId);

    /**
     * @brief 最小化窗口。
     *
     * @param windowId 窗口标识。
     * @return 操作结果。
     */
    static GB_SystemResult MinimizeWindow(const GB_WindowId& windowId);

    /**
     * @brief 最大化窗口。
     *
     * @param windowId 窗口标识。
     * @return 操作结果。
     */
    static GB_SystemResult MaximizeWindow(const GB_WindowId& windowId);

    /**
     * @brief 还原窗口。
     *
     * @param windowId 窗口标识。
     * @return 操作结果。
     */
    static GB_SystemResult RestoreWindow(const GB_WindowId& windowId);

    /**
     * @brief 移动并调整窗口大小。
     *
     * @param windowId 窗口标识。
     * @param rectangle 目标矩形，使用屏幕坐标。对子窗口会自动转换到父窗口客户区坐标。
     * @param repaint 为 false 时使用 SWP_NOREDRAW 抑制重绘。
     * @param activate 为 true 时允许 SetWindowPos 激活窗口；为 false 时使用 SWP_NOACTIVATE。
     * @return 操作结果。
     *
     * @remarks
     * 跨线程窗口使用 SWP_ASYNCWINDOWPOS，以避免调用方被目标 GUI 线程阻塞。
     */
    static GB_SystemResult MoveResizeWindow(const GB_WindowId& windowId, const GB_Rectangle& rectangle, bool repaint = true, bool activate = false);

    /**
     * @brief 修改窗口置顶状态。
     *
     * @param windowId 窗口标识。
     * @param topMost 为 true 时设为 TopMost；为 false 时取消 TopMost。
     * @return 操作结果。
     */
    static GB_SystemResult SetTopMost(const GB_WindowId& windowId, bool topMost);

    /**
     * @brief 尝试将窗口切换到前台。
     *
     * @param windowId 窗口标识。
     * @param restoreIfMinimized 目标窗口最小化时是否先请求还原。
     * @return 操作结果。
     *
     * @remarks
     * 该操作受 Windows 前台窗口保护策略、完整性级别、目标线程状态等因素影响，失败并不一定代表目标窗口无效。
     */
    static GB_SystemResult TryActivateWindow(const GB_WindowId& windowId, bool restoreIfMinimized = true);

    /**
     * @brief 请求关闭窗口。
     *
     * @param windowId 窗口标识。
     * @param sendTimeoutMilliseconds 发送 WM_CLOSE 的超时时间，单位毫秒。
     * @return 操作结果。
     *
     * @remarks
     * 本函数只发送 WM_CLOSE 请求，不强制终止目标进程；目标窗口可以忽略或拦截关闭请求。
     */
    static GB_SystemResult RequestCloseWindow(const GB_WindowId& windowId, uint32_t sendTimeoutMilliseconds = 2000);

    /**
     * @brief 等待出现一个满足条件的窗口。
     *
     * @param findOptions 查找条件。
     * @param windowInfo 输出匹配窗口信息。
     * @param waitOptions 等待选项。
     * @return 操作结果。超时返回 Timeout，取消返回 Cancelled。
     */
    static GB_SystemResult WaitForWindow(const GB_WindowFindOptions& findOptions, GB_WindowInfo& windowInfo, const GB_WindowWaitOptions& waitOptions = GB_WindowWaitOptions());

    /**
     * @brief 等待指定窗口关闭或销毁。
     *
     * @param windowId 窗口标识。
     * @param waitOptions 等待选项。
     * @return 操作结果。目标已不存在时返回成功。
     */
    static GB_SystemResult WaitForWindowClosed(const GB_WindowId& windowId, const GB_WindowWaitOptions& waitOptions = GB_WindowWaitOptions());

    /**
     * @brief 等待指定窗口变为可见。
     *
     * @param windowId 窗口标识。
     * @param windowInfo 输出可见时的窗口信息。
     * @param waitOptions 等待选项。
     * @return 操作结果。
     */
    static GB_SystemResult WaitForWindowVisible(const GB_WindowId& windowId, GB_WindowInfo& windowInfo, const GB_WindowWaitOptions& waitOptions = GB_WindowWaitOptions());

    /**
     * @brief 等待指定窗口标题发生变化。
     *
     * @param windowId 窗口标识。
     * @param originalTitle 原始标题，使用 UTF-8 编码并按精确字符串比较。
     * @param windowInfo 输出标题变化后的窗口信息。
     * @param waitOptions 等待选项。
     * @return 操作结果。
     */
    static GB_SystemResult WaitForWindowTitleChanged(const GB_WindowId& windowId, const std::string& originalTitle, GB_WindowInfo& windowInfo, const GB_WindowWaitOptions& waitOptions = GB_WindowWaitOptions());

    /**
     * @brief 等待指定窗口成为前台窗口。
     *
     * @param windowId 窗口标识。
     * @param waitOptions 等待选项。
     * @return 操作结果。
     */
    static GB_SystemResult WaitForForegroundWindow(const GB_WindowId& windowId, const GB_WindowWaitOptions& waitOptions = GB_WindowWaitOptions());

    /**
     * @brief 获取窗口事件类型名称。
     *
     * @param eventType 窗口事件类型。
     * @return 可读事件名，例如 "Created"、"LocationChanged"、"Resized"；未知值返回 "Unknown"。
     */
    static std::string GetWindowEventTypeName(GB_SystemWindowEventType eventType);
};

/**
 * @brief Windows 窗口事件监听器。
 *
 * @remarks
 * - 内部使用专用消息线程注册 WINEVENT_OUTOFCONTEXT Hook，并使用工作线程完成快照、过滤和派生事件。
 * - 用户强类型回调与通用事件均通过 GB_EventDispatcher 异步串行分发，不在 WinEventProc 中执行。
 * - Start()/Stop() 可重复调用；析构时自动停止；可以在强类型回调中调用 Stop()。
 * - 回调异常由 GB_EventDispatcher 捕获，不会终止监听线程。
 */
class GLOBALBASE_PORT GB_SystemWindowWatcher final
{
public:
    using WindowEventCallback = std::function<void(const GB_SystemWindowEvent& event)>;

    /**
     * @brief 构造窗口事件监听器。
     *
     * @remarks
     * 使用默认 GB_SystemWindowWatcherOptions：只监听顶层窗口，队列上限为 1024，并合并连续 LocationChanged 事件。
     */
    GB_SystemWindowWatcher();

    /**
     * @brief 使用指定选项构造窗口事件监听器。
     *
     * @param options 监听选项。
     */
    explicit GB_SystemWindowWatcher(const GB_SystemWindowWatcherOptions& options);

    /**
     * @brief 析构监听器并停止内部线程。
     *
     * @remarks
     * 析构函数不会抛出异常；如果监听器仍在运行，会尽量执行 Stop()。
     */
    ~GB_SystemWindowWatcher() noexcept;

    /**
     * @brief 禁止拷贝构造。
     */
    GB_SystemWindowWatcher(const GB_SystemWindowWatcher&) = delete;

    /**
     * @brief 禁止拷贝赋值。
     */
    GB_SystemWindowWatcher& operator=(const GB_SystemWindowWatcher&) = delete;

    /**
     * @brief 启动窗口事件监听。
     *
     * @return 操作结果。
     *
     * @remarks
     * Start() 会创建专用消息线程注册 SetWinEventHook，并创建工作线程处理原生事件队列。重复启动会返回成功。
     */
    GB_SystemResult Start();

    /**
     * @brief 停止窗口事件监听。
     *
     * @return 操作结果。
     *
     * @remarks
     * Stop() 会注销 WinEvent Hook，停止原生事件工作线程，并停止强类型和通用事件分发器。该函数可在强类型回调中调用。
     */
    GB_SystemResult Stop();

    /**
     * @brief 判断监听器当前是否处于运行状态。
     *
     * @return 正在运行且未处于停止流程时返回 true。
     */
    bool IsRunning() const;

    /**
     * @brief 设置强类型窗口事件回调。
     *
     * @param callback 回调函数；传入空函数可清除当前回调。
     *
     * @remarks
     * 回调由内部 GB_EventDispatcher 异步串行调用，不在 WinEventProc 或原生事件工作线程中直接执行。
     */
    void SetWindowEventCallback(const WindowEventCallback& callback);

    /**
     * @brief 获取通用事件分发器。
     *
     * @return 通用事件分发器引用。
     *
     * @remarks
     * 分发器发布的事件名为 GB_SystemWindowEvent::eventName，payload 可通过 GB_Variant::AnyCast<GB_SystemWindowEvent>() 取回强类型事件对象。
     */
    GB_EventDispatcher& GetEventDispatcher();

    /**
     * @brief 获取已丢弃的原生窗口事件数量。
     *
     * @return 因原生事件队列溢出而丢弃的事件总数。
     */
    uint64_t GetDroppedNativeEventCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl; ///< Pimpl 实现对象，隐藏 Win32 头文件、线程、Hook 句柄和同步状态。
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_WINDOW_H_H
