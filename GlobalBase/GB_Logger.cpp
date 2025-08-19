#include "GB_Logger.h"
#include "GB_Config.h"
#include "GB_FileSystem.h"
#include "GB_IO.h"
#include "GB_Utility.h"
#include "GB_Timer.h"

#if defined(_WIN32)
#  include <windows.h>
#endif

using namespace std;

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

#if defined(_WIN32)
	struct WinConsoleState
	{
		bool vtEnabled = false;
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
				DWORD mode = 0;
				if (hOut && GetConsoleMode(hOut, &mode))
				{
					const DWORD newMode = mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */;
					if (SetConsoleMode(hOut, newMode))
					{
						st.vtEnabled = true;
					}
				}
				if (hErr && GetConsoleMode(hErr, &mode))
				{
					const DWORD newMode = mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */;
					SetConsoleMode(hErr, newMode);
				}
				// 参考：Windows Console Virtual Terminal sequences / SetConsoleMode。
			});
	}
#endif // _WIN32

	// 如果 Windows 支持 VT：发 VT 转义 + UTF-8 文本；否则回退 SetConsoleTextAttribute 着色后直接输出 UTF-8 文本。
	static void ConsoleWriteColoredUtf8(const std::string& textUtf8, GB_LogLevel level)
	{
		unsigned int originCodePage = 0;
		GetConsoleEncodingCode(originCodePage);
		if (originCodePage != 65001)
		{
			SetConsoleEncodingToUtf8();
		}

#if defined(_WIN32)
		EnableWinVtOnce();
		WinConsoleState& st = GetWinConsoleState();

		std::ostream& os = SelectStream(level);
		const bool toErr = (&os == &std::cerr);
		const HANDLE h = GetStdHandle(toErr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);

		if (st.vtEnabled)
		{
			os << GetAnsiColorByLevel(level) << textUtf8 << "\x1b[0m";
			os.flush();
		}
		else
		{
			const WORD oldAttr = toErr ? st.defaultAttrErr : st.defaultAttrOut;
			const WORD newAttr = GetWinAttrByLevel(level);
			if (h) SetConsoleTextAttribute(h, newAttr); // 仅改变颜色属性，不影响编码。:contentReference[oaicite:3]{index=3}
			os << textUtf8;
			os.flush();
			if (h) SetConsoleTextAttribute(h, oldAttr);
		}
#else
		std::ostream& os = SelectStream(level);
		os << GetAnsiColorByLevel(level) << textUtf8 << "\x1b[0m"; // ANSI/ECMA-48 转义序列（着色），正文仍是 UTF-8。:contentReference[oaicite:4]{index=4}
		os.flush();
#endif

		if (originCodePage != 65001)
		{
			SetConsoleEncoding(originCodePage);
		}
	}
}


string LogLevelToString(GB_LogLevel level)
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

string GB_LogItem::ToJsonString() const
{
	string out;
	const size_t reserveGuess = 64 + message.size() + threadId.size() + file.size();
	out.reserve(reserveGuess);

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
	{
		std::ostringstream oss;
		oss << line;
		out += oss.str();
	}

	out += GB_STR(",\"msg\":\"");
	internal::AppendJsonEscaped(out, message);
	out += GB_STR("\"");

	out += GB_STR("}\n");
	return out;
}

string GB_LogItem::ToPlainTextString() const
{
	string out;
	const size_t reserveGuess = 64 + message.size() + threadId.size() + file.size();
	out.reserve(reserveGuess);

	out += GB_STR("[");
	out += timestamp;
	out += GB_STR("] [");

	out += LogLevelToString(level);
	out += GB_STR("] [");

	out += threadId;
	out += GB_STR("] [");

	out += file;
	out += GB_STR(":");
	{
		std::ostringstream oss;
		oss << line;
		out += oss.str();
	}

	out += GB_STR("] ");
	out += message;
	out += GB_STR("\n");

	return out;
}

GB_Logger& GB_Logger::GetInstance()
{
	static GB_Logger instance;
	return instance;
}

void GB_Logger::Log(GB_LogLevel level, const string& msgUtf8, const string& fileUtf8, int line)
{
	GB_LogItem logItem;
	logItem.timestamp = GetLocalTimeStr();
	logItem.level = level;
	logItem.message = msgUtf8;
	{
		std::ostringstream oss;
		oss << std::this_thread::get_id();
		logItem.threadId = oss.str();
	}
	logItem.file = fileUtf8;
	logItem.line = line;
	{
		std::lock_guard<std::mutex> lock(logQueueMtx);
		logQueue.push(logItem);
	}
	logQueueCv.notify_one();
}

