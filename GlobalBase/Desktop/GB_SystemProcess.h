#ifndef GLOBALBASE_SYSTEM_PROCESS_H_H
#define GLOBALBASE_SYSTEM_PROCESS_H_H

#include "../GlobalBasePort.h"
#include "GB_SystemResult.h"
#include "GB_SystemWindow.h"

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

/** @brief Windows 进程架构。 */
enum class GB_ProcessArchitecture : uint16_t
{
    Unknown = 0, ///< 无法确定进程架构。
    X86 = 1,     ///< 32 位 x86 进程。
    X64 = 2,     ///< 64 位 x64 进程。
    Arm32 = 3,   ///< 32 位 ARM 进程。
    Arm64 = 4    ///< 64 位 ARM64 进程。
};

/** @brief 进程运行状态。 */
enum class GB_ProcessState : uint16_t
{
    Unknown = 0, ///< 无法确定进程当前状态。
    Running = 1, ///< 进程内核对象尚未进入有信号状态。
    Exited = 2   ///< 进程已经退出。
};

/**
 * @brief 子进程窗口显示建议。
 *
 * @remarks 该值通过 STARTUPINFOW::wShowWindow 传递，目标程序可以忽略显示建议。
 */
enum class GB_ProcessShowMode : uint16_t
{
    Default = 0,   ///< 使用目标程序或系统默认显示方式。
    Hidden = 1,    ///< 请求隐藏主窗口。
    Normal = 2,    ///< 请求普通显示主窗口。
    Minimized = 3, ///< 请求最小化显示主窗口。
    Maximized = 4  ///< 请求最大化显示主窗口。
};

/** @brief 子进程控制台创建策略。 */
enum class GB_ProcessConsoleMode : uint16_t
{
    Inherit = 0, ///< 不附加控制台创建标志，沿用 CreateProcessW 默认语义。
    New = 1,     ///< 使用 CREATE_NEW_CONSOLE 创建新控制台。
    None = 2     ///< 使用 CREATE_NO_WINDOW；不能与 New 同时使用。
};

/** @brief 子进程优先级类别。 */
enum class GB_ProcessPriority : uint16_t
{
    Normal = 0,      ///< NORMAL_PRIORITY_CLASS。
    Idle = 1,        ///< IDLE_PRIORITY_CLASS。
    BelowNormal = 2, ///< BELOW_NORMAL_PRIORITY_CLASS。
    AboveNormal = 3, ///< ABOVE_NORMAL_PRIORITY_CLASS。
    High = 4         ///< HIGH_PRIORITY_CLASS；不提供实时优先级以避免系统失去响应。
};

/** @brief 捕获输出字节的文本编码。 */
enum class GB_ProcessOutputEncoding : uint16_t
{
    Utf8 = 0,              ///< 子进程输出为 UTF-8。
    SystemAnsi = 1,        ///< 子进程输出使用当前系统 ANSI 代码页。
    Oem = 2,               ///< 子进程输出使用当前系统 OEM 控制台代码页。
    Utf16LittleEndian = 3  ///< 子进程输出为无 BOM 或带 BOM 的 UTF-16LE。
};

/** @brief 输出事件所属标准流。 */
enum class GB_ProcessOutputStream : uint16_t
{
    StandardOutput = 0, ///< 标准输出。
    StandardError = 1   ///< 标准错误。
};

/**
 * @brief 进程瞬时身份。
 *
 * @remarks
 * - processId 单独使用时可能在旧进程退出后被系统复用；
 * - hasCreationTime=true 时，破坏性操作会核对进程创建时间，拒绝作用于复用后的新进程；
 * - creationTime100Nanoseconds 保存 Win32 FILETIME 的原始 100ns 计数，不是 Unix 时间。
 */
struct GB_ProcessIdentity
{
    uint32_t processId = 0;                  ///< 进程 ID。
    uint64_t creationTime100Nanoseconds = 0; ///< 进程创建 FILETIME 的 64 位原始值。
    bool hasCreationTime = false;            ///< creationTime100Nanoseconds 是否有效。

