#include "GB_Logger.h"
#include "GB_Config.h"
#include "GB_FileSystem.h"
#include "GB_IO.h"
#include "GB_Utility.h"
#include "GB_Timer.h"

#include <cstring>
#include <ctime>
#include <chrono>
#include <sstream>
#include <utility>
#include <iostream>
#include <exception>
#include <cstdlib>
#include <cstdint>

#if defined(_WIN32)
#  include <windows.h>
#  ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#    define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#  endif
#else
#  include <unistd.h> 
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <signal.h>
#  include <execinfo.h>
#endif
namespace internal
{
	static void AppendJsonEscaped(std::string& out, const std::string& s)
	{
		out.reserve(out.size() + s.size());
		for (size_t i = 0; i < s.size(); i++)
		{
			const unsigned char ch = static_cast<unsigned char>(s[i]);
			switch (ch)
			{
			case '\"': out += GB_STR("\\\""); break;
			case '\\': out += GB_STR("\\\\"); break;
			case '\b': out += GB_STR("\\b");  break;
			case '\f': out += GB_STR("\\f");  break;
			case '\n': out += GB_STR("\\n");  break;
			case '\r': out += GB_STR("\\r");  break;
			case '\t': out += GB_STR("\\t");  break;
			default:
				if (ch < 0x20)
				{
					static const char* hex = "0123456789ABCDEF";
					out += GB_STR("\\u00");
					out += hex[(ch >> 4) & 0xF];
					out += hex[ch & 0xF];
				}
				else
				{
					out += static_cast<char>(ch);
				}
				break;
			}
		}
	}

	// —— 颜色映射：VT/ANSI 转义（非“ANSI 编码”）——
	static const char* GetAnsiColorByLevel(GB_LogLevel level)
	{
		switch (level)
		{
		case GB_LogLevel::GBLOGLEVEL_TRACE:   return "\x1b[90m"; // 灰
		case GB_LogLevel::GBLOGLEVEL_DEBUG:   return "\x1b[36m"; // 青
		case GB_LogLevel::GBLOGLEVEL_INFO:    return "\x1b[32m"; // 绿
		case GB_LogLevel::GBLOGLEVEL_WARNING: return "\x1b[33m"; // 黄
		case GB_LogLevel::GBLOGLEVEL_ERROR:   return "\x1b[31m"; // 红
		case GB_LogLevel::GBLOGLEVEL_FATAL:   return "\x1b[35m"; // 品红
		default:                         return "\x1b[0m";
		}
	}

	static std::ostream& SelectStream(GB_LogLevel level)
	{
		return (level >= GB_LogLevel::GBLOGLEVEL_ERROR) ? std::cerr : std::cout;
	}

	static const std::string& GetThreadIdString()
	{
		thread_local std::string threadIdUtf8;
		if (threadIdUtf8.empty())
		{
			std::ostringstream oss;
			oss << std::this_thread::get_id();
			threadIdUtf8 = oss.str();
		}
		return threadIdUtf8;
	}

	static std::string NormalizeFilePathUtf8(const char* file)
	{
		if (!file)
		{
			return std::string();
		}

#if defined(_WIN32)
		// __FILE__ 在 MSVC 下可能是 UTF-8（/utf-8）或本地 ANSI。这里做一次轻量判定：
		// - 若看起来是合法 UTF-8：直接当 UTF-8 使用；
		// - 否则按系统 ANSI 转 UTF-8。
		std::string fileStrUtf8 = "";
		if (!GB_LooksLikeAnsi(file))
		{
			fileStrUtf8 = file;
		}
		else
		{
			fileStrUtf8 = GB_AnsiToUtf8(file);
		}
#else
		std::string fileStrUtf8 = file;
#endif
		fileStrUtf8 = GB_Utf8Replace(fileStrUtf8, GB_STR("\\"), GB_STR("/"));
		return fileStrUtf8;
	}

#if defined(_WIN32)
	struct WinConsoleState
	{
		bool vtEnabledOut = false;
		bool vtEnabledErr = false;
		WORD defaultAttrOut = 0;
		WORD defaultAttrErr = 0;
	};

	static WinConsoleState& GetWinConsoleState()
	{
		static WinConsoleState state;
		return state;
	}

	static WORD GetWinAttrByLevel(GB_LogLevel level)
	{
		switch (level)
		{
		case GB_LogLevel::GBLOGLEVEL_TRACE:   return FOREGROUND_INTENSITY;
		case GB_LogLevel::GBLOGLEVEL_DEBUG:   return FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
		case GB_LogLevel::GBLOGLEVEL_INFO:    return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
		case GB_LogLevel::GBLOGLEVEL_WARNING: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
		case GB_LogLevel::GBLOGLEVEL_ERROR:   return FOREGROUND_RED | FOREGROUND_INTENSITY;
		case GB_LogLevel::GBLOGLEVEL_FATAL:   return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
		default:                         return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
		}
	}

	static void EnableWinVtOnce()
	{
		static std::once_flag once;
		std::call_once(once, []()
			{
				WinConsoleState& st = GetWinConsoleState();

				const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
				const HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);

				// 记录默认属性（用于回退恢复），不改代码页、不做任何编码转换
				CONSOLE_SCREEN_BUFFER_INFO info;
				if (hOut && GetConsoleScreenBufferInfo(hOut, &info))
				{
					st.defaultAttrOut = info.wAttributes;
				}
				if (hErr && GetConsoleScreenBufferInfo(hErr, &info))
				{
					st.defaultAttrErr = info.wAttributes;
				}

				// 尝试开启 VT（虚拟终端处理）
				// 注意：VT 模式是“按句柄”生效的（stdout/stderr 可能不同）。
				DWORD mode = 0;
				if (hOut && GetConsoleMode(hOut, &mode))
				{
					const DWORD newMode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
					if (SetConsoleMode(hOut, newMode))
					{
						st.vtEnabledOut = true;
					}
				}
				if (hErr && GetConsoleMode(hErr, &mode))
				{
					const DWORD newMode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
					if (SetConsoleMode(hErr, newMode))
					{
						st.vtEnabledErr = true;
					}
				}
				// 参考：Windows Console Virtual Terminal sequences / SetConsoleMode。
			});
	}
