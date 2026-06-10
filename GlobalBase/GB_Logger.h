#ifndef GLOBALBASE_LOGGER_H
#define GLOBALBASE_LOGGER_H

#include "GB_Utility.h"
#include "GB_Utf8String.h"
#include "GlobalBasePort.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// 可自愈或已回退=WARNING；关键业务事件=INFO；实现细节=DEBUG；逐步跟踪=TRACE。
enum class GB_LogLevel : int
{
    GBLOGLEVEL_TRACE = 0,      // 定位疑难杂症时临时开启，记录循环内部变量、函数入参/出参、分支路径、重试细节等。生产环境通常关闭，避免海量日志噪声与成本。
    GBLOGLEVEL_DEBUG = 1,      // 调试信息。记录关键状态变更、外部调用请求与响应摘要（脱敏）、缓存命中/失效、重要分支选择等。
    GBLOGLEVEL_INFO = 2,       // 业务里程碑与正常运转事实。记录服务启动/停止、配置加载结果、计划任务开始/结束、批处理完成等。
    GBLOGLEVEL_WARNING = 3,    // 潜在问题或可自愈异常。重试后成功、超时但已回退、使用默认配置、资源接近阈值等。
    GBLOGLEVEL_ERROR = 4,      // 操作已经失败，需要人工或自动补偿。例如多次重试仍失败、外部依赖导致当前请求失败等。
    GBLOGLEVEL_FATAL = 5,      // 不可恢复错误，通常表示进程必须退出或重启，例如核心配置缺失、严重一致性破坏等。
    GBLOGLEVEL_DISABLELOG = 6  // 禁止普通输出日志的特殊级别；不建议作为单条日志级别写入。
};

/**
 * @brief 日志配置选项。
 *
 * @details
 *  本结构体只保存运行期内存配置，不读取注册表、不读取外部配置文件。
 *  所有字符串均约定为 UTF-8 编码的 std::string。
 */
struct GB_LoggerOptions
{
    bool isLogEnabled = false;                                      // 是否启用日志记录。默认 false；为 false 时不会入队，也不会写入 GBLog_all.log。
    bool isLogToConsole = false;                                    // 是否同时输出到 Console。默认 false；Console 输出受 filterLevel 控制。
    GB_LogLevel filterLevel = GB_LogLevel::GBLOGLEVEL_WARNING;      // 普通日志 GBLog.log 与 Console 的筛选级别。默认 WARNING；GBLog_all.log 不受该级别控制。
    std::string logDirectoryUtf8;                                   // 日志文件存放目录，UTF-8。默认当前 exe 所在目录；设置相对路径时按当前 exe 所在目录拼接。
    std::string logFileNameUtf8 = "GBLog";                          // 日志文件基础名，UTF-8，不包含扩展名。默认 GBLog，对应 GBLog.log 与 GBLog_all.log。
    bool isOutputThreadId = true;                                   // 是否输出线程 ID。默认 true。
    bool isOutputSourceLocation = true;                             // 是否输出源文件名与行号。默认 true。
    std::size_t maxPendingLogItems = 200000;                        // 最大待写日志条数。默认 200000；0 表示不限制队列长度；超过后丢弃新日志并记录丢弃数量，防止内存无限增长。
};

/**
 * @brief 单条日志项。
 *
 * @details
 *  该结构体主要由 GB_Logger 内部使用，也保留公开类型，便于调用方在需要时复用格式化逻辑。
 */
struct GB_LogItem
{
    uint64_t sequenceNumber = 0;                 // 日志序号。由 GB_Logger 分配，用于 Flush 等待；普通调用方无需设置。
    std::string timestampUtf8;                   // 日志时间戳，UTF-8，通常形如 YYYY-MM-DDTHH:MM:SS.mmm。
    GB_LogLevel level = GB_LogLevel::GBLOGLEVEL_INFO; // 日志级别。
    std::string messageUtf8;                     // 日志正文，UTF-8。
    std::string threadIdUtf8;                    // 线程 ID 字符串，UTF-8；当不输出线程 ID 时可以为空。
    std::string fileNameUtf8;                    // 源文件名，UTF-8；当不输出源文件位置时可以为空。
    int line = 0;                                // 源文件行号；当不输出源文件位置时可以为 0。
    bool isOutputThreadId = true;                // 格式化时是否输出线程 ID。
    bool isOutputSourceLocation = true;          // 格式化时是否输出源文件名与行号。
    bool isOutputToFilteredTargets = true;       // 是否输出到 GBLog.log 和 Console；GBLog_all.log 始终记录已入队日志。