    /** @brief 判断 processId 是否具备基本有效性。 */
    bool IsValid() const
    {
        return processId != 0;
    }

    /** @brief 判断当前身份是否包含可用于防止 PID 复用的创建时间。 */
    bool IsStrong() const
    {
        return processId != 0 && hasCreationTime;
    }

    /** @brief 比较两个进程身份的全部字段是否相等。 */
    bool operator==(const GB_ProcessIdentity& other) const
    {
        return processId == other.processId && creationTime100Nanoseconds == other.creationTime100Nanoseconds && hasCreationTime == other.hasCreationTime;
    }

    /** @brief 比较两个进程身份的任一字段是否不同。 */
    bool operator!=(const GB_ProcessIdentity& other) const
    {
        return !(*this == other);
    }

    /** @brief 清空全部身份字段。 */
    void Reset()
    {
        processId = 0;
        creationTime100Nanoseconds = 0;
        hasCreationTime = false;
    }
};

/** @brief 单个进程字段读取失败信息。 */
struct GB_ProcessFieldError
{
    std::string fieldName = "";                                    ///< 读取失败的字段名称，UTF-8 编码。
    GB_SystemErrorCode errorCode = GB_SystemErrorCode::UnknownError; ///< 归一化错误码。
    uint64_t nativeErrorCode = 0;                                   ///< 原生错误码；无原生错误时为 0。
    std::string message = "";                                      ///< 字段读取失败说明，UTF-8 编码。
};

/**
 * @brief 进程信息快照。
 *
 * @remarks
 * - 为兼容旧 GB_Process API，原有字段保持原顺序，新增字段只追加在结构体末尾；
 * - 所有 std::string 均约定为 UTF-8；
 * - 单个字段失败不会使整个进程从枚举结果中消失，失败信息记录在 fieldErrors。
 */
struct GB_ProcessInfo
{
    int processId = 0;                              ///< 进程 ID。
    int parentProcessId = 0;                        ///< 父进程 ID 快照。
    std::string processNameUtf8 = "";               ///< 进程可执行文件名。
    std::string executablePathUtf8 = "";            ///< 可执行文件完整路径。
    bool hasExecutablePath = false;                 ///< executablePathUtf8 是否有效。
    std::string commandLineUtf8 = "";               ///< 完整命令行；Windows 下只保证当前进程可读取。
    bool hasCommandLine = false;                    ///< commandLineUtf8 是否有效。
    std::string userNameUtf8 = "";                  ///< 进程令牌对应的 Domain\User。
    bool hasUserName = false;                       ///< userNameUtf8 是否有效。
    std::string workingDirectoryUtf8 = "";          ///< 工作目录；Windows 下只保证当前进程可读取。
    bool hasWorkingDirectory = false;               ///< workingDirectoryUtf8 是否有效。
    bool is64Bit = false;                           ///< 兼容字段；架构为 X64 或 Arm64 时为 true。
    bool isElevated = false;                        ///< 进程令牌是否提升。
    std::string stateUtf8 = "";                     ///< 兼容字段；Windows 下为 Running、Exited 或 Unknown。
    unsigned int threadCount = 0;                   ///< Toolhelp 快照中的线程数量。
    unsigned int handleCount = 0;                   ///< 进程句柄数量。
    unsigned int priorityClass = 0;                 ///< Win32 优先级类别原始值。
    int niceValue = 0;                              ///< Linux nice 兼容字段；Windows 下保持 0。
    double cpuUserSeconds = 0;                      ///< 用户态累计 CPU 时间。
    double cpuKernelSeconds = 0;                    ///< 内核态累计 CPU 时间。
    bool hasCpuTimes = false;                       ///< CPU 时间字段是否有效。
    long long startTimeUnixMs = 0;                  ///< 进程创建时间，Unix epoch 毫秒。
    bool hasStartTime = false;                      ///< startTimeUnixMs 是否有效。
    unsigned long long virtualMemoryBytes = 0;      ///< 已提交内存的近似值。
    unsigned long long residentSetBytes = 0;        ///< 当前工作集字节数。
    unsigned long long peakResidentSetBytes = 0;    ///< 峰值工作集字节数。
    unsigned long long privateMemoryBytes = 0;      ///< 私有提交字节数。
    bool hasMemoryInfo = false;                     ///< 内存字段是否有效。