#endif // _WIN32

	// 如果 Windows 支持 VT：发 VT 转义 + UTF-8 文本；否则回退 SetConsoleTextAttribute 着色后直接输出 UTF-8 文本。
	static void ConsoleWriteColoredUtf8(const std::string& textUtf8, GB_LogLevel level)
	{
		static std::mutex consoleMtx;
		std::lock_guard<std::mutex> lock(consoleMtx);

#if defined(_WIN32)
		EnableWinVtOnce();
		WinConsoleState& st = GetWinConsoleState();

		std::ostream& os = SelectStream(level);
		const bool toErr = (&os == &std::cerr);
		const bool vtEnabled = toErr ? st.vtEnabledErr : st.vtEnabledOut;
		const HANDLE h = GetStdHandle(toErr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);

		DWORD mode = 0;
		const bool isConsole = (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode));

		if (!isConsole)
		{
			// 重定向到文件/管道：不加色，不改编码，直接输出 UTF-8 字节。
			os << textUtf8;
			os.flush();
			return;
		}

		auto Utf8ToWide = [](const std::string& s) -> std::wstring
			{
				if (s.empty())
				{
					return std::wstring();
				}

				int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), nullptr, 0);
				if (wlen <= 0)
				{
					wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
				}
				if (wlen <= 0)
				{
					return std::wstring();
				}

				std::wstring result((size_t)wlen, L'\0');
				MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &result[0], wlen);
				return result;
			};

		auto WriteWide = [&](const std::wstring& w) -> void
			{
				if (w.empty())
				{
					return;
				}

				DWORD written = 0;
				(void)WriteConsoleW(h, w.c_str(), (DWORD)w.size(), &written, nullptr);
			};

		if (vtEnabled)
		{
			std::string colored;
			colored.reserve(textUtf8.size() + 16);
			colored += GetAnsiColorByLevel(level);
			colored += textUtf8;
			colored += "\x1b[0m";

			const std::wstring w = Utf8ToWide(colored);
			if (!w.empty())
			{
				WriteWide(w);
			}
			else
			{
				os << colored;
				os.flush();
			}
		}
		else
		{
			const WORD oldAttr = toErr ? st.defaultAttrErr : st.defaultAttrOut;
			const WORD newAttr = GetWinAttrByLevel(level);
			SetConsoleTextAttribute(h, newAttr);

			const std::wstring w = Utf8ToWide(textUtf8);
			if (!w.empty())
			{
				WriteWide(w);
			}
			else
			{
				os << textUtf8;
				os.flush();
			}

			SetConsoleTextAttribute(h, oldAttr);
		}
#else
		std::ostream& os = SelectStream(level);
		const int fd = (level >= GB_LogLevel::GBLOGLEVEL_ERROR) ? STDERR_FILENO : STDOUT_FILENO;
		const bool isTty = (::isatty(fd) == 1);
		if (isTty)
		{
			os << GetAnsiColorByLevel(level) << textUtf8 << "\x1b[0m"; // ANSI/ECMA-48 转义序列（着色），正文仍是 UTF-8。
		}
		else
		{
			os << textUtf8;
		}
		os.flush();