    /**
     * @brief 追加为 JSON Lines 格式的一行文本。
     * @param outUtf8 输出字符串，函数会在末尾追加内容。
     */
    void AppendJsonTo(std::string& outUtf8) const;

    /**
     * @brief 追加为普通可读文本的一行日志。
     * @param outUtf8 输出字符串，函数会在末尾追加内容。
     */
    void AppendPlainTextTo(std::string& outUtf8) const;

    /**
     * @brief 返回 JSON Lines 格式的一行文本。
     * @return std::string UTF-8 编码的 JSON 文本，末尾包含换行符。
     */
    std::string ToJsonString() const;

    /**
     * @brief 返回普通可读文本的一行日志。
     * @return std::string UTF-8 编码的普通日志文本，末尾包含换行符。
     */
    std::string ToPlainTextString() const;
};

/**
 * @brief 将日志级别转换为 UTF-8 文本。
 * @param level 日志级别。
 * @return std::string TRACE/DEBUG/INFO/WARNING/ERROR/FATAL/DISABLELOG/UNKNOWN。
 */
GLOBALBASE_PORT std::string LogLevelToString(GB_LogLevel level);

/**
 * @brief 将 UTF-8 文本解析为日志级别。
 * @param levelTextUtf8 日志级别文本，支持 TRACE/DEBUG/INFO/WARNING/WARN/ERROR/FATAL/DISABLELOG/OFF 以及 0~6。
 * @param outLevel 输出日志级别；解析失败时保持原值不变。
 * @return true 解析成功；false 解析失败。
 */
GLOBALBASE_PORT bool GB_ParseLogLevel(const std::string& levelTextUtf8, GB_LogLevel& outLevel);

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief GlobalBase 异步日志器。
 *
 * @details
 *  - 只使用接口配置，不读取注册表，不依赖 GB_Config。
 *  - 默认不启用日志；启用后调用线程只负责快速构造日志项并尝试入队，文件 I/O 由后台线程完成。
 *  - 采用有界队列保护内存；当队列达到上限时会丢弃新日志并累计丢弃数量，避免内存无限增长；调用线程不会等待磁盘 I/O。
 *  - 默认生成两个文件：
 *      1. GBLog_all.log：JSON Lines 格式，记录所有已经成功入队的日志，不受 filterLevel 控制；
 *      2. GBLog.log：普通文本格式，只记录满足 filterLevel 的日志。
 */
class GLOBALBASE_PORT GB_Logger
{
public:
    /**
     * @brief 获取日志器单例。
     * @return GB_Logger& 日志器单例引用。
     */
    static GB_Logger& GetInstance();

    /**
     * @brief 写入一条日志。
     *
     * @param level 日志级别；GBLOGLEVEL_DISABLELOG 会被忽略。
     * @param msgUtf8 日志正文，UTF-8。
     * @param fileUtf8 源文件路径或文件名，UTF-8；内部只保留文件名部分。
     * @param line 源文件行号。
     *
     * @remarks
     *  该函数不按 filterLevel 拒绝日志；只要日志开关已启用，日志就会进入 GBLog_all.log。
     *  filterLevel 只决定该日志是否进入 GBLog.log 与 Console。
     */
    void Log(GB_LogLevel level, const std::string& msgUtf8, const std::string& fileUtf8, int line);

    /**
     * @brief 写入一条日志，file 使用 __FILE__ 传入的窄字符串。
     *
     * @param level 日志级别；GBLOGLEVEL_DISABLELOG 会被忽略。
     * @param msgUtf8 日志正文，UTF-8。
     * @param file 源文件路径或文件名，通常传入 __FILE__；Windows 下会尽量识别并转为 UTF-8。
     * @param line 源文件行号。
     */
    void LogChecked(GB_LogLevel level, const std::string& msgUtf8, const char* file, int line);