    GB_ProcessIdentity identity;                    ///< 包含 PID 和创建时间的进程身份。
    uint32_t sessionId = 0;                         ///< Windows 会话 ID。
    bool hasSessionId = false;                      ///< sessionId 是否有效。
    GB_ProcessArchitecture architecture = GB_ProcessArchitecture::Unknown; ///< 进程架构。
    bool hasArchitecture = false;                   ///< architecture 是否成功读取。
    GB_ProcessState processState = GB_ProcessState::Unknown; ///< 归一化运行状态。
    uint32_t exitCode = 0;                          ///< 进程退出码。
    bool hasExitCode = false;                       ///< 仅在确认进程已退出后为 true。
    long long exitTimeUnixMs = 0;                   ///< 进程退出时间，Unix epoch 毫秒。
    bool hasExitTime = false;                       ///< exitTimeUnixMs 是否有效。
    GB_WindowId mainWindowId;                       ///< 按模块策略选出的顶层主窗口。
    std::string mainWindowTitle = "";               ///< 主窗口标题，UTF-8 编码。
    bool hasMainWindow = false;                     ///< mainWindowId 是否有效。
    bool isCurrentProcess = false;                  ///< 是否为调用进程自身。
    bool isSystemProcess = false;                   ///< 是否为 PID 0 或 PID 4。
    bool isCriticalProcess = false;                 ///< Windows 是否把该进程标记为关键进程。
    bool hasCriticalProcessState = false;           ///< isCriticalProcess 是否成功读取。
    bool isProtectedProcess = false;                ///< 是否具有受保护进程保护级别。
    bool hasProtectedProcessState = false;          ///< isProtectedProcess 是否成功读取。
    bool hasElevationState = false;                 ///< isElevated 是否成功读取。
    bool hasHandleCount = false;                    ///< handleCount 是否成功读取。
    bool hasPriorityClass = false;                  ///< priorityClass 是否成功读取。
    std::vector<GB_ProcessFieldError> fieldErrors;  ///< 可选字段读取失败列表。
};

/** @brief 环境变量覆盖项。 */
struct GB_ProcessEnvironmentVariable
{
    std::string name = "";  ///< 环境变量名，UTF-8 编码；不能为空且不能包含等号或 NUL。
    std::string value = ""; ///< 环境变量值，UTF-8 编码；允许为空但不能包含 NUL。
};

/** @brief Job Object 约束选项。 */
struct GB_ProcessJobOptions
{
    bool enabled = false;                  ///< 是否创建 Job 并把新进程加入其中。
    bool terminateOnJobClose = false;      ///< 最后一个 Job 句柄关闭时是否终止 Job 内全部进程。
    uint32_t maximumActiveProcessCount = 0; ///< 活动进程数上限；0 表示不限制。
    uint64_t processMemoryLimitBytes = 0;   ///< 单进程提交内存上限；0 表示不限制。
    uint64_t jobMemoryLimitBytes = 0;       ///< 整个 Job 提交内存上限；0 表示不限制。
    uint32_t cpuRatePercent = 0;            ///< CPU 硬上限百分比，范围 1 到 100；0 表示不限制。
    bool breakawayAllowed = false;          ///< 是否允许显式使用 CREATE_BREAKAWAY_FROM_JOB 的后代脱离 Job。
};

/** @brief 子进程输出字节块事件。 */
struct GB_ProcessOutputEvent
{
    GB_ProcessOutputStream outputStream = GB_ProcessOutputStream::StandardOutput; ///< 事件所属标准流。
    std::vector<uint8_t> bytes;                                                  ///< 本次读取到的原始字节。
};

/**
 * @brief 子进程原始输出回调。
 *
 * @remarks 字节块边界不保证与文本字符或行边界对齐；回调在独立线程执行，必须最终返回，且不能在回调线程内销毁或移动所属实例，否则无法安全完成工作线程回收。
 */