#endif
	}
	static const std::string& GetAllLogFilePath()
	{
		static const std::string filePathUtf8 = GB_GetExeDirectory() + GB_STR("GB_Logs/GB_AllLog.log");
		return filePathUtf8;
	}

	static const std::string& GetOutputLogFilePath()
	{
		static const std::string filePathUtf8 = GB_GetExeDirectory() + GB_STR("GB_Logs/GB_OutputLog.log");
		return filePathUtf8;
	}

	static void EnsureLogFilesCreated()
	{
		(void)GB_CreateFileRecursive(GetAllLogFilePath());
		(void)GB_CreateFileRecursive(GetOutputLogFilePath());
	}
	static const size_t kMaxPendingLogItems = 200000; // 保护内存：队列最多积压的条目数
	static std::atomic<uint64_t> gDroppedLogCount{ 0 };

	// -------- 日志配置缓存（无锁读取 + 后台轮询刷新）--------
	// 目标：
	// 1) GB_CheckLogLevel / GB_IsLogToConsole / GB_IsLogEnabled 等热路径只做 atomic 读，避免每条日志都查配置。
	// 2) 支持外部动态修改配置：后台线程每 500ms 轮询同步一次。
	class LogConfigCache
	{
	public:
		static LogConfigCache& Get();

		bool IsLogEnabled() const;
		bool IsLogToConsole() const;
		GB_LogLevel GetFilterLevel() const;

		// 供 Set* 接口调用：写入配置后立即刷新一次，避免等待轮询周期。
		void ForceRefresh();

		// 可重复调用；用于进程退出/特殊生命周期（如 DLL 卸载/单测）回收后台线程。
		void Shutdown();

	private:
		LogConfigCache();
		LogConfigCache(const LogConfigCache&) = delete;
		LogConfigCache& operator=(const LogConfigCache&) = delete;

		void PollThreadFunc();
		void RefreshOnce();

		static GB_LogLevel ParseLogLevel(const std::string& valueRaw);

		std::atomic_bool isLogEnabled{ false };
		std::atomic_bool isLogToConsole{ false };
		std::atomic<int> filterLevelInt{ static_cast<int>(GB_LogLevel::GBLOGLEVEL_DISABLELOG) };

		std::atomic_bool stop{ false };
		std::atomic_bool hasShutdown{ false };

		std::mutex waitMtx;
		std::condition_variable waitCv;
		std::thread pollThread;
	};

	static std::atomic<LogConfigCache*> gLogConfigCacheInstance{ nullptr };

	static void ShutdownLogConfigCacheAtExit()
	{
		LogConfigCache* cache = gLogConfigCacheInstance.load(std::memory_order_acquire);
		if (cache)
		{
			cache->Shutdown();
		}
	}

	LogConfigCache& LogConfigCache::Get()
	{
		static std::once_flag once;
		std::call_once(once, []()
			{
				LogConfigCache* cache = new LogConfigCache();
				gLogConfigCacheInstance.store(cache, std::memory_order_release);
				std::atexit(&ShutdownLogConfigCacheAtExit);
			});
		LogConfigCache* cache = gLogConfigCacheInstance.load(std::memory_order_acquire);
		return *cache;
	}

	LogConfigCache::LogConfigCache()
	{
		RefreshOnce();
		pollThread = std::thread(&LogConfigCache::PollThreadFunc, this);
	}

	void LogConfigCache::Shutdown()
	{
		if (hasShutdown.exchange(true, std::memory_order_acq_rel))
		{
			return;
		}

		stop.store(true, std::memory_order_release);
		waitCv.notify_all();

		if (pollThread.joinable())
		{
			pollThread.join();
		}
	}

	void LogConfigCache::ForceRefresh()
	{
		RefreshOnce();
	}

	bool LogConfigCache::IsLogEnabled() const
	{
		return isLogEnabled.load(std::memory_order_relaxed);
	}

	bool LogConfigCache::IsLogToConsole() const
	{
		return isLogToConsole.load(std::memory_order_relaxed);
	}

	GB_LogLevel LogConfigCache::GetFilterLevel() const
	{
		const int value = filterLevelInt.load(std::memory_order_relaxed);
		return static_cast<GB_LogLevel>(value);
	}

	static std::string NormalizeLevelString(const std::string& input)
	{
		std::string result;
		result.reserve(input.size());
		for (size_t i = 0; i < input.size(); i++)
		{
			const unsigned char ch = static_cast<unsigned char>(input[i]);
			if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
			{
				continue;
			}
			if (ch >= 'a' && ch <= 'z')
			{
				result.push_back(static_cast<char>(ch - 'a' + 'A'));
			}
			else
			{
				result.push_back(static_cast<char>(ch));
			}
		}
		return result;
	}

	GB_LogLevel LogConfigCache::ParseLogLevel(const std::string& valueRaw)
	{
		const std::string value = NormalizeLevelString(valueRaw);

		if (value == GB_STR("TRACE") || value == GB_STR("0"))
		{
			return GB_LogLevel::GBLOGLEVEL_TRACE;
		}
		if (value == GB_STR("DEBUG") || value == GB_STR("1"))
		{
			return GB_LogLevel::GBLOGLEVEL_DEBUG;
		}
		if (value == GB_STR("INFO") || value == GB_STR("2"))
		{
			return GB_LogLevel::GBLOGLEVEL_INFO;
		}
		if (value == GB_STR("WARNING") || value == GB_STR("WARN") || value == GB_STR("3"))
		{
			return GB_LogLevel::GBLOGLEVEL_WARNING;
		}
		if (value == GB_STR("ERROR") || value == GB_STR("4"))
		{
			return GB_LogLevel::GBLOGLEVEL_ERROR;
		}
		if (value == GB_STR("FATAL") || value == GB_STR("5"))
		{
			return GB_LogLevel::GBLOGLEVEL_FATAL;
		}
		if (value == GB_STR("DISABLELOG") || value == GB_STR("DISABLE") || value == GB_STR("OFF") || value == GB_STR("6"))
		{
			return GB_LogLevel::GBLOGLEVEL_DISABLELOG;
		}

		// 未知值：按最保守的策略回退到 TRACE，避免“误关日志”导致排障困难。
		return GB_LogLevel::GBLOGLEVEL_TRACE;
	}

	void LogConfigCache::RefreshOnce()
	{
		// 1) Enable
		bool enable = false;
		{
			const std::string enableKey = GB_STR("GB_EnableLog");
			if (GB_IsExistsGbConfig(enableKey))
			{
				std::string value;
				enable = (GB_GetGbConfig(enableKey, value) && value == GB_STR("1"));
			}
		}
		isLogEnabled.store(enable, std::memory_order_relaxed);

		// 2) Console
		bool toConsole = false;
		{
			const std::string consoleKey = GB_STR("GB_IsLogToConsole");
			if (GB_IsExistsGbConfig(consoleKey))
			{
				std::string value;
				toConsole = (GB_GetGbConfig(consoleKey, value) && value == GB_STR("1"));
			}
		}
		isLogToConsole.store(toConsole, std::memory_order_relaxed);

		// 3) FilterLevel
		GB_LogLevel filterLevel = GB_LogLevel::GBLOGLEVEL_DISABLELOG;
		if (enable)
		{
			const std::string levelKey = GB_STR("GB_LogLevel");
			if (!GB_IsExistsGbConfig(levelKey))
			{
				filterLevel = GB_LogLevel::GBLOGLEVEL_TRACE;
			}
			else
			{
				std::string value;
				if (!GB_GetGbConfig(levelKey, value))
				{
					filterLevel = GB_LogLevel::GBLOGLEVEL_TRACE;
				}
				else
				{
					filterLevel = ParseLogLevel(value);
				}
			}
		}
		filterLevelInt.store(static_cast<int>(filterLevel), std::memory_order_relaxed);
	}

	void LogConfigCache::PollThreadFunc()
	{
		std::unique_lock<std::mutex> lock(waitMtx);
		while (!stop.load(std::memory_order_acquire))
		{
			lock.unlock();
			RefreshOnce();
			lock.lock();

			waitCv.wait_for(lock, std::chrono::milliseconds(500), [this]()
				{
					return stop.load(std::memory_order_acquire);
				});
		}
	}
}


