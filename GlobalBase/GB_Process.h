#ifndef GLOBALBASE_PROCESS_H_H
#define GLOBALBASE_PROCESS_H_H

#include "GlobalBasePort.h"
#include <string>
#include <vector>

// 判断当前进程是否“以管理员方式运行”
GLOBALBASE_PORT bool GB_IsRunningAsAdmin();

/**
 * @brief 确保当前进程以管理员/root 权限运行；若不是则请求提权并重新启动自身。
 *
 * 行为：
 * - 若已是管理员/root：返回 true。
 * - 若不是：
 *   - Windows：使用 ShellExecuteEx + "runas" 触发 UAC 提权启动新进程；成功后退出当前进程。
 *   - Linux：使用 execvp 执行 sudo 来启动自身（通常会要求输入密码）；成功后当前进程映像被替换（不返回）。
 *
 * @warning
 * - Windows 路径：当提权启动成功后，会调用 ExitProcess(0) 终止当前进程，
 *   不会触发栈展开与对象析构（属于“硬退出”）。如需更温和的退出方式，
 *   建议业务层在调用本函数前自行完成必要的清理/持久化工作。
 *
 * @return true 表示当前进程已处于管理员/root 权限；false 表示提权/重启失败（或被用户取消）。
 */
GLOBALBASE_PORT bool GB_EnsureRunningAsAdmin();

/**
 * @brief 进程信息。
 *
 * @remarks
 * - 所有字符串均为 UTF-8 编码。
 * - 由于权限、系统限制或进程瞬时退出等原因，部分字段可能无法获取；
 *   此时对应 hasXXX 标记为 false（或保持默认值）。
 * - Linux 下多数信息来自 /proc，受 ptrace 访问控制影响，某些字段可能读取不到或显示为 0。
 */
struct GB_ProcessInfo
{
    // --- 基本标识 ---
    int processId = 0;                 // PID
    int parentProcessId = 0;           // PPID

    // --- 名称/路径/命令行/用户 ---
    std::string processNameUtf8 = "";       // 进程名（Windows: EXE 文件名；Linux: /proc/[pid]/comm）
    std::string executablePathUtf8 = "";    // 可执行文件完整路径
    bool hasExecutablePath = false;

    std::string commandLineUtf8 = "";       // 命令行（Windows 仅保证当前进程可靠获取）
    bool hasCommandLine = false;

    std::string userNameUtf8 = "";          // 用户名（Windows: Domain\User；Linux: pw_name）
    bool hasUserName = false;

    std::string workingDirectoryUtf8 = "";  // 当前工作目录（Windows 仅保证当前进程可靠获取）
    bool hasWorkingDirectory = false;

    // --- 运行态属性 ---
    bool is64Bit = false;
    bool isElevated = false;

    std::string stateUtf8;             // Linux: R/S/D/Z/T/I 等

    unsigned int threadCount = 0;
    unsigned int handleCount = 0;      // Windows: GetProcessHandleCount

    unsigned int priorityClass = 0;    // Windows: priority class（GetPriorityClass）
    int niceValue = 0;                 // Linux: nice

    // --- 时间 ---
    double cpuUserSeconds = 0;
    double cpuKernelSeconds = 0;
    bool hasCpuTimes = false;

    long long startTimeUnixMs = 0;     // UNIX epoch（毫秒）
    bool hasStartTime = false;

    // --- 内存 ---
    unsigned long long virtualMemoryBytes = 0;
    unsigned long long residentSetBytes = 0;
    unsigned long long peakResidentSetBytes = 0;
    unsigned long long privateMemoryBytes = 0;
    bool hasMemoryInfo = false;
};

/**
 * @brief 获取当前系统的所有进程信息。
 *
 * - Windows：基于 Toolhelp 快照遍历进程；并尽量补齐可执行路径、用户名、CPU 时间、内存等信息。
 * - Linux：遍历 /proc/<pid>/，读取 stat/cmdline/status/exe/cwd 等信息。
 *
 * @return 进程信息列表。即便部分进程因权限无法读取详细信息，也会尽量返回基础信息（pid/name 等）。
 */
