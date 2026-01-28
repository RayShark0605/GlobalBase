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


#endif