std::string LogLevelToString(GB_LogLevel level)
{
	switch (level)
	{
	case GB_LogLevel::GBLOGLEVEL_TRACE:			return GB_STR("TRACE");
	case GB_LogLevel::GBLOGLEVEL_DEBUG:			return GB_STR("DEBUG");
	case GB_LogLevel::GBLOGLEVEL_INFO:			return GB_STR("INFO");
	case GB_LogLevel::GBLOGLEVEL_WARNING:		return GB_STR("WARNING");
	case GB_LogLevel::GBLOGLEVEL_ERROR:			return GB_STR("ERROR");
	case GB_LogLevel::GBLOGLEVEL_FATAL:			return GB_STR("FATAL");
	case GB_LogLevel::GBLOGLEVEL_DISABLELOG:	return GB_STR("DISABLELOG");
	default:									return GB_STR("UNKNOWN");
	}
}

void GB_LogItem::AppendJsonTo(std::string& out) const
{
	out += GB_STR("{");

	out += GB_STR("\"ts\":\"");
	internal::AppendJsonEscaped(out, timestamp);
	out += GB_STR("\"");

	out += GB_STR(",\"level\":\"");
	out += LogLevelToString(level);
	out += GB_STR("\"");

	out += GB_STR(",\"thread\":\"");
	internal::AppendJsonEscaped(out, threadId);
	out += GB_STR("\"");

	out += GB_STR(",\"file\":\"");
	internal::AppendJsonEscaped(out, file);
	out += GB_STR("\"");

	out += GB_STR(",\"line\":");
	out += std::to_string(line);

	out += GB_STR(",\"msg\":\"");
	internal::AppendJsonEscaped(out, message);
	out += GB_STR("\"");

	out += GB_STR("}\n");
}

void GB_LogItem::AppendPlainTextTo(std::string& out) const
{
	out += GB_STR("[");
	out += timestamp;
	out += GB_STR("] [");

	out += LogLevelToString(level);
	out += GB_STR("] [");

	out += threadId;
	out += GB_STR("] [");

	out += file;
	out += GB_STR(":");
	out += std::to_string(line);

	out += GB_STR("] ");
	out += message;
	out += GB_STR("\n");
}

std::string GB_LogItem::ToJsonString() const
{
	std::string out;
	const size_t reserveGuess = 64 + message.size() + threadId.size() + file.size();
	out.reserve(reserveGuess);

	AppendJsonTo(out);
	return out;
}

std::string GB_LogItem::ToPlainTextString() const
{
	std::string out;
	const size_t reserveGuess = 64 + message.size() + threadId.size() + file.size();
	out.reserve(reserveGuess);

	AppendPlainTextTo(out);
	return out;
}

namespace
{
	GB_Logger* gLoggerInstance = nullptr;
	void ShutdownLoggerAtExit()
	{
		if (gLoggerInstance)
		{
			gLoggerInstance->Shutdown();
		}
	}
}

GB_Logger& GB_Logger::GetInstance()
{
	static std::once_flag once;
	std::call_once(once, []()
		{
			gLoggerInstance = new GB_Logger();
			std::atexit(&ShutdownLoggerAtExit);
		});
	return *gLoggerInstance;
}

bool GB_Logger::EnqueueLogItem(GB_LogItem&& logItem)
{
	std::lock_guard<std::mutex> lock(logQueueMtx);

	if (isStop.load(std::memory_order_acquire))
	{
		return false;
	}

	// 防止日志写入端过快导致队列无限膨胀（OOM）。
	// - 对于 ERROR/FATAL：尽量保留新日志，必要时丢弃最旧的日志；
	// - 对于更低级别：当队列已满时直接丢弃。
	if (logQueue.size() >= internal::kMaxPendingLogItems)
	{
		if (logItem.level >= GB_LogLevel::GBLOGLEVEL_ERROR)
		{
			while (logQueue.size() >= internal::kMaxPendingLogItems)
			{
				logQueue.pop();
				internal::gDroppedLogCount.fetch_add(1, std::memory_order_relaxed);
			}
		}
		else
		{
			internal::gDroppedLogCount.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}

	logQueue.push(std::move(logItem));
	return true;
}


void GB_Logger::Log(GB_LogLevel level, const std::string& msgUtf8, const std::string& fileUtf8, int line)
{
	if (isStop.load(std::memory_order_acquire))
	{
		return;
	}

	if (!GB_CheckLogLevel(level))
	{
		return;
	}

	GB_LogItem logItem;
	logItem.timestamp = GetLocalTimeStr();
	logItem.level = level;
	logItem.message = msgUtf8;
	logItem.threadId = internal::GetThreadIdString();
	logItem.file = GB_Utf8Replace(fileUtf8, GB_STR("\\"), GB_STR("/"));
	logItem.line = line;

	if (EnqueueLogItem(std::move(logItem)))
	{
		logQueueCv.notify_one();
	}
}


void GB_Logger::LogChecked(GB_LogLevel level, const std::string& msgUtf8, const char* file, int line)
{
	if (isStop.load(std::memory_order_acquire))
	{
		return;
	}

	GB_LogItem logItem;
	logItem.timestamp = GetLocalTimeStr();
	logItem.level = level;
	logItem.message = msgUtf8;
	logItem.threadId = internal::GetThreadIdString();
	logItem.file = internal::NormalizeFilePathUtf8(file);
	logItem.line = line;

	if (EnqueueLogItem(std::move(logItem)))
	{
		logQueueCv.notify_one();
	}
}


void GB_Logger::LogTrace(const std::string& msgUtf8, const char* file, int line)
{
	if (!GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_TRACE))
	{
		return;
	}
	LogChecked(GB_LogLevel::GBLOGLEVEL_TRACE, msgUtf8, file, line);
}