GLOBALBASE_PORT std::vector<GB_ProcessInfo> GB_GetAllProcessesInfo();

/**
 * @brief 按进程名查找进程 ID（PID）。
 *
 * @param processNameUtf8      进程名（UTF-8）。
 *                            - Windows：通常为 "xxx.exe"（来自 Toolhelp 的 szExeFile）。
 *                            - Linux：通常为 /proc/<pid>/comm。
 * @param allowSubstringMatch 是否允许“包含匹配”。为 true 时，只要进程名包含该字符串即可命中。
 * @param caseSensitive       是否大小写敏感。
 * @return 匹配到的 PID 列表；未找到则返回空 vector。
 */
GLOBALBASE_PORT std::vector<int> GB_FindProcessIdsByName(const std::string& processNameUtf8, bool allowSubstringMatch = false, bool caseSensitive = false);

/**
 * @brief 根据进程 ID（PID）获取进程信息。
 *
 * @param processId 进程 ID。
 * @param info      输出的进程信息。
 * @return true 表示成功获取（至少包含 pid/name 等基础字段）；false 表示进程不存在或无法访问。
 */
GLOBALBASE_PORT bool GB_GetProcessInfo(int processId, GB_ProcessInfo& info);

/**
 * @brief 根据可执行文件路径启动进程。
 *
 * @param executablePathUtf8 可执行文件路径（UTF-8）。
 * @param outProcessId       可选输出：新进程 PID。
 * @return true 表示启动成功。
 */
GLOBALBASE_PORT bool GB_StartProcess(const std::string& executablePathUtf8, int* outProcessId = nullptr);

/**
 * @brief 根据可执行文件路径启动进程（可携带参数与工作目录）。
 *
 * @param executablePathUtf8   可执行文件路径（UTF-8）。
 * @param argsUtf8             参数列表（UTF-8），不需要自行做引号转义。
 * @param workingDirectoryUtf8 工作目录（UTF-8）。为空则沿用当前进程工作目录。
 * @param outProcessId         可选输出：新进程 PID。
 * @return true 表示启动成功。
 */
GLOBALBASE_PORT bool GB_StartProcess(const std::string& executablePathUtf8, const std::vector<std::string>& argsUtf8, const std::string& workingDirectoryUtf8 = "", int* outProcessId = nullptr);

/**
 * @brief 根据进程 ID（PID）终止（关闭/杀死）进程。
 *
 * 终止策略（尽量先“温和”，再“强制”）：
 * - Windows：尝试向该进程的顶层窗口投递 WM_CLOSE（如果有）；可选等待；超时且允许时使用 TerminateProcess 强杀。
 * - Linux：发送 SIGTERM；可选等待；超时且允许时发送 SIGKILL 强杀。
 *
 * @param processId     进程 ID。
 * @param waitMs        等待进程退出的时间（毫秒）。0 表示不等待。
 * @param allowForceKill 若为 true，等待超时后会尝试强杀。
 * @return true 表示已成功发出终止请求或确认进程已退出；false 表示无法访问/终止失败。
 */
GLOBALBASE_PORT bool GB_TerminateProcessById(int processId, unsigned int waitMs = 0, bool allowForceKill = true);

/**
 * @brief 根据进程名终止（关闭/杀死）进程。
 *
 * @param processNameUtf8      进程名（UTF-8）。
 * @param allowSubstringMatch 是否允许“包含匹配”。为 true 时，只要进程名包含该字符串即可命中。
 * @param caseSensitive       是否大小写敏感。
 * @param waitMs              等待进程退出的时间（毫秒）。0 表示不等待。
 * @param allowForceKill      若为 true，等待超时后会尝试强杀。
 * @return 成功发出终止请求（或确认已退出）的进程数量。
 */
GLOBALBASE_PORT size_t GB_TerminateProcessesByName(const std::string& processNameUtf8, bool allowSubstringMatch = false, bool caseSensitive = false, unsigned int waitMs = 0, bool allowForceKill = true);













#endif