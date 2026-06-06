#ifndef GLOBALBASE_SYSTEM_SHELL_H_H
#define GLOBALBASE_SYSTEM_SHELL_H_H

#include "../GlobalBasePort.h"
#include "GB_SystemResult.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief Shell 启动后的窗口显示方式。
 *
 * @remarks
 * - 该枚举会映射到 Win32 SW_SHOWDEFAULT、SW_HIDE、SW_SHOWNORMAL、SW_SHOWMINIMIZED、SW_SHOWMAXIMIZED 等显示参数。
 * - Shell 目标程序或关联程序可以选择忽略该显示建议，因此该值不保证最终窗口一定按预期显示。
 */
enum class GB_ShellShowMode : uint16_t
{
    /** @brief 使用 Shell/系统默认显示方式。 */
    Default = 0,

    /** @brief 请求隐藏窗口启动；目标程序可能忽略该请求。 */
    Hide = 1,

    /** @brief 请求按普通窗口显示。 */
    Normal = 2,

    /** @brief 请求最小化显示。 */
    Minimized = 3,

    /** @brief 请求最大化显示。 */
    Maximized = 4
};

/**
 * @brief Windows Shell verb。
 *
 * @remarks
 * - Shell verb 表示对目标对象执行的动作，实际可用 verb 取决于目标文件类型、注册表关联和 Shell 扩展。
 * - Default 会传递空 verb，让 Shell 使用目标对象的默认动作。
 * - RunAs 通常用于触发 UAC 提权运行，仅适用于可执行类目标，不适用于 URI。
 */
enum class GB_ShellExecuteVerb : uint16_t
{
    /** @brief 使用目标对象默认 verb。 */
    Default = 0,

    /** @brief 打开目标对象；文件会由关联程序打开，可执行文件会被启动。 */
    Open = 1,

    /** @brief 用资源管理器浏览目标文件夹。 */
    Explore = 2,

    /** @brief 编辑目标文档；是否可用取决于文件关联。 */
    Edit = 3,

    /** @brief 打印目标文档；是否可用取决于文件关联。 */
    Print = 4,

    /** @brief 以管理员权限运行目标程序，通常会触发 UAC。 */
    RunAs = 5
};

/**
 * @brief 常用 Windows 设置页。
 *
 * @remarks
 * - 该枚举会映射到 ms-settings URI。
 * - 不同 Windows 版本可能对具体设置 URI 的支持程度不同；Shell 接受请求不代表设置页一定存在或加载成功。
 */
enum class GB_SystemSettingsPage : uint16_t
{
    /** @brief Windows 设置首页，ms-settings:。 */
    Home = 0,

    /** @brief 显示设置页，ms-settings:display。 */
    Display = 1,

    /** @brief 声音设置页，ms-settings:sound。 */
    Sound = 2,

    /** @brief 蓝牙设置页，ms-settings:bluetooth。 */
    Bluetooth = 3,

    /** @brief Wi-Fi 设置页，ms-settings:network-wifi。 */
    Wifi = 4,

    /** @brief 网络设置页，ms-settings:network。 */
    Network = 5,

    /** @brief 默认应用设置页，ms-settings:defaultapps。 */
    DefaultApps = 6,

    /** @brief 应用和功能设置页，ms-settings:appsfeatures。 */
    AppsFeatures = 7,

    /** @brief 剪贴板设置页，ms-settings:clipboard。 */
    Clipboard = 8,

    /** @brief 电源和睡眠设置页，ms-settings:powersleep。 */
    PowerSleep = 9,

    /** @brief 麦克风隐私设置页，ms-settings:privacy-microphone。 */
    PrivacyMicrophone = 10,

    /** @brief 摄像头隐私设置页，ms-settings:privacy-webcam。 */
    PrivacyCamera = 11
};