using GB_ProcessOutputCallback = std::function<void(const GB_ProcessOutputEvent&)>;

/** @brief 进程启动选项。 */
struct GB_ProcessStartOptions
{
    std::string executablePath = "";             ///< 可执行文件路径，UTF-8 编码；必须是已存在的普通文件。
    std::vector<std::string> arguments;          ///< 独立参数列表；内部按 Windows 命令行规则转义。
    std::string rawCommandLine = "";             ///< 高级完整命令行；非空时 arguments 必须为空，并由调用方提供 argv[0]。
    std::string workingDirectory = "";           ///< 工作目录；为空表示继承当前目录。
    std::vector<GB_ProcessEnvironmentVariable> environmentVariables; ///< 环境变量覆盖项。
    bool inheritCurrentEnvironment = true;       ///< 是否先继承当前环境，再应用覆盖项。
    GB_ProcessShowMode showMode = GB_ProcessShowMode::Default; ///< 窗口显示建议。
    GB_ProcessConsoleMode consoleMode = GB_ProcessConsoleMode::Inherit; ///< 控制台策略。
    GB_ProcessPriority priority = GB_ProcessPriority::Normal; ///< 进程优先级。
    bool createNewProcessGroup = false;           ///< 是否使用 CREATE_NEW_PROCESS_GROUP。
    bool startSuspended = false;                  ///< 返回实例时主线程是否仍保持挂起。
    bool redirectStandardInput = false;           ///< 是否创建可写标准输入管道。
    bool redirectStandardOutput = false;          ///< 是否捕获标准输出。
    bool redirectStandardError = false;           ///< 是否捕获标准错误。
    bool mergeStandardErrorToOutput = false;      ///< 是否让 stderr 与 stdout 共用写端；要求重定向 stdout。
    size_t maximumCapturedBytesPerStream = 16 * 1024 * 1024; ///< 每个流最多保留的原始字节数；允许为 0，且不能超过 INT_MAX。
    GB_ProcessOutputEncoding outputEncoding = GB_ProcessOutputEncoding::Utf8; ///< 捕获结果编码。
    GB_ProcessOutputCallback outputCallback;      ///< 可选原始输出回调；在独立线程执行。
    size_t maximumPendingOutputCallbacks = 1024;  ///< 回调队列上限；满时丢弃最旧事件。
    GB_ProcessJobOptions jobOptions;              ///< 可选 Job Object 约束。
    int64_t runTimeoutMilliseconds = -1;          ///< Run 最大等待时间；-1 表示无限等待。
    bool terminateOnRunTimeout = true;            ///< Run 超时后是否终止进程或 Job。
    uint32_t timeoutTerminationExitCode = 1;      ///< Run 超时终止时使用的退出码。
};

/** @brief 通用进程等待选项。 */
struct GB_ProcessWaitOptions
{
    int64_t timeoutMilliseconds = 5000;         ///< 最大等待时间；-1 表示无限等待，0 表示只检查一次。
    uint32_t pollIntervalMilliseconds = 50;     ///< 轮询间隔；必须大于 0。
    const std::atomic<bool>* cancellationFlag = nullptr; ///< 外部取消标志；不转移所有权。
};

/** @brief 进程查询选项。 */
struct GB_ProcessQueryOptions
{
    bool queryExecutablePath = true; ///< 是否查询完整可执行路径。
    bool queryUserName = true;       ///< 是否查询令牌用户名。
    bool queryElevation = true;      ///< 是否查询提升状态。
    bool queryArchitecture = true;   ///< 是否查询进程架构。
    bool queryTimes = true;          ///< 是否查询创建、退出和 CPU 时间。
    bool queryMemory = true;         ///< 是否查询内存统计。
    bool queryHandleCount = true;    ///< 是否查询句柄数量。
    bool queryPriority = true;       ///< 是否查询优先级类别。
    bool queryProtection = true;     ///< 是否查询关键和受保护状态。
    bool queryMainWindow = true;     ///< 是否查询可见的顶层应用主窗口。
    bool recordFieldErrors = true;   ///< 是否记录可选字段失败。
};