void GB_Logger::LogTrace(const std::string& msgUtf8, const char* file, int line)
{
	if (!file)
	{
		Log(GB_LogLevel::GBLOGLEVEL_TRACE, msgUtf8, "", line);
		return;
	}

#if defined(_WIN32)
	string fileStrUtf8 = AnsiToUtf8(file);
#else
	string fileStrUtf8 = file;
#endif
	fileStrUtf8 = Utf8Replace(fileStrUtf8, GB_STR("\\"), GB_STR("/"));
	Log(GB_LogLevel::GBLOGLEVEL_TRACE, msgUtf8, fileStrUtf8, line);
}

void GB_Logger::LogDebug(const std::string& msgUtf8, const char* file, int line)
{
	if (!file)
	{
		Log(GB_LogLevel::GBLOGLEVEL_DEBUG, msgUtf8, "", line);
		return;
	}

#if defined(_WIN32)
	string fileStrUtf8 = AnsiToUtf8(file);
#else
	string fileStrUtf8 = file;
#endif
	fileStrUtf8 = Utf8Replace(fileStrUtf8, GB_STR("\\"), GB_STR("/"));
	Log(GB_LogLevel::GBLOGLEVEL_DEBUG, msgUtf8, fileStrUtf8, line);
}

void GB_Logger::LogInfo(const std::string& msgUtf8, const char* file, int line)
{
	if (!file)
	{
		Log(GB_LogLevel::GBLOGLEVEL_INFO, msgUtf8, "", line);
		return;
	}

#if defined(_WIN32)
	string fileStrUtf8 = AnsiToUtf8(file);
#else
	string fileStrUtf8 = file;
#endif
	fileStrUtf8 = Utf8Replace(fileStrUtf8, GB_STR("\\"), GB_STR("/"));
	Log(GB_LogLevel::GBLOGLEVEL_INFO, msgUtf8, fileStrUtf8, line);
}

void GB_Logger::LogWarning(const std::string& msgUtf8, const char* file, int line)
{
	if (!file)
	{
		Log(GB_LogLevel::GBLOGLEVEL_WARNING, msgUtf8, "", line);
		return;
	}

#if defined(_WIN32)
	string fileStrUtf8 = AnsiToUtf8(file);
#else
	string fileStrUtf8 = file;
#endif
	fileStrUtf8 = Utf8Replace(fileStrUtf8, GB_STR("\\"), GB_STR("/"));
	Log(GB_LogLevel::GBLOGLEVEL_WARNING, msgUtf8, fileStrUtf8, line);
}

void GB_Logger::LogError(const std::string& msgUtf8, const char* file, int line)
{
	if (!file)
	{
		Log(GB_LogLevel::GBLOGLEVEL_ERROR, msgUtf8, "", line);
		return;
	}

#if defined(_WIN32)
	string fileStrUtf8 = AnsiToUtf8(file);
#else
	string fileStrUtf8 = file;
#endif
	fileStrUtf8 = Utf8Replace(fileStrUtf8, GB_STR("\\"), GB_STR("/"));
	Log(GB_LogLevel::GBLOGLEVEL_ERROR, msgUtf8, fileStrUtf8, line);
}

void GB_Logger::LogFatal(const std::string& msgUtf8, const char* file, int line)
{
	if (!file)
	{
		Log(GB_LogLevel::GBLOGLEVEL_FATAL, msgUtf8, "", line);
		return;
	}

#if defined(_WIN32)
	string fileStrUtf8 = AnsiToUtf8(file);
#else
	string fileStrUtf8 = file;
#endif
	fileStrUtf8 = Utf8Replace(fileStrUtf8, GB_STR("\\"), GB_STR("/"));
	Log(GB_LogLevel::GBLOGLEVEL_FATAL, msgUtf8, fileStrUtf8, line);
}

const static string allLogFilePath = GB_GetExeDirectory() + GB_STR("GB_Logs/GB_AllLog.log");
const static string outputLogFilePath = GB_GetExeDirectory() + GB_STR("GB_Logs/GB_OutputLog.log");

bool GB_Logger::ClearLogFiles() const
{
	const bool success1 = GB_CreateFileRecursive(allLogFilePath);
	const bool success2 = GB_CreateFileRecursive(outputLogFilePath);
	return success1 && success2;
}