    /** @brief 写入 TRACE 日志。参数含义同 LogChecked。 */
    void LogTrace(const std::string& msgUtf8, const char* file, int line);

    /** @brief 写入 DEBUG 日志。参数含义同 LogChecked。 */
    void LogDebug(const std::string& msgUtf8, const char* file, int line);

    /** @brief 写入 INFO 日志。参数含义同 LogChecked。 */
    void LogInfo(const std::string& msgUtf8, const char* file, int line);

    /** @brief 写入 WARNING 日志。参数含义同 LogChecked。 */
    void LogWarning(const std::string& msgUtf8, const char* file, int line);

    /** @brief 写入 ERROR 日志。参数含义同 LogChecked。 */
    void LogError(const std::string& msgUtf8, const char* file, int line);

    /** @brief 写入 FATAL 日志。参数含义同 LogChecked。 */
    void LogFatal(const std::string& msgUtf8, const char* file, int line);

    /**
     * @brief 启用或关闭日志记录。
     * @param enable true 启用；false 关闭。
     * @return true 设置成功；false 日志器已经 Shutdown 或后台线程启动失败。
     */
    bool SetLogEnabled(bool enable);

    /**
     * @brief 查询日志记录是否启用。
     * @return true 已启用；false 未启用。
     */
    bool IsLogEnabled() const;

    /**
     * @brief 设置是否输出到 Console。
     * @param enable true 输出到 Console；false 不输出到 Console。
     * @return true 设置成功。
     */
    bool SetLogToConsole(bool enable);

    /**
     * @brief 查询是否输出到 Console。
     * @return true 输出到 Console；false 不输出到 Console。
     */
    bool IsLogToConsole() const;

    /**
     * @brief 设置普通日志与 Console 的筛选级别。
     * @param level 筛选级别；GBLOGLEVEL_DISABLELOG 表示不输出到普通日志与 Console。
     * @return true 设置成功；false level 非法。
     */
    bool SetLogFilterLevel(GB_LogLevel level);

    /**
     * @brief 获取普通日志与 Console 的筛选级别。
     * @return GB_LogLevel 当前筛选级别。
     */
    GB_LogLevel GetLogFilterLevel() const;

    /**
     * @brief 设置日志文件存放目录。
     *
     * @param directoryUtf8 目录路径，UTF-8。为空时恢复为当前 exe 所在目录；相对路径按当前 exe 所在目录拼接。
     * @return true 设置成功；false 目录创建失败。
     */
    bool SetLogDirectory(const std::string& directoryUtf8);

    /**
     * @brief 获取日志文件存放目录。
     * @return std::string UTF-8 编码的目录路径，末尾带 '/'。
     */
    std::string GetLogDirectory() const;

    /**
     * @brief 设置日志文件基础名。
     *
     * @param fileNameUtf8 文件基础名，UTF-8。不应包含目录分隔符；若以 .log 结尾会自动去掉该扩展名。
     * @return true 设置成功；false 文件名为空或包含非法字符。
     */
    bool SetLogFileName(const std::string& fileNameUtf8);

    /**
     * @brief 获取日志文件基础名。
     * @return std::string UTF-8 编码的文件基础名，不含扩展名。
     */
    std::string GetLogFileName() const;

    /**
     * @brief 设置日志是否输出线程 ID。
     * @param enable true 输出；false 不输出。
     * @return true 设置成功。
     */
    bool SetOutputThreadId(bool enable);

    /**
     * @brief 查询日志是否输出线程 ID。
     * @return true 输出；false 不输出。
     */
    bool IsOutputThreadId() const;

    /**
     * @brief 设置日志是否输出源文件名和行号。
     * @param enable true 输出；false 不输出。
     * @return true 设置成功。
     */
    bool SetOutputSourceLocation(bool enable);

    /**
     * @brief 查询日志是否输出源文件名和行号。
     * @return true 输出；false 不输出。
     */
    bool IsOutputSourceLocation() const;