/**
 * @brief ShellExecuteExW 执行选项。
 *
 * @remarks
 * - 所有 std::string 均约定为 UTF-8 编码。
 * - arguments 中每个元素表示一个独立参数，调用方不需要自行添加引号或转义。
 * - 当 target 是已存在的文档文件而非可执行类文件/应用快捷方式时，不允许附加 arguments。
 * - waitTimeoutMilliseconds=-1 表示无限等待，其他负数非法；非负值不能等于或超过 Win32 INFINITE。
 * - allowUnsafeUriScheme=false 时，通用 Execute 只允许 http、https 和 ms-settings URI。
 * - URI 目标不允许包含 ASCII 控制字符或未转义空白字符，也不允许再追加 arguments。
 * - OpenUrl 只接受 http / https URL；file URI 或其他协议如确需交给 Shell，应使用 Execute 并显式设置 allowUnsafeUriScheme。
 * - 普通业务应优先调用 OpenUrl、OpenFile、OpenFolder、RunApplication 等语义化接口。
 */
struct GB_ShellExecuteOptions
{
    /** @brief Shell 执行目标；可以是 URL、ms-settings URI、文件路径、文件夹路径、可执行文件路径或可执行文件名。 */
    std::string target = "";

    /** @brief 传给目标程序的独立命令行参数列表；内部会按 Windows 命令行规则拼接和转义。 */
    std::vector<std::string> arguments;

    /** @brief 工作目录；为空表示不显式指定，非空时必须是已存在的文件夹路径。 */
    std::string workingDirectory = "";

    /** @brief Shell verb，决定对 target 执行 open、explore、edit、print、runas 或默认动作。 */
    GB_ShellExecuteVerb verb = GB_ShellExecuteVerb::Open;

    /** @brief 目标窗口显示方式建议。 */
    GB_ShellShowMode showMode = GB_ShellShowMode::Default;

    /** @brief 是否等待 Shell 返回的进程句柄退出。 */
    bool waitForExit = false;

    /** @brief 等待超时时间，单位为毫秒；-1 表示无限等待，仅在 waitForExit=true 时生效。 */
    int64_t waitTimeoutMilliseconds = -1;

    /** @brief 是否允许 Execute 处理 http、https、ms-settings 之外的 URI scheme。 */
    bool allowUnsafeUriScheme = false;
};

/**
 * @brief Shell 执行后可获取的进程信息。
 *
 * @remarks
 * - ShellExecuteExW 成功不保证一定创建新进程，也不保证一定返回进程句柄。
 * - hasProcessId=false 表示无法获得进程 ID。
 * - hasExitCode=true 仅在成功等待到进程退出并取得退出码后成立。
 * - processHandleReturned=true 仅表示 Shell 曾返回进程句柄，该句柄会在 Execute 返回前由模块关闭。
 * - waitCompleted=false 表示未请求等待；如果请求等待但 Shell 未返回可等待进程句柄，Execute 会返回失败。
 */
struct GB_ShellExecuteResult
{
    /** @brief Shell 返回进程句柄时查询到的进程 ID；仅在 hasProcessId=true 时有效。 */
    uint32_t processId = 0;

    /** @brief 等待进程正常退出后取得的退出码；仅在 hasExitCode=true 时有效。 */
    uint32_t exitCode = 0;

    /** @brief processId 字段是否有效。 */
    bool hasProcessId = false;

    /** @brief exitCode 字段是否有效。 */
    bool hasExitCode = false;

    /** @brief ShellExecuteExW 是否返回过可用进程句柄；句柄不会泄漏给调用方。 */
    bool processHandleReturned = false;

    /** @brief 是否完成了等待；只有 waitForExit=true 且等待到进程退出时为 true。 */
    bool waitCompleted = false;
};

/**
 * @brief Windows Shell 启动能力封装。
 *
 * @remarks
 * - 本类负责把 URL、文件、文件夹、设置 URI、应用程序或应用程序快捷方式交给 Windows Shell。
 * - 本类不负责进程枚举、标准输入输出重定向、环境变量配置、窗口管理或网页加载状态判断。
 * - 打开成功仅表示 Windows Shell 接受了请求，不表示目标业务已经完成。
 * - 对 ShellExecuteExW 的通用调用会先初始化 COM，以兼容可能通过 Shell 扩展完成的 verb 实现。
 * - 非 Windows 平台下所有执行接口返回 UnsupportedPlatform。
 */