GB_Logger::GB_Logger()
{
	isStop.store(false, std::memory_order_release);

	logThread = std::thread(&GB_Logger::LogThreadFunc, this);
	logThread.detach();
}

GB_Logger::~GB_Logger()
{
	isStop.store(true, std::memory_order_release);
	logQueueCv.notify_all();

	if (logThread.joinable())
	{
		logThread.join();
	}

	std::queue<GB_LogItem> emptyQueue;
	std::swap(logQueue, emptyQueue);
}

void GB_Logger::LogThreadFunc()
{
	while (!isStop.load(std::memory_order_acquire))
	{
		std::queue<GB_LogItem> localQueue;
		{
			std::unique_lock<std::mutex> lock(logQueueMtx);
			logQueueCv.wait(lock, [this] {
				return !logQueue.empty() || isStop.load(std::memory_order_acquire); });
			if (isStop.load(std::memory_order_acquire))
			{
				return;
			}
			while (!logQueue.empty())
			{
				localQueue.push(logQueue.front());
				logQueue.pop();
			}
		}

		if (localQueue.empty())
		{
			continue;
		}

		while (!localQueue.empty())
		{
			const GB_LogItem logItem = localQueue.front();
			localQueue.pop();

			if (isStop)
			{
				return;
			}

			if (!IsLogEnabled())
			{
				continue;
			}

			const string logJsonUtf8 = logItem.ToJsonString();
			WriteUtf8ToFile(allLogFilePath, logJsonUtf8);

			if (CheckLogLevel(logItem.level))
			{
				WriteUtf8ToFile(outputLogFilePath, logJsonUtf8);
				if (IsLogToConsole())
				{
					const string logTextUtf8 = logItem.ToPlainTextString();
					internal::ConsoleWriteColoredUtf8(logTextUtf8, logItem.level);
				}
			}
		}
	}
}

bool IsLogEnabled()
{
	const static string targetKey = GB_STR("GB_EnableLog");

	if (!IsExistsGbConfig(targetKey))
	{
		return false;
	}

	string value;
	return (GetGbConfig(targetKey, value) && "1" == value);
}

bool SetLogEnabled(bool enable)
{
	const static string targetKey = GB_STR("GB_EnableLog");
	const string value = enable ? GB_STR("1") : GB_STR("0");
	return SetGbConfig(targetKey, value);
}

bool IsLogToConsole()
{
	const static string targetKey = GB_STR("GB_IsLogToConsole");

	if (!IsExistsGbConfig(targetKey))
	{
		return false;
	}

	string value;
	return (GetGbConfig(targetKey, value) && "1" == value);
}

bool SetLogToConsole(bool enable)
{
	const static string targetKey = GB_STR("GB_IsLogToConsole");
	const string value = enable ? GB_STR("1") : GB_STR("0");
	return SetGbConfig(targetKey, value);
}

GB_LogLevel GetLogFilterLevel()
{
	const static string targetKey = GB_STR("GB_LogLevel");

	if (!IsLogEnabled())
	{
		return GB_LogLevel::GBLOGLEVEL_DISABLELOG;
	}

	if (!IsExistsGbConfig(targetKey))
	{
		return GB_LogLevel::GBLOGLEVEL_TRACE;
	}
	string value;
	if (!GetGbConfig(targetKey, value))
	{
		return GB_LogLevel::GBLOGLEVEL_TRACE;
	}
	if (value == GB_STR("TRACE") || value == GB_STR("0"))
	{
		return GB_LogLevel::GBLOGLEVEL_TRACE;
	}
	else if (value == GB_STR("DEBUG") || value == GB_STR("1"))
	{
		return GB_LogLevel::GBLOGLEVEL_DEBUG;
	}
	else if (value == GB_STR("INFO") || value == GB_STR("2"))
	{
		return GB_LogLevel::GBLOGLEVEL_INFO;
	}
	else if (value == GB_STR("WARNING") || value == GB_STR("3"))
	{
		return GB_LogLevel::GBLOGLEVEL_WARNING;
	}
	else if (value == GB_STR("ERROR") || value == GB_STR("4"))
	{
		return GB_LogLevel::GBLOGLEVEL_ERROR;
	}
	else if (value == GB_STR("FATAL") || value == GB_STR("5"))
	{
		return GB_LogLevel::GBLOGLEVEL_FATAL;
	}
	else if (value == GB_STR("DISABLELOG") || value == GB_STR("6"))
	{
		return GB_LogLevel::GBLOGLEVEL_DISABLELOG;
	}
	return GB_LogLevel::GBLOGLEVEL_TRACE;
}

