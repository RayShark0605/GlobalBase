#include "GB_Logger.h"
#include "GB_FileSystem.h"
#include "GB_IO.h"
#include "GB_Timer.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#  include <windows.h>
#  ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#    define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#  endif
#else
#  include <unistd.h>
#endif

namespace
{
    static bool IsValidLogLevel(GB_LogLevel level)
    {
        const int value = static_cast<int>(level);
        return value >= static_cast<int>(GB_LogLevel::GBLOGLEVEL_TRACE) && value <= static_cast<int>(GB_LogLevel::GBLOGLEVEL_DISABLELOG);
    }

    static bool IsRealLogLevel(GB_LogLevel level)
    {
        const int value = static_cast<int>(level);
        return value >= static_cast<int>(GB_LogLevel::GBLOGLEVEL_TRACE) && value <= static_cast<int>(GB_LogLevel::GBLOGLEVEL_FATAL);
    }

    static std::string TrimAscii(const std::string& textUtf8)
    {
        return GB_Utf8Trim(textUtf8, GB_STR(" \t\r\n"));
    }

#if defined(_WIN32)
    static std::string ToUpperAscii(const std::string& textUtf8)
    {
        std::string result;
        result.reserve(textUtf8.size());
        for (size_t i = 0; i < textUtf8.size(); i++)
        {
            const unsigned char ch = static_cast<unsigned char>(textUtf8[i]);
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

    static bool IsReservedWindowsDeviceName(const std::string& fileNameUtf8)
    {
        std::string baseNameUtf8 = fileNameUtf8;
        const size_t dotPos = baseNameUtf8.find('.');
        if (dotPos != std::string::npos)
        {
            baseNameUtf8 = baseNameUtf8.substr(0, dotPos);
        }

        const std::string upperBaseNameUtf8 = ToUpperAscii(baseNameUtf8);
        if (upperBaseNameUtf8 == GB_STR("CON") || upperBaseNameUtf8 == GB_STR("PRN") || upperBaseNameUtf8 == GB_STR("AUX") || upperBaseNameUtf8 == GB_STR("NUL") || upperBaseNameUtf8 == GB_STR("CONIN$") || upperBaseNameUtf8 == GB_STR("CONOUT$"))
        {
            return true;
        }
        if (upperBaseNameUtf8.size() == 4)
        {
            const std::string prefixUtf8 = upperBaseNameUtf8.substr(0, 3);
            const char indexChar = upperBaseNameUtf8[3];
            if ((prefixUtf8 == GB_STR("COM") || prefixUtf8 == GB_STR("LPT")) && indexChar >= '1' && indexChar <= '9')
            {
                return true;
            }
        }
        return false;
    }
#endif

    static std::string NormalizeLevelText(const std::string& textUtf8)
    {
        std::string result;
        result.reserve(textUtf8.size());
        for (size_t i = 0; i < textUtf8.size(); i++)
        {
            const unsigned char ch = static_cast<unsigned char>(textUtf8[i]);
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '_' || ch == '-')
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

    static void AppendJsonEscaped(std::string& outUtf8, const std::string& textUtf8)
    {
        outUtf8.reserve(outUtf8.size() + textUtf8.size());
        for (size_t i = 0; i < textUtf8.size(); i++)
        {
            const unsigned char ch = static_cast<unsigned char>(textUtf8[i]);
            switch (ch)
            {
            case '"': outUtf8 += GB_STR("\\\""); break;
            case '\\': outUtf8 += GB_STR("\\\\"); break;
            case '\b': outUtf8 += GB_STR("\\b"); break;
            case '\f': outUtf8 += GB_STR("\\f"); break;
            case '\n': outUtf8 += GB_STR("\\n"); break;
            case '\r': outUtf8 += GB_STR("\\r"); break;
            case '\t': outUtf8 += GB_STR("\\t"); break;
            default:
                if (ch < 0x20)
                {
                    static const char* hexChars = "0123456789ABCDEF";
                    outUtf8 += GB_STR("\\u00");
                    outUtf8 += hexChars[(ch >> 4) & 0xF];
                    outUtf8 += hexChars[ch & 0xF];
                }
                else
                {
                    outUtf8 += static_cast<char>(ch);
                }
                break;
            }
        }
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

    static std::string NormalizeFileNameUtf8(const std::string& fileUtf8)
    {
        if (fileUtf8.empty())
        {
            return std::string();
        }

        const std::string normalizedPathUtf8 = GB_Utf8Replace(fileUtf8, GB_STR("\\"), GB_STR("/"));
        std::string fileNameUtf8 = GB_GetFileName(normalizedPathUtf8, true);
        if (fileNameUtf8.empty())
        {
            fileNameUtf8 = normalizedPathUtf8;
        }
        return fileNameUtf8;
    }

    static std::string NormalizeFileNameUtf8(const char* file)
    {
        if (!file)
        {
            return std::string();
        }

#if defined(_WIN32)
        std::string fileUtf8;
        if (!GB_LooksLikeAnsi(file))
        {
            fileUtf8 = file;
        }
        else
        {
            fileUtf8 = GB_AnsiToUtf8(file);
        }
#else
        const std::string fileUtf8 = file;
#endif
        return NormalizeFileNameUtf8(fileUtf8);
    }

    static std::string AddTrailingSlash(const std::string& directoryUtf8)
    {
        if (directoryUtf8.empty())
        {
            return std::string();
        }

        std::string result = GB_Utf8Replace(directoryUtf8, GB_STR("\\"), GB_STR("/"));
        if (!result.empty() && result[result.size() - 1] != '/')
        {
            result += GB_STR("/");
        }
        return result;
    }

    static std::string GetDefaultLogDirectory()
    {
        std::string directoryUtf8 = GB_GetExeDirectory();
        if (directoryUtf8.empty())
        {
            directoryUtf8 = GB_STR("./");
        }
        return AddTrailingSlash(directoryUtf8);
    }

    static bool IsAbsolutePathLike(const std::string& pathUtf8)
    {
        if (pathUtf8.empty())
        {
            return false;
        }
        if (pathUtf8[0] == '/' || pathUtf8[0] == '\\')
        {
            return true;
        }
        if (pathUtf8.size() >= 3)
        {
            const unsigned char drive = static_cast<unsigned char>(pathUtf8[0]);
            const bool hasDrive = ((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z')) && pathUtf8[1] == ':';
            if (hasDrive && (pathUtf8[2] == '/' || pathUtf8[2] == '\\'))
            {
                return true;
            }
        }
        return false;
    }

    static std::string NormalizeLogDirectoryForSet(const std::string& directoryUtf8)
    {
        const std::string trimmedUtf8 = TrimAscii(directoryUtf8);
        if (trimmedUtf8.empty())
        {
            return GetDefaultLogDirectory();
        }

        const std::string slashPathUtf8 = GB_Utf8Replace(trimmedUtf8, GB_STR("\\"), GB_STR("/"));
        std::string normalizedUtf8;
        if (IsAbsolutePathLike(slashPathUtf8))
        {
            normalizedUtf8 = slashPathUtf8;
        }
        else
        {
            normalizedUtf8 = GB_JoinPath(GetDefaultLogDirectory(), slashPathUtf8);
        }
        return AddTrailingSlash(normalizedUtf8);
    }

    static bool HasInvalidLogFileNameChar(const std::string& fileNameUtf8)
    {
        if (fileNameUtf8.empty() || fileNameUtf8 == GB_STR(".") || fileNameUtf8 == GB_STR(".."))
        {
            return true;
        }

        const char lastChar = fileNameUtf8[fileNameUtf8.size() - 1];
        if (lastChar == ' ' || lastChar == '.')
        {
            return true;
        }

        for (size_t i = 0; i < fileNameUtf8.size(); i++)
        {
            const unsigned char ch = static_cast<unsigned char>(fileNameUtf8[i]);
            if (ch < 0x20)
            {
                return true;
            }

            switch (ch)
            {
            case '/':
            case '\\':
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
            case '|':
                return true;
            default:
                break;
            }
        }

#if defined(_WIN32)
        if (IsReservedWindowsDeviceName(fileNameUtf8))
        {
            return true;
        }
#endif

        return false;
    }

    static std::string NormalizeLogFileNameForSet(const std::string& fileNameUtf8)
    {
        std::string result = TrimAscii(fileNameUtf8);
        if (GB_Utf8EndsWith(result, GB_STR(".log"), false))
        {
            result = result.substr(0, result.size() - 4);
            result = TrimAscii(result);
        }
        return result;
    }

    static std::string BuildOutputLogFilePath(const std::string& directoryUtf8, const std::string& fileNameUtf8)
    {
        return AddTrailingSlash(directoryUtf8) + fileNameUtf8 + GB_STR(".log");
    }

    static std::string BuildAllLogFilePath(const std::string& directoryUtf8, const std::string& fileNameUtf8)
    {
        return AddTrailingSlash(directoryUtf8) + fileNameUtf8 + GB_STR("_all.log");
    }

    class LogFileWriter
    {
    public:
        LogFileWriter() = default;
        ~LogFileWriter()
        {
            Close();
        }

        LogFileWriter(const LogFileWriter&) = delete;
        LogFileWriter& operator=(const LogFileWriter&) = delete;

        bool Append(const std::string& filePathUtf8, const std::string& contentUtf8)
        {
            if (contentUtf8.empty())
            {
                return true;
            }
            if (!EnsureOpen(filePathUtf8))
            {
                return false;
            }
            if (!fileStream.WriteBytes(contentUtf8))
            {
                fileStream.Close();
                currentFilePathUtf8.clear();
                return false;
            }
            return true;
        }

        bool Flush()
        {
            if (!fileStream.IsOpen())
            {
                return true;
            }
            return fileStream.Flush();
        }

        void Close()
        {
            if (fileStream.IsOpen())
            {
                (void)fileStream.Flush();
                fileStream.Close();
            }
            currentFilePathUtf8.clear();
        }

    private:
        bool EnsureOpen(const std::string& filePathUtf8)
        {
            if (filePathUtf8.empty())
            {
                return false;
            }
            if (fileStream.IsOpen() && currentFilePathUtf8 == filePathUtf8)
            {
                return true;
            }

            Close();

            GB_FileStreamOpenOptions options;
            options.accessMode = GB_StreamAccessMode::WriteOnly;
            options.openMode = GB_FileStreamOpenMode::OpenAlways;
            options.shareMode = GB_FileShareMode::All;
            options.createParentDirectories = true;
            options.seekToEndAfterOpen = true;
            options.appendMode = true;
            options.sequentialAccessHint = true;
            options.writeThrough = false;

            if (!fileStream.Open(filePathUtf8, options))
            {
                return false;
            }

            currentFilePathUtf8 = filePathUtf8;
            return true;
        }

    private:
        GB_FileStream fileStream;
        std::string currentFilePathUtf8;
    };

    static bool TruncateUtf8NoBom(const std::string& filePathUtf8)
    {
        if (filePathUtf8.empty())
        {
            return false;
        }

        GB_FileStreamOpenOptions options;
        options.accessMode = GB_StreamAccessMode::WriteOnly;
        options.openMode = GB_FileStreamOpenMode::CreateAlways;
        options.shareMode = GB_FileShareMode::All;
        options.createParentDirectories = true;
        options.seekToEndAfterOpen = false;
        options.appendMode = false;
        options.sequentialAccessHint = true;
        options.writeThrough = false;

        GB_FileStream fileStream(filePathUtf8, options);
        if (!fileStream.IsOpen())
        {
            return false;
        }
        const bool flushSuccess = fileStream.Flush();
        fileStream.Close();
        return flushSuccess;
    }

    static const char* GetAnsiColorByLevel(GB_LogLevel level)
    {
        switch (level)
        {
        case GB_LogLevel::GBLOGLEVEL_TRACE: return "\x1b[90m";
        case GB_LogLevel::GBLOGLEVEL_DEBUG: return "\x1b[36m";
        case GB_LogLevel::GBLOGLEVEL_INFO: return "\x1b[32m";
        case GB_LogLevel::GBLOGLEVEL_WARNING: return "\x1b[33m";
        case GB_LogLevel::GBLOGLEVEL_ERROR: return "\x1b[31m";
        case GB_LogLevel::GBLOGLEVEL_FATAL: return "\x1b[35m";
        default: return "\x1b[0m";
        }
    }

    static std::ostream& SelectConsoleStream(GB_LogLevel level)
    {
        return (level >= GB_LogLevel::GBLOGLEVEL_ERROR) ? std::cerr : std::cout;
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
        case GB_LogLevel::GBLOGLEVEL_TRACE: return FOREGROUND_INTENSITY;
        case GB_LogLevel::GBLOGLEVEL_DEBUG: return FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        case GB_LogLevel::GBLOGLEVEL_INFO: return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case GB_LogLevel::GBLOGLEVEL_WARNING: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case GB_LogLevel::GBLOGLEVEL_ERROR: return FOREGROUND_RED | FOREGROUND_INTENSITY;
        case GB_LogLevel::GBLOGLEVEL_FATAL: return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        default: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        }
    }

    static void EnableWinVtOnce()
    {
        static std::once_flag onceFlag;
        std::call_once(onceFlag, []()
            {
                WinConsoleState& state = GetWinConsoleState();
                const HANDLE outHandle = GetStdHandle(STD_OUTPUT_HANDLE);
                const HANDLE errHandle = GetStdHandle(STD_ERROR_HANDLE);

                CONSOLE_SCREEN_BUFFER_INFO info;
                if (outHandle && outHandle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(outHandle, &info))
                {
                    state.defaultAttrOut = info.wAttributes;
                }
                if (errHandle && errHandle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(errHandle, &info))
                {
                    state.defaultAttrErr = info.wAttributes;
                }

                DWORD mode = 0;
                if (outHandle && outHandle != INVALID_HANDLE_VALUE && GetConsoleMode(outHandle, &mode))
                {
                    const DWORD newMode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                    if (SetConsoleMode(outHandle, newMode))
                    {
                        state.vtEnabledOut = true;
                    }
                }
                if (errHandle && errHandle != INVALID_HANDLE_VALUE && GetConsoleMode(errHandle, &mode))
                {
                    const DWORD newMode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                    if (SetConsoleMode(errHandle, newMode))
                    {
                        state.vtEnabledErr = true;
                    }
                }
            });
    }

    static std::wstring Utf8ToWideForConsole(const std::string& textUtf8)
    {
        if (textUtf8.empty())
        {
            return std::wstring();
        }

        int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, textUtf8.c_str(), static_cast<int>(textUtf8.size()), nullptr, 0);
        if (wideLength <= 0)
        {
            wideLength = MultiByteToWideChar(CP_UTF8, 0, textUtf8.c_str(), static_cast<int>(textUtf8.size()), nullptr, 0);
        }
        if (wideLength <= 0)
        {
            return std::wstring();
        }

        std::wstring result(static_cast<size_t>(wideLength), L'\0');
        (void)MultiByteToWideChar(CP_UTF8, 0, textUtf8.c_str(), static_cast<int>(textUtf8.size()), &result[0], wideLength);
        return result;
    }
#endif

    static void ConsoleWriteColoredUtf8(const std::string& textUtf8, GB_LogLevel level)
    {
        static std::mutex consoleMtx;
        std::lock_guard<std::mutex> lock(consoleMtx);

#if defined(_WIN32)
        EnableWinVtOnce();
        WinConsoleState& state = GetWinConsoleState();
        std::ostream& os = SelectConsoleStream(level);
        const bool isErrorStream = (&os == &std::cerr);
        const HANDLE consoleHandle = GetStdHandle(isErrorStream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);

        DWORD mode = 0;
        const bool isConsole = consoleHandle && consoleHandle != INVALID_HANDLE_VALUE && GetConsoleMode(consoleHandle, &mode);
        if (!isConsole)
        {
            os << textUtf8;
            os.flush();
            return;
        }

        auto writeWide = [&](const std::wstring& textWide)
            {
                if (textWide.empty())
                {
                    return;
                }
                DWORD written = 0;
                (void)WriteConsoleW(consoleHandle, textWide.c_str(), static_cast<DWORD>(textWide.size()), &written, nullptr);
            };

        const bool vtEnabled = isErrorStream ? state.vtEnabledErr : state.vtEnabledOut;
        if (vtEnabled)
        {
            std::string coloredUtf8;
            coloredUtf8.reserve(textUtf8.size() + 16);
            coloredUtf8 += GetAnsiColorByLevel(level);
            coloredUtf8 += textUtf8;
            coloredUtf8 += "\x1b[0m";
            writeWide(Utf8ToWideForConsole(coloredUtf8));
        }
        else
        {
            const WORD oldAttr = isErrorStream ? state.defaultAttrErr : state.defaultAttrOut;
            (void)SetConsoleTextAttribute(consoleHandle, GetWinAttrByLevel(level));
            writeWide(Utf8ToWideForConsole(textUtf8));
            (void)SetConsoleTextAttribute(consoleHandle, oldAttr);
        }
#else
        std::ostream& os = SelectConsoleStream(level);
        const int fd = (level >= GB_LogLevel::GBLOGLEVEL_ERROR) ? STDERR_FILENO : STDOUT_FILENO;
        const bool isTty = (::isatty(fd) == 1);
        if (isTty)
        {
            os << GetAnsiColorByLevel(level) << textUtf8 << "\x1b[0m";
        }
        else
        {
            os << textUtf8;
        }
        os.flush();
#endif
    }

    static GB_Logger* gLoggerInstance = nullptr;

    static void ShutdownLoggerAtExit()
    {
        if (gLoggerInstance)
        {
            gLoggerInstance->Shutdown();
        }
    }
}

std::string LogLevelToString(GB_LogLevel level)
{
    switch (level)
    {
    case GB_LogLevel::GBLOGLEVEL_TRACE: return GB_STR("TRACE");
    case GB_LogLevel::GBLOGLEVEL_DEBUG: return GB_STR("DEBUG");
    case GB_LogLevel::GBLOGLEVEL_INFO: return GB_STR("INFO");
    case GB_LogLevel::GBLOGLEVEL_WARNING: return GB_STR("WARNING");
    case GB_LogLevel::GBLOGLEVEL_ERROR: return GB_STR("ERROR");
    case GB_LogLevel::GBLOGLEVEL_FATAL: return GB_STR("FATAL");
    case GB_LogLevel::GBLOGLEVEL_DISABLELOG: return GB_STR("DISABLELOG");
    default: return GB_STR("UNKNOWN");
    }
}

bool GB_ParseLogLevel(const std::string& levelTextUtf8, GB_LogLevel& outLevel)
{
    const std::string valueUtf8 = NormalizeLevelText(levelTextUtf8);
    if (valueUtf8 == GB_STR("TRACE") || valueUtf8 == GB_STR("0"))
    {
        outLevel = GB_LogLevel::GBLOGLEVEL_TRACE;
        return true;
    }
    if (valueUtf8 == GB_STR("DEBUG") || valueUtf8 == GB_STR("1"))
    {
        outLevel = GB_LogLevel::GBLOGLEVEL_DEBUG;
        return true;
    }
    if (valueUtf8 == GB_STR("INFO") || valueUtf8 == GB_STR("2"))
    {
        outLevel = GB_LogLevel::GBLOGLEVEL_INFO;
        return true;
    }
    if (valueUtf8 == GB_STR("WARNING") || valueUtf8 == GB_STR("WARN") || valueUtf8 == GB_STR("3"))
    {
        outLevel = GB_LogLevel::GBLOGLEVEL_WARNING;
        return true;
    }
    if (valueUtf8 == GB_STR("ERROR") || valueUtf8 == GB_STR("4"))
    {
        outLevel = GB_LogLevel::GBLOGLEVEL_ERROR;
        return true;
    }
    if (valueUtf8 == GB_STR("FATAL") || valueUtf8 == GB_STR("5"))
    {
        outLevel = GB_LogLevel::GBLOGLEVEL_FATAL;
        return true;
    }
    if (valueUtf8 == GB_STR("DISABLELOG") || valueUtf8 == GB_STR("DISABLE") || valueUtf8 == GB_STR("OFF") || valueUtf8 == GB_STR("6"))
    {
        outLevel = GB_LogLevel::GBLOGLEVEL_DISABLELOG;
        return true;
    }
    return false;
}

void GB_LogItem::AppendJsonTo(std::string& outUtf8) const
{
    outUtf8 += GB_STR("{");

    outUtf8 += GB_STR("\"ts\":\"");
    AppendJsonEscaped(outUtf8, timestampUtf8);
    outUtf8 += GB_STR("\"");

    outUtf8 += GB_STR(",\"level\":\"");
    outUtf8 += LogLevelToString(level);
    outUtf8 += GB_STR("\"");

    outUtf8 += GB_STR(",\"seq\":");
    outUtf8 += std::to_string(sequenceNumber);

    if (isOutputThreadId)
    {
        outUtf8 += GB_STR(",\"thread\":\"");
        AppendJsonEscaped(outUtf8, threadIdUtf8);
        outUtf8 += GB_STR("\"");
    }

    if (isOutputSourceLocation)
    {
        outUtf8 += GB_STR(",\"file\":\"");
        AppendJsonEscaped(outUtf8, fileNameUtf8);
        outUtf8 += GB_STR("\"");

        outUtf8 += GB_STR(",\"line\":");
        outUtf8 += std::to_string(line);
    }

    outUtf8 += GB_STR(",\"msg\":\"");
    AppendJsonEscaped(outUtf8, messageUtf8);
    outUtf8 += GB_STR("\"}");
    outUtf8 += GB_STR("\n");
}

void GB_LogItem::AppendPlainTextTo(std::string& outUtf8) const
{
    outUtf8 += GB_STR("[");
    outUtf8 += timestampUtf8;
    outUtf8 += GB_STR("] [");
    outUtf8 += LogLevelToString(level);
    outUtf8 += GB_STR("]");

    if (isOutputThreadId)
    {
        outUtf8 += GB_STR(" [TID:");
        outUtf8 += threadIdUtf8;
        outUtf8 += GB_STR("]");
    }

    if (isOutputSourceLocation)
    {
        outUtf8 += GB_STR(" [");
        outUtf8 += fileNameUtf8;
        outUtf8 += GB_STR(":");
        outUtf8 += std::to_string(line);
        outUtf8 += GB_STR("]");
    }

    outUtf8 += GB_STR(" ");
    outUtf8 += messageUtf8;
    outUtf8 += GB_STR("\n");
}

std::string GB_LogItem::ToJsonString() const
{
    std::string outUtf8;
    outUtf8.reserve(96 + timestampUtf8.size() + messageUtf8.size() + threadIdUtf8.size() + fileNameUtf8.size());
    AppendJsonTo(outUtf8);
    return outUtf8;
}

std::string GB_LogItem::ToPlainTextString() const
{
    std::string outUtf8;
    outUtf8.reserve(96 + timestampUtf8.size() + messageUtf8.size() + threadIdUtf8.size() + fileNameUtf8.size());
    AppendPlainTextTo(outUtf8);
    return outUtf8;
}

GB_Logger& GB_Logger::GetInstance()
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, []()
        {
            gLoggerInstance = new GB_Logger();
            std::atexit(&ShutdownLoggerAtExit);
        });
    return *gLoggerInstance;
}

GB_Logger::GB_Logger()
{
    logDirectoryUtf8 = GetDefaultLogDirectory();
    logFileNameUtf8 = GB_STR("GBLog");
}

GB_Logger::~GB_Logger()
{
    Shutdown();
}

bool GB_Logger::EnsureWorkerStarted()
{
    if (hasShutdown.load(std::memory_order_acquire))
    {
        return false;
    }
    if (isWorkerStarted.load(std::memory_order_acquire))
    {
        // 线程对象已经创建但 worker 可能尚未进入 LogThreadFunc，此时 isStop 仍为 false，仍视为可用。
        // 如果 worker 因异常退出，则 LogThreadFunc 会设置 isStop=true/isWorkerRunning=false，此后不再静默接受日志。
        return !isStop.load(std::memory_order_acquire) || isWorkerRunning.load(std::memory_order_acquire);
    }

    std::lock_guard<std::mutex> lock(workerMtx);
    if (hasShutdown.load(std::memory_order_acquire))
    {
        return false;
    }
    if (isWorkerStarted.load(std::memory_order_acquire))
    {
        return !isStop.load(std::memory_order_acquire) || isWorkerRunning.load(std::memory_order_acquire);
    }

    try
    {
        isStop.store(false, std::memory_order_release);
        logThread = std::thread(&GB_Logger::LogThreadFunc, this);
        isWorkerStarted.store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        isLogEnabled.store(false, std::memory_order_release);
        isWorkerStarted.store(false, std::memory_order_release);
        isWorkerRunning.store(false, std::memory_order_release);
        isStop.store(true, std::memory_order_release);
        return false;
    }
}

bool GB_Logger::ShouldWriteToFilteredTargets(GB_LogLevel level) const
{
    if (!IsRealLogLevel(level))
    {
        return false;
    }

    const GB_LogLevel filterLevel = static_cast<GB_LogLevel>(filterLevelInt.load(std::memory_order_relaxed));
    if (!IsRealLogLevel(filterLevel))
    {
        return false;
    }
    return level >= filterLevel;
}

GB_LogItem GB_Logger::BuildLogItem(GB_LogLevel level, const std::string& msgUtf8, const std::string& fileUtf8, int line)
{
    const bool outputThreadId = isOutputThreadId.load(std::memory_order_relaxed);
    const bool outputSourceLocation = isOutputSourceLocation.load(std::memory_order_relaxed);

    GB_LogItem logItem;
    logItem.timestampUtf8 = GetLocalTimeStr();
    logItem.level = level;
    logItem.messageUtf8 = msgUtf8;
    logItem.isOutputThreadId = outputThreadId;
    logItem.isOutputSourceLocation = outputSourceLocation;
    logItem.isOutputToFilteredTargets = ShouldWriteToFilteredTargets(level);

    if (outputThreadId)
    {
        logItem.threadIdUtf8 = GetThreadIdString();
    }
    if (outputSourceLocation)
    {
        logItem.fileNameUtf8 = NormalizeFileNameUtf8(fileUtf8);
        logItem.line = line;
    }

    return logItem;
}

void GB_Logger::RecordDroppedLogItemLocked()
{
    const uint64_t sequenceNumber = nextSequenceNumber.fetch_add(1, std::memory_order_relaxed) + 1;
    pendingDroppedLogCount++;
    if (sequenceNumber > pendingDroppedSequenceNumber)
    {
        pendingDroppedSequenceNumber = sequenceNumber;
    }
    totalDroppedLogCount.fetch_add(1, std::memory_order_relaxed);
}

bool GB_Logger::TakePendingDroppedLogItemsLocked(uint64_t& droppedCount, uint64_t& droppedSequenceNumber)
{
    droppedCount = pendingDroppedLogCount;
    droppedSequenceNumber = pendingDroppedSequenceNumber;
    pendingDroppedLogCount = 0;
    pendingDroppedSequenceNumber = 0;
    return droppedCount > 0;
}

bool GB_Logger::IsCurrentWorkerThread() const
{
    std::lock_guard<std::mutex> lock(workerMtx);
    return logThread.joinable() && logThread.get_id() == std::this_thread::get_id();
}

bool GB_Logger::EnqueueLogItem(GB_LogItem&& logItem)
{
    std::unique_lock<std::mutex> lock(logQueueMtx);
    if (isStop.load(std::memory_order_acquire) || hasShutdown.load(std::memory_order_acquire))
    {
        return false;
    }

    const std::size_t maxCount = maxPendingLogItems.load(std::memory_order_relaxed);
    if (maxCount > 0 && logQueue.size() >= maxCount)
    {
        RecordDroppedLogItemLocked();
        return false;
    }

    logItem.sequenceNumber = nextSequenceNumber.fetch_add(1, std::memory_order_relaxed) + 1;
    logQueue.push_back(std::move(logItem));
    return true;
}

void GB_Logger::Log(GB_LogLevel level, const std::string& msgUtf8, const std::string& fileUtf8, int line)
{
    if (!isLogEnabled.load(std::memory_order_relaxed) || !IsRealLogLevel(level))
    {
        return;
    }
    if (!EnsureWorkerStarted())
    {
        return;
    }

    GB_LogItem logItem = BuildLogItem(level, msgUtf8, fileUtf8, line);
    (void)EnqueueLogItem(std::move(logItem));
    logQueueCv.notify_one();
}

void GB_Logger::LogChecked(GB_LogLevel level, const std::string& msgUtf8, const char* file, int line)
{
    if (!isLogEnabled.load(std::memory_order_relaxed) || !IsRealLogLevel(level))
    {
        return;
    }
    if (!EnsureWorkerStarted())
    {
        return;
    }

    const bool outputThreadId = isOutputThreadId.load(std::memory_order_relaxed);
    const bool outputSourceLocation = isOutputSourceLocation.load(std::memory_order_relaxed);

    GB_LogItem logItem;
    logItem.timestampUtf8 = GetLocalTimeStr();
    logItem.level = level;
    logItem.messageUtf8 = msgUtf8;
    logItem.isOutputThreadId = outputThreadId;
    logItem.isOutputSourceLocation = outputSourceLocation;
    logItem.isOutputToFilteredTargets = ShouldWriteToFilteredTargets(level);

    if (outputThreadId)
    {
        logItem.threadIdUtf8 = GetThreadIdString();
    }
    if (outputSourceLocation)
    {
        logItem.fileNameUtf8 = NormalizeFileNameUtf8(file);
        logItem.line = line;
    }

    (void)EnqueueLogItem(std::move(logItem));
    logQueueCv.notify_one();
}

void GB_Logger::LogTrace(const std::string& msgUtf8, const char* file, int line)
{
    LogChecked(GB_LogLevel::GBLOGLEVEL_TRACE, msgUtf8, file, line);
}

void GB_Logger::LogDebug(const std::string& msgUtf8, const char* file, int line)
{
    LogChecked(GB_LogLevel::GBLOGLEVEL_DEBUG, msgUtf8, file, line);
}

void GB_Logger::LogInfo(const std::string& msgUtf8, const char* file, int line)
{
    LogChecked(GB_LogLevel::GBLOGLEVEL_INFO, msgUtf8, file, line);
}

void GB_Logger::LogWarning(const std::string& msgUtf8, const char* file, int line)
{
    LogChecked(GB_LogLevel::GBLOGLEVEL_WARNING, msgUtf8, file, line);
}

void GB_Logger::LogError(const std::string& msgUtf8, const char* file, int line)
{
    LogChecked(GB_LogLevel::GBLOGLEVEL_ERROR, msgUtf8, file, line);
}

void GB_Logger::LogFatal(const std::string& msgUtf8, const char* file, int line)
{
    LogChecked(GB_LogLevel::GBLOGLEVEL_FATAL, msgUtf8, file, line);
}

bool GB_Logger::SetLogEnabled(bool enable)
{
    if (hasShutdown.load(std::memory_order_acquire))
    {
        return false;
    }
    if (enable && !EnsureWorkerStarted())
    {
        return false;
    }

    isLogEnabled.store(enable, std::memory_order_release);
    return true;
}

bool GB_Logger::IsLogEnabled() const
{
    return isLogEnabled.load(std::memory_order_relaxed);
}

bool GB_Logger::SetLogToConsole(bool enable)
{
    isLogToConsole.store(enable, std::memory_order_release);
    return true;
}

bool GB_Logger::IsLogToConsole() const
{
    return isLogToConsole.load(std::memory_order_relaxed);
}

bool GB_Logger::SetLogFilterLevel(GB_LogLevel level)
{
    if (!IsValidLogLevel(level))
    {
        return false;
    }
    filterLevelInt.store(static_cast<int>(level), std::memory_order_release);
    return true;
}

GB_LogLevel GB_Logger::GetLogFilterLevel() const
{
    return static_cast<GB_LogLevel>(filterLevelInt.load(std::memory_order_relaxed));
}

bool GB_Logger::SetLogDirectory(const std::string& directoryUtf8)
{
    const std::string normalizedDirectoryUtf8 = NormalizeLogDirectoryForSet(directoryUtf8);
    if (normalizedDirectoryUtf8.empty())
    {
        return false;
    }
    if (!GB_CreateDirectory(normalizedDirectoryUtf8))
    {
        return false;
    }

    (void)Flush();

    std::lock_guard<std::mutex> lock(settingsMtx);
    logDirectoryUtf8 = normalizedDirectoryUtf8;
    return true;
}

std::string GB_Logger::GetLogDirectory() const
{
    std::lock_guard<std::mutex> lock(settingsMtx);
    return logDirectoryUtf8;
}

bool GB_Logger::SetLogFileName(const std::string& fileNameUtf8)
{
    const std::string normalizedFileNameUtf8 = NormalizeLogFileNameForSet(fileNameUtf8);
    if (normalizedFileNameUtf8.empty() || HasInvalidLogFileNameChar(normalizedFileNameUtf8))
    {
        return false;
    }

    (void)Flush();

    std::lock_guard<std::mutex> lock(settingsMtx);
    logFileNameUtf8 = normalizedFileNameUtf8;
    return true;
}

std::string GB_Logger::GetLogFileName() const
{
    std::lock_guard<std::mutex> lock(settingsMtx);
    return logFileNameUtf8;
}

bool GB_Logger::SetOutputThreadId(bool enable)
{
    isOutputThreadId.store(enable, std::memory_order_release);
    return true;
}

bool GB_Logger::IsOutputThreadId() const
{
    return isOutputThreadId.load(std::memory_order_relaxed);
}

bool GB_Logger::SetOutputSourceLocation(bool enable)
{
    isOutputSourceLocation.store(enable, std::memory_order_release);
    return true;
}

bool GB_Logger::IsOutputSourceLocation() const
{
    return isOutputSourceLocation.load(std::memory_order_relaxed);
}

bool GB_Logger::SetMaxPendingLogItems(std::size_t maxPendingLogItemsValue)
{
    maxPendingLogItems.store(maxPendingLogItemsValue, std::memory_order_release);
    return true;
}

std::size_t GB_Logger::GetMaxPendingLogItems() const
{
    return maxPendingLogItems.load(std::memory_order_relaxed);
}

GB_LoggerOptions GB_Logger::GetOptions() const
{
    GB_LoggerOptions options;
    options.isLogEnabled = isLogEnabled.load(std::memory_order_relaxed);
    options.isLogToConsole = isLogToConsole.load(std::memory_order_relaxed);
    options.filterLevel = static_cast<GB_LogLevel>(filterLevelInt.load(std::memory_order_relaxed));
    options.isOutputThreadId = isOutputThreadId.load(std::memory_order_relaxed);
    options.isOutputSourceLocation = isOutputSourceLocation.load(std::memory_order_relaxed);
    options.maxPendingLogItems = maxPendingLogItems.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(settingsMtx);
        options.logDirectoryUtf8 = logDirectoryUtf8;
        options.logFileNameUtf8 = logFileNameUtf8;
    }
    return options;
}

bool GB_Logger::SetOptions(const GB_LoggerOptions& options)
{
    if (!IsValidLogLevel(options.filterLevel))
    {
        return false;
    }

    const std::string normalizedDirectoryUtf8 = NormalizeLogDirectoryForSet(options.logDirectoryUtf8);
    const std::string normalizedFileNameUtf8 = NormalizeLogFileNameForSet(options.logFileNameUtf8.empty() ? GB_STR("GBLog") : options.logFileNameUtf8);
    if (normalizedDirectoryUtf8.empty() || normalizedFileNameUtf8.empty() || HasInvalidLogFileNameChar(normalizedFileNameUtf8))
    {
        return false;
    }
    if (!GB_CreateDirectory(normalizedDirectoryUtf8))
    {
        return false;
    }

    (void)Flush();

    {
        std::lock_guard<std::mutex> lock(settingsMtx);
        logDirectoryUtf8 = normalizedDirectoryUtf8;
        logFileNameUtf8 = normalizedFileNameUtf8;
    }

    isLogToConsole.store(options.isLogToConsole, std::memory_order_release);
    filterLevelInt.store(static_cast<int>(options.filterLevel), std::memory_order_release);
    isOutputThreadId.store(options.isOutputThreadId, std::memory_order_release);
    isOutputSourceLocation.store(options.isOutputSourceLocation, std::memory_order_release);
    maxPendingLogItems.store(options.maxPendingLogItems, std::memory_order_release);

    if (options.isLogEnabled && !EnsureWorkerStarted())
    {
        isLogEnabled.store(false, std::memory_order_release);
        return false;
    }

    isLogEnabled.store(options.isLogEnabled, std::memory_order_release);
    return true;
}

std::string GB_Logger::GetOutputLogFilePath() const
{
    std::lock_guard<std::mutex> lock(settingsMtx);
    return BuildOutputLogFilePath(logDirectoryUtf8, logFileNameUtf8);
}

std::string GB_Logger::GetAllLogFilePath() const
{
    std::lock_guard<std::mutex> lock(settingsMtx);
    return BuildAllLogFilePath(logDirectoryUtf8, logFileNameUtf8);
}

bool GB_Logger::ClearOutputLogFile()
{
    if (!Flush())
    {
        return false;
    }

    const std::string outputLogFilePathUtf8 = GetOutputLogFilePath();
    std::lock_guard<std::mutex> lock(fileIoMtx);
    return TruncateUtf8NoBom(outputLogFilePathUtf8);
}

bool GB_Logger::ClearAllLogFile()
{
    if (!Flush())
    {
        return false;
    }

    const std::string allLogFilePathUtf8 = GetAllLogFilePath();
    std::lock_guard<std::mutex> lock(fileIoMtx);
    return TruncateUtf8NoBom(allLogFilePathUtf8);
}

bool GB_Logger::ClearLogFiles()
{
    if (!Flush())
    {
        return false;
    }

    const std::string outputLogFilePathUtf8 = GetOutputLogFilePath();
    const std::string allLogFilePathUtf8 = GetAllLogFilePath();
    std::lock_guard<std::mutex> lock(fileIoMtx);
    const bool success1 = TruncateUtf8NoBom(outputLogFilePathUtf8);
    const bool success2 = TruncateUtf8NoBom(allLogFilePathUtf8);
    return success1 && success2;
}

bool GB_Logger::Flush()
{
    if (IsCurrentWorkerThread())
    {
        return true;
    }

    const uint64_t targetSequenceNumber = nextSequenceNumber.load(std::memory_order_acquire);
    if (targetSequenceNumber == 0 || completedSequenceNumber.load(std::memory_order_acquire) >= targetSequenceNumber)
    {
        return true;
    }
    if (!isWorkerStarted.load(std::memory_order_acquire))
    {
        return false;
    }

    logQueueCv.notify_one();
    std::unique_lock<std::mutex> lock(flushMtx);
    flushCv.wait(lock, [this, targetSequenceNumber]()
        {
            return completedSequenceNumber.load(std::memory_order_acquire) >= targetSequenceNumber || (isStop.load(std::memory_order_acquire) && !isWorkerRunning.load(std::memory_order_acquire));
        });
    return completedSequenceNumber.load(std::memory_order_acquire) >= targetSequenceNumber;
}

bool GB_Logger::FlushFor(unsigned int timeoutMs)
{
    if (IsCurrentWorkerThread())
    {
        return true;
    }

    const uint64_t targetSequenceNumber = nextSequenceNumber.load(std::memory_order_acquire);
    if (targetSequenceNumber == 0 || completedSequenceNumber.load(std::memory_order_acquire) >= targetSequenceNumber)
    {
        return true;
    }
    if (!isWorkerStarted.load(std::memory_order_acquire))
    {
        return false;
    }

    logQueueCv.notify_one();
    std::unique_lock<std::mutex> lock(flushMtx);
    const bool success = flushCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this, targetSequenceNumber]()
        {
            return completedSequenceNumber.load(std::memory_order_acquire) >= targetSequenceNumber || (isStop.load(std::memory_order_acquire) && !isWorkerRunning.load(std::memory_order_acquire));
        });
    return success && completedSequenceNumber.load(std::memory_order_acquire) >= targetSequenceNumber;
}

