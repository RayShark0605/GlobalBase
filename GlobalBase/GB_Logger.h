#ifndef GLOBALBASE_LOGGER_H_H
#define GLOBALBASE_LOGGER_H_H

#include "GB_Utility.h"
#include "GB_Utf8String.h"
#include "GlobalBasePort.h"
#include <mutex>
#include <condition_variable>
#include <atomic>

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

    void Log(GB_LogLevel level, const std::string& msgUtf8, const std::string& fileUtf8, int line);
    void LogTrace(const std::string& msgUtf8, const char* file, int line);
    void LogDebug(const std::string& msgUtf8, const char* file, int line);
    void LogInfo(const std::string& msgUtf8, const char* file, int line);
    void LogWarning(const std::string& msgUtf8, const char* file, int line);
    void LogError(const std::string& msgUtf8, const char* file, int line);
    void LogFatal(const std::string& msgUtf8, const char* file, int line);

    bool ClearLogFiles() const;

private:
    std::queue<GB_LogItem> logQueue; // 日志队列
    std::mutex logQueueMtx;
    std::condition_variable logQueueCv;

    std::atomic_bool isStop{ false };
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

#define GBLOG_TRACE(msg) do { GB_Logger::GetInstance().LogTrace(msg, __FILE__, __LINE__); } while (0)
#define GBLOG_DEBUG(msg) do { GB_Logger::GetInstance().LogDebug(msg, __FILE__, __LINE__); } while (0)
#define GBLOG_INFO(msg) do { GB_Logger::GetInstance().LogInfo(msg, __FILE__, __LINE__); } while (0)
#define GBLOG_WARNING(msg) do { GB_Logger::GetInstance().LogWarning(msg, __FILE__, __LINE__); } while (0)
#define GBLOG_ERROR(msg) do { GB_Logger::GetInstance().LogError(msg, __FILE__, __LINE__); } while (0)
#define GBLOG_FATAL(msg) do { GB_Logger::GetInstance().LogFatal(msg, __FILE__, __LINE__); } while (0)

GLOBALBASE_PORT bool GB_IsLogEnabled();
GLOBALBASE_PORT bool GB_SetLogEnabled(bool enable);

GLOBALBASE_PORT bool GB_IsLogToConsole();
GLOBALBASE_PORT bool GB_SetLogToConsole(bool enable);

GLOBALBASE_PORT GB_LogLevel GB_GetLogFilterLevel();
GLOBALBASE_PORT bool GB_CheckLogLevel(GB_LogLevel level);

GLOBALBASE_PORT void GB_InstallCrashHandlers();
GLOBALBASE_PORT void GB_RemoveCrashHandlers();

#endif