bool CheckLogLevel(GB_LogLevel level)
{
	const GB_LogLevel filterLevel = GetLogFilterLevel();
	return (level >= filterLevel && filterLevel != GB_LogLevel::GBLOGLEVEL_DISABLELOG);
}

namespace crashlog
{
#if defined(_WIN32)
	static HANDLE crashFile = INVALID_HANDLE_VALUE;
#else
	static int crashFd = -1;
#endif
	static std::atomic<bool> installed{ false };

	// 十进制/十六进制安全拼接（无malloc）
	static size_t AppendStr(char* buf, size_t cap, const char* s)
	{
		size_t n = 0;
		while (s && s[n] && n < cap) n++;
		size_t m = (n > cap ? cap : n);
		memcpy(buf, s, m);
		return m;
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
		EmergencyWrite(s, strlen(s));
		EmergencyWrite(&nl, 1);
	}

	static void OpenCrashFileOnce()
	{
#if defined(_WIN32)
		if (crashFile != INVALID_HANDLE_VALUE)
		{
			return;
		}

		const std::string path = GB_GetExeDirectory() + GB_STR("GB_Logs/GB_Crash.log");
		// UTF-8 -> UTF-16
		int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), NULL, 0);
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
		// 注意：backtrace() 在 glibc 上不保证 AS-safe，但实践中常用于崩溃处理；
		// backtrace_symbols_fd() 文档标注为 AS-safe，会直接写到 fd。:contentReference[oaicite:7]{index=7}
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

void InstallCrashHandlers()
{
	if (crashlog::installed.exchange(true))
	{
		return;
	}
	crashlog::OpenCrashFileOnce();

	// 层 1：未捕获 C++ 异常
	std::set_terminate([]()
		{
			try
			{
				// 尽量取异常信息；此处仍在正常环境，可调用你的异步日志
				if (auto ep = std::current_exception())
				{
					try
					{
						std::rethrow_exception(ep);
					}
					catch (const std::exception& e)
					{
						GB_Logger::GetInstance().LogFatal(std::string("std::terminate: ") + e.what(), __FILE__, __LINE__);
					}
					catch (...)
					{
						GB_Logger::GetInstance().LogFatal("std::terminate: non-std exception", __FILE__, __LINE__);
					}
				}
				else
				{
					GB_Logger::GetInstance().LogFatal("std::terminate: no current_exception", __FILE__, __LINE__);
				}
			}
			catch (...)
			{
				/* 避免再抛 */
			}

			// 保险：在崩溃环境外也直写一份
			char buf[256]; size_t p = 0;
			p += crashlog::AppendStr(buf + p, sizeof(buf) - p, "FATAL: std::terminate at ");
			p += crashlog::AppendDec(buf + p, sizeof(buf) - p, (uint64_t)time(nullptr));
			p += crashlog::AppendStr(buf + p, sizeof(buf) - p, "\n");
			crashlog::EmergencyWrite(buf, p);

			std::abort(); // 维持标准行为
		});

#if defined(_WIN32)
	// 层 3：未处理 SEH 异常
	SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ex) -> LONG
		{
			char head[256]; size_t p = 0;
			p += crashlog::AppendStr(head + p, sizeof(head) - p, "FATAL: Unhandled SEH, code=0x");
			p += crashlog::AppendDec(head + p, sizeof(head) - p, (uint64_t)ex->ExceptionRecord->ExceptionCode);
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
			memset(&sa, 0, sizeof(sa));
			sa.sa_sigaction = [](int s, siginfo_t* info, void* uctx)
				{
					(void)uctx;
					char head[256]; size_t p = 0;
					p += crashlog::AppendStr(head + p, sizeof(head) - p, "FATAL: Signal ");
					p += crashlog::AppendDec(head + p, sizeof(head) - p, (uint64_t)s);
					p += crashlog::AppendStr(head + p, sizeof(head) - p, " addr=");
					p += crashlog::AppendHexPtr(head + p, sizeof(head) - p, info ? info->si_addr : nullptr);
					p += crashlog::AppendStr(head + p, sizeof(head) - p, " t=");
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
			sigaction(sig, &sa, nullptr); // 参考 signal-safety & sigaction
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

void RemoveCrashHandlers()
{
	// 轻实现：当前仅用于关闭重复安装；详细还原可按平台补充
	crashlog::installed.store(false);
}