uint64_t GB_Logger::GetDroppedLogCount() const
{
    return totalDroppedLogCount.load(std::memory_order_relaxed);
}

uint64_t GB_Logger::GetWriteFailureCount() const
{
    return totalWriteFailureCount.load(std::memory_order_relaxed);
}

void GB_Logger::Shutdown()
{
    if (hasShutdown.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    isLogEnabled.store(false, std::memory_order_release);
    isStop.store(true, std::memory_order_release);
    logQueueCv.notify_all();

    {
        std::lock_guard<std::mutex> lock(workerMtx);
        if (logThread.joinable())
        {
            if (logThread.get_id() == std::this_thread::get_id())
            {
                logThread.detach();
            }
            else
            {
                logThread.join();
            }
        }
    }

    flushCv.notify_all();
}

void GB_Logger::LogThreadFunc()
{
    isWorkerRunning.store(true, std::memory_order_release);

    LogFileWriter allLogWriter;
    LogFileWriter outputLogWriter;

    try
    {
        for (;;)
        {
            std::deque<GB_LogItem> localQueue;
            uint64_t droppedCount = 0;
            uint64_t droppedSequenceNumber = 0;
            bool hasDroppedLogItems = false;
            {
                std::unique_lock<std::mutex> lock(logQueueMtx);
                logQueueCv.wait(lock, [this]()
                    {
                        return isStop.load(std::memory_order_acquire) || !logQueue.empty() || pendingDroppedLogCount > 0;
                    });

                if (logQueue.empty() && pendingDroppedLogCount == 0 && isStop.load(std::memory_order_acquire))
                {
                    break;
                }

                localQueue.swap(logQueue);
                hasDroppedLogItems = TakePendingDroppedLogItemsLocked(droppedCount, droppedSequenceNumber);
            }

            std::string logDirectorySnapshotUtf8;
            std::string logFileNameSnapshotUtf8;
            {
                std::lock_guard<std::mutex> lock(settingsMtx);
                logDirectorySnapshotUtf8 = logDirectoryUtf8;
                logFileNameSnapshotUtf8 = logFileNameUtf8;
            }

            const std::string allLogFilePathUtf8 = BuildAllLogFilePath(logDirectorySnapshotUtf8, logFileNameSnapshotUtf8);
            const std::string outputLogFilePathUtf8 = BuildOutputLogFilePath(logDirectorySnapshotUtf8, logFileNameSnapshotUtf8);
            const bool outputToConsole = isLogToConsole.load(std::memory_order_relaxed);

            std::string allBatchUtf8;
            std::string outputBatchUtf8;
            static const size_t maxBatchBytes = 4 * 1024 * 1024;
            const size_t batchReserveCount = localQueue.empty() ? 1 : localQueue.size();
            const size_t allReserveBytes = batchReserveCount > maxBatchBytes / static_cast<size_t>(192) ? maxBatchBytes : batchReserveCount * static_cast<size_t>(192);
            const size_t outputReserveBytes = batchReserveCount > maxBatchBytes / static_cast<size_t>(160) ? maxBatchBytes : batchReserveCount * static_cast<size_t>(160);
            allBatchUtf8.reserve(allReserveBytes);
            outputBatchUtf8.reserve(outputReserveBytes);

            uint64_t maxCompletedSequenceNumber = completedSequenceNumber.load(std::memory_order_relaxed);

            auto flushBatches = [&]()
                {
                    std::lock_guard<std::mutex> lock(fileIoMtx);
                    bool success = true;
                    if (!allBatchUtf8.empty())
                    {
                        success = allLogWriter.Append(allLogFilePathUtf8, allBatchUtf8) && success;
                        allBatchUtf8.clear();
                    }
                    if (!outputBatchUtf8.empty())
                    {
                        success = outputLogWriter.Append(outputLogFilePathUtf8, outputBatchUtf8) && success;
                        outputBatchUtf8.clear();
                    }
                    success = allLogWriter.Flush() && success;
                    success = outputLogWriter.Flush() && success;
                    if (!success)
                    {
                        totalWriteFailureCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    return success;
                };

            std::string plainTextUtf8;
            plainTextUtf8.reserve(256);

            while (!localQueue.empty())
            {
                GB_LogItem logItem = std::move(localQueue.front());
                localQueue.pop_front();

                logItem.AppendJsonTo(allBatchUtf8);
                if (logItem.isOutputToFilteredTargets)
                {
                    if (outputToConsole)
                    {
                        plainTextUtf8.clear();
                        const size_t reserveGuess = 96 + logItem.timestampUtf8.size() + logItem.messageUtf8.size() + logItem.threadIdUtf8.size() + logItem.fileNameUtf8.size();
                        if (plainTextUtf8.capacity() < reserveGuess)
                        {
                            plainTextUtf8.reserve(reserveGuess);
                        }
                        logItem.AppendPlainTextTo(plainTextUtf8);
                        outputBatchUtf8 += plainTextUtf8;
                        ConsoleWriteColoredUtf8(plainTextUtf8, logItem.level);
                    }
                    else
                    {
                        logItem.AppendPlainTextTo(outputBatchUtf8);
                    }
                }

                if (logItem.sequenceNumber > maxCompletedSequenceNumber)
                {
                    maxCompletedSequenceNumber = logItem.sequenceNumber;
                }

                if (allBatchUtf8.size() >= maxBatchBytes || outputBatchUtf8.size() >= maxBatchBytes)
                {
                    (void)flushBatches();
                }
            }

            if (hasDroppedLogItems)
            {
                GB_LogItem droppedItem;
                droppedItem.sequenceNumber = droppedSequenceNumber;
                droppedItem.timestampUtf8 = GetLocalTimeStr();
                droppedItem.level = GB_LogLevel::GBLOGLEVEL_WARNING;
                droppedItem.messageUtf8 = GB_STR("Dropped ") + std::to_string(droppedCount) + GB_STR(" log items because logger queue was full.");
                droppedItem.threadIdUtf8 = GB_STR("GB_Logger");
                droppedItem.fileNameUtf8 = GB_STR("GB_Logger.cpp");
                droppedItem.line = 0;
                droppedItem.isOutputThreadId = true;
                droppedItem.isOutputSourceLocation = true;
                droppedItem.isOutputToFilteredTargets = ShouldWriteToFilteredTargets(droppedItem.level);

                droppedItem.AppendJsonTo(allBatchUtf8);
                if (droppedItem.sequenceNumber > maxCompletedSequenceNumber)
                {
                    maxCompletedSequenceNumber = droppedItem.sequenceNumber;
                }
                if (droppedItem.isOutputToFilteredTargets)
                {
                    if (outputToConsole)
                    {
                        plainTextUtf8.clear();
                        droppedItem.AppendPlainTextTo(plainTextUtf8);
                        outputBatchUtf8 += plainTextUtf8;
                        ConsoleWriteColoredUtf8(plainTextUtf8, droppedItem.level);
                    }
                    else
                    {
                        droppedItem.AppendPlainTextTo(outputBatchUtf8);
                    }
                }
            }

            (void)flushBatches();
            completedSequenceNumber.store(maxCompletedSequenceNumber, std::memory_order_release);
            flushCv.notify_all();
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

    {
        std::lock_guard<std::mutex> lock(fileIoMtx);
        allLogWriter.Close();
        outputLogWriter.Close();
    }

    isStop.store(true, std::memory_order_release);
    isWorkerRunning.store(false, std::memory_order_release);
    flushCv.notify_all();
}

bool GB_IsLogEnabled()
{
    return GB_Logger::GetInstance().IsLogEnabled();
}

bool GB_SetLogEnabled(bool enable)
{
    return GB_Logger::GetInstance().SetLogEnabled(enable);
}

bool GB_IsLogToConsole()
{
    return GB_Logger::GetInstance().IsLogToConsole();
}

bool GB_SetLogToConsole(bool enable)
{
    return GB_Logger::GetInstance().SetLogToConsole(enable);
}

GB_LogLevel GB_GetLogFilterLevel()
{
    return GB_Logger::GetInstance().GetLogFilterLevel();
}

bool GB_SetLogFilterLevel(GB_LogLevel level)
{
    return GB_Logger::GetInstance().SetLogFilterLevel(level);
}

bool GB_CheckLogLevel(GB_LogLevel level)
{
    GB_Logger& logger = GB_Logger::GetInstance();
    if (!logger.IsLogEnabled() || !IsRealLogLevel(level))
    {
        return false;
    }

    const GB_LogLevel filterLevel = logger.GetLogFilterLevel();
    return IsRealLogLevel(filterLevel) && level >= filterLevel;
}

std::string GB_GetLogDirectory()
{
    return GB_Logger::GetInstance().GetLogDirectory();
}

bool GB_SetLogDirectory(const std::string& directoryUtf8)
{
    return GB_Logger::GetInstance().SetLogDirectory(directoryUtf8);
}

std::string GB_GetLogFileName()
{
    return GB_Logger::GetInstance().GetLogFileName();
}

bool GB_SetLogFileName(const std::string& fileNameUtf8)
{
    return GB_Logger::GetInstance().SetLogFileName(fileNameUtf8);
}

bool GB_IsLogOutputThreadId()
{
    return GB_Logger::GetInstance().IsOutputThreadId();
}

bool GB_SetLogOutputThreadId(bool enable)
{
    return GB_Logger::GetInstance().SetOutputThreadId(enable);
}

bool GB_IsLogOutputSourceLocation()
{
    return GB_Logger::GetInstance().IsOutputSourceLocation();
}

bool GB_SetLogOutputSourceLocation(bool enable)
{
    return GB_Logger::GetInstance().SetOutputSourceLocation(enable);
}

std::size_t GB_GetMaxPendingLogItems()
{
    return GB_Logger::GetInstance().GetMaxPendingLogItems();
}

bool GB_SetMaxPendingLogItems(std::size_t maxPendingLogItems)
{
    return GB_Logger::GetInstance().SetMaxPendingLogItems(maxPendingLogItems);
}

GB_LoggerOptions GB_GetLoggerOptions()
{
    return GB_Logger::GetInstance().GetOptions();
}

bool GB_SetLoggerOptions(const GB_LoggerOptions& options)
{
    return GB_Logger::GetInstance().SetOptions(options);
}

std::string GB_GetOutputLogFilePath()
{
    return GB_Logger::GetInstance().GetOutputLogFilePath();
}

std::string GB_GetAllLogFilePath()
{
    return GB_Logger::GetInstance().GetAllLogFilePath();
}

bool GB_ClearOutputLogFile()
{
    return GB_Logger::GetInstance().ClearOutputLogFile();
}

bool GB_ClearAllLogFile()
{
    return GB_Logger::GetInstance().ClearAllLogFile();
}

bool GB_ClearLogFiles()
{
    return GB_Logger::GetInstance().ClearLogFiles();
}

bool GB_FlushLog()
{
    return GB_Logger::GetInstance().Flush();
}

bool GB_FlushLogFor(unsigned int timeoutMs)
{
    return GB_Logger::GetInstance().FlushFor(timeoutMs);
}

uint64_t GB_GetDroppedLogCount()
{
    return GB_Logger::GetInstance().GetDroppedLogCount();
}

uint64_t GB_GetLogWriteFailureCount()
{
    return GB_Logger::GetInstance().GetWriteFailureCount();
}

void GB_ShutdownLogger()
{
    GB_Logger::GetInstance().Shutdown();
}