class GLOBALBASE_PORT GB_SystemShell final
{
public:
    /** @brief 静态工具类，不允许构造实例。 */
    GB_SystemShell() = delete;

    /** @brief 静态工具类，不允许析构实例。 */
    ~GB_SystemShell() = delete;

    /**
     * @brief 使用系统默认浏览器打开 HTTP/HTTPS URL。
     *
     * @param url 目标 URL，必须是有效 UTF-8，且 scheme 只能为 http 或 https。
     * @return Shell 接受打开请求时返回 Succeeded；URL 非法、无关联程序、权限不足或平台不支持时返回对应错误。
     *
     * @remarks
     * - 不接受 file、ftp、自定义协议等 URL；如确需打开其他 URI，请使用 Execute 并显式设置 allowUnsafeUriScheme。
     * - 成功返回不代表网页已经加载完成，只代表 ShellExecuteExW 调用成功。
     */
    static GB_SystemResult OpenUrl(const std::string& url);

    /**
     * @brief 使用系统关联程序打开已存在文件。
     *
     * @param filePath 文件路径，UTF-8 编码；必须存在且不能是文件夹。
     * @return Shell 接受打开请求时返回 Succeeded；路径不存在、目标是文件夹、无关联程序或平台不支持时返回对应错误。
     */
    static GB_SystemResult OpenFile(const std::string& filePath);

    /**
     * @brief 使用资源管理器或系统默认方式打开已存在文件夹。
     *
     * @param folderPath 文件夹路径，UTF-8 编码；必须存在且必须是文件夹。
     * @return Shell 接受打开请求时返回 Succeeded；路径不存在、目标不是文件夹或平台不支持时返回对应错误。
     */
    static GB_SystemResult OpenFolder(const std::string& folderPath);

    /**
     * @brief 使用资源管理器浏览已存在文件夹。
     *
     * @param folderPath 文件夹路径，UTF-8 编码；必须存在且必须是文件夹。
     * @return Shell 接受 explore 请求时返回 Succeeded；路径不存在、目标不是文件夹或平台不支持时返回对应错误。
     *
     * @remarks 与 OpenFolder 相比，该接口显式使用 explore verb，更强调以资源管理器方式浏览。 */
    static GB_SystemResult ExploreFolder(const std::string& folderPath);

    /**
     * @brief 在资源管理器中定位并选中指定文件或文件夹。
     *
     * @param path 文件或文件夹路径，UTF-8 编码；必须存在。
     * @return 成功打开资源管理器并选中目标时返回 Succeeded；路径不存在、COM 初始化失败或 Shell PIDL 解析失败时返回对应错误。
     *
     * @remarks
     * - 内部使用 SHParseDisplayName 和 SHOpenFolderAndSelectItems，而不是简单拼接 explorer.exe /select 参数。
     * - 该接口只负责请求 Explorer 选中目标，不保证 Explorer 窗口最终一定置顶。
     */
    static GB_SystemResult RevealInExplorer(const std::string& path);

    /**
     * @brief 打开 Windows 设置首页。
     *
     * @return Shell 接受 ms-settings: 请求时返回 Succeeded；平台不支持或 URI 打开失败时返回对应错误。
     */
    static GB_SystemResult OpenSettings();

    /**
     * @brief 打开预置 Windows 设置页。
     *
     * @param settingsPage 设置页枚举值。
     * @return 枚举合法且 Shell 接受打开请求时返回 Succeeded；枚举非法、平台不支持或 URI 打开失败时返回对应错误。
     *
     * @remarks 实际页面是否存在取决于 Windows 版本和系统策略。 */
    static GB_SystemResult OpenSettingsPage(GB_SystemSettingsPage settingsPage);

    /**
     * @brief 打开指定 ms-settings URI。
     *
     * @param settingsUri 设置 URI，必须使用 ms-settings scheme，且不能包含控制字符或未转义空白字符。
     * @return Shell 接受打开请求时返回 Succeeded；URI 非法、平台不支持或打开失败时返回对应错误。
     */
    static GB_SystemResult OpenSettingsUri(const std::string& settingsUri);