/** @brief 进程查找条件；所有非空条件之间按 AND 关系组合。 */
struct GB_ProcessFindOptions
{
    uint32_t processId = 0;               ///< 指定进程 ID；0 表示不限制。
    uint32_t parentProcessId = 0;         ///< 指定父进程 ID；0 表示不限制。
    uint32_t sessionId = 0;               ///< 指定会话 ID。
    bool hasSessionId = false;            ///< 是否启用 sessionId 条件。
    std::string processName = "";         ///< 进程名条件，UTF-8 编码。
    std::string executablePath = "";      ///< 完整可执行路径条件，UTF-8 编码。
    std::string commandLineContains = ""; ///< 命令行包含条件，UTF-8 编码。
    bool exactNameMatch = true;           ///< processName 是否精确匹配。
    bool caseSensitive = false;           ///< 文本匹配是否区分大小写。
    bool onlyCurrentSession = false;      ///< 是否只返回当前会话进程。
    bool includeSystemProcesses = true;   ///< 是否允许系统进程参与结果。
    GB_ProcessQueryOptions queryOptions;  ///< 匹配进程的字段查询选项。
};

/** @brief 温和关闭后可选强制终止的策略。 */
struct GB_ProcessCloseOptions
{
    uint32_t closeMessageTimeoutMilliseconds = 2000; ///< 单个窗口处理 WM_CLOSE 的超时。
    int64_t waitForExitMilliseconds = 5000;          ///< 发出关闭请求后等待退出的时间。
    bool forceTerminateAfterTimeout = false;          ///< 温和关闭无法完成时是否升级为强制终止。
    bool terminateJobWhenAvailable = true;            ///< 实例持有 Job 时是否优先终止整个 Job。
    uint32_t forcedExitCode = 1;                       ///< 强制终止时使用的退出码。
};

/** @brief 启动并等待进程结束的结果。 */
struct GB_ProcessRunResult
{
    uint32_t processId = 0;              ///< 已启动进程 ID。
    uint32_t exitCode = 0;               ///< 进程退出码。
    bool hasExitCode = false;            ///< exitCode 是否有效。
    bool timedOut = false;               ///< 是否发生运行超时。
    bool terminatedByModule = false;     ///< 是否由 Run 在超时后终止。
    std::string standardOutput = "";     ///< 转换后的标准输出。
    std::string standardError = "";      ///< 转换后的标准错误；合并模式下为空。
    bool standardOutputTruncated = false; ///< 标准输出是否被截断。
    bool standardErrorTruncated = false;  ///< 标准错误是否被截断。
    bool standardOutputEncodingValid = true; ///< 标准输出是否按指定编码成功转换为 UTF-8。
    bool standardErrorEncodingValid = true;  ///< 标准错误是否按指定编码成功转换为 UTF-8。
    std::string standardOutputEncodingError = ""; ///< 标准输出编码转换失败说明；不覆盖进程退出结果。
    std::string standardErrorEncodingError = "";  ///< 标准错误编码转换失败说明；不覆盖进程退出结果。
    uint64_t droppedOutputCallbackCount = 0; ///< 丢弃的输出回调事件数量。
    uint64_t elapsedMilliseconds = 0;     ///< 单调时钟耗时。
};

/**
 * @brief 由 GB_SystemProcess 创建并持有的进程实例。
 *
 * @remarks
 * - 本类不可复制，只能移动；
 * - 普通实例析构不会终止仍在运行的进程；
 * - 仅当 Job 启用且 terminateOnJobClose=true 时，析构关闭 Job 会终止 Job 内进程。
 * - 输出读取与退出等待可并发调用；同一实例的关闭、终止、移动和析构仍应由调用方协调生命周期。
 */
class GLOBALBASE_PORT GB_ProcessInstance final
{
public:
    /** @brief 构造空进程实例。 */
    GB_ProcessInstance();

    /** @brief 按声明的析构策略停止内部线程并释放资源。 */
    ~GB_ProcessInstance();

    GB_ProcessInstance(const GB_ProcessInstance&) = delete;
    GB_ProcessInstance& operator=(const GB_ProcessInstance&) = delete;

