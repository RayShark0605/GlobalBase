#ifndef GLOBALBASE_LOGGER_H
#define GLOBALBASE_LOGGER_H

#include "GB_Utility.h"
#include "GB_Utf8String.h"
#include "GlobalBasePort.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// 可自愈或已回退=WARNING；关键业务事件=INFO；实现细节=DEBUG；逐步跟踪=TRACE
enum class GB_LogLevel : int
{
    GBLOGLEVEL_TRACE = 0, // 定位疑难杂症时临时开启，记录循环内部变量、函数入参/出参、分支路径、重试细节等。生产环境默认关闭，避免海量日志噪声与成本
    GBLOGLEVEL_DEBUG = 1, // 调试信息。仅在问题定位窗口开启。记录关键状态变更、外部调用请求与响应摘要（脱敏）、缓存命中/失效、重要分支选择等
    GBLOGLEVEL_INFO = 2, // 业务里程碑与正常运转的“事实，记录服务启动/停止、配置加载结果、计划任务开始/结束、订单创建成功、批处理完成等，让运营/排障能还原事件时间线
    GBLOGLEVEL_WARNING = 3, // 潜在问题或可自愈的异常。重试后成功、超时但已回退、使用了默认配置、资源接近阈值（磁盘 80%/连接池将满）等。需要关注，但通常不打断用户请求。
    GBLOGLEVEL_ERROR = 4, // 操作已失败，需要人工或自动补偿。数据库写入多次重试仍失败、不可用的外部依赖导致当前请求失败、数据校验失败使任务无法继续等。
    GBLOGLEVEL_FATAL = 5, // 不可恢复错误，进程必须立即退出重启。关键配置缺失且无法启动、核心数据结构损坏、严重一致性/安全性破坏。
    GBLOGLEVEL_DISABLELOG = 6
};

struct GB_LogItem
{
    std::string timestamp; // 时间戳
    GB_LogLevel level; // 日志级别
    std::string message; // 日志消息
    std::string threadId; // 线程 ID
    std::string file; // 文件名
    int line; // 行号

    void AppendJsonTo(std::string& out) const;
    void AppendPlainTextTo(std::string& out) const;

    std::string ToJsonString() const;
    std::string ToPlainTextString() const;
};

std::string LogLevelToString(GB_LogLevel level);

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

class GLOBALBASE_PORT GB_Logger
{
public:
    static GB_Logger& GetInstance();

    // 完整检查（包含读取配置的级别过滤），并写入队列。
    // fileUtf8 要求已经是 UTF-8，且路径分隔符建议使用 '/'
    void Log(GB_LogLevel level, const std::string& msgUtf8, const std::string& fileUtf8, int line);

    // 已通过 GB_CheckLogLevel(level) 的日志写入（避免重复读取配置）。
    // file 使用 __FILE__ 传入的窄字符串，内部会尽可能转换/规范化为 UTF-8。
    void LogChecked(GB_LogLevel level, const std::string& msgUtf8, const char* file, int line);

    void LogTrace(const std::string& msgUtf8, const char* file, int line);
    void LogDebug(const std::string& msgUtf8, const char* file, int line);
    void LogInfo(const std::string& msgUtf8, const char* file, int line);
    void LogWarning(const std::string& msgUtf8, const char* file, int line);
    void LogError(const std::string& msgUtf8, const char* file, int line);
    void LogFatal(const std::string& msgUtf8, const char* file, int line);

    bool ClearLogFiles() const; // 清空日志文件（将 GB_AllLog.log 和 GB_OutputLog.log 截断为 0 字节）

    // 主动停止后台线程并清理队列。可重复调用，线程安全。
    // - 正常情况下无需手动调用：内部会在进程退出时自动调用一次；
    // - 若你需要在 DLL 卸载、单元测试或特殊生命周期下提前回收资源，可显式调用。
    void Shutdown();

private:
    std::queue<GB_LogItem> logQueue; // 日志队列
    std::mutex logQueueMtx;
    std::condition_variable logQueueCv;

    std::atomic_bool isStop{ false };
    std::atomic_bool hasShutdown{ false };
    std::thread logThread;

    GB_Logger();
    ~GB_Logger();
    GB_Logger(const GB_Logger&) = delete;
    GB_Logger& operator=(const GB_Logger&) = delete;

    void LogThreadFunc(); // 日志处理线程函数
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#define GBLOG_TRACE(msg)   do { if (GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_TRACE))   { GB_Logger::GetInstance().LogChecked(GB_LogLevel::GBLOGLEVEL_TRACE,   (msg), __FILE__, __LINE__); } } while (0)
#define GBLOG_DEBUG(msg)   do { if (GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_DEBUG))   { GB_Logger::GetInstance().LogChecked(GB_LogLevel::GBLOGLEVEL_DEBUG,   (msg), __FILE__, __LINE__); } } while (0)
#define GBLOG_INFO(msg)    do { if (GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_INFO))    { GB_Logger::GetInstance().LogChecked(GB_LogLevel::GBLOGLEVEL_INFO,    (msg), __FILE__, __LINE__); } } while (0)
#define GBLOG_WARNING(msg) do { if (GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_WARNING)) { GB_Logger::GetInstance().LogChecked(GB_LogLevel::GBLOGLEVEL_WARNING, (msg), __FILE__, __LINE__); } } while (0)
#define GBLOG_ERROR(msg)   do { if (GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_ERROR))   { GB_Logger::GetInstance().LogChecked(GB_LogLevel::GBLOGLEVEL_ERROR,   (msg), __FILE__, __LINE__); } } while (0)
#define GBLOG_FATAL(msg)   do { if (GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_FATAL))   { GB_Logger::GetInstance().LogChecked(GB_LogLevel::GBLOGLEVEL_FATAL,   (msg), __FILE__, __LINE__); } } while (0)

GLOBALBASE_PORT bool GB_IsLogEnabled();
GLOBALBASE_PORT bool GB_SetLogEnabled(bool enable);

GLOBALBASE_PORT bool GB_IsLogToConsole();
GLOBALBASE_PORT bool GB_SetLogToConsole(bool enable);

GLOBALBASE_PORT GB_LogLevel GB_GetLogFilterLevel();
GLOBALBASE_PORT bool GB_CheckLogLevel(GB_LogLevel level);

GLOBALBASE_PORT void GB_InstallCrashHandlers();
GLOBALBASE_PORT void GB_RemoveCrashHandlers();

#endif