    /**
     * @brief 启动普通应用程序。
     *
     * @param applicationPath 应用程序路径、快捷方式路径或可由 Shell 解析的可执行文件名；不能是 URI。
     * @param arguments 传给应用程序的独立参数列表；内部负责 Windows 命令行转义。
     * @return Shell 接受启动请求时返回 Succeeded；目标非法、参数非法、路径不可访问或平台不支持时返回对应错误。
     *
     * @remarks
     * - 若 applicationPath 是显式文件系统路径，则必须存在且属于可执行类扩展名。
     * - 普通文档应使用 OpenFile，不应通过该接口附加 arguments 打开。
     */
    static GB_SystemResult RunApplication(const std::string& applicationPath, const std::vector<std::string>& arguments = std::vector<std::string>());

    /**
     * @brief 以管理员权限启动应用程序。
     *
     * @param applicationPath 应用程序路径、快捷方式路径或可由 Shell 解析的可执行文件名；不能是 URI。
     * @param arguments 传给应用程序的独立参数列表；内部负责 Windows 命令行转义。
     * @return Shell 接受 runas 请求时返回 Succeeded；用户取消 UAC、目标非法、权限不足或平台不支持时返回对应错误。
     *
     * @remarks
     * - 内部使用 runas verb，通常会触发 UAC 提权提示。
     * - 成功返回只表示 Shell 接受提权启动请求，不表示被启动程序的业务逻辑已经完成。
     */
    static GB_SystemResult RunApplicationAsAdmin(const std::string& applicationPath, const std::vector<std::string>& arguments = std::vector<std::string>());

    /**
     * @brief 通用 ShellExecuteExW 封装。
     *
     * @param options Shell 执行选项。
     * @param executeResult 可选输出执行结果；传入 nullptr 表示不关心进程 ID、退出码等信息。
     * @return ShellExecuteExW 成功并满足等待要求时返回 Succeeded；参数非法、COM 初始化失败、Shell 调用失败或等待超时时返回对应错误。
     *
     * @remarks
     * - 如果 executeResult 非空或 waitForExit=true，内部会请求 SEE_MASK_NOCLOSEPROCESS。
     * - 若 waitForExit=true 但 Shell 未返回可等待进程句柄，接口会返回 InvalidState。
     * - 内部会校验命令行长度，避免超过 Windows 典型 32767 字符限制。
     * - 调用方不应在 DllMain 或持有 loader lock 的上下文中调用该接口。
     */
    static GB_SystemResult Execute(const GB_ShellExecuteOptions& options, GB_ShellExecuteResult* executeResult = nullptr);

    /**
     * @brief 判断数值是否是合法 GB_ShellShowMode 枚举值。
     *
     * @param showModeValue 待校验的无符号整数值。
     * @return true 表示可安全转换为 GB_ShellShowMode；false 表示非法。
     */
    static bool IsValidShowModeValue(uint64_t showModeValue);

    /**
     * @brief 判断数值是否是合法 GB_ShellExecuteVerb 枚举值。
     *
     * @param executeVerbValue 待校验的无符号整数值。
     * @return true 表示可安全转换为 GB_ShellExecuteVerb；false 表示非法。
     */
    static bool IsValidExecuteVerbValue(uint64_t executeVerbValue);

    /**
     * @brief 判断数值是否是合法 GB_SystemSettingsPage 枚举值。
     *
     * @param settingsPageValue 待校验的无符号整数值。
     * @return true 表示可安全转换为 GB_SystemSettingsPage；false 表示非法。
     */
    static bool IsValidSettingsPageValue(uint64_t settingsPageValue);

    /**
     * @brief 获取预置 Windows 设置页对应的 ms-settings URI。
     *
     * @param settingsPage 设置页枚举值。
     * @return 合法枚举返回对应 URI；未知枚举返回空字符串。
     */
    static std::string GetSettingsPageUri(GB_SystemSettingsPage settingsPage);
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_SHELL_H_H
