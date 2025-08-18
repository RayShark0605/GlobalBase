#ifndef GLOBALBASE_LOGGER_H_H
#define GLOBALBASE_LOGGER_H_H

#include "GB_Utility.h"
#include "GB_Utf8String.h"
#include "GlobalBasePort.h"
#include <mutex>
#include <condition_variable>
#include <atomic>

enum class GLOBALBASE_PORT GB_LogLevel : int
{
    GBLOG_TRACE = 0,
    GBLOG_DEBUG = 1,
    GBLOG_INFO = 2,
    GBLOG_WARNING = 3,
    GBLOG_ERROR = 4,
    GBLOG_FATAL = 5,
    GBLOG_DISABLELOG = 6
};

struct GLOBALBASE_PORT GB_LogItem
{
	std::string timestamp; // 时间戳
	GB_LogLevel level; // 日志级别
	std::string message; // 日志消息
	std::string threadId; // 线程 ID
	std::string file; // 文件名
	int line; // 行号
};


class GLOBALBASE_PORT GB_Logger
{
public:
    static GB_Logger& GetInstance();







private:
    std::queue<GB_LogItem> logQueue; // 日志队列
    std::mutex logQueueMtx; // 互斥锁保护日志队列
    std::condition_variable logQueueCv; // 用于通知日志处理线程

    std::atomic_bool isStop{ false }; // 停止标志
    std::thread logThread; // 日志处理线程

    GB_Logger();
    ~GB_Logger();
	GB_Logger(const GB_Logger&) = delete;
    GB_Logger& operator=(const GB_Logger&) = delete;

	void LogThreadFunc(); // 日志处理线程函数
};

GLOBALBASE_PORT bool IsLogEnabled();
GLOBALBASE_PORT bool SetLogEnabled(bool enable);

GLOBALBASE_PORT bool IsLogToConsole();
GLOBALBASE_PORT bool SetLogToConsole(bool enable);

GLOBALBASE_PORT GB_LogLevel GetLogFilterLevel();
GLOBALBASE_PORT bool CheckLogLevel(GB_LogLevel level);









#endif