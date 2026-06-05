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
 */
enum class GB_ShellShowMode : uint16_t
{
    Default = 0,
    Hide = 1,
    Normal = 2,
    Minimized = 3,
    Maximized = 4
};

/**
 * @brief Windows Shell verb。
 */
enum class GB_ShellExecuteVerb : uint16_t
{
    Default = 0,
    Open = 1,
    Explore = 2,
    Edit = 3,
    Print = 4,
    RunAs = 5
};

/**
 * @brief 常用 Windows 设置页。
 */
enum class GB_SystemSettingsPage : uint16_t
{
    Home = 0,
    Display = 1,
    Sound = 2,
    Bluetooth = 3,
    Wifi = 4,
    Network = 5,
    DefaultApps = 6,
    AppsFeatures = 7,
    Clipboard = 8,
    PowerSleep = 9,
    PrivacyMicrophone = 10,
    PrivacyCamera = 11
};

/**
 * @brief ShellExecuteExW 执行选项。
 *
 * 说明：
 * - 所有 std::string 均约定为 UTF-8 编码；
 * - arguments 中每个元素表示一个独立参数，调用方不需要自行添加引号或转义；
 * - waitTimeoutMilliseconds=-1 表示无限等待，其他负数非法；
 * - allowUnsafeUriScheme=false 时，通用 Execute 只允许 http、https、file 和 ms-settings URI；
 * - 普通业务应优先调用 OpenUrl、OpenFile、OpenFolder、RunApplication 等语义化接口。
 */
struct GB_ShellExecuteOptions
{
    std::string target = "";
    std::vector<std::string> arguments;
    std::string workingDirectory = "";
    GB_ShellExecuteVerb verb = GB_ShellExecuteVerb::Open;
    GB_ShellShowMode showMode = GB_ShellShowMode::Default;
    bool waitForExit = false;
    int64_t waitTimeoutMilliseconds = -1;
    bool allowUnsafeUriScheme = false;
};

/**
 * @brief Shell 执行后可获取的进程信息。
 *
 * 说明：
 * - ShellExecuteExW 成功不保证一定创建新进程，也不保证一定返回进程句柄；
 * - hasProcessId=false 表示无法获得进程 ID；
 * - hasExitCode=true 仅在成功等待到进程退出并取得退出码后成立；
 * - processHandleReturned=true 仅表示 Shell 曾返回进程句柄，该句柄会在 Execute 返回前由模块关闭；
 * - waitCompleted=false 可能表示未请求等待、等待超时，或 Shell 未返回可等待的进程句柄。
 */
struct GB_ShellExecuteResult
{
    uint32_t processId = 0;
    uint32_t exitCode = 0;
    bool hasProcessId = false;
    bool hasExitCode = false;
    bool processHandleReturned = false;
    bool waitCompleted = false;
};

/**
 * @brief Windows Shell 启动能力封装。
 *
 * 说明：
 * - 本类负责把 URL、文件、文件夹、设置 URI 或应用程序交给 Windows Shell；
 * - 本类不负责进程枚举、标准输入输出、环境变量、窗口管理或网页加载状态；
 * - 打开成功仅表示 Windows Shell 接受了请求，不表示目标业务已经完成；
 * - 非 Windows 平台下所有执行接口返回 UnsupportedPlatform。
 */
class GLOBALBASE_PORT GB_SystemShell final
{
public:
    GB_SystemShell() = delete;
    ~GB_SystemShell() = delete;

    static GB_SystemResult OpenUrl(const std::string& url);
    static GB_SystemResult OpenFile(const std::string& filePath);
    static GB_SystemResult OpenFolder(const std::string& folderPath);
    static GB_SystemResult ExploreFolder(const std::string& folderPath);
    static GB_SystemResult RevealInExplorer(const std::string& path);

    static GB_SystemResult OpenSettings();
    static GB_SystemResult OpenSettingsPage(GB_SystemSettingsPage settingsPage);
    static GB_SystemResult OpenSettingsUri(const std::string& settingsUri);

    static GB_SystemResult RunApplication(const std::string& applicationPath, const std::vector<std::string>& arguments = std::vector<std::string>());
    static GB_SystemResult RunApplicationAsAdmin(const std::string& applicationPath, const std::vector<std::string>& arguments = std::vector<std::string>());

    static GB_SystemResult Execute(const GB_ShellExecuteOptions& options, GB_ShellExecuteResult* executeResult = nullptr);

    static bool IsValidShowModeValue(uint64_t showModeValue);
    static bool IsValidExecuteVerbValue(uint64_t executeVerbValue);
    static bool IsValidSettingsPageValue(uint64_t settingsPageValue);
    static std::string GetSettingsPageUri(GB_SystemSettingsPage settingsPage);
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_SHELL_H_H