    /**
     * @brief 设置最大待写日志条数。
     *
     * @param maxPendingLogItems 最大待写条数；0 表示不限制队列长度。
     * @return true 设置成功。
     */
    bool SetMaxPendingLogItems(std::size_t maxPendingLogItems);

    /**
     * @brief 获取最大待写日志条数。
     * @return std::size_t 当前最大待写条数。
     */
    std::size_t GetMaxPendingLogItems() const;

    /**
     * @brief 获取当前完整配置快照。
     * @return GB_LoggerOptions 当前配置。
     */
    GB_LoggerOptions GetOptions() const;

    /**
     * @brief 批量设置配置。
     * @param options 配置选项。logDirectoryUtf8 为空表示当前 exe 所在目录。
     * @return true 设置成功；false 文件名非法、目录创建失败或后台线程启动失败。
     */
    bool SetOptions(const GB_LoggerOptions& options);

    /**
     * @brief 获取普通输出日志文件路径。
     * @return std::string UTF-8 路径，例如 .../GBLog.log。
     */
    std::string GetOutputLogFilePath() const;

    /**
     * @brief 获取全量日志文件路径。
     * @return std::string UTF-8 路径，例如 .../GBLog_all.log。
     */
    std::string GetAllLogFilePath() const;

    /**
     * @brief 清空普通输出日志文件 GBLog.log。
     * @return true 清空成功；false 写文件失败。
     */
    bool ClearOutputLogFile();

    /**
     * @brief 清空全量日志文件 GBLog_all.log。
     * @return true 清空成功；false 写文件失败。
     */
    bool ClearAllLogFile();

    /**
     * @brief 清空当前两个日志文件 GBLog.log 与 GBLog_all.log。
     * @return true 清空成功；false 任意一个文件清空失败。
     */
    bool ClearLogFiles();

    /**
     * @brief 等待当前已经入队或已经记录为“丢弃事件”的日志被后台线程处理完成。
     * @return true 处理完成；false 日志线程未启动、已经停止或异常退出。
     *
     * @note
     *  本函数会等待后台线程完成当前序号之前的处理流程。若底层文件写入失败，后台线程仍会推进处理序号，
     *  调用方可通过 GetWriteFailureCount() 查询累计写入失败次数。
     */
    bool Flush();

    /**
     * @brief 在指定超时时间内等待当前已经入队或已经记录为“丢弃事件”的日志被后台线程处理完成。
     * @param timeoutMs 超时时间，毫秒；0 表示只做一次即时状态检查。
     * @return true 处理完成；false 超时、日志线程未启动、已经停止或异常退出。
     *
     * @note
     *  本函数用于生命周期边界或测试场景下的确定性等待；不建议在高频业务路径中调用。
     */
    bool FlushFor(unsigned int timeoutMs);

    /**
     * @brief 获取累计丢弃的日志条数。
     * @return uint64_t 自进程启动以来累计丢弃的日志条数。
     */
    uint64_t GetDroppedLogCount() const;

    /**
     * @brief 获取后台文件写入失败累计次数。
     * @return uint64_t 自进程启动以来后台写入 GBLog.log 或 GBLog_all.log 失败的批次数。
     *
     * @remarks
     *  该计数是“批次级”失败计数，不等同于丢失的日志条数。失败原因通常包括磁盘满、路径权限不足、
     *  文件被外部进程独占锁定或目标目录被删除等。
     */
    uint64_t GetWriteFailureCount() const;

    /**
     * @brief 主动停止后台日志线程并尽量写完队列中的日志。
     *
     * @remarks
     *  正常情况下无需手动调用；内部会在进程退出时自动调用一次。
     *  若在 DLL 卸载、单元测试或特殊生命周期下需要提前回收资源，可以显式调用。
     *  调用 Shutdown 后，日志器不再接受新日志，也不再重新启动后台线程。
     */
    void Shutdown();

private:
    std::deque<GB_LogItem> logQueue;                    // 待写日志队列。
    mutable std::mutex logQueueMtx;                     // 日志队列互斥量；只保护内存队列，不在该锁内执行磁盘 I/O。
    std::condition_variable logQueueCv;                 // 日志线程等待条件变量。