    /** @brief 移动构造并转移全部进程所有权。 */
    GB_ProcessInstance(GB_ProcessInstance&& other) noexcept;

    /** @brief 移动赋值并转移全部进程所有权。 */
    GB_ProcessInstance& operator=(GB_ProcessInstance&& other) noexcept;

    /** @brief 判断实例是否持有有效进程句柄。 */
    bool IsValid() const;

    /** @brief 获取进程身份快照。 */
    GB_ProcessIdentity GetIdentity() const;

    /** @brief 获取进程 ID；空实例返回 0。 */
    uint32_t GetProcessId() const;

    /** @brief 获取初始主线程 ID；空实例返回 0。 */
    uint32_t GetMainThreadId() const;

    /** @brief 判断进程当前是否仍在运行。 */
    GB_SystemResult IsRunning(bool& running) const;

    /** @brief 获取退出码；进程仍在运行时 hasExitCode=false。 */
    GB_SystemResult GetExitCode(uint32_t& exitCode, bool& hasExitCode) const;

    /** @brief 等待进程退出并返回退出码。 */
    GB_SystemResult WaitForExit(uint32_t& exitCode, const GB_ProcessWaitOptions& waitOptions = GB_ProcessWaitOptions());

    /** @brief 等待 GUI 进程首次进入输入空闲状态。 */
    GB_SystemResult WaitForInputIdle(const GB_ProcessWaitOptions& waitOptions = GB_ProcessWaitOptions());

    /** @brief 等待该进程出现可见的顶层应用主窗口；隐藏辅助窗口和工具窗口不计入。 */
    GB_SystemResult WaitForMainWindow(GB_WindowInfo& windowInfo, const GB_ProcessWaitOptions& waitOptions = GB_ProcessWaitOptions());

    /** @brief 恢复以挂起状态创建的主线程。 */
    GB_SystemResult ResumeMainThread();

    /** @brief 向重定向标准输入写入 UTF-8 字节。 */
    GB_SystemResult WriteStandardInput(const std::string& bytes);

    /** @brief 关闭标准输入写端，使子进程读到 EOF。 */
    GB_SystemResult CloseStandardInput();

    /** @brief 获取当前已捕获并转换的标准输出。 */
    GB_SystemResult GetStandardOutput(std::string& output, bool& truncated) const;

    /** @brief 获取当前已捕获并转换的标准错误。 */
    GB_SystemResult GetStandardError(std::string& output, bool& truncated) const;

    /** @brief 获取丢弃的输出回调事件数量。 */
    uint64_t GetDroppedOutputCallbackCount() const;

    /** @brief 温和关闭进程，并按选项等待或升级为强制终止。 */
    GB_SystemResult Close(const GB_ProcessCloseOptions& closeOptions = GB_ProcessCloseOptions());

    /** @brief 强制终止当前进程；可优先终止持有的 Job。 */
    GB_SystemResult Terminate(uint32_t exitCode = 1, bool terminateJob = true);

private:
    class Impl;
    explicit GB_ProcessInstance(std::unique_ptr<Impl> implementation);
    void Swap(GB_ProcessInstance& other) noexcept;

private:
    std::unique_ptr<Impl> implementation;

    friend class GB_SystemProcess;
};

/** @brief Windows 进程生命周期、查询、等待和控制入口。 */
class GLOBALBASE_PORT GB_SystemProcess final
{
public:
    GB_SystemProcess() = delete;
    ~GB_SystemProcess() = delete;

    /** @brief 根据启动选项创建可控进程实例。 */
    static GB_SystemResult Start(const GB_ProcessStartOptions& options, GB_ProcessInstance& processInstance);

    /** @brief 创建进程、等待结束并收集退出码和标准流。 */
    static GB_SystemResult Run(const GB_ProcessStartOptions& options, GB_ProcessRunResult& runResult);

    /** @brief 枚举当前系统进程。 */
    static GB_SystemResult GetAllProcesses(std::vector<GB_ProcessInfo>& processes, const GB_ProcessQueryOptions& queryOptions = GB_ProcessQueryOptions());