void GB_Logger::LogDebug(const std::string& msgUtf8, const char* file, int line)
{
	if (!GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_DEBUG))
	{
		return;
	}
	LogChecked(GB_LogLevel::GBLOGLEVEL_DEBUG, msgUtf8, file, line);
}


void GB_Logger::LogInfo(const std::string& msgUtf8, const char* file, int line)
{
	if (!GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_INFO))
	{
		return;
	}
	LogChecked(GB_LogLevel::GBLOGLEVEL_INFO, msgUtf8, file, line);
}


void GB_Logger::LogWarning(const std::string& msgUtf8, const char* file, int line)
{
	if (!GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_WARNING))
	{
		return;
	}
	LogChecked(GB_LogLevel::GBLOGLEVEL_WARNING, msgUtf8, file, line);
}


void GB_Logger::LogError(const std::string& msgUtf8, const char* file, int line)
{
	if (!GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_ERROR))
	{
		return;
	}
	LogChecked(GB_LogLevel::GBLOGLEVEL_ERROR, msgUtf8, file, line);
}


void GB_Logger::LogFatal(const std::string& msgUtf8, const char* file, int line)
{
	if (!GB_CheckLogLevel(GB_LogLevel::GBLOGLEVEL_FATAL))
	{
		return;
	}
	LogChecked(GB_LogLevel::GBLOGLEVEL_FATAL, msgUtf8, file, line);
}



bool GB_Logger::ClearLogFiles() const
{
	internal::EnsureLogFilesCreated();

	const bool success1 = GB_WriteUtf8ToFile(internal::GetAllLogFilePath(), std::string(), false);
	const bool success2 = GB_WriteUtf8ToFile(internal::GetOutputLogFilePath(), std::string(), false);
	return success1 && success2;
}


GB_Logger::GB_Logger()
{
	isStop.store(false, std::memory_order_release);
	hasShutdown.store(false, std::memory_order_release);

	logThread = std::thread(&GB_Logger::LogThreadFunc, this);
}

GB_Logger::~GB_Logger()
{
	Shutdown();
}

void GB_Logger::Shutdown()
{
	if (hasShutdown.exchange(true, std::memory_order_acq_rel))
	{
		return;
	}

	isStop.store(true, std::memory_order_release);
	logQueueCv.notify_all();

	if (logThread.joinable())
	{
		logThread.join();
	}

	{
		std::lock_guard<std::mutex> lock(logQueueMtx);
		std::queue<GB_LogItem> emptyQueue;
		std::swap(logQueue, emptyQueue);
	}

	// 若日志模块被主动回收（例如 DLL 卸载/单元测试），则同时停止配置轮询线程，避免残留后台线程。
	internal::LogConfigCache* cache = internal::gLogConfigCacheInstance.load(std::memory_order_acquire);
	if (cache)
	{
		cache->Shutdown();
	}

}

void GB_Logger::LogThreadFunc()
{
	try
	{
		internal::EnsureLogFilesCreated();

		for (;;)
		{
			std::queue<GB_LogItem> localQueue;
			{
				std::unique_lock<std::mutex> lock(logQueueMtx);
				logQueueCv.wait(lock, [this]
					{
						return isStop.load(std::memory_order_acquire) || !logQueue.empty();
					});


				if (logQueue.empty() && isStop.load(std::memory_order_acquire))
				{
					break;
				}

				std::swap(localQueue, logQueue);
			}

			if (localQueue.empty())
			{
				continue;
			}

			const bool logToConsole = GB_IsLogToConsole();

			const size_t itemCount = localQueue.size();
			std::string allBatchUtf8;
			std::string outputBatchUtf8;
			allBatchUtf8.reserve(itemCount * 192);
			outputBatchUtf8.reserve(itemCount * 160);

			static const size_t kFlushThresholdBytes = 4 * 1024 * 1024; // 单次写入过大时分块落盘，避免一次性拼接造成巨大内存峰值

			auto FlushBatches = [&]()
				{
					if (!allBatchUtf8.empty())
					{
						(void)GB_WriteUtf8ToFile(internal::GetAllLogFilePath(), allBatchUtf8);
						allBatchUtf8.clear();
					}
					if (!outputBatchUtf8.empty())
					{
						(void)GB_WriteUtf8ToFile(internal::GetOutputLogFilePath(), outputBatchUtf8);
						outputBatchUtf8.clear();
					}
				};

			const uint64_t droppedLogCount = internal::gDroppedLogCount.exchange(0, std::memory_order_relaxed);
			if (droppedLogCount > 0)
			{
				GB_LogItem droppedItem;
				droppedItem.timestamp = GetLocalTimeStr();
				droppedItem.level = GB_LogLevel::GBLOGLEVEL_WARNING;
				droppedItem.message = GB_STR("Dropped ") + std::to_string(droppedLogCount) + GB_STR(" log items due to queue overflow.");
				droppedItem.threadId = GB_STR("GB_Logger");
				droppedItem.file = GB_STR("GB_Logger");
				droppedItem.line = 0;

				droppedItem.AppendJsonTo(allBatchUtf8);
				std::string droppedPlain;
				droppedPlain.reserve(128);
				droppedItem.AppendPlainTextTo(droppedPlain);
				outputBatchUtf8 += droppedPlain;
				if (logToConsole)
				{
					internal::ConsoleWriteColoredUtf8(droppedPlain, droppedItem.level);
				}
			}

			std::string plainTextUtf8;
			plainTextUtf8.reserve(256);

			while (!localQueue.empty())
			{
				GB_LogItem logItem = std::move(localQueue.front());
				localQueue.pop();

				logItem.AppendJsonTo(allBatchUtf8);

				plainTextUtf8.clear();
				const size_t reserveGuess = 64 + logItem.message.size() + logItem.threadId.size() + logItem.file.size();
				if (plainTextUtf8.capacity() < reserveGuess)
				{
					plainTextUtf8.reserve(reserveGuess);
				}
				logItem.AppendPlainTextTo(plainTextUtf8);

				outputBatchUtf8 += plainTextUtf8;
				if (logToConsole)
				{
					internal::ConsoleWriteColoredUtf8(plainTextUtf8, logItem.level);
				}
				if (allBatchUtf8.size() >= kFlushThresholdBytes || outputBatchUtf8.size() >= kFlushThresholdBytes)
				{
					FlushBatches();
				}
			}

			FlushBatches();
		}
	}
	catch (const std::exception& e)
	{
		try
		{
			std::cerr << "GB_Logger thread exception: " << e.what() << std::endl;
		}
		catch (...)
		{
		}
	}
	catch (...)
	{
		try
		{
			std::cerr << "GB_Logger thread exception: unknown" << std::endl;
		}
		catch (...)
		{
		}
	}
}