    mutable std::mutex settingsMtx;                     // 字符串配置互斥量，保护日志目录与日志文件基础名。
    std::string logDirectoryUtf8;                       // 日志目录，UTF-8，末尾带 '/'。
    std::string logFileNameUtf8;                        // 日志文件基础名，UTF-8，不含扩展名。

    std::atomic_bool isLogEnabled{ false };             // 是否启用日志记录。
    std::atomic_bool isLogToConsole{ false };           // 是否输出到 Console。
    std::atomic<int> filterLevelInt{ static_cast<int>(GB_LogLevel::GBLOGLEVEL_WARNING) }; // 普通日志筛选级别。
    std::atomic_bool isOutputThreadId{ true };          // 是否输出线程 ID。
    std::atomic_bool isOutputSourceLocation{ true };    // 是否输出源文件名和行号。
    std::atomic<std::size_t> maxPendingLogItems{ 200000 }; // 最大待写日志条数；0 表示不限制队列长度。

    std::atomic_bool isStop{ false };                   // 日志线程停止标志。
    std::atomic_bool hasShutdown{ false };              // 是否已经 Shutdown。
    std::atomic_bool isWorkerStarted{ false };          // 后台线程是否已经启动。
    std::atomic_bool isWorkerRunning{ false };          // 后台线程当前是否处于运行状态。
    mutable std::mutex workerMtx;                       // 后台线程启动/停止互斥量。
    std::thread logThread;                              // 后台日志线程。

    std::atomic<uint64_t> nextSequenceNumber{ 0 };      // 下一个日志序号。
    std::atomic<uint64_t> completedSequenceNumber{ 0 }; // 已完成写入的最大日志序号。
    mutable std::mutex flushMtx;                        // Flush 等待互斥量。
    std::condition_variable flushCv;                    // Flush 等待条件变量。

    mutable std::mutex fileIoMtx;                       // 文件清空与后台写文件之间的互斥量。
    uint64_t pendingDroppedLogCount = 0;                // 尚未写入提示日志的丢弃条数；只在持有 logQueueMtx 时读写，避免条件变量漏唤醒。
    uint64_t pendingDroppedSequenceNumber = 0;          // 尚未写入提示日志的最大丢弃事件序号；只在持有 logQueueMtx 时读写。
    std::atomic<uint64_t> totalDroppedLogCount{ 0 };    // 累计丢弃日志条数。
    std::atomic<uint64_t> totalWriteFailureCount{ 0 };  // 后台文件写入失败批次数。

    GB_Logger();
    ~GB_Logger();
    GB_Logger(const GB_Logger&) = delete;
    GB_Logger& operator=(const GB_Logger&) = delete;

    bool EnsureWorkerStarted();
    bool EnqueueLogItem(GB_LogItem&& logItem);
    void RecordDroppedLogItemLocked();
    bool TakePendingDroppedLogItemsLocked(uint64_t& droppedCount, uint64_t& droppedSequenceNumber);
    bool IsCurrentWorkerThread() const;
    GB_LogItem BuildLogItem(GB_LogLevel level, const std::string& msgUtf8, const std::string& fileUtf8, int line);
    bool ShouldWriteToFilteredTargets(GB_LogLevel level) const;
    void LogThreadFunc();
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

/**
 * @brief 写入一条 TRACE 级别日志。
 *
 * @details
 *  宏内部自动携带调用点的 __FILE__ 与 __LINE__，用于输出源文件名和行号。
 *  当日志总开关关闭时，@p msg 不会被求值；当日志总开关开启时，日志会进入异步队列，
 *  并始终写入全量日志文件 `GBLog_all.log`。是否同时写入普通日志文件和 Console，
 *  由当前 filterLevel 决定。
 *
 * @note TRACE 级别适合临时排查疑难问题。高频循环中大量调用会产生显著日志量，生产环境应谨慎开启。
 */
#define GBLOG_TRACE(msg)   do { GB_Logger& gbLogger = GB_Logger::GetInstance(); if (gbLogger.IsLogEnabled()) { gbLogger.LogChecked(GB_LogLevel::GBLOGLEVEL_TRACE,   (msg), __FILE__, __LINE__); } } while (0)