    /** @brief 根据 PID 查询进程。 */
    static GB_SystemResult GetProcessInfo(uint32_t processId, GB_ProcessInfo& processInfo, const GB_ProcessQueryOptions& queryOptions = GB_ProcessQueryOptions());

    /** @brief 根据强身份查询并核对创建时间。 */
    static GB_SystemResult GetProcessInfo(const GB_ProcessIdentity& identity, GB_ProcessInfo& processInfo, const GB_ProcessQueryOptions& queryOptions = GB_ProcessQueryOptions());

    /** @brief 获取当前调用进程的信息。 */
    static GB_SystemResult GetCurrentProcessInfo(GB_ProcessInfo& processInfo, const GB_ProcessQueryOptions& queryOptions = GB_ProcessQueryOptions());

    /** @brief 按组合条件查找进程。 */
    static GB_SystemResult FindProcesses(const GB_ProcessFindOptions& findOptions, std::vector<GB_ProcessInfo>& processes);

    /** @brief 获取前台窗口所属进程；当前无前台窗口时成功且 found=false。 */
    static GB_SystemResult GetForegroundProcess(GB_ProcessInfo& processInfo, bool& found, const GB_ProcessQueryOptions& queryOptions = GB_ProcessQueryOptions());

    /** @brief 等待出现一个匹配进程。 */
    static GB_SystemResult WaitForProcess(const GB_ProcessFindOptions& findOptions, GB_ProcessInfo& processInfo, const GB_ProcessWaitOptions& waitOptions = GB_ProcessWaitOptions());

    /** @brief 等待所有匹配进程消失。 */
    static GB_SystemResult WaitForProcessesExit(const GB_ProcessFindOptions& findOptions, const GB_ProcessWaitOptions& waitOptions = GB_ProcessWaitOptions());

    /** @brief 等待指定强身份进程退出并返回退出码。 */
    static GB_SystemResult WaitForProcessExit(const GB_ProcessIdentity& identity, uint32_t& exitCode, const GB_ProcessWaitOptions& waitOptions = GB_ProcessWaitOptions());

    /** @brief 温和关闭外部进程。 */
    static GB_SystemResult CloseProcess(const GB_ProcessIdentity& identity, const GB_ProcessCloseOptions& closeOptions = GB_ProcessCloseOptions());

    /** @brief 强制终止外部进程，默认拒绝当前、系统、关键和受保护进程。 */
    static GB_SystemResult TerminateProcess(const GB_ProcessIdentity& identity, uint32_t exitCode = 1);

    /** @brief 以快照方式尽力递归终止外部进程树；预检或执行期间已经退出的目标会被跳过。 */
    static GB_SystemResult TerminateProcessTreeSnapshot(const GB_ProcessIdentity& rootIdentity, uint32_t exitCode = 1);

    /** @brief 修改外部进程优先级；不提供实时优先级。 */
    static GB_SystemResult SetProcessPriority(const GB_ProcessIdentity& identity, GB_ProcessPriority priority);

    /** @brief 获取当前进程可执行文件完整路径。 */
    static GB_SystemResult GetCurrentExecutablePath(std::string& executablePath);

    /** @brief 获取当前进程工作目录。 */
    static GB_SystemResult GetCurrentWorkingDirectory(std::string& workingDirectory);

    /** @brief 判断进程架构枚举值是否合法。 */
    static bool IsValidArchitectureValue(uint64_t value);

    /** @brief 判断进程状态枚举值是否合法。 */
    static bool IsValidStateValue(uint64_t value);

    /** @brief 判断显示模式枚举值是否合法。 */
    static bool IsValidShowModeValue(uint64_t value);

    /** @brief 判断控制台模式枚举值是否合法。 */
    static bool IsValidConsoleModeValue(uint64_t value);

    /** @brief 判断优先级枚举值是否合法。 */
    static bool IsValidPriorityValue(uint64_t value);

    /** @brief 判断输出编码枚举值是否合法。 */
    static bool IsValidOutputEncodingValue(uint64_t value);
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_PROCESS_H_H