bool GB_IsLogEnabled()
{
	internal::LogConfigCache* cache = internal::gLogConfigCacheInstance.load(std::memory_order_acquire);
	if (!cache)
	{
		cache = &internal::LogConfigCache::Get();
	}
	return cache->IsLogEnabled();
}

bool GB_SetLogEnabled(bool enable)
{
	const static std::string targetKey = GB_STR("GB_EnableLog");
	const std::string value = enable ? GB_STR("1") : GB_STR("0");

	const bool success = GB_SetGbConfig(targetKey, value);
	if (success)
	{
		internal::LogConfigCache::Get().ForceRefresh();
	}
	return success;
}

bool GB_IsLogToConsole()
{
	internal::LogConfigCache* cache = internal::gLogConfigCacheInstance.load(std::memory_order_acquire);
	if (!cache)
	{
		cache = &internal::LogConfigCache::Get();
	}
	return cache->IsLogToConsole();
}

bool GB_SetLogToConsole(bool enable)
{
	const static std::string targetKey = GB_STR("GB_IsLogToConsole");
	const std::string value = enable ? GB_STR("1") : GB_STR("0");

	const bool success = GB_SetGbConfig(targetKey, value);
	if (success)
	{
		internal::LogConfigCache::Get().ForceRefresh();
	}
	return success;
}

GB_LogLevel GB_GetLogFilterLevel()
{
	internal::LogConfigCache* cache = internal::gLogConfigCacheInstance.load(std::memory_order_acquire);
	if (!cache)
	{
		cache = &internal::LogConfigCache::Get();
	}
	return cache->GetFilterLevel();
}

bool GB_CheckLogLevel(GB_LogLevel level)
{
	internal::LogConfigCache* cache = internal::gLogConfigCacheInstance.load(std::memory_order_acquire);
	if (!cache)
	{
		cache = &internal::LogConfigCache::Get();
	}

	const GB_LogLevel filterLevel = cache->GetFilterLevel();
	return (filterLevel != GB_LogLevel::GBLOGLEVEL_DISABLELOG) && (level >= filterLevel);
}

namespace crashlog
{
#if defined(_WIN32)
	static HANDLE crashFile = INVALID_HANDLE_VALUE;
#else
	static int crashFd = -1;
#endif
	static std::atomic<bool> installed{ false };

	static std::terminate_handler oldTerminateHandler = nullptr;
#if defined(_WIN32)
	static LPTOP_LEVEL_EXCEPTION_FILTER oldSehFilter = nullptr;
#else
	struct SigActionBackup
	{
		int sig = 0;
		struct sigaction oldAction;
		bool hasOld = false;
	};
	static SigActionBackup sigBackups[8];
	static size_t sigBackupCount = 0;
#endif

	// 十进制/十六进制安全拼接（无malloc）
	static size_t AppendStr(char* buf, size_t cap, const char* s)
	{
		if (!buf || cap == 0 || !s)
		{
			return 0;
		}
		size_t n = 0;
		while (n < cap && s[n])
		{
			n++;
		}
		std::memcpy(buf, s, n);
		return n;
	}

	static size_t AppendDec(char* buf, size_t cap, uint64_t v)
	{
		char tmp[32];
		size_t p = 0;
		do { tmp[p++] = char('0' + (v % 10)); v /= 10; } while (v && p < sizeof(tmp));
		size_t w = 0;
		while (p && w < cap) buf[w++] = tmp[--p];
		return w;
	}

	static size_t AppendHexU64(char* buf, size_t cap, uint64_t v, bool withPrefix)
	{
		static const char* hex = "0123456789ABCDEF";
		size_t w = 0;
		if (withPrefix && cap >= 2)
		{
			buf[w++] = '0';
			buf[w++] = 'x';
		}

		bool started = false;
		for (int i = (int)(sizeof(v) * 2 - 1); i >= 0 && w < cap; --i)
		{
			const unsigned nib = (unsigned)((v >> (i * 4)) & 0xFULL);
			if (nib || started || i == 0)
			{
				started = true;
				buf[w++] = hex[nib];
			}
		}
		return w;
	}