 /**
  * @brief 写入一条 DEBUG 级别日志。
  *
  * @details
  *  宏自动携带 __FILE__ 与 __LINE__。日志总开关关闭时，@p msg 不会被求值；日志总开关开启时，
  *  该日志会写入全量日志文件，并在满足 filterLevel 时写入普通日志文件和 Console。
  */
#define GBLOG_DEBUG(msg)   do { GB_Logger& gbLogger = GB_Logger::GetInstance(); if (gbLogger.IsLogEnabled()) { gbLogger.LogChecked(GB_LogLevel::GBLOGLEVEL_DEBUG,   (msg), __FILE__, __LINE__); } } while (0)

  /**
   * @brief 写入一条 INFO 级别日志。
   *
   * @details
   *  适合记录业务里程碑、模块启动/停止、配置加载结果、批处理完成等正常运转事实。
   *  日志总开关关闭时，@p msg 不会被求值；日志总开关开启时，日志会进入异步队列。
   */
#define GBLOG_INFO(msg)    do { GB_Logger& gbLogger = GB_Logger::GetInstance(); if (gbLogger.IsLogEnabled()) { gbLogger.LogChecked(GB_LogLevel::GBLOGLEVEL_INFO,    (msg), __FILE__, __LINE__); } } while (0)

   /**
    * @brief 写入一条 WARNING 级别日志。
    *
    * @details
    *  适合记录可自愈异常、降级、重试成功、使用默认配置、资源接近阈值等需要关注但通常不打断流程的问题。
    */
#define GBLOG_WARNING(msg) do { GB_Logger& gbLogger = GB_Logger::GetInstance(); if (gbLogger.IsLogEnabled()) { gbLogger.LogChecked(GB_LogLevel::GBLOGLEVEL_WARNING, (msg), __FILE__, __LINE__); } } while (0)

    /**
     * @brief 写入一条 ERROR 级别日志。
     *
     * @details
     *  适合记录当前操作已经失败、需要补偿或人工排查的问题。Console 输出开启时，ERROR 会输出到 stderr。
     */
#define GBLOG_ERROR(msg)   do { GB_Logger& gbLogger = GB_Logger::GetInstance(); if (gbLogger.IsLogEnabled()) { gbLogger.LogChecked(GB_LogLevel::GBLOGLEVEL_ERROR,   (msg), __FILE__, __LINE__); } } while (0)

     /**
      * @brief 写入一条 FATAL 级别日志。
      *
      * @details
      *  适合记录不可恢复错误。该宏只负责记录日志，不会主动 abort、exit 或抛异常；调用方应自行决定进程退出策略。
      *  Console 输出开启时，FATAL 会输出到 stderr。
      */
#define GBLOG_FATAL(msg)   do { GB_Logger& gbLogger = GB_Logger::GetInstance(); if (gbLogger.IsLogEnabled()) { gbLogger.LogChecked(GB_LogLevel::GBLOGLEVEL_FATAL,   (msg), __FILE__, __LINE__); } } while (0)

      /**
       * @brief 查询全局日志总开关是否启用。
       * @return true 已启用；false 未启用。
       */
GLOBALBASE_PORT bool GB_IsLogEnabled();

/**
 * @brief 设置全局日志总开关。
 *
 * @param enable true 启用日志；false 关闭日志。
 * @return true 设置成功；false 日志器已经 Shutdown 或后台线程启动失败。
 *
 * @note 启用时会按需启动后台日志线程；关闭时不会销毁后台线程，只是不再接受普通日志。
 */
GLOBALBASE_PORT bool GB_SetLogEnabled(bool enable);

/**
 * @brief 查询满足 filterLevel 的日志是否同步输出到 Console。
 * @return true 输出到 Console；false 不输出到 Console。
 */
GLOBALBASE_PORT bool GB_IsLogToConsole();

/**
 * @brief 设置是否把满足 filterLevel 的日志同步输出到 Console。
 *
 * @param enable true 输出到 Console；false 只写文件。
 * @return true 设置成功。
 */
GLOBALBASE_PORT bool GB_SetLogToConsole(bool enable);

/**
 * @brief 获取普通日志文件和 Console 的筛选级别。
 * @return GB_LogLevel 当前筛选级别；GBLOGLEVEL_DISABLELOG 表示关闭普通日志文件与 Console 输出。
 *
 * @note 全量日志文件不受该级别控制；只要日志总开关开启，成功入队的日志都会写入全量日志文件。
 */
GLOBALBASE_PORT GB_LogLevel GB_GetLogFilterLevel();

/**
 * @brief 设置普通日志文件和 Console 的筛选级别。
 *
 * @param level 筛选级别。低于该级别的日志只进入全量日志文件，不进入普通日志文件和 Console。
 * @return true 设置成功；false level 不在 GB_LogLevel 的有效范围内。
 */
GLOBALBASE_PORT bool GB_SetLogFilterLevel(GB_LogLevel level);

/**
 * @brief 快速检查某个级别的日志是否会进入普通日志文件或 Console。
 *
 * @param level 待检查的日志级别。
 * @return true 日志总开关已开启，且 level 为真实日志级别并满足当前 filterLevel；false 否则。
 *
 * @remarks
 *  该函数主要用于调用方自行保护昂贵日志构造逻辑，例如：
 *  @code
 *  if (GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_DEBUG))
 *  {
 *      GBLOG_DEBUG(BuildLargeDebugMessage());
 *  }
 *  @endcode
 *  注意：GBLOG_* 宏本身只检查日志总开关，不使用该函数过滤全量日志。
 */
GLOBALBASE_PORT bool GB_CheckLogLevel(GB_LogLevel level);

/**
 * @brief 获取日志目录。
 * @return std::string UTF-8 编码的目录路径，末尾带 '/'。
 */
GLOBALBASE_PORT std::string GB_GetLogDirectory();

/**
 * @brief 设置日志目录。
 *
 * @param directoryUtf8 UTF-8 编码的目录路径。为空表示恢复为当前 exe 所在目录；相对路径按当前 exe 所在目录拼接。
 * @return true 设置成功；false 目录创建失败。
 *
 * @note 设置前会先 Flush 已入队日志，尽量避免同一批日志跨目录写入。
 */
GLOBALBASE_PORT bool GB_SetLogDirectory(const std::string& directoryUtf8);

/**
 * @brief 获取日志文件基础名。
 * @return std::string UTF-8 编码的文件基础名，不包含扩展名。
 */
GLOBALBASE_PORT std::string GB_GetLogFileName();

/**
 * @brief 设置日志文件基础名。
 *
 * @param fileNameUtf8 UTF-8 编码的文件基础名，不应包含目录分隔符；若以 `.log` 结尾会自动去掉扩展名。
 * @return true 设置成功；false 文件名为空、包含非法字符或为 Windows 保留设备名。
 */
GLOBALBASE_PORT bool GB_SetLogFileName(const std::string& fileNameUtf8);

/**
 * @brief 查询普通文本日志和 JSON 全量日志中是否输出线程 ID。
 * @return true 输出线程 ID；false 不输出线程 ID。
 */
GLOBALBASE_PORT bool GB_IsLogOutputThreadId();

/**
 * @brief 设置普通文本日志和 JSON 全量日志中是否输出线程 ID。
 * @param enable true 输出；false 不输出。
 * @return true 设置成功。
 */
GLOBALBASE_PORT bool GB_SetLogOutputThreadId(bool enable);

/**
 * @brief 查询普通文本日志和 JSON 全量日志中是否输出源文件名与行号。
 * @return true 输出源文件名与行号；false 不输出。
 */
GLOBALBASE_PORT bool GB_IsLogOutputSourceLocation();

/**
 * @brief 设置普通文本日志和 JSON 全量日志中是否输出源文件名与行号。
 * @param enable true 输出；false 不输出。
 * @return true 设置成功。
 */
GLOBALBASE_PORT bool GB_SetLogOutputSourceLocation(bool enable);

/**
 * @brief 获取最大待写日志条数。
 * @return std::size_t 当前队列上限；0 表示不限制。
 */
GLOBALBASE_PORT std::size_t GB_GetMaxPendingLogItems();

/**
 * @brief 设置最大待写日志条数。
 *
 * @param maxPendingLogItems 最大待写条数；0 表示不限制。
 * @return true 设置成功。
 *
 * @warning 设置为 0 时，极端产生日志速度高于磁盘写入速度的场景可能导致内存持续增长。
 */
GLOBALBASE_PORT bool GB_SetMaxPendingLogItems(std::size_t maxPendingLogItems);

/**
 * @brief 获取日志器完整配置快照。
 * @return GB_LoggerOptions 当前配置快照。
 */
GLOBALBASE_PORT GB_LoggerOptions GB_GetLoggerOptions();

/**
 * @brief 批量设置日志器配置。
 *
 * @param options 配置选项。logDirectoryUtf8 为空表示当前 exe 所在目录；logFileNameUtf8 为空表示使用默认 `GBLog`。
 * @return true 设置成功；false 参数非法、目录创建失败或后台线程启动失败。
 */
GLOBALBASE_PORT bool GB_SetLoggerOptions(const GB_LoggerOptions& options);

/**
 * @brief 获取普通文本日志文件路径。
 * @return std::string UTF-8 编码路径，通常形如 `.../GBLog.log`。
 */
GLOBALBASE_PORT std::string GB_GetOutputLogFilePath();

/**
 * @brief 获取全量 JSON Lines 日志文件路径。
 * @return std::string UTF-8 编码路径，通常形如 `.../GBLog_all.log`。
 */
GLOBALBASE_PORT std::string GB_GetAllLogFilePath();

/**
 * @brief 清空普通文本日志文件。
 * @return true 清空成功；false Flush 失败或文件截断失败。
 */
GLOBALBASE_PORT bool GB_ClearOutputLogFile();

/**
 * @brief 清空全量 JSON Lines 日志文件。
 * @return true 清空成功；false Flush 失败或文件截断失败。
 */
GLOBALBASE_PORT bool GB_ClearAllLogFile();

/**
 * @brief 清空当前两个日志文件。
 * @return true 两个文件均清空成功；false 任意一个文件清空失败。
 */
GLOBALBASE_PORT bool GB_ClearLogFiles();

/**
 * @brief 等待当前已经入队或已经记录为“丢弃事件”的日志被后台线程处理完成。
 * @return true 处理完成；false 后台线程未启动、已经停止或等待过程中发现无法完成。
 *
 * @note
 *  后台线程会对当前批次执行写入和刷新流程；若底层文件写入失败，本函数仍可能返回 true，
 *  调用方可通过 GB_GetLogWriteFailureCount() 查询累计失败批次数。
 */
GLOBALBASE_PORT bool GB_FlushLog();

/**
 * @brief 在指定超时时间内等待当前已经入队或已经记录为“丢弃事件”的日志被后台线程处理完成。
 *
 * @param timeoutMs 超时时间，毫秒。0 表示只做一次即时状态检查。
 * @return true 处理完成；false 超时、后台线程未启动或已经停止。
 */
GLOBALBASE_PORT bool GB_FlushLogFor(unsigned int timeoutMs);

/**
 * @brief 获取累计被丢弃的日志条数。
 * @return uint64_t 自进程启动以来由于队列达到上限而丢弃的新日志条数。
 */
GLOBALBASE_PORT uint64_t GB_GetDroppedLogCount();

/**
 * @brief 获取后台文件写入失败累计次数。
 * @return uint64_t 自进程启动以来后台写入普通日志或全量日志失败的批次数。
 *
 * @remarks
 *  该函数适合用于健康检查或单元测试。失败计数大于 0 时，通常需要检查日志目录权限、磁盘空间、
 *  文件是否被外部程序独占打开，以及路径是否仍然存在。
 */
GLOBALBASE_PORT uint64_t GB_GetLogWriteFailureCount();

/**
 * @brief 关闭日志器并停止后台日志线程。
 *
 * @details
 *  调用后日志器不再接受新日志，也不会重新启动后台线程。该函数可重复调用。
 *  常规 EXE 通常不需要手动调用；DLL 卸载、单元测试和插件生命周期明确的场景建议主动调用。
 */
GLOBALBASE_PORT void GB_ShutdownLogger();

#endif