	static size_t AppendHexPtr(char* buf, size_t cap, const void* p)
	{
		static const char* hex = "0123456789ABCDEF";
		uintptr_t v = (uintptr_t)p;
		size_t w = 0;
		if (cap >= 2) { buf[w++] = '0'; buf[w++] = 'x'; }
		bool started = false;
		for (int i = (int)(sizeof(v) * 2 - 1); i >= 0 && w < cap; --i)
		{
			unsigned nib = (v >> (i * 4)) & 0xF;
			if (nib || started || i == 0) { started = true; buf[w++] = hex[nib]; }
		}
		return w;
	}

	static void EmergencyWrite(const char* s, size_t n)
	{
#if defined(_WIN32)
		DWORD w = 0;
		if (crashFile != INVALID_HANDLE_VALUE)
		{
			WriteFile(crashFile, s, (DWORD)n, &w, NULL);
			FlushFileBuffers(crashFile);
		}
		HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);

		if (hErr && hErr != INVALID_HANDLE_VALUE)
		{
			WriteFile(hErr, s, (DWORD)n, &w, NULL);
		}
#else
		if (crashFd >= 0)
		{
			(void)::write(crashFd, s, n);
			(void)::fsync(crashFd);
		}
		(void)::write(STDERR_FILENO, s, n);
#endif
	}

	static void EmergencyWriteLine(const char* s)
	{
		char nl = '\n';
		EmergencyWrite(s, std::strlen(s));
		EmergencyWrite(&nl, 1);
	}

	static void OpenCrashFileOnce()
	{
#if defined(_WIN32)
		if (crashFile != INVALID_HANDLE_VALUE)
		{
			return;
		}

		const std::string dir = GB_GetExeDirectory() + GB_STR("GB_Logs/");
		GB_CreateDirectory(dir);
		const std::string path = dir + GB_STR("GB_Crash.log");
		// UTF-8 -> UTF-16
		int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), NULL, 0);
		if (wlen <= 0)
		{
			return;
		}
		std::wstring wpath((size_t)wlen, L'\0');
		if (wlen > 0)
		{
			MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &wpath[0], wlen);
		}
		crashFile = CreateFileW(wpath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (crashFile != INVALID_HANDLE_VALUE) SetFilePointer(crashFile, 0, NULL, FILE_END);
#else
		if (crashFd >= 0) return;
		const std::string dir = GB_GetExeDirectory() + GB_STR("GB_Logs/");
		GB_CreateDirectory(dir);
		const std::string path = dir + GB_STR("GB_Crash.log");
		crashFd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
#endif
	}

#if !defined(_WIN32)
	static void LogBacktraceLinux()
	{
		// 注意：在信号处理函数里调用 backtrace/backtrace_symbols_fd 的可用性与 async-signal-safe 属性依赖 libc 实现；
		// 本实现尽量将输出落到 backtrace_symbols_fd（直接写 fd）并避免 malloc/stdio。
		void* frames[64];
		int n = ::backtrace(frames, 64);
		if (n > 0 && crashFd >= 0)
		{
			EmergencyWrite("Backtrace:\n", sizeof("Backtrace:\n") - 1);
			::backtrace_symbols_fd(frames, n, crashFd);
		}
	}
#endif
}

void GB_InstallCrashHandlers()
{
	if (crashlog::installed.exchange(true))
	{
		return;
	}
	crashlog::OpenCrashFileOnce();

	crashlog::oldTerminateHandler = nullptr;
#if defined(_WIN32)
	crashlog::oldSehFilter = nullptr;
#else
	crashlog::sigBackupCount = 0;
#endif

		// 层 1：未捕获 C++ 异常（尽量避免在 terminate 环境里走“异步日志队列”，以免死锁）
	crashlog::oldTerminateHandler = std::set_terminate([]()
		{
			crashlog::OpenCrashFileOnce();

			const char* reason = "unknown";
			try
			{
				if (auto ep = std::current_exception())
				{
					try
					{
						std::rethrow_exception(ep);
					}
					catch (const std::exception& e)
					{
						reason = e.what();
					}
					catch (...)
					{
						reason = "non-std exception";
					}
				}
				else
				{
					reason = "no current_exception";
				}
			}
			catch (...)
			{
				reason = "failed to extract exception";
			}

			char buf[512];
			size_t p = 0;
			p += crashlog::AppendStr(buf + p, sizeof(buf) - p, "FATAL: std::terminate: ");
			p += crashlog::AppendStr(buf + p, sizeof(buf) - p, reason);

#if defined(_WIN32)
			p += crashlog::AppendStr(buf + p, sizeof(buf) - p, " tid=");
			p += crashlog::AppendDec(buf + p, sizeof(buf) - p, (uint64_t)GetCurrentThreadId());
#else
			p += crashlog::AppendStr(buf + p, sizeof(buf) - p, " pid=");
			p += crashlog::AppendDec(buf + p, sizeof(buf) - p, (uint64_t)getpid());
#endif

			p += crashlog::AppendStr(buf + p, sizeof(buf) - p, " ts=");
			p += crashlog::AppendDec(buf + p, sizeof(buf) - p, (uint64_t)time(nullptr));

#if defined(_WIN32)
			p += crashlog::AppendStr(buf + p, sizeof(buf) - p, "\r\n");
#else
			p += crashlog::AppendStr(buf + p, sizeof(buf) - p, "\n");
#endif
			crashlog::EmergencyWrite(buf, p);

			std::abort(); // 维持标准行为
		});


#if defined(_WIN32)
	// 层 3：未处理 SEH 异常
	crashlog::oldSehFilter = SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ex) -> LONG
		{
			char head[256]; size_t p = 0;
			p += crashlog::AppendStr(head + p, sizeof(head) - p, "FATAL: Unhandled SEH, code=0x");
			p += crashlog::AppendHexU64(head + p, sizeof(head) - p, (uint64_t)ex->ExceptionRecord->ExceptionCode, false);
			p += crashlog::AppendStr(head + p, sizeof(head) - p, " at ");
			p += crashlog::AppendHexPtr(head + p, sizeof(head) - p, ex->ExceptionRecord->ExceptionAddress);
			p += crashlog::AppendStr(head + p, sizeof(head) - p, " t=");
			p += crashlog::AppendDec(head + p, sizeof(head) - p, (uint64_t)GetCurrentThreadId());
			p += crashlog::AppendStr(head + p, sizeof(head) - p, " ts=");
			p += crashlog::AppendDec(head + p, sizeof(head) - p, (uint64_t)time(nullptr));
			p += crashlog::AppendStr(head + p, sizeof(head) - p, "\r\n");
			crashlog::EmergencyWrite(head, p);

			// 栈
			void* frames[62];
			USHORT n = CaptureStackBackTrace(0, 62, frames, nullptr);
			crashlog::EmergencyWrite("Backtrace:\r\n", sizeof("Backtrace:\r\n") - 1);
			for (USHORT i = 0; i < n; i++)
			{
				char ln[64]; size_t q = 0;
				q += crashlog::AppendStr(ln + q, sizeof(ln) - q, "  #");
				q += crashlog::AppendDec(ln + q, sizeof(ln) - q, i);
				q += crashlog::AppendStr(ln + q, sizeof(ln) - q, " ");
				q += crashlog::AppendHexPtr(ln + q, sizeof(ln) - q, frames[i]);
				q += crashlog::AppendStr(ln + q, sizeof(ln) - q, "\r\n");
				crashlog::EmergencyWrite(ln, q);
			}

			// 可选：写 .dmp （DbgHelp，系统库），但官方建议最好由**独立进程**写 dump，避免目标进程已不稳定时再调 loader。
			// 返回后系统终止进程
			return EXCEPTION_EXECUTE_HANDLER;
		});

#else
	// 层 2：Linux 信号
	auto install = [](int sig)
		{
			struct sigaction sa;
			std::memset(&sa, 0, sizeof(sa));
			sa.sa_sigaction = [](int s, siginfo_t* info, void* uctx)
				{
					(void)uctx;
					char head[256]; size_t p = 0;
					p += crashlog::AppendStr(head + p, sizeof(head) - p, "FATAL: Signal ");
					p += crashlog::AppendDec(head + p, sizeof(head) - p, (uint64_t)s);
					p += crashlog::AppendStr(head + p, sizeof(head) - p, " addr=");
					p += crashlog::AppendHexPtr(head + p, sizeof(head) - p, info ? info->si_addr : nullptr);
					p += crashlog::AppendStr(head + p, sizeof(head) - p, " pid=");
					p += crashlog::AppendDec(head + p, sizeof(head) - p, (uint64_t)getpid());
					p += crashlog::AppendStr(head + p, sizeof(head) - p, " ts=");
					p += crashlog::AppendDec(head + p, sizeof(head) - p, (uint64_t)time(nullptr));
					p += crashlog::AppendStr(head + p, sizeof(head) - p, "\n");
					crashlog::EmergencyWrite(head, p);

					crashlog::LogBacktraceLinux();

					// 恢复默认并再发一次，让系统生成 core（如已启用）
					::signal(s, SIG_DFL);
					::raise(s);
				};
			sigemptyset(&sa.sa_mask);
			sa.sa_flags = SA_SIGINFO;
			struct sigaction oldSa;
			if (sigaction(sig, &sa, &oldSa) == 0)
			{
				if (crashlog::sigBackupCount < (sizeof(crashlog::sigBackups) / sizeof(crashlog::sigBackups[0])))
				{
					crashlog::sigBackups[crashlog::sigBackupCount].sig = sig;
					crashlog::sigBackups[crashlog::sigBackupCount].oldAction = oldSa;
					crashlog::sigBackups[crashlog::sigBackupCount].hasOld = true;
					crashlog::sigBackupCount++;
				}
			}
		};

	install(SIGSEGV);
	install(SIGABRT);
	install(SIGFPE);
	install(SIGILL);
#   if defined(SIGBUS)
	install(SIGBUS);
#   endif
	install(SIGTRAP);
#endif
}

void GB_RemoveCrashHandlers()
{
	if (!crashlog::installed.exchange(false))
	{
		return;
	}

	if (crashlog::oldTerminateHandler)
	{
		std::set_terminate(crashlog::oldTerminateHandler);
		crashlog::oldTerminateHandler = nullptr;
	}

#if defined(_WIN32)
	SetUnhandledExceptionFilter(crashlog::oldSehFilter);
	crashlog::oldSehFilter = nullptr;

	if (crashlog::crashFile != INVALID_HANDLE_VALUE)
	{
		CloseHandle(crashlog::crashFile);
		crashlog::crashFile = INVALID_HANDLE_VALUE;
	}
#else
	for (size_t i = 0; i < crashlog::sigBackupCount; i++)
	{
		if (crashlog::sigBackups[i].hasOld)
		{
			sigaction(crashlog::sigBackups[i].sig, &crashlog::sigBackups[i].oldAction, nullptr);
		}
	}
	crashlog::sigBackupCount = 0;

	if (crashlog::crashFd >= 0)
	{
		(void)::close(crashlog::crashFd);
		crashlog::crashFd = -1;
	}
#endif
}
