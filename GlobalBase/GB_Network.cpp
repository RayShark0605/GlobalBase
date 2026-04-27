#include "GB_Network.h"
#include "GB_Utf8String.h"
#include "GB_FileSystem.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <winhttp.h>
#  pragma comment(lib, "Ws2_32.lib")
#  pragma comment(lib, "winhttp.lib")
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netdb.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <arpa/inet.h>
#endif

#include <curl/curl.h>
#include <atomic>
#include <thread>

namespace
{
#ifdef _WIN32
    class WsaStartupGuard
    {
    public:
        WsaStartupGuard()
        {
            WSADATA wsaData;
            m_ok = (::WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
        }

        ~WsaStartupGuard()
        {
            if (m_ok)
            {
                ::WSACleanup();
            }
        }

        bool IsOk() const
        {
            return m_ok;
        }

    private:
        bool m_ok = false;
    };

    using SocketHandle = SOCKET;
    static const SocketHandle kInvalidSocket = INVALID_SOCKET;

    static void CloseSocket(SocketHandle socketHandle)
    {
        if (socketHandle != kInvalidSocket)
        {
            ::closesocket(socketHandle);
        }
    }

    static bool SetNonBlocking(SocketHandle socketHandle, bool nonBlocking)
    {
        u_long mode = nonBlocking ? 1UL : 0UL;
        return (::ioctlsocket(socketHandle, FIONBIO, &mode) == 0);
    }

    static int GetLastSocketError()
    {
        return ::WSAGetLastError();
    }
#else
    using SocketHandle = int;
    static const SocketHandle kInvalidSocket = -1;

    static void CloseSocket(SocketHandle socketHandle)
    {
        if (socketHandle >= 0)
        {
            ::close(socketHandle);
        }
    }

    static bool SetNonBlocking(SocketHandle socketHandle, bool nonBlocking)
    {
        const int flags = ::fcntl(socketHandle, F_GETFL, 0);
        if (flags < 0)
        {
            return false;
        }

        int newFlags = flags;
        if (nonBlocking)
        {
            newFlags |= O_NONBLOCK;
        }
        else
        {
            newFlags &= ~O_NONBLOCK;
        }

        return (::fcntl(socketHandle, F_SETFL, newFlags) == 0);
    }

    static int GetLastSocketError()
    {
        return errno;
    }
#endif

    static bool IsNumericHost(const std::string& hostUtf8)
    {
        if (hostUtf8.empty())
        {
            return false;
        }

#ifdef _WIN32
        IN_ADDR addr4;
        if (::InetPtonA(AF_INET, hostUtf8.c_str(), &addr4) == 1)
        {
            return true;
        }

        IN6_ADDR addr6;
        if (::InetPtonA(AF_INET6, hostUtf8.c_str(), &addr6) == 1)
        {
            return true;
        }

        return false;
#else
        in_addr addr4;
        if (::inet_pton(AF_INET, hostUtf8.c_str(), &addr4) == 1)
        {
            return true;
        }

        in6_addr addr6;
        if (::inet_pton(AF_INET6, hostUtf8.c_str(), &addr6) == 1)
        {
            return true;
        }

        return false;
#endif
    }

    static bool WaitForConnect(SocketHandle socketHandle, unsigned int timeoutMs)
    {
        if (timeoutMs == 0)
        {
            return false;
        }

        const auto startTime = std::chrono::steady_clock::now();

        while (true)
        {
            const auto nowTime = std::chrono::steady_clock::now();
            const auto elapsedMs = static_cast<unsigned int>(std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - startTime).count());
            if (elapsedMs >= timeoutMs)
            {
                return false;
            }

            const unsigned int remainingMs = timeoutMs - elapsedMs;

            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(socketHandle, &writeSet);

            fd_set exceptSet;
            FD_ZERO(&exceptSet);
            FD_SET(socketHandle, &exceptSet);

            timeval tv;
            tv.tv_sec = static_cast<long>(remainingMs / 1000);
            tv.tv_usec = static_cast<long>((remainingMs % 1000) * 1000);

#ifdef _WIN32
            const int selectResult = ::select(0, nullptr, &writeSet, &exceptSet, &tv);
#else
            const int selectResult = ::select(socketHandle + 1, nullptr, &writeSet, &exceptSet, &tv);
#endif
            if (selectResult > 0)
            {
                break;
            }

            if (selectResult == 0)
            {
                return false;
            }

#ifdef _WIN32
            const int lastError = ::WSAGetLastError();
            if (lastError == WSAEINTR)
            {
                continue;
            }
#else
            if (errno == EINTR)
            {
                continue;
            }
#endif
            return false;
        }

        int soError = 0;
#ifdef _WIN32
        int optLen = static_cast<int>(sizeof(soError));
        const int result = ::getsockopt(socketHandle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &optLen);
        if (result != 0)
        {
            return false;
        }
#else
        socklen_t optLen = static_cast<socklen_t>(sizeof(soError));
        const int result = ::getsockopt(socketHandle, SOL_SOCKET, SO_ERROR, &soError, &optLen);
        if (result != 0)
        {
            return false;
        }
#endif

        return (soError == 0);
    }

    static bool ConnectTcpWithTimeout(const std::string& hostUtf8, unsigned short port, unsigned int timeoutMs)
    {
        if (timeoutMs == 0)
        {
            return false;
        }

        const std::string portString = std::to_string(static_cast<unsigned int>(port));

        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        const bool isNumericHost = IsNumericHost(hostUtf8);

#ifdef AI_NUMERICHOST
        if (isNumericHost)
        {
            hints.ai_flags |= AI_NUMERICHOST;
        }
#endif

#ifdef AI_ADDRCONFIG
        if (!isNumericHost)
        {
            hints.ai_flags |= AI_ADDRCONFIG;
        }
#endif

        addrinfo* results = nullptr;
        const int gaiResult = ::getaddrinfo(hostUtf8.c_str(), portString.c_str(), &hints, &results);
        if (gaiResult != 0 || results == nullptr)
        {
            return false;
        }

        struct AddrInfoGuard
        {
            addrinfo* ptr = nullptr;
            ~AddrInfoGuard()
            {
                if (ptr != nullptr)
                {
                    ::freeaddrinfo(ptr);
                    ptr = nullptr;
                }
            }
        };

        AddrInfoGuard resultsGuard;
        resultsGuard.ptr = results;

        const auto startTime = std::chrono::steady_clock::now();

        for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next)
        {
            const auto nowTime = std::chrono::steady_clock::now();
            const auto elapsedMs = static_cast<unsigned int>(std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - startTime).count());
            if (elapsedMs >= timeoutMs)
            {
                break;
            }
            const unsigned int remainingMs = timeoutMs - elapsedMs;
            if (remainingMs == 0)
            {
                break;
            }

            SocketHandle socketHandle = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (socketHandle == kInvalidSocket)
            {
                continue;
            }

            if (!SetNonBlocking(socketHandle, true))
            {
                CloseSocket(socketHandle);
                continue;
            }

#ifdef _WIN32
            const int addrLen = static_cast<int>(ai->ai_addrlen);
            const int connectResult = ::connect(socketHandle, ai->ai_addr, addrLen);
#else
            const socklen_t addrLen = static_cast<socklen_t>(ai->ai_addrlen);
            const int connectResult = ::connect(socketHandle, ai->ai_addr, addrLen);
#endif

            if (connectResult == 0)
            {
                CloseSocket(socketHandle);
                return true;
            }

            const int lastError = GetLastSocketError();
#ifdef _WIN32
            const bool inProgress = (lastError == WSAEWOULDBLOCK) || (lastError == WSAEINPROGRESS) || (lastError == WSAEALREADY);
#else
            const bool inProgress = (lastError == EINPROGRESS) || (lastError == EALREADY);
#endif

            if (!inProgress)
            {
                CloseSocket(socketHandle);
                continue;
            }

            const bool ok = WaitForConnect(socketHandle, remainingMs);
            CloseSocket(socketHandle);

            if (ok)
            {
                return true;
            }
        }

        return false;
    }

    static bool EnsureCurlGlobalInit()
    {
        static std::once_flag initFlag;
        static bool initOk = false;

        std::call_once(initFlag, []() {
            initOk = (::curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
            });

        return initOk;
    }

    static bool TryComputeTotalSize(size_t size, size_t nmemb, size_t& totalSize)
    {
        if (size == 0 || nmemb == 0)
        {
            totalSize = 0;
            return true;
        }

        if (size > (std::numeric_limits<size_t>::max() / nmemb))
        {
            return false;
        }

        totalSize = size * nmemb;
        return true;
    }

    static bool TryAddSize(size_t a, size_t b, size_t& outValue)
    {
        if (a > (std::numeric_limits<size_t>::max() - b))
        {
            return false;
        }

        outValue = a + b;
        return true;
    }

    static bool TryCastUnsignedLongLongToSize(unsigned long long value, size_t& outValue)
    {
        if (value > static_cast<unsigned long long>(std::numeric_limits<size_t>::max()))
        {
            return false;
        }

        outValue = static_cast<size_t>(value);
        return true;
    }
    static long ClampUnsignedIntToCurlLong(unsigned int value)
    {
        const unsigned long long valueUll = static_cast<unsigned long long>(value);
        const unsigned long long maxLongUll = static_cast<unsigned long long>(std::numeric_limits<long>::max());
        if (valueUll > maxLongUll)
        {
            return std::numeric_limits<long>::max();
        }
        return static_cast<long>(value);
    }



    static std::string GetParentDirectoryUtf8(const std::string& filePathUtf8)
    {
        // GB_GetDirectoryPath 返回的目录路径统一使用“/”并以“/”结尾。
        // 为空表示无父目录（例如仅文件名）。
        return GB_GetDirectoryPath(filePathUtf8);
    }

    static bool EnsureDirectoryExistsUtf8(const std::string& dirPathUtf8)
    {
        if (dirPathUtf8.empty())
        {
            return true;
        }

        if (GB_IsDirectoryExists(dirPathUtf8))
        {
            return true;
        }

        return GB_CreateDirectory(dirPathUtf8);
    }

    static bool TryDeleteFileUtf8(const std::string& filePathUtf8)
    {
        if (filePathUtf8.empty())
        {
            return true;
        }

        if (!GB_IsFileExists(filePathUtf8))
        {
            return true;
        }

        return GB_DeleteFile(filePathUtf8);
    }

    static bool CreateOrTruncateFileUtf8(const std::string& filePathUtf8, bool sizeKnown, size_t totalBytes)
    {
        if (filePathUtf8.empty())
        {
            return false;
        }

        // 递归创建父目录，并创建/覆盖文件（截断为 0 字节）
        if (!GB_CreateFileRecursive(filePathUtf8, true))
        {
            return false;
        }

        if (!sizeKnown)
        {
            return true;
        }

#ifdef _WIN32
        const std::wstring filePathW = GB_Utf8ToWString(filePathUtf8);
        if (filePathW.empty())
        {
            return false;
        }

        const HANDLE fileHandle = ::CreateFileW(filePathW.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        bool ok = true;
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(totalBytes);
        if (::SetFilePointerEx(fileHandle, li, nullptr, FILE_BEGIN) == 0)
        {
            ok = false;
        }
        else if (::SetEndOfFile(fileHandle) == 0)
        {
            ok = false;
        }

        ::CloseHandle(fileHandle);
        return ok;
#else
        const int fd = ::open(filePathUtf8.c_str(), O_WRONLY);
        if (fd < 0)
        {
            return false;
        }

        const bool ok = (::ftruncate(fd, static_cast<off_t>(totalBytes)) == 0);
        ::close(fd);
        return ok;
#endif
    }

    static bool TryAppendCurlSlist(struct curl_slist*& list, const char* headerValue)
    {
        if (headerValue == nullptr)
        {
            return true;
        }

        struct curl_slist* newList = ::curl_slist_append(list, headerValue);
        if (newList == nullptr)
        {
            return false;
        }

        list = newList;
        return true;
    }

    static void SafeCopyErrorMessage(char* errorBuffer, size_t errorBufferSize, const char* message)
    {
        if (errorBuffer != nullptr && errorBufferSize > 0)
        {
            if (message == nullptr)
            {
                errorBuffer[0] = '\0';
                return;
            }

            const size_t messageLength = std::strlen(message);
            const size_t maxCopyLength = errorBufferSize - 1;
            const size_t copyLength = std::min(maxCopyLength, messageLength);

            if (copyLength > 0)
            {
                std::memcpy(errorBuffer, message, copyLength);
            }
            errorBuffer[copyLength] = '\0';
        }
    }

    static size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userData)
    {
        if (ptr == nullptr || userData == nullptr)
        {
            return 0;
        }

        size_t totalSize = 0;
        if (!TryComputeTotalSize(size, nmemb, totalSize))
        {
            return 0;
        }

        std::string* buffer = static_cast<std::string*>(userData);

        try
        {
            buffer->append(ptr, totalSize);
        }
        catch (...)
        {
            // 回调异常不能跨越 C 边界，返回 0 让 libcurl 以 CURLE_WRITE_ERROR 终止
            return 0;
        }

        return totalSize;
    }

    static size_t CurlHeaderCallback(char* buffer, size_t size, size_t nitems, void* userData)
    {
        if (buffer == nullptr || userData == nullptr)
        {
            return 0;
        }

        size_t totalSize = 0;
        if (!TryComputeTotalSize(size, nitems, totalSize))
        {
            return 0;
        }

        std::vector<std::string>* headers = static_cast<std::vector<std::string>*>(userData);

        try
        {
            headers->emplace_back(buffer, totalSize);
        }
        catch (...)
        {
            return 0;
        }

        return totalSize;
    }

    static inline bool IsSpaceChar(char c)
    {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    }

    static std::string TrimCopy(const std::string& text)
    {
        size_t begin = 0;
        size_t end = text.size();

        while (begin < end && IsSpaceChar(text[begin]))
        {
            begin++;
        }

        while (end > begin && IsSpaceChar(text[end - 1]))
        {
            end--;
        }

        return text.substr(begin, end - begin);
    }

    static std::string ToLowerCopy(std::string text)
    {
        for (size_t i = 0; i < text.size(); i++)
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            text[i] = static_cast<char>(std::tolower(c));
        }
        return text;
    }

    static std::string GetUrlSchemeLower(const std::string& urlUtf8)
    {
        const size_t pos = urlUtf8.find(':');
        if (pos == std::string::npos)
        {
            return std::string();
        }

        const std::string scheme = urlUtf8.substr(0, pos);
        return ToLowerCopy(scheme);
    }

    static std::string NormalizeNoProxyList(std::string noProxyUtf8)
    {
        for (size_t i = 0; i < noProxyUtf8.size(); i++)
        {
            if (noProxyUtf8[i] == ';')
            {
                noProxyUtf8[i] = ',';
            }
        }

        // libcurl 的 CURLOPT_NOPROXY 使用逗号分隔列表。
        // 这里做一次“逐项 Trim + 去空项”，避免包含空白/空项导致匹配异常。
        std::string result;
        size_t begin = 0;
        while (begin < noProxyUtf8.size())
        {
            size_t end = noProxyUtf8.find(',', begin);
            if (end == std::string::npos)
            {
                end = noProxyUtf8.size();
            }

            const std::string token = TrimCopy(noProxyUtf8.substr(begin, end - begin));
            if (!token.empty())
            {
                if (!result.empty())
                {
                    result += ",";
                }
                result += token;
            }

            begin = end + 1;
        }

        return result;
    }
#ifdef _WIN32

    static std::string NormalizeProxyToken(std::string token)
    {
        token = TrimCopy(token);
        if (token.empty())
        {
            return std::string();
        }

        const std::string tokenLower = ToLowerCopy(token);
        if (tokenLower == "direct")
        {
            return std::string();
        }

        const size_t spacePos = token.find(' ');
        if (spacePos != std::string::npos)
        {
            const std::string prefixLower = ToLowerCopy(TrimCopy(token.substr(0, spacePos)));
            std::string value = TrimCopy(token.substr(spacePos + 1));
            if (value.empty())
            {
                return std::string();
            }

            if (value.find("://") != std::string::npos)
            {
                return value;
            }

            if (prefixLower == "proxy" || prefixLower == "http")
            {
                return "http://" + value;
            }
            if (prefixLower == "https")
            {
                return "https://" + value;
            }
            if (prefixLower == "socks" || prefixLower == "socks5")
            {
                return "socks5h://" + value;
            }
            if (prefixLower == "socks4")
            {
                return "socks4a://" + value;
            }
        }

        return token;
    }

    static std::string RemoveLocalBypassToken(const std::string& bypassUtf8)
    {
        std::string result;
        size_t begin = 0;
        while (begin < bypassUtf8.size())
        {
            size_t end = bypassUtf8.find(',', begin);
            if (end == std::string::npos)
            {
                end = bypassUtf8.size();
            }

            const std::string token = TrimCopy(bypassUtf8.substr(begin, end - begin));
            if (!token.empty())
            {
                const std::string tokenLower = ToLowerCopy(token);
                if (tokenLower != "<local>")
                {
                    if (!result.empty())
                    {
                        result += ",";
                    }
                    result += token;
                }
            }

            begin = end + 1;
        }
        return result;
    }

    static std::string PickProxyFromProtocolList(const std::string& proxyListUtf8, const std::string& schemeLower)
    {
        std::string fallbackProxy;
        bool hasFallbackProxy = false;

        size_t begin = 0;
        while (begin < proxyListUtf8.size())
        {
            size_t end = proxyListUtf8.find(';', begin);
            if (end == std::string::npos)
            {
                end = proxyListUtf8.size();
            }

            const std::string token = TrimCopy(proxyListUtf8.substr(begin, end - begin));
            if (!token.empty())
            {
                const size_t eqPos = token.find('=');
                if (eqPos == std::string::npos)
                {
                    // PAC 结果通常类似 "PROXY 127.0.0.1:7890; DIRECT" 或 "DIRECT; PROXY 127.0.0.1:7890"。
                    // 应优先尊重列表中的第一个可用结果，不能因为后续存在 PROXY 就覆盖前面的 DIRECT。
                    if (!hasFallbackProxy)
                    {
                        fallbackProxy = NormalizeProxyToken(token);
                        hasFallbackProxy = true;
                    }
                }
                else
                {
                    const std::string keyLower = ToLowerCopy(TrimCopy(token.substr(0, eqPos)));
                    if (keyLower == schemeLower)
                    {
                        return NormalizeProxyToken(token.substr(eqPos + 1)); // 可能为空（DIRECT）
                    }
                }
            }

            begin = end + 1;
        }

        if (hasFallbackProxy)
        {
            return fallbackProxy;
        }
        return std::string();
    }

#endif

#ifdef _WIN32
    struct ScopedGlobalFreeW
    {
        LPWSTR ptr = nullptr;
        ~ScopedGlobalFreeW()
        {
            if (ptr != nullptr)
            {
                ::GlobalFree(ptr);
                ptr = nullptr;
            }
        }
    };


    static const unsigned int DefaultWindowsSystemProxyResolveTimeoutMs = 2000;
    static const size_t MaxWindowsSystemProxyCacheSize = 4096;

    struct WindowsSystemProxyCacheItem
    {
        bool querySucceeded = false;
        std::string proxyUtf8 = "";
        std::string bypassUtf8 = "";
        unsigned long long expireTickMs = 0;
    };

    struct WindowsAutoProxySessionState
    {
        std::mutex mutex;
        HINTERNET session = nullptr;

        ~WindowsAutoProxySessionState()
        {
            if (session != nullptr)
            {
                ::WinHttpCloseHandle(session);
                session = nullptr;
            }
        }
    };

    static unsigned long long GetSteadyClockMilliseconds()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    }

    static int GetWinHttpTimeoutValue(unsigned int timeoutMs)
    {
        if (timeoutMs == 0)
        {
            timeoutMs = DefaultWindowsSystemProxyResolveTimeoutMs;
        }

        const unsigned int maxIntValue = static_cast<unsigned int>(std::numeric_limits<int>::max());
        if (timeoutMs > maxIntValue)
        {
            timeoutMs = maxIntValue;
        }

        return static_cast<int>(timeoutMs);
    }

    static std::mutex& GetWindowsSystemProxyCacheMutex()
    {
        static std::mutex cacheMutex;
        return cacheMutex;
    }

    static std::unordered_map<std::string, WindowsSystemProxyCacheItem>& GetWindowsSystemProxyCache()
    {
        static std::unordered_map<std::string, WindowsSystemProxyCacheItem> cache;
        return cache;
    }

    static WindowsAutoProxySessionState& GetWindowsAutoProxySessionState()
    {
        static WindowsAutoProxySessionState sessionState;
        return sessionState;
    }

    static void ClearWindowsSystemProxyCache()
    {
        std::lock_guard<std::mutex> lock(GetWindowsSystemProxyCacheMutex());
        GetWindowsSystemProxyCache().clear();
    }

    static std::string BuildWindowsSystemProxyCacheKey(const std::string& urlUtf8, const GB_NetworkProxySettings& proxySettings)
    {
        std::string key;
        if (proxySettings.systemProxyCacheByHost)
        {
            GB_UrlOperator::UrlComponents components;
            if (GB_UrlOperator::TryParseUrl(urlUtf8, components))
            {
                key.reserve(128);
                key += ToLowerCopy(components.schemeLower);
                key += "://";
                key += ToLowerCopy(components.hostUtf8);
                if (components.hasPort)
                {
                    key += ":";
                    key += std::to_string(static_cast<unsigned int>(components.port));
                }
            }
        }

        if (key.empty())
        {
            key = urlUtf8;
        }

        key += "|autoDetect=";
        key += proxySettings.enableSystemProxyAutoDetect ? "1" : "0";
        return key;
    }

    static bool TryGetWindowsSystemProxyFromCache(const std::string& cacheKey, std::string& proxyUtf8, std::string& bypassUtf8, bool& querySucceeded)
    {
        const unsigned long long nowTickMs = GetSteadyClockMilliseconds();

        std::lock_guard<std::mutex> lock(GetWindowsSystemProxyCacheMutex());
        std::unordered_map<std::string, WindowsSystemProxyCacheItem>& cache = GetWindowsSystemProxyCache();

        const auto iter = cache.find(cacheKey);
        if (iter == cache.end())
        {
            return false;
        }

        if (iter->second.expireTickMs <= nowTickMs)
        {
            cache.erase(iter);
            return false;
        }

        querySucceeded = iter->second.querySucceeded;
        proxyUtf8 = iter->second.proxyUtf8;
        bypassUtf8 = iter->second.bypassUtf8;
        return true;
    }

    static void PutWindowsSystemProxyToCache(const std::string& cacheKey, bool querySucceeded, const std::string& proxyUtf8, const std::string& bypassUtf8, unsigned int cacheTtlMs)
    {
        if (cacheKey.empty() || cacheTtlMs == 0)
        {
            return;
        }

        WindowsSystemProxyCacheItem item;
        item.querySucceeded = querySucceeded;
        item.proxyUtf8 = proxyUtf8;
        item.bypassUtf8 = bypassUtf8;
        item.expireTickMs = GetSteadyClockMilliseconds() + static_cast<unsigned long long>(cacheTtlMs);

        std::lock_guard<std::mutex> lock(GetWindowsSystemProxyCacheMutex());
        std::unordered_map<std::string, WindowsSystemProxyCacheItem>& cache = GetWindowsSystemProxyCache();
        if (cache.size() >= MaxWindowsSystemProxyCacheSize)
        {
            cache.clear();
        }
        cache[cacheKey] = item;
    }

    static bool QueryWindowsSystemProxyForUrlUtf8(const std::string& urlUtf8, const GB_NetworkProxySettings& proxySettings, std::string& proxyUtf8, std::string& bypassUtf8)
    {
        proxyUtf8.clear();
        bypassUtf8.clear();

        const std::wstring urlW = GB_Utf8ToWString(urlUtf8);
        if (urlW.empty())
        {
            return false;
        }

        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ieProxyConfig;
        std::memset(&ieProxyConfig, 0, sizeof(ieProxyConfig));

        const bool ieConfigOk = (::WinHttpGetIEProxyConfigForCurrentUser(&ieProxyConfig) != FALSE);
        if (!ieConfigOk)
        {
            WINHTTP_PROXY_INFO defaultProxyInfo;
            std::memset(&defaultProxyInfo, 0, sizeof(defaultProxyInfo));

            if (!::WinHttpGetDefaultProxyConfiguration(&defaultProxyInfo))
            {
                return false;
            }

            ScopedGlobalFreeW defaultProxyFree;
            ScopedGlobalFreeW defaultBypassFree;
            defaultProxyFree.ptr = defaultProxyInfo.lpszProxy;
            defaultBypassFree.ptr = defaultProxyInfo.lpszProxyBypass;

            if (defaultProxyInfo.lpszProxy != nullptr && defaultProxyInfo.lpszProxy[0] != 0)
            {
                proxyUtf8 = GB_WStringToUtf8(defaultProxyInfo.lpszProxy);
            }
            else
            {
                proxyUtf8.clear();
            }

            if (defaultProxyInfo.lpszProxyBypass != nullptr && defaultProxyInfo.lpszProxyBypass[0] != 0)
            {
                bypassUtf8 = GB_WStringToUtf8(defaultProxyInfo.lpszProxyBypass);
            }

            return true;
        }

        ScopedGlobalFreeW proxyFree;
        ScopedGlobalFreeW bypassFree;
        ScopedGlobalFreeW pacUrlFree;
        proxyFree.ptr = ieProxyConfig.lpszProxy;
        bypassFree.ptr = ieProxyConfig.lpszProxyBypass;
        pacUrlFree.ptr = ieProxyConfig.lpszAutoConfigUrl;

        bool gotExplicitProxy = false;
        if (ieProxyConfig.lpszProxy != nullptr && ieProxyConfig.lpszProxy[0] != 0)
        {
            proxyUtf8 = GB_WStringToUtf8(ieProxyConfig.lpszProxy);
            gotExplicitProxy = !proxyUtf8.empty();
        }

        if (ieProxyConfig.lpszProxyBypass != nullptr && ieProxyConfig.lpszProxyBypass[0] != 0)
        {
            bypassUtf8 = GB_WStringToUtf8(ieProxyConfig.lpszProxyBypass);
        }

        const bool needsAuto = proxySettings.enableSystemProxyAutoDetect && ((ieProxyConfig.fAutoDetect != FALSE) || (ieProxyConfig.lpszAutoConfigUrl != nullptr));
        if (!needsAuto)
        {
            // IE 配置读取成功。此时 proxyUtf8 可能为空（DIRECT），也应返回 true。
            return true;
        }

        WindowsAutoProxySessionState& sessionState = GetWindowsAutoProxySessionState();
        std::lock_guard<std::mutex> sessionLock(sessionState.mutex);

        if (sessionState.session == nullptr)
        {
            sessionState.session = ::WinHttpOpen(L"GlobalBase/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }

        if (sessionState.session == nullptr)
        {
            // 自动代理需要 WinHTTP session，但创建失败时回退到“显式代理”（若有）。
            return gotExplicitProxy;
        }

        // 复用同一个 WinHTTP session，既可避免频繁 Open/Close，也可让 WinHTTP 复用自身的 PAC/WPAD 缓存。
        const int timeoutValue = GetWinHttpTimeoutValue(proxySettings.systemProxyResolveTimeoutMs);
        ::WinHttpSetTimeouts(sessionState.session, timeoutValue, timeoutValue, timeoutValue, timeoutValue);

        WINHTTP_AUTOPROXY_OPTIONS autoProxyOptions;
        std::memset(&autoProxyOptions, 0, sizeof(autoProxyOptions));
        autoProxyOptions.fAutoLogonIfChallenged = TRUE;

        if (ieProxyConfig.fAutoDetect != FALSE)
        {
            autoProxyOptions.dwFlags |= WINHTTP_AUTOPROXY_AUTO_DETECT;
            autoProxyOptions.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
        }

        if (ieProxyConfig.lpszAutoConfigUrl != nullptr)
        {
            autoProxyOptions.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
            autoProxyOptions.lpszAutoConfigUrl = ieProxyConfig.lpszAutoConfigUrl;
        }

        WINHTTP_PROXY_INFO proxyInfo;
        std::memset(&proxyInfo, 0, sizeof(proxyInfo));

        ScopedGlobalFreeW proxyInfoFree;
        ScopedGlobalFreeW bypassInfoFree;

        bool gotAutoResult = false;
        if (::WinHttpGetProxyForUrl(sessionState.session, urlW.c_str(), &autoProxyOptions, &proxyInfo))
        {
            gotAutoResult = true;
            proxyInfoFree.ptr = proxyInfo.lpszProxy;
            bypassInfoFree.ptr = proxyInfo.lpszProxyBypass;

            if (proxyInfo.lpszProxy != nullptr && proxyInfo.lpszProxy[0] != 0)
            {
                proxyUtf8 = GB_WStringToUtf8(proxyInfo.lpszProxy);
            }
            else
            {
                proxyUtf8.clear();
            }

            if (proxyInfo.lpszProxyBypass != nullptr && proxyInfo.lpszProxyBypass[0] != 0)
            {
                bypassUtf8 = GB_WStringToUtf8(proxyInfo.lpszProxyBypass);
            }
        }

        if (gotAutoResult)
        {
            return true;
        }

        // 自动代理失败时：若存在显式代理，则仍认为获取成功；否则交由上层决定回退策略。
        return gotExplicitProxy;
    }

    static bool GetWindowsSystemProxyForUrlUtf8(const std::string& urlUtf8, const GB_NetworkProxySettings& proxySettings, std::string& proxyUtf8, std::string& bypassUtf8)
    {
        proxyUtf8.clear();
        bypassUtf8.clear();

        const unsigned int cacheTtlMs = proxySettings.systemProxyCacheTtlMs;
        if (!proxySettings.cacheSystemProxy || cacheTtlMs == 0)
        {
            return QueryWindowsSystemProxyForUrlUtf8(urlUtf8, proxySettings, proxyUtf8, bypassUtf8);
        }

        const std::string cacheKey = BuildWindowsSystemProxyCacheKey(urlUtf8, proxySettings);

        bool querySucceeded = false;
        if (TryGetWindowsSystemProxyFromCache(cacheKey, proxyUtf8, bypassUtf8, querySucceeded))
        {
            return querySucceeded;
        }

        querySucceeded = QueryWindowsSystemProxyForUrlUtf8(urlUtf8, proxySettings, proxyUtf8, bypassUtf8);
        PutWindowsSystemProxyToCache(cacheKey, querySucceeded, proxyUtf8, bypassUtf8, cacheTtlMs);
        return querySucceeded;
    }
#endif

    static void ApplyProxySettings(CURL* curlHandle, const std::string& urlUtf8, const GB_NetworkProxySettings& proxySettings)
    {
        if (curlHandle == nullptr)
        {
            return;
        }

        const std::string schemeLower = GetUrlSchemeLower(urlUtf8);

        if (proxySettings.useSystemProxy)
        {
#ifdef _WIN32
            std::string systemProxyUtf8;
            std::string systemBypassUtf8;

            if (GetWindowsSystemProxyForUrlUtf8(urlUtf8, proxySettings, systemProxyUtf8, systemBypassUtf8))
            {
                const std::string selectedProxy = PickProxyFromProtocolList(systemProxyUtf8, schemeLower);

                if (!selectedProxy.empty())
                {
                    ::curl_easy_setopt(curlHandle, CURLOPT_PROXY, selectedProxy.c_str());

                    const std::string rawBypass = NormalizeNoProxyList(systemBypassUtf8);
                    const bool hadLocalBypass = (ToLowerCopy(rawBypass).find("<local>") != std::string::npos);

                    std::string bypass = RemoveLocalBypassToken(rawBypass);
                    if (hadLocalBypass)
                    {
                        if (!bypass.empty())
                        {
                            bypass += ",";
                        }
                        bypass += "localhost,127.0.0.1,::1";
                    }

                    bypass = NormalizeNoProxyList(bypass);
                    if (!bypass.empty())
                    {
                        ::curl_easy_setopt(curlHandle, CURLOPT_NOPROXY, bypass.c_str());
                    }
                    else
                    {
                        // 设为空串以覆盖环境变量 no_proxy（显式让所有主机都走代理）
                        ::curl_easy_setopt(curlHandle, CURLOPT_NOPROXY, "");
                    }
                }
                else
                {
                    // 系统配置为 DIRECT：显式禁用代理（包括环境变量代理）
                    ::curl_easy_setopt(curlHandle, CURLOPT_PROXY, "");
                    ::curl_easy_setopt(curlHandle, CURLOPT_NOPROXY, "*");
                }

                return;
            }
#endif
            // 非 Windows 或系统代理获取失败：不做任何设置，让 libcurl 按默认（环境变量等）处理
            return;
        }

        if (!proxySettings.enableProxy)
        {
            // 显式禁用所有代理（包括环境变量代理）
            ::curl_easy_setopt(curlHandle, CURLOPT_PROXY, "");
            ::curl_easy_setopt(curlHandle, CURLOPT_NOPROXY, "*");
            return;
        }

        if (proxySettings.proxyHostUtf8.empty())
        {
            // enableProxy=true 但未给出代理主机：为避免“意外使用环境变量代理”，这里选择直连
            ::curl_easy_setopt(curlHandle, CURLOPT_PROXY, "");
            ::curl_easy_setopt(curlHandle, CURLOPT_NOPROXY, "*");
            return;
        }

        const bool proxyHasScheme = (proxySettings.proxyHostUtf8.find("://") != std::string::npos);
        ::curl_easy_setopt(curlHandle, CURLOPT_PROXY, proxySettings.proxyHostUtf8.c_str());

        // 如果 proxy 字符串自带 scheme（如 http:// / https:// / socks5h://），则 libcurl 可以自行推导代理类型与端口。
        // 为避免 scheme 与 CURLOPT_PROXYTYPE 冲突，这里仅在“不带 scheme”时设置 CURLOPT_PROXYTYPE / CURLOPT_PROXYPORT。
        if (!proxyHasScheme)
        {
            if (proxySettings.proxyPort != 0)
            {
                ::curl_easy_setopt(curlHandle, CURLOPT_PROXYPORT, static_cast<long>(proxySettings.proxyPort));
            }

            switch (proxySettings.proxyType)
            {
            case GB_NetworkProxyType::Http:
                ::curl_easy_setopt(curlHandle, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
                break;
            case GB_NetworkProxyType::Https:
#ifdef CURLPROXY_HTTPS
                ::curl_easy_setopt(curlHandle, CURLOPT_PROXYTYPE, CURLPROXY_HTTPS);
#else
                ::curl_easy_setopt(curlHandle, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
#endif
                break;
            case GB_NetworkProxyType::Socks4:
                ::curl_easy_setopt(curlHandle, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
                break;
            case GB_NetworkProxyType::Socks4a:
                ::curl_easy_setopt(curlHandle, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4A);
                break;
            case GB_NetworkProxyType::Socks5:
                ::curl_easy_setopt(curlHandle, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
                break;
            case GB_NetworkProxyType::Socks5Hostname:
                ::curl_easy_setopt(curlHandle, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
                break;
            default:
                ::curl_easy_setopt(curlHandle, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
                break;
            }
        }

        if (!proxySettings.proxyUserNameUtf8.empty())
        {
            ::curl_easy_setopt(curlHandle, CURLOPT_PROXYUSERNAME, proxySettings.proxyUserNameUtf8.c_str());
        }

        if (!proxySettings.proxyPasswordUtf8.empty())
        {
            ::curl_easy_setopt(curlHandle, CURLOPT_PROXYPASSWORD, proxySettings.proxyPasswordUtf8.c_str());
        }

        bool isHttpOrHttpsProxy = false;
        if (proxyHasScheme)
        {
            const std::string proxySchemeLower = GetUrlSchemeLower(proxySettings.proxyHostUtf8);
            isHttpOrHttpsProxy = (proxySchemeLower == "http" || proxySchemeLower == "https");
        }
        else
        {
            isHttpOrHttpsProxy = (proxySettings.proxyType == GB_NetworkProxyType::Http || proxySettings.proxyType == GB_NetworkProxyType::Https);
        }

        const bool useTunnel = proxySettings.proxyTunnel && (schemeLower == "https") && isHttpOrHttpsProxy;
        ::curl_easy_setopt(curlHandle, CURLOPT_HTTPPROXYTUNNEL, useTunnel ? 1L : 0L);

        const std::string noProxy = NormalizeNoProxyList(proxySettings.noProxyUtf8);
        if (!noProxy.empty())
        {
            ::curl_easy_setopt(curlHandle, CURLOPT_NOPROXY, noProxy.c_str());
        }
        else
        {
            // 设为空串以覆盖环境变量 no_proxy（显式让所有主机都走代理）
            ::curl_easy_setopt(curlHandle, CURLOPT_NOPROXY, "");
        }
    }

    struct GbDownloadProgressPointers
    {
        std::atomic_size_t* totalBytesPtr = nullptr;
        std::atomic_size_t* downloadedBytesPtr = nullptr;
        bool enabled = false;
    };

    static GbDownloadProgressPointers GetDownloadProgressPointers(void* totalSizeAtomicPtr, void* downloadedSizeAtomicPtr)
    {
        GbDownloadProgressPointers result;
        if (totalSizeAtomicPtr != nullptr && downloadedSizeAtomicPtr != nullptr)
        {
            result.totalBytesPtr = static_cast<std::atomic_size_t*>(totalSizeAtomicPtr);
            result.downloadedBytesPtr = static_cast<std::atomic_size_t*>(downloadedSizeAtomicPtr);
            result.enabled = true;
        }
        return result;
    }

    static bool TryParseUnsignedLongLong(const std::string& text, unsigned long long& value)
    {
        try
        {
            const std::string trimmed = TrimCopy(text);
            if (trimmed.empty())
            {
                return false;
            }

            size_t idx = 0;
            const unsigned long long v = std::stoull(trimmed, &idx, 10);
            if (idx != trimmed.size())
            {
                return false;
            }

            value = v;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    static std::vector<std::string> SplitHeaderParameters(const std::string& headerValue)
    {
        std::vector<std::string> parts;
        std::string current;
        current.reserve(headerValue.size());

        bool inQuotes = false;
        bool escaping = false;

        for (size_t i = 0; i < headerValue.size(); i++)
        {
            const char c = headerValue[i];

            if (escaping)
            {
                current.push_back(c);
                escaping = false;
                continue;
            }

            if (c == '\\' && inQuotes)
            {
                current.push_back(c);
                escaping = true;
                continue;
            }

            if (c == '"')
            {
                inQuotes = !inQuotes;
                current.push_back(c);
                continue;
            }

            if (c == ';' && !inQuotes)
            {
                parts.push_back(TrimCopy(current));
                current.clear();
                continue;
            }

            current.push_back(c);
        }

        if (!current.empty() || !parts.empty())
        {
            parts.push_back(TrimCopy(current));
        }

        return parts;
    }

    static std::string PercentDecode(const std::string& text)
    {
        const size_t firstPercent = text.find('%');
        if (firstPercent == std::string::npos)
        {
            return text;
        }

        std::string result;
        result.reserve(text.size());
        result.append(text, 0, firstPercent);

        auto HexToNibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
            };

        for (size_t i = firstPercent; i < text.size(); i++)
        {
            const char c = text[i];
            if (c == '%' && i + 2 < text.size())
            {
                const int hi = HexToNibble(text[i + 1]);
                const int lo = HexToNibble(text[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    const char decoded = static_cast<char>((hi << 4) | lo);
                    result.push_back(decoded);
                    i += 2;
                    continue;
                }
            }
            result.push_back(c);
        }
        return result;
    }

    static std::string ConvertIso88591ToUtf8(const std::string& bytes)
    {
        std::string result;
        result.reserve(bytes.size() * 2);

        for (size_t i = 0; i < bytes.size(); i++)
        {
            const unsigned char ch = static_cast<unsigned char>(bytes[i]);
            if (ch < 0x80)
            {
                result.push_back(static_cast<char>(ch));
            }
            else
            {
                // U+00XX
                result.push_back(static_cast<char>(0xC0 | (ch >> 6)));
                result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
            }
        }
        return result;
    }

    static std::string UnquoteToken(const std::string& value)
    {
        const std::string trimmed = TrimCopy(value);
        if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"')
        {
            const std::string inner = trimmed.substr(1, trimmed.size() - 2);

            // 简单反斜杠转义处理（例如 \\\" 和 \\\\ ）
            std::string out;
            out.reserve(inner.size());

            bool escaping = false;
            for (size_t i = 0; i < inner.size(); i++)
            {
                const char c = inner[i];
                if (escaping)
                {
                    out.push_back(c);
                    escaping = false;
                    continue;
                }

                if (c == '\\')
                {
                    escaping = true;
                    continue;
                }

                out.push_back(c);
            }
            return out;
        }

        return trimmed;
    }

    static std::string SanitizeFileName(const std::string& fileNameUtf8)
    {
        std::string result;
        result.reserve(fileNameUtf8.size());

        for (size_t i = 0; i < fileNameUtf8.size(); i++)
        {
            const unsigned char ch = static_cast<unsigned char>(fileNameUtf8[i]);
            if (ch < 0x20)
            {
                continue;
            }

            const char c = fileNameUtf8[i];
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            {
                result.push_back('_');
            }
            else
            {
                result.push_back(c);
            }
        }

        result = TrimCopy(result);
        while (!result.empty() && (result.back() == '.' || result.back() == ' '))
        {
            result.pop_back();
        }

        return result;
    }

    static std::string ExtractFileNameFromUrlUtf8(const std::string& urlUtf8)
    {
        // 去掉 fragment
        std::string s = urlUtf8;
        const size_t hashPos = s.find('#');
        if (hashPos != std::string::npos)
        {
            s = s.substr(0, hashPos);
        }

        // 去掉 query
        const size_t qPos = s.find('?');
        if (qPos != std::string::npos)
        {
            s = s.substr(0, qPos);
        }

        const size_t slashPos = s.find_last_of('/');
        if (slashPos == std::string::npos || slashPos + 1 >= s.size())
        {
            return "";
        }

        const std::string tail = s.substr(slashPos + 1);
        return SanitizeFileName(PercentDecode(tail));
    }

    static bool ParseContentDispositionFileNameUtf8(const std::string& headerValue, std::string& outFileNameUtf8)
    {
        // 参考 RFC 6266：filename / filename*。filename* 形如：utf-8''%E4%B8%AD%E6%96%87.txt
        std::string filename = "";
        std::string filenameStar = "";

        const std::vector<std::string> parts = SplitHeaderParameters(headerValue);

        for (size_t i = 0; i < parts.size(); i++)
        {
            const std::string p = parts[i];
            const size_t eqPos = p.find('=');
            if (eqPos == std::string::npos)
            {
                continue;
            }

            const std::string key = ToLowerCopy(TrimCopy(p.substr(0, eqPos)));
            const std::string val = TrimCopy(p.substr(eqPos + 1));

            if (key == "filename")
            {
                filename = UnquoteToken(val);
            }
            else if (key == "filename*")
            {
                const std::string extValue = UnquoteToken(val);

                // ext-value: charset'lang'value
                const size_t p1 = extValue.find('\'');
                if (p1 == std::string::npos)
                {
                    continue;
                }
                const size_t p2 = extValue.find('\'', p1 + 1);
                if (p2 == std::string::npos)
                {
                    continue;
                }

                const std::string charset = ToLowerCopy(extValue.substr(0, p1));
                const std::string encoded = extValue.substr(p2 + 1);

                const std::string decodedBytes = PercentDecode(encoded);
                if (charset == "utf-8" || charset == "utf8")
                {
                    filenameStar = decodedBytes;
                }
                else if (charset == "iso-8859-1" || charset == "latin1")
                {
                    filenameStar = ConvertIso88591ToUtf8(decodedBytes);
                }
                else
                {
                    // 其它 charset：保底直接返回解码后的字节序列（可能仍是 UTF-8）
                    filenameStar = decodedBytes;
                }
            }
        }

        std::string chosen = !filenameStar.empty() ? filenameStar : filename;
        chosen = SanitizeFileName(chosen);
        if (chosen.empty())
        {
            return false;
        }

        outFileNameUtf8 = chosen;
        return true;
    }

    struct GbDownloadHeaderState
    {
        GB_NetworkDownloadedFile* result = nullptr;
        GbDownloadProgressPointers progress;
        bool includeResponseHeaders = false;
        bool hasReserved = false;
        bool hasSeenStatusLine = false;
    };

    static size_t DownloadHeaderCallback(char* buffer, size_t size, size_t nitems, void* userData)
    {
        size_t totalSize = 0;
        if (!TryComputeTotalSize(size, nitems, totalSize))
        {
            return 0;
        }
        if (userData == nullptr || buffer == nullptr || totalSize == 0)
        {
            return totalSize;
        }

        GbDownloadHeaderState* state = static_cast<GbDownloadHeaderState*>(userData);
        if (state == nullptr || state->result == nullptr)
        {
            return totalSize;
        }

        try
        {
            std::string headerLine(buffer, totalSize);
            headerLine = TrimCopy(headerLine);

            // libcurl 在跟随重定向/多次响应时，会多次回调 header（每段响应都会以 HTTP/... 状态行开头）。
            // 为避免把中间 3xx 的 body/headers 混入最终结果，这里在检测到新的状态行时重置相关缓存。
            if (!headerLine.empty())
            {
                const std::string headerLower = ToLowerCopy(headerLine);
                if (headerLower.find("http/") == 0)
                {
                    if (state->hasSeenStatusLine)
                    {
                        // 进入新的响应段（例如重定向后的最终 200）。
                        state->result->data.clear();
                        state->result->contentTypeUtf8.clear();
                        state->result->fileNameUtf8.clear();
                        state->result->totalSizeKnown = false;
                        state->result->totalBytes = 0;
                        state->hasReserved = false;

                        if (state->includeResponseHeaders)
                        {
                            state->result->responseHeadersUtf8.clear();
                        }

                        if (state->progress.enabled)
                        {
                            state->progress.totalBytesPtr->store(0, std::memory_order_relaxed);
                            state->progress.downloadedBytesPtr->store(0, std::memory_order_relaxed);
                        }
                    }

                    state->hasSeenStatusLine = true;
                }

                if (state->includeResponseHeaders)
                {
                    state->result->responseHeadersUtf8.push_back(headerLine);
                }
            }

            const std::string lower = ToLowerCopy(headerLine);
            if (lower.find("content-type:") == 0)
            {
                state->result->contentTypeUtf8 = TrimCopy(headerLine.substr(std::string("content-type:").size()));
            }
            else if (lower.find("content-length:") == 0)
            {
                const std::string valueText = TrimCopy(headerLine.substr(std::string("content-length:").size()));
                unsigned long long v = 0;
                size_t totalBytes = 0;
                if (TryParseUnsignedLongLong(valueText, v) && TryCastUnsignedLongLongToSize(v, totalBytes))
                {
                    state->result->totalSizeKnown = true;
                    state->result->totalBytes = totalBytes;

                    if (state->progress.enabled)
                    {
                        state->progress.totalBytesPtr->store(state->result->totalBytes, std::memory_order_relaxed);
                    }

                    if (!state->hasReserved && state->result->totalBytes > 0)
                    {
                        // 只 reserve，不 resize，避免先填充 0。
                        // 注意：Content-Length 可能不准确；并且一次性 reserve 超大容量可能因内存碎片/策略而失败。
                        // 因此这里做一个上限，并且 reserve 失败不视为致命错误。
                        constexpr size_t kReserveCapBytes = 256ULL * 1024ULL * 1024ULL;
                        const size_t reserveBytes = (state->result->totalBytes < kReserveCapBytes) ? state->result->totalBytes : kReserveCapBytes;
                        try
                        {
                            state->result->data.reserve(reserveBytes);
                        }
                        catch (...)
                        {
                            // ignore
                        }
                        state->hasReserved = true;
                    }
                }
            }
            else if (lower.find("content-disposition:") == 0)
            {
                const std::string valueText = TrimCopy(headerLine.substr(std::string("content-disposition:").size()));
                std::string fileNameUtf8;
                if (ParseContentDispositionFileNameUtf8(valueText, fileNameUtf8))
                {
                    state->result->fileNameUtf8 = fileNameUtf8;
                }
            }

            return totalSize;
        }
        catch (...)
        {
            // 回调异常不能跨越 C 边界
            return 0;
        }
    }
    struct GbWriteState
    {
        GB_ByteBuffer* data = nullptr;
        GbDownloadProgressPointers progress;
    };

    static size_t DownloadWriteCallback(char* ptr, size_t size, size_t nmemb, void* userData)
    {
        size_t totalSize = 0;
        if (!TryComputeTotalSize(size, nmemb, totalSize))
        {
            return 0;
        }
        if (userData == nullptr || ptr == nullptr || totalSize == 0)
        {
            return 0;
        }

        GbWriteState* state = static_cast<GbWriteState*>(userData);
        if (state == nullptr || state->data == nullptr)
        {
            return 0;
        }

        try
        {
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(ptr);
            const size_t oldSize = state->data->size();
            size_t newSize = 0;
            if (!TryAddSize(oldSize, totalSize, newSize))
            {
                // 溢出风险：让 libcurl 以写入错误退出
                return 0;
            }

            // 使用 insert 直接追加，避免 resize 导致的额外零填充成本。
            state->data->insert(state->data->end(), bytes, bytes + totalSize);
            return totalSize;
        }
        catch (...)
        {
            return 0;
        }
    }
    static int DownloadXferInfoCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
    {
        (void)ultotal;
        (void)ulnow;

        if (clientp == nullptr)
        {
            return 0;
        }

        GbDownloadProgressPointers* progress = static_cast<GbDownloadProgressPointers*>(clientp);
        if (!progress->enabled)
        {
            return 0;
        }

        size_t total = 0;
        size_t now = 0;

        if (dltotal > 0)
        {
            const unsigned long long dltotalUll = static_cast<unsigned long long>(dltotal);
            if (!TryCastUnsignedLongLongToSize(dltotalUll, total))
            {
                total = std::numeric_limits<size_t>::max();
            }
        }

        if (dlnow > 0)
        {
            const unsigned long long dlnowUll = static_cast<unsigned long long>(dlnow);
            if (!TryCastUnsignedLongLongToSize(dlnowUll, now))
            {
                now = std::numeric_limits<size_t>::max();
            }
        }

        progress->totalBytesPtr->store(total, std::memory_order_relaxed);
        progress->downloadedBytesPtr->store(now, std::memory_order_relaxed);

        return 0;
    }

#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM < 0x072000)
    // 兼容旧版 libcurl：使用 CURLOPT_PROGRESSFUNCTION（double）回调。
    static int DownloadProgressCallback(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow)
    {
        (void)ultotal;
        (void)ulnow;

        if (clientp == nullptr)
        {
            return 0;
        }

        GbDownloadProgressPointers* progress = static_cast<GbDownloadProgressPointers*>(clientp);
        if (!progress->enabled)
        {
            return 0;
        }

        size_t total = 0;
        size_t now = 0;

        if (dltotal > 0.0)
        {
            const unsigned long long totalUll = static_cast<unsigned long long>(dltotal);
            if (!TryCastUnsignedLongLongToSize(totalUll, total))
            {
                total = std::numeric_limits<size_t>::max();
            }
        }

        if (dlnow > 0)
        {
            const unsigned long long nowUll = static_cast<unsigned long long>(dlnow);
            if (!TryCastUnsignedLongLongToSize(nowUll, now))
            {
                now = std::numeric_limits<size_t>::max();
            }
        }

        progress->totalBytesPtr->store(total, std::memory_order_relaxed);
        progress->downloadedBytesPtr->store(now, std::memory_order_relaxed);

        return 0;
    }
#endif


    static bool ConfigureDownloadCurlCommon(CURL* curlHandle, const std::string& urlUtf8, const GB_NetworkRequestOptions& options, GbDownloadProgressPointers& progress, char* errorBuffer, size_t errorBufferSize, struct curl_slist** outHeaders)
    {
        if (curlHandle == nullptr)
        {
            return false;
        }

        if (errorBuffer != nullptr && errorBufferSize > 0)
        {
            errorBuffer[0] = '\0';
            curl_easy_setopt(curlHandle, CURLOPT_ERRORBUFFER, errorBuffer);
        }

        curl_easy_setopt(curlHandle, CURLOPT_URL, urlUtf8.c_str());
        curl_easy_setopt(curlHandle, CURLOPT_TIMEOUT_MS, ClampUnsignedIntToCurlLong(options.totalTimeoutMs));
        curl_easy_setopt(curlHandle, CURLOPT_CONNECTTIMEOUT_MS, ClampUnsignedIntToCurlLong(options.connectTimeoutMs));
        // 只允许 http/https
#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x075500)
        curl_easy_setopt(curlHandle, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curlHandle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curlHandle, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(curlHandle, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

        if (options.followRedirects)
        {
            curl_easy_setopt(curlHandle, CURLOPT_FOLLOWLOCATION, 1L);
            const long maxRedirects = (options.maxRedirects > 0) ? static_cast<long>(options.maxRedirects) : 0L;
            curl_easy_setopt(curlHandle, CURLOPT_MAXREDIRS, maxRedirects);
        }
        else
        {
            curl_easy_setopt(curlHandle, CURLOPT_FOLLOWLOCATION, 0L);
        }
        // 多线程/超时场景下避免信号（Unix 下尤其重要）
        curl_easy_setopt(curlHandle, CURLOPT_NOSIGNAL, 1L);

#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x071900)
        curl_easy_setopt(curlHandle, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curlHandle, CURLOPT_TCP_KEEPIDLE, 60L);
        curl_easy_setopt(curlHandle, CURLOPT_TCP_KEEPINTVL, 30L);
#endif

        std::string userAgentUtf8 = options.userAgentUtf8;
        if (userAgentUtf8.empty() && options.impersonateBrowser)
        {
            userAgentUtf8 = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
        }
        if (!userAgentUtf8.empty())
        {
            curl_easy_setopt(curlHandle, CURLOPT_USERAGENT, userAgentUtf8.c_str());
        }

        if (!options.refererUtf8.empty())
        {
            curl_easy_setopt(curlHandle, CURLOPT_REFERER, options.refererUtf8.c_str());
        }

#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x072F00)
        if (options.enableHttp2)
        {
            curl_easy_setopt(curlHandle, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
        }
#endif
        if (options.verifyTlsPeer)
        {
            curl_easy_setopt(curlHandle, CURLOPT_SSL_VERIFYPEER, 1L);
        }
        else
        {
            curl_easy_setopt(curlHandle, CURLOPT_SSL_VERIFYPEER, 0L);
        }

        if (options.verifyTlsHost)
        {
            curl_easy_setopt(curlHandle, CURLOPT_SSL_VERIFYHOST, 2L);
        }
        else
        {
            curl_easy_setopt(curlHandle, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        if (!options.caBundlePathUtf8.empty())
        {
            curl_easy_setopt(curlHandle, CURLOPT_CAINFO, options.caBundlePathUtf8.c_str());
        }
        if (!options.caPathUtf8.empty())
        {
            curl_easy_setopt(curlHandle, CURLOPT_CAPATH, options.caPathUtf8.c_str());
        }

        // 文件下载：默认不主动要求压缩，减少“Range + 压缩”带来的复杂性
        curl_easy_setopt(curlHandle, CURLOPT_ACCEPT_ENCODING, "identity");

        // 自定义 headers
        struct curl_slist* headers = nullptr;
        if (options.impersonateBrowser)
        {
            if (!TryAppendCurlSlist(headers, "Accept: */*"))
            {
                SafeCopyErrorMessage(errorBuffer, errorBufferSize, "Out of memory (curl_slist_append failed)");
                return false;
            }
        }

        for (size_t i = 0; i < options.headersUtf8.size(); i++)
        {
            if (!TryAppendCurlSlist(headers, options.headersUtf8[i].c_str()))
            {
                if (headers != nullptr)
                {
                    curl_slist_free_all(headers);
                    headers = nullptr;
                }
                if (outHeaders != nullptr)
                {
                    *outHeaders = nullptr;
                }
                SafeCopyErrorMessage(errorBuffer, errorBufferSize, "Out of memory (curl_slist_append failed)");
                return false;
            }
        }

        if (outHeaders != nullptr)
        {
            *outHeaders = headers;
        }

        if (headers != nullptr)
        {
            curl_easy_setopt(curlHandle, CURLOPT_HTTPHEADER, headers);
        }
        if (progress.enabled)
        {
            curl_easy_setopt(curlHandle, CURLOPT_NOPROGRESS, 0L);
#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x072000)
            curl_easy_setopt(curlHandle, CURLOPT_XFERINFOFUNCTION, DownloadXferInfoCallback);
            curl_easy_setopt(curlHandle, CURLOPT_XFERINFODATA, &progress);
#else
            curl_easy_setopt(curlHandle, CURLOPT_PROGRESSFUNCTION, DownloadProgressCallback);
            curl_easy_setopt(curlHandle, CURLOPT_PROGRESSDATA, &progress);
#endif
        }
        else
        {
            curl_easy_setopt(curlHandle, CURLOPT_NOPROGRESS, 1L);
        }

        ApplyProxySettings(curlHandle, urlUtf8, options.proxy);
        return true;
    }

    struct GbProbeInfo
    {
        bool ok = false;
        bool acceptRangesBytes = false;
        bool totalSizeKnown = false;
        size_t totalBytes = 0;
        std::string fileNameUtf8 = "";
        std::string contentTypeUtf8 = "";
        std::string effectiveUrlUtf8 = "";
    };

    struct GbProbeHeaderState
    {
        GbProbeInfo* info = nullptr;
    };

    static size_t ProbeHeaderCallback(char* buffer, size_t size, size_t nitems, void* userData)
    {
        size_t totalSize = 0;
        if (!TryComputeTotalSize(size, nitems, totalSize))
        {
            return 0;
        }
        if (userData == nullptr || buffer == nullptr || totalSize == 0)
        {
            return totalSize;
        }

        GbProbeHeaderState* state = static_cast<GbProbeHeaderState*>(userData);
        if (state == nullptr || state->info == nullptr)
        {
            return totalSize;
        }

        try
        {
            std::string headerLine(buffer, totalSize);
            headerLine = TrimCopy(headerLine);

            const std::string lower = ToLowerCopy(headerLine);

            if (lower.find("accept-ranges:") == 0)
            {
                const std::string valueText = TrimCopy(headerLine.substr(std::string("accept-ranges:").size()));
                if (ToLowerCopy(valueText).find("bytes") != std::string::npos)
                {
                    state->info->acceptRangesBytes = true;
                }
            }
            else if (lower.find("content-range:") == 0)
            {
                // bytes start-end/total
                const std::string valueText = TrimCopy(headerLine.substr(std::string("content-range:").size()));
                const std::string valueLower = ToLowerCopy(valueText);
                if (valueLower.find("bytes") == 0)
                {
                    state->info->acceptRangesBytes = true;
                }

                const size_t slashPos = valueText.find('/');
                if (slashPos != std::string::npos && slashPos + 1 < valueText.size())
                {
                    const std::string totalText = TrimCopy(valueText.substr(slashPos + 1));
                    unsigned long long v = 0;
                    size_t totalBytes = 0;
                    if (totalText != "*" && TryParseUnsignedLongLong(totalText, v) && TryCastUnsignedLongLongToSize(v, totalBytes))
                    {
                        state->info->totalSizeKnown = true;
                        state->info->totalBytes = totalBytes;
                    }
                }
            }
            else if (lower.find("content-length:") == 0)
            {
                const std::string valueText = TrimCopy(headerLine.substr(std::string("content-length:").size()));
                unsigned long long v = 0;
                size_t totalBytes = 0;
                if (TryParseUnsignedLongLong(valueText, v) && TryCastUnsignedLongLongToSize(v, totalBytes))
                {
                    state->info->totalSizeKnown = true;
                    state->info->totalBytes = totalBytes;
                }
            }
            else if (lower.find("content-type:") == 0)
            {
                state->info->contentTypeUtf8 = TrimCopy(headerLine.substr(std::string("content-type:").size()));
            }
            else if (lower.find("content-disposition:") == 0)
            {
                const std::string valueText = TrimCopy(headerLine.substr(std::string("content-disposition:").size()));
                std::string fileNameUtf8;
                if (ParseContentDispositionFileNameUtf8(valueText, fileNameUtf8))
                {
                    state->info->fileNameUtf8 = fileNameUtf8;
                }
            }

            return totalSize;
        }
        catch (...)
        {
            return 0;
        }
    }

    struct GbProbeWriteState
    {
        size_t receivedBytes = 0;
        size_t maxBytes = 1024;
    };

    static GB_NetworkRequestOptions MakeProbeOptions(const GB_NetworkRequestOptions& options)
    {
        // 仅用于“探测”是否支持 Range/总大小等信息。
        // 探测请求应当有一个合理上限，避免用户将 totalTimeoutMs 设为 0（无限）导致探测卡死。
        GB_NetworkRequestOptions probeOptions = options;

        constexpr unsigned int kDefaultProbeTotalTimeoutMs = 15000;
        constexpr unsigned int kDefaultProbeConnectTimeoutMs = 5000;

        if (probeOptions.totalTimeoutMs == 0)
        {
            probeOptions.totalTimeoutMs = kDefaultProbeTotalTimeoutMs;
        }
        if (probeOptions.connectTimeoutMs == 0)
        {
            probeOptions.connectTimeoutMs = kDefaultProbeConnectTimeoutMs;
        }

        // 探测不需要收集 response headers
        probeOptions.includeResponseHeaders = false;
        return probeOptions;
    }

    static size_t ProbeWriteCallback(char* ptr, size_t size, size_t nmemb, void* userData)
    {
        (void)ptr;

        size_t totalSize = 0;
        if (!TryComputeTotalSize(size, nmemb, totalSize))
        {
            return 0;
        }
        if (userData == nullptr)
        {
            return totalSize;
        }

        GbProbeWriteState* state = static_cast<GbProbeWriteState*>(userData);
        if (state == nullptr)
        {
            return totalSize;
        }

        size_t newReceivedBytes = 0;
        if (!TryAddSize(state->receivedBytes, totalSize, newReceivedBytes))
        {
            return 0;
        }
        state->receivedBytes = newReceivedBytes;

        // 如果服务端忽略 Range 且开始狂发 body，这里尽快中止
        if (state->receivedBytes > state->maxBytes)
        {
            return 0;
        }

        return totalSize;
    }
    static GbProbeInfo ProbeUrlForRangeAndSize(const std::string& urlUtf8, const GB_NetworkRequestOptions& options)
    {
        if (!EnsureCurlGlobalInit())
        {
            GbProbeInfo info;
            info.ok = false;
            return info;
        }

        const GB_NetworkRequestOptions probeOptions = MakeProbeOptions(options);

        GbProbeInfo info;

        // 先 HEAD：无 body 成本
        {
            CURL* curlHandle = curl_easy_init();
            if (curlHandle != nullptr)
            {
                char errorBuffer[CURL_ERROR_SIZE] = { 0 };
                struct curl_slist* headers = nullptr;

                GbDownloadProgressPointers disabledProgress{};
                if (!ConfigureDownloadCurlCommon(curlHandle, urlUtf8, probeOptions, disabledProgress, errorBuffer, sizeof(errorBuffer), &headers))
                {
                    if (headers != nullptr)
                    {
                        curl_slist_free_all(headers);
                    }
                    curl_easy_cleanup(curlHandle);
                    info.ok = false;
                    return info;
                }

                GbProbeHeaderState headerState;
                headerState.info = &info;

                curl_easy_setopt(curlHandle, CURLOPT_NOBODY, 1L);
                curl_easy_setopt(curlHandle, CURLOPT_HEADERFUNCTION, ProbeHeaderCallback);
                curl_easy_setopt(curlHandle, CURLOPT_HEADERDATA, &headerState);
                curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, nullptr);

                CURLcode res = curl_easy_perform(curlHandle);

                long httpCode = 0;
                curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &httpCode);
                const bool headOk = (res == CURLE_OK) && (httpCode == 0 || (httpCode >= 200 && httpCode < 400));
                info.ok = headOk;

                char* effectiveUrl = nullptr;
                curl_easy_getinfo(curlHandle, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
                if (effectiveUrl != nullptr)
                {
                    info.effectiveUrlUtf8 = effectiveUrl;
                }

                if (headers != nullptr)
                {
                    curl_slist_free_all(headers);
                }
                curl_easy_cleanup(curlHandle);

                // 仅当 HEAD 已确认“可用的总大小 + Accept-Ranges: bytes”时，才提前返回。
                // 否则继续做 Range=0-0 探测以确认 Range 支持。
                if (info.ok && info.totalSizeKnown && info.acceptRangesBytes)
                {
                    return info;
                }
            }
        }

        // 再用 Range=0-0 做探测（如果服务端忽略 Range，会被 ProbeWriteCallback 快速中止）
        {
            CURL* curlHandle = curl_easy_init();
            if (curlHandle == nullptr)
            {
                return info;
            }

            char errorBuffer[CURL_ERROR_SIZE] = { 0 };
            struct curl_slist* headers = nullptr;

            GbDownloadProgressPointers disabledProgress{};
            if (!ConfigureDownloadCurlCommon(curlHandle, urlUtf8, probeOptions, disabledProgress, errorBuffer, sizeof(errorBuffer), &headers))
            {
                if (headers != nullptr)
                {
                    curl_slist_free_all(headers);
                }
                curl_easy_cleanup(curlHandle);
                info.ok = false;
                return info;
            }

            GbProbeHeaderState headerState;
            headerState.info = &info;

            GbProbeWriteState writeState;
            writeState.receivedBytes = 0;
            writeState.maxBytes = 1024;

            curl_easy_setopt(curlHandle, CURLOPT_NOBODY, 0L);
            curl_easy_setopt(curlHandle, CURLOPT_HTTPGET, 1L);
            curl_easy_setopt(curlHandle, CURLOPT_RANGE, "0-0");

            curl_easy_setopt(curlHandle, CURLOPT_HEADERFUNCTION, ProbeHeaderCallback);
            curl_easy_setopt(curlHandle, CURLOPT_HEADERDATA, &headerState);
            curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, ProbeWriteCallback);
            curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &writeState);

            CURLcode res = curl_easy_perform(curlHandle);

            long httpCode = 0;
            curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &httpCode);

            // res 可能因为我们主动中止（返回 0）而变成 CURLE_WRITE_ERROR；但 headers 可能已足够。
            const bool rangeOk = (res == CURLE_OK || res == CURLE_WRITE_ERROR) && (httpCode == 0 || (httpCode >= 200 && httpCode < 400));
            info.ok = rangeOk;

            char* effectiveUrl = nullptr;
            curl_easy_getinfo(curlHandle, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
            if (effectiveUrl != nullptr)
            {
                info.effectiveUrlUtf8 = effectiveUrl;
            }

            if (headers != nullptr)
            {
                curl_slist_free_all(headers);
            }
            curl_easy_cleanup(curlHandle);
        }

        return info;
    }

    static bool ShouldUseMultiThreadDownload(size_t totalBytes, bool acceptRangesBytes)
    {
        const size_t kMinMultiThreadBytes = 32ULL * 1024ULL * 1024ULL;
        if (!acceptRangesBytes)
        {
            return false;
        }
        if (totalBytes < kMinMultiThreadBytes)
        {
            return false;
        }
        return true;
    }

    struct GbRangeChunkState
    {
        unsigned char* bufferBase = nullptr;
        size_t baseOffset = 0;
        size_t expectedBytes = 0;
        size_t writtenBytes = 0;
        GbDownloadProgressPointers progress;
    };

    static size_t RangeWriteCallback(char* ptr, size_t size, size_t nmemb, void* userData)
    {
        size_t totalSize = 0;
        if (!TryComputeTotalSize(size, nmemb, totalSize))
        {
            return 0;
        }
        if (userData == nullptr || ptr == nullptr || totalSize == 0)
        {
            return 0;
        }

        GbRangeChunkState* state = static_cast<GbRangeChunkState*>(userData);
        if (state == nullptr || state->bufferBase == nullptr)
        {
            return 0;
        }

        size_t newWrittenBytes = 0;
        if (!TryAddSize(state->writtenBytes, totalSize, newWrittenBytes))
        {
            return 0;
        }
        if (newWrittenBytes > state->expectedBytes)
        {
            // 服务端忽略 Range 时，可能会超出预期，直接失败让上层回退
            return 0;
        }

        std::memcpy(state->bufferBase + state->baseOffset + state->writtenBytes, ptr, totalSize);
        state->writtenBytes = newWrittenBytes;

        if (state->progress.enabled)
        {
            state->progress.downloadedBytesPtr->fetch_add(totalSize, std::memory_order_relaxed);
        }

        return totalSize;
    }

    struct GbFileRangeChunkState
    {
#ifdef _WIN32
        HANDLE fileHandle = INVALID_HANDLE_VALUE;
#else
        int fileDescriptor = -1;
#endif
        size_t expectedBytes = 0;
        size_t writtenBytes = 0;
        GbDownloadProgressPointers progress;
    };

    static void CloseFileRangeChunkState(GbFileRangeChunkState& state)
    {
#ifdef _WIN32
        if (state.fileHandle != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(state.fileHandle);
            state.fileHandle = INVALID_HANDLE_VALUE;
        }
#else
        if (state.fileDescriptor >= 0)
        {
            ::close(state.fileDescriptor);
            state.fileDescriptor = -1;
        }
#endif
    }

    static bool OpenFileRangeChunkForWriteUtf8(const std::string& filePathUtf8, size_t rangeBegin, GbFileRangeChunkState& state, std::string& errorMessageUtf8)
    {
        errorMessageUtf8.clear();
        CloseFileRangeChunkState(state);

        if (filePathUtf8.empty())
        {
            errorMessageUtf8 = "File path is empty";
            return false;
        }

#ifdef _WIN32
        const std::wstring filePathW = GB_Utf8ToWString(filePathUtf8);
        if (filePathW.empty())
        {
            errorMessageUtf8 = "UTF-8 to wide string conversion failed";
            return false;
        }

        state.fileHandle = ::CreateFileW(filePathW.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (state.fileHandle == INVALID_HANDLE_VALUE)
        {
            errorMessageUtf8 = "CreateFileW failed";
            return false;
        }

        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(rangeBegin);
        if (::SetFilePointerEx(state.fileHandle, li, nullptr, FILE_BEGIN) == 0)
        {
            errorMessageUtf8 = "SetFilePointerEx failed";
            CloseFileRangeChunkState(state);
            return false;
        }
        return true;
#else
        state.fileDescriptor = ::open(filePathUtf8.c_str(), O_WRONLY);
        if (state.fileDescriptor < 0)
        {
            errorMessageUtf8 = "open failed";
            return false;
        }

        if (::lseek(state.fileDescriptor, static_cast<off_t>(rangeBegin), SEEK_SET) == static_cast<off_t>(-1))
        {
            errorMessageUtf8 = "lseek failed";
            CloseFileRangeChunkState(state);
            return false;
        }
        return true;
#endif
    }

    static bool WriteAllToFileRangeChunk(GbFileRangeChunkState& state, const unsigned char* bytes, size_t totalSize)
    {
        if (bytes == nullptr || totalSize == 0)
        {
            return true;
        }

#ifdef _WIN32
        if (state.fileHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        size_t remaining = totalSize;
        const unsigned char* p = bytes;
        while (remaining > 0)
        {
            const DWORD chunk = (remaining > static_cast<size_t>(std::numeric_limits<DWORD>::max())) ? std::numeric_limits<DWORD>::max() : static_cast<DWORD>(remaining);
            DWORD written = 0;
            if (::WriteFile(state.fileHandle, p, chunk, &written, nullptr) == 0)
            {
                return false;
            }
            if (written == 0)
            {
                return false;
            }
            p += written;
            remaining -= written;
        }

        return true;
#else
        if (state.fileDescriptor < 0)
        {
            return false;
        }

        size_t remaining = totalSize;
        const unsigned char* p = bytes;
        while (remaining > 0)
        {
            const size_t maxChunk = static_cast<size_t>(std::numeric_limits<ssize_t>::max());
            const size_t chunk = (remaining > maxChunk) ? maxChunk : remaining;
            const ssize_t w = ::write(state.fileDescriptor, p, chunk);
            if (w < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return false;
            }
            if (w == 0)
            {
                return false;
            }
            p += static_cast<size_t>(w);
            remaining -= static_cast<size_t>(w);
        }

        return true;
#endif
    }

    static size_t RangeFileWriteCallback(char* ptr, size_t size, size_t nmemb, void* userData)
    {
        size_t totalSize = 0;
        if (!TryComputeTotalSize(size, nmemb, totalSize))
        {
            return 0;
        }
        if (userData == nullptr || ptr == nullptr || totalSize == 0)
        {
            return 0;
        }

        GbFileRangeChunkState* state = static_cast<GbFileRangeChunkState*>(userData);
        if (state == nullptr)
        {
            return 0;
        }

        size_t newWrittenBytes = 0;
        if (!TryAddSize(state->writtenBytes, totalSize, newWrittenBytes))
        {
            return 0;
        }
        if (newWrittenBytes > state->expectedBytes)
        {
            return 0;
        }

        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(ptr);
        if (!WriteAllToFileRangeChunk(*state, bytes, totalSize))
        {
            return 0;
        }

        state->writtenBytes = newWrittenBytes;
        if (state->progress.enabled)
        {
            state->progress.downloadedBytesPtr->fetch_add(totalSize, std::memory_order_relaxed);
        }

        return totalSize;
    }

    struct GbRangeThreadResult
    {
        CURLcode curlCode = CURLE_OK;
        long httpStatusCode = 0;
        std::string errorMessageUtf8 = "";
    };

    static GbRangeThreadResult DownloadRangeChunk(const std::string& urlUtf8, const GB_NetworkRequestOptions& options, size_t rangeBegin, size_t rangeEnd, GbRangeChunkState& state)
    {
        GbRangeThreadResult result;

        CURL* curlHandle = curl_easy_init();
        if (curlHandle == nullptr)
        {
            result.curlCode = CURLE_FAILED_INIT;
            result.errorMessageUtf8 = "curl_easy_init failed";
            return result;
        }

        char errorBuffer[CURL_ERROR_SIZE] = { 0 };
        struct curl_slist* headers = nullptr;

        GbDownloadProgressPointers disabledProgress{};
        if (!ConfigureDownloadCurlCommon(curlHandle, urlUtf8, options, disabledProgress, errorBuffer, sizeof(errorBuffer), &headers))
        {
            if (headers != nullptr)
            {
                curl_slist_free_all(headers);
            }
            curl_easy_cleanup(curlHandle);
            result.curlCode = CURLE_FAILED_INIT;
            result.errorMessageUtf8 = "Failed to configure curl options";
            return result;
        }

        const std::string rangeText = std::to_string(static_cast<unsigned long long>(rangeBegin)) + "-" + std::to_string(static_cast<unsigned long long>(rangeEnd));
        curl_easy_setopt(curlHandle, CURLOPT_RANGE, rangeText.c_str());

        curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, RangeWriteCallback);
        curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(curlHandle, CURLOPT_HEADERFUNCTION, nullptr);

        const CURLcode res = curl_easy_perform(curlHandle);

        long httpCode = 0;
        curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &httpCode);
        result.httpStatusCode = httpCode;

        if (headers != nullptr)
        {
            curl_slist_free_all(headers);
        }

        curl_easy_cleanup(curlHandle);

        result.curlCode = res;
        if (res != CURLE_OK)
        {
            result.errorMessageUtf8 = (errorBuffer[0] != '\0') ? errorBuffer : curl_easy_strerror(res);
        }
        else if (state.writtenBytes != state.expectedBytes)
        {
            result.curlCode = CURLE_WRITE_ERROR;
            result.errorMessageUtf8 = "Range download size mismatch";
        }
        else if (result.httpStatusCode != 0 && result.httpStatusCode != 206)
        {
            result.curlCode = CURLE_HTTP_RETURNED_ERROR;
            result.errorMessageUtf8 = "HTTP status " + std::to_string(static_cast<long long>(result.httpStatusCode));
        }

        return result;
    }

    static GB_NetworkDownloadedFile DownloadFileSingleThread(const std::string& urlUtf8, const GB_NetworkRequestOptions& options, GbDownloadProgressPointers progress)
    {
        if (!EnsureCurlGlobalInit())
        {
            GB_NetworkDownloadedFile result;
            result.ok = false;
            result.curlErrorCode = static_cast<int>(CURLE_FAILED_INIT);
            result.errorMessageUtf8 = "curl_global_init failed";
            return result;
        }

        GB_NetworkDownloadedFile result;

        if (progress.enabled)
        {
            progress.totalBytesPtr->store(0, std::memory_order_relaxed);
            progress.downloadedBytesPtr->store(0, std::memory_order_relaxed);
        }

        CURL* curlHandle = curl_easy_init();
        if (curlHandle == nullptr)
        {
            result.ok = false;
            result.errorMessageUtf8 = "curl_easy_init failed";
            result.curlErrorCode = static_cast<int>(CURLE_FAILED_INIT);
            return result;
        }

        char errorBuffer[CURL_ERROR_SIZE] = { 0 };
        struct curl_slist* headers = nullptr;

        if (!ConfigureDownloadCurlCommon(curlHandle, urlUtf8, options, progress, errorBuffer, sizeof(errorBuffer), &headers))
        {
            curl_easy_cleanup(curlHandle);
            result.ok = false;
            result.errorMessageUtf8 = "Failed to configure curl options";
            return result;
        }

        GbDownloadHeaderState headerState;
        headerState.result = &result;
        headerState.progress = progress;
        headerState.includeResponseHeaders = options.includeResponseHeaders;

        GbWriteState writeState;
        writeState.data = &result.data;
        writeState.progress = progress;

        curl_easy_setopt(curlHandle, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curlHandle, CURLOPT_NOBODY, 0L);

        curl_easy_setopt(curlHandle, CURLOPT_HEADERFUNCTION, DownloadHeaderCallback);
        curl_easy_setopt(curlHandle, CURLOPT_HEADERDATA, &headerState);

        curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, DownloadWriteCallback);
        curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &writeState);

        const CURLcode res = curl_easy_perform(curlHandle);

        long httpCode = 0;
        curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &httpCode);
        result.httpStatusCode = httpCode;

        char* effectiveUrl = nullptr;
        curl_easy_getinfo(curlHandle, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
        if (effectiveUrl != nullptr)
        {
            result.effectiveUrlUtf8 = effectiveUrl;
        }

        char* contentType = nullptr;
        curl_easy_getinfo(curlHandle, CURLINFO_CONTENT_TYPE, &contentType);
        if (contentType != nullptr && result.contentTypeUtf8.empty())
        {
            result.contentTypeUtf8 = contentType;
        }

#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x073700)
        if (!result.totalSizeKnown)
        {
            curl_off_t contentLength = -1;
            const CURLcode lenRes = curl_easy_getinfo(curlHandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &contentLength);
            if (lenRes == CURLE_OK && contentLength > 0)
            {
                size_t totalBytes = 0;
                if (TryCastUnsignedLongLongToSize(static_cast<unsigned long long>(contentLength), totalBytes))
                {
                    result.totalSizeKnown = true;
                    result.totalBytes = totalBytes;
                    if (progress.enabled)
                    {
                        progress.totalBytesPtr->store(totalBytes, std::memory_order_relaxed);
                    }
                }
            }
        }
#else
        if (!result.totalSizeKnown)
        {
            double contentLength = -1.0;
            const CURLcode lenRes = curl_easy_getinfo(curlHandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &contentLength);
            if (lenRes == CURLE_OK && contentLength > 0.0)
            {
                const unsigned long long contentLengthUll = static_cast<unsigned long long>(contentLength + 0.5);
                size_t totalBytes = 0;
                if (TryCastUnsignedLongLongToSize(contentLengthUll, totalBytes))
                {
                    result.totalSizeKnown = true;
                    result.totalBytes = totalBytes;
                    if (progress.enabled)
                    {
                        progress.totalBytesPtr->store(totalBytes, std::memory_order_relaxed);
                    }
                }
            }
        }
#endif

        if (headers != nullptr)
        {
            curl_slist_free_all(headers);
        }

        curl_easy_cleanup(curlHandle);

        result.curlErrorCode = static_cast<int>(res);
        if (res != CURLE_OK)
        {
            result.ok = false;
            result.errorMessageUtf8 = (errorBuffer[0] != '\0') ? errorBuffer : curl_easy_strerror(res);
            return result;
        }

        if (result.fileNameUtf8.empty())
        {
            const std::string baseUrl = !result.effectiveUrlUtf8.empty() ? result.effectiveUrlUtf8 : urlUtf8;
            result.fileNameUtf8 = ExtractFileNameFromUrlUtf8(baseUrl);
        }

        if (httpCode >= 200 && httpCode < 300)
        {
            result.ok = true;
        }
        else
        {
            result.ok = false;
            result.errorMessageUtf8 = "HTTP status " + std::to_string(static_cast<long long>(httpCode));
        }

        return result;
    }

    struct GbMultiCurlChunkContext
    {
        CURL* easyHandle = nullptr;
        struct curl_slist* headers = nullptr;

        std::string rangeText = "";

        char errorBuffer[CURL_ERROR_SIZE] = { 0 };
        GbRangeChunkState* chunkState = nullptr;

        CURLcode curlCode = CURLE_OK;
        long httpStatusCode = 0;
        bool completed = false;

        std::string errorMessageUtf8 = "";
    };

    struct GbChunkRange
    {
        size_t begin = 0;
        size_t end = 0;
    };

    enum class GbParallelPrepareResult
    {
        Ok,
        FallbackToSingleThread,
        FatalError
    };

    static void ResetDownloadProgress(GbDownloadProgressPointers progress)
    {
        if (progress.enabled)
        {
            progress.totalBytesPtr->store(0, std::memory_order_relaxed);
            progress.downloadedBytesPtr->store(0, std::memory_order_relaxed);
        }
    }

    static size_t ComputeParallelPartCount(size_t totalBytes)
    {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0)
        {
            hw = 4;
        }

        size_t partCount = static_cast<size_t>(hw);
        if (partCount > 8)
        {
            partCount = 8;
        }
        if (partCount < 2)
        {
            partCount = 2;
        }

        const size_t kMinChunkSize = 4ULL * 1024ULL * 1024ULL;
        if (partCount > 0 && totalBytes / partCount < kMinChunkSize)
        {
            partCount = std::max<size_t>(2, totalBytes / kMinChunkSize);
        }

        return partCount;
    }

    static bool BuildChunkRanges(size_t totalBytes, size_t partCount, std::vector<GbChunkRange>& outRanges)
    {
        outRanges.clear();
        if (totalBytes == 0 || partCount < 2)
        {
            return false;
        }

        const size_t chunkSize = totalBytes / partCount;
        if (chunkSize == 0)
        {
            return false;
        }

        outRanges.reserve(partCount);

        size_t begin = 0;
        for (size_t i = 0; i < partCount; i++)
        {
            size_t end = 0;
            if (i + 1 == partCount)
            {
                end = totalBytes - 1;
            }
            else
            {
                end = begin + chunkSize - 1;
            }

            GbChunkRange range;
            range.begin = begin;
            range.end = end;
            outRanges.push_back(range);

            begin = end + 1;
        }

        return true;
    }

    static GbParallelPrepareResult PrepareParallelDownloadPlan(const std::string& urlUtf8, const GbProbeInfo& probe, GbDownloadProgressPointers progress, GB_NetworkDownloadedFile& outResult, std::string& outFinalUrl, std::vector<GbChunkRange>& outRanges, std::vector<GbRangeChunkState>& outChunkStates)
    {
        outResult = GB_NetworkDownloadedFile();
        outFinalUrl.clear();
        outRanges.clear();
        outChunkStates.clear();

        if (!probe.totalSizeKnown || probe.totalBytes == 0)
        {
            return GbParallelPrepareResult::FallbackToSingleThread;
        }

        const size_t totalBytes = probe.totalBytes;
        const size_t partCount = ComputeParallelPartCount(totalBytes);
        if (partCount < 2)
        {
            return GbParallelPrepareResult::FallbackToSingleThread;
        }

        if (!BuildChunkRanges(totalBytes, partCount, outRanges))
        {
            return GbParallelPrepareResult::FallbackToSingleThread;
        }

        if (progress.enabled)
        {
            progress.totalBytesPtr->store(totalBytes, std::memory_order_relaxed);
            progress.downloadedBytesPtr->store(0, std::memory_order_relaxed);
        }

        outResult.totalSizeKnown = true;
        outResult.totalBytes = totalBytes;
        outResult.fileNameUtf8 = !probe.fileNameUtf8.empty() ? probe.fileNameUtf8 : ExtractFileNameFromUrlUtf8(!probe.effectiveUrlUtf8.empty() ? probe.effectiveUrlUtf8 : urlUtf8);
        outResult.contentTypeUtf8 = probe.contentTypeUtf8;

        try
        {
            outResult.data.resize(totalBytes);
        }
        catch (...)
        {
            outResult.ok = false;
            outResult.curlErrorCode = static_cast<int>(CURLE_OUT_OF_MEMORY);
            outResult.errorMessageUtf8 = "Not enough memory to allocate download buffer";
            ResetDownloadProgress(progress);
            return GbParallelPrepareResult::FatalError;
        }

        outChunkStates.resize(outRanges.size());
        for (size_t i = 0; i < outRanges.size(); i++)
        {
            const GbChunkRange& range = outRanges[i];

            GbRangeChunkState& state = outChunkStates[i];
            state.bufferBase = outResult.data.data();
            state.baseOffset = range.begin;
            state.expectedBytes = range.end - range.begin + 1;
            state.writtenBytes = 0;
            state.progress = progress;
        }

        outFinalUrl = !probe.effectiveUrlUtf8.empty() ? probe.effectiveUrlUtf8 : urlUtf8;
        return GbParallelPrepareResult::Ok;
    }

    static void MarkParallelDownloadSuccess(const std::string& finalUrl, GB_NetworkDownloadedFile& result)
    {
        result.ok = true;
        result.curlErrorCode = static_cast<int>(CURLE_OK);
        result.httpStatusCode = 206; // 多个 206 合成的“完成态”
        result.effectiveUrlUtf8 = finalUrl;
    }

    static GB_NetworkDownloadedFile DownloadFileMultiCurl(const std::string& urlUtf8, const GB_NetworkRequestOptions& options, const GbProbeInfo& probe, GbDownloadProgressPointers progress)
    {
        if (!EnsureCurlGlobalInit())
        {
            GB_NetworkDownloadedFile result;
            result.ok = false;
            result.curlErrorCode = static_cast<int>(CURLE_FAILED_INIT);
            result.errorMessageUtf8 = "curl_global_init failed";
            return result;
        }

        GB_NetworkDownloadedFile result;
        std::string finalUrl;
        std::vector<GbChunkRange> chunkRanges;
        std::vector<GbRangeChunkState> chunkStates;

        const GbParallelPrepareResult prepareRes = PrepareParallelDownloadPlan(urlUtf8, probe, progress, result, finalUrl, chunkRanges, chunkStates);
        if (prepareRes == GbParallelPrepareResult::FatalError)
        {
            return result;
        }
        if (prepareRes != GbParallelPrepareResult::Ok)
        {
            return DownloadFileSingleThread(urlUtf8, options, progress);
        }

        const size_t numTransfers = chunkRanges.size();

        CURLM* multiHandle = curl_multi_init();
        if (multiHandle == nullptr)
        {
            ResetDownloadProgress(progress);
            return DownloadFileSingleThread(urlUtf8, options, progress);
        }

        struct CurlMultiGuard
        {
            CURLM* handle = nullptr;
            ~CurlMultiGuard()
            {
                if (handle != nullptr)
                {
                    curl_multi_cleanup(handle);
                    handle = nullptr;
                }
            }
        };

        CurlMultiGuard multiGuard;
        multiGuard.handle = multiHandle;

#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x071E00)
        curl_multi_setopt(multiHandle, CURLMOPT_MAX_TOTAL_CONNECTIONS, static_cast<long>(numTransfers));
        curl_multi_setopt(multiHandle, CURLMOPT_MAX_HOST_CONNECTIONS, static_cast<long>(numTransfers));
#endif

        std::vector<GbMultiCurlChunkContext> contexts(numTransfers);

        auto CleanupMultiCurlContexts = [&](std::vector<GbMultiCurlChunkContext>& ctxs) {
            for (size_t i = 0; i < ctxs.size(); i++)
            {
                GbMultiCurlChunkContext& ctx = ctxs[i];
                if (ctx.easyHandle != nullptr)
                {
                    curl_multi_remove_handle(multiHandle, ctx.easyHandle);
                    curl_easy_cleanup(ctx.easyHandle);
                    ctx.easyHandle = nullptr;
                }
                if (ctx.headers != nullptr)
                {
                    curl_slist_free_all(ctx.headers);
                    ctx.headers = nullptr;
                }
            }
            };

        auto DrainDoneMessages = [&](bool& hasError, std::string& firstError) {
            int msgsInQueue = 0;
            while (true)
            {
                CURLMsg* msg = curl_multi_info_read(multiHandle, &msgsInQueue);
                if (msg == nullptr)
                {
                    break;
                }
                if (msg->msg != CURLMSG_DONE)
                {
                    continue;
                }

                CURL* easyHandle = msg->easy_handle;
                GbMultiCurlChunkContext* ctxPtr = nullptr;
                curl_easy_getinfo(easyHandle, CURLINFO_PRIVATE, &ctxPtr);

                if (ctxPtr != nullptr)
                {
                    ctxPtr->curlCode = msg->data.result;
                    ctxPtr->completed = true;

                    long httpCode = 0;
                    curl_easy_getinfo(easyHandle, CURLINFO_RESPONSE_CODE, &httpCode);
                    ctxPtr->httpStatusCode = httpCode;

                    if (ctxPtr->curlCode != CURLE_OK)
                    {
                        ctxPtr->errorMessageUtf8 = (ctxPtr->errorBuffer[0] != '\0') ? ctxPtr->errorBuffer : curl_easy_strerror(ctxPtr->curlCode);
                        if (!hasError)
                        {
                            hasError = true;
                            firstError = ctxPtr->errorMessageUtf8;
                        }
                    }
                    else if (ctxPtr->httpStatusCode != 0 && ctxPtr->httpStatusCode != 206)
                    {
                        ctxPtr->errorMessageUtf8 = "HTTP status " + std::to_string(static_cast<long long>(ctxPtr->httpStatusCode));
                        if (!hasError)
                        {
                            hasError = true;
                            firstError = ctxPtr->errorMessageUtf8;
                        }
                    }
                }
                else
                {
                    if (!hasError)
                    {
                        hasError = true;
                        firstError = "curl_easy_getinfo(CURLINFO_PRIVATE) failed";
                    }
                }

                // 移除并销毁完成的 easy 句柄
                curl_multi_remove_handle(multiHandle, easyHandle);
                curl_easy_cleanup(easyHandle);

                if (ctxPtr != nullptr)
                {
                    ctxPtr->easyHandle = nullptr;
                    if (ctxPtr->headers != nullptr)
                    {
                        curl_slist_free_all(ctxPtr->headers);
                        ctxPtr->headers = nullptr;
                    }
                }
            }
            };

        bool setupOk = true;
        for (size_t i = 0; i < numTransfers; i++)
        {
            GbRangeChunkState& state = chunkStates[i];
            const GbChunkRange& range = chunkRanges[i];

            GbMultiCurlChunkContext& ctx = contexts[i];
            std::memset(ctx.errorBuffer, 0, sizeof(ctx.errorBuffer));
            ctx.chunkState = &state;

            CURL* easyHandle = curl_easy_init();
            if (easyHandle == nullptr)
            {
                setupOk = false;
                break;
            }

            ctx.easyHandle = easyHandle;

            // 分段下载不使用 XFERINFO（避免多个句柄互相覆盖），进度在 RangeWriteCallback 中累加。
            GbDownloadProgressPointers disabledProgress{};
            if (!ConfigureDownloadCurlCommon(easyHandle, finalUrl, options, disabledProgress, ctx.errorBuffer, sizeof(ctx.errorBuffer), &ctx.headers))
            {
                setupOk = false;
                break;
            }

            ctx.rangeText = std::to_string(static_cast<unsigned long long>(range.begin)) + "-" + std::to_string(static_cast<unsigned long long>(range.end));
            curl_easy_setopt(easyHandle, CURLOPT_RANGE, ctx.rangeText.c_str());
            curl_easy_setopt(easyHandle, CURLOPT_HTTPGET, 1L);
            curl_easy_setopt(easyHandle, CURLOPT_NOBODY, 0L);
            curl_easy_setopt(easyHandle, CURLOPT_HEADERFUNCTION, nullptr);
            curl_easy_setopt(easyHandle, CURLOPT_WRITEFUNCTION, RangeWriteCallback);
            curl_easy_setopt(easyHandle, CURLOPT_WRITEDATA, &state);

            // 便于在 CURLMSG_DONE 中定位到上下文
            curl_easy_setopt(easyHandle, CURLOPT_PRIVATE, &ctx);

            const CURLMcode addRes = curl_multi_add_handle(multiHandle, easyHandle);
            if (addRes != CURLM_OK)
            {
                setupOk = false;
                break;
            }
        }

        if (!setupOk)
        {
            CleanupMultiCurlContexts(contexts);
            ResetDownloadProgress(progress);
            return DownloadFileSingleThread(urlUtf8, options, progress);
        }

        int stillRunning = 0;
        CURLMcode mc = CURLM_OK;
        do
        {
            mc = curl_multi_perform(multiHandle, &stillRunning);
        } while (mc == CURLM_CALL_MULTI_PERFORM);

        if (mc != CURLM_OK)
        {
            CleanupMultiCurlContexts(contexts);
            ResetDownloadProgress(progress);
            return DownloadFileSingleThread(urlUtf8, options, progress);
        }

        bool hasError = false;
        std::string firstError;

        while (stillRunning && !hasError)
        {
            int numFds = 0;
            mc = curl_multi_wait(multiHandle, nullptr, 0, 1000, &numFds);
            if (mc != CURLM_OK)
            {
                hasError = true;
                firstError = "curl_multi_wait failed";
                break;
            }

            do
            {
                mc = curl_multi_perform(multiHandle, &stillRunning);
            } while (mc == CURLM_CALL_MULTI_PERFORM);

            if (mc != CURLM_OK)
            {
                hasError = true;
                firstError = "curl_multi_perform failed";
                break;
            }

            DrainDoneMessages(hasError, firstError);
        }

        // stillRunning 归零后，可能仍有完成消息留在队列中
        if (!hasError)
        {
            DrainDoneMessages(hasError, firstError);
        }

        CleanupMultiCurlContexts(contexts);

        if (hasError)
        {
            ResetDownloadProgress(progress);
            return DownloadFileSingleThread(urlUtf8, options, progress);
        }

        for (size_t i = 0; i < chunkStates.size(); i++)
        {
            const GbRangeChunkState& state = chunkStates[i];
            if (state.writtenBytes != state.expectedBytes)
            {
                ResetDownloadProgress(progress);
                return DownloadFileSingleThread(urlUtf8, options, progress);
            }
        }

        MarkParallelDownloadSuccess(finalUrl, result);
        return result;
    }

    static GB_NetworkDownloadedFile DownloadFileMultiThread(const std::string& urlUtf8, const GB_NetworkRequestOptions& options, const GbProbeInfo& probe, GbDownloadProgressPointers progress)
    {
        if (!EnsureCurlGlobalInit())
        {
            GB_NetworkDownloadedFile result;
            result.ok = false;
            result.curlErrorCode = static_cast<int>(CURLE_FAILED_INIT);
            result.errorMessageUtf8 = "curl_global_init failed";
            return result;
        }

        GB_NetworkDownloadedFile result;
        std::string finalUrl;
        std::vector<GbChunkRange> chunkRanges;
        std::vector<GbRangeChunkState> chunkStates;

        const GbParallelPrepareResult prepareRes = PrepareParallelDownloadPlan(urlUtf8, probe, progress, result, finalUrl, chunkRanges, chunkStates);
        if (prepareRes == GbParallelPrepareResult::FatalError)
        {
            return result;
        }
        if (prepareRes != GbParallelPrepareResult::Ok)
        {
            return DownloadFileSingleThread(urlUtf8, options, progress);
        }

        const size_t numThreads = chunkRanges.size();

        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        std::vector<GbRangeThreadResult> threadResults;
        threadResults.resize(numThreads);

        for (size_t i = 0; i < numThreads; i++)
        {
            const size_t threadIndex = i;
            const size_t rangeBegin = chunkRanges[i].begin;
            const size_t rangeEnd = chunkRanges[i].end;

            try
            {
                threads.emplace_back([&, threadIndex, rangeBegin, rangeEnd]()
                    {
                        threadResults[threadIndex] = DownloadRangeChunk(finalUrl, options, rangeBegin, rangeEnd, chunkStates[threadIndex]);
                    });
            }
            catch (...)
            {
                for (size_t t = 0; t < threads.size(); t++)
                {
                    threads[t].join();
                }

                ResetDownloadProgress(progress);
                return DownloadFileSingleThread(urlUtf8, options, progress);
            }
        }

        for (size_t i = 0; i < threads.size(); i++)
        {
            threads[i].join();
        }

        for (size_t i = 0; i < threadResults.size(); i++)
        {
            const GbRangeThreadResult& tr = threadResults[i];
            if (tr.curlCode != CURLE_OK || (tr.httpStatusCode != 0 && tr.httpStatusCode != 206))
            {
                ResetDownloadProgress(progress);
                return DownloadFileSingleThread(urlUtf8, options, progress);
            }
        }

        for (size_t i = 0; i < chunkStates.size(); i++)
        {
            const GbRangeChunkState& state = chunkStates[i];
            if (state.writtenBytes != state.expectedBytes)
            {
                ResetDownloadProgress(progress);
                return DownloadFileSingleThread(urlUtf8, options, progress);
            }
        }

        MarkParallelDownloadSuccess(finalUrl, result);
        return result;
    }

    static bool LooksLikeFileNameWithExtensionUtf8(const std::string& fileNameUtf8)
    {
        const std::string trimmed = TrimCopy(fileNameUtf8);
        if (trimmed.empty())
        {
            return false;
        }
        if (trimmed == "." || trimmed == "..")
        {
            return false;
        }
        if (trimmed.find('/') != std::string::npos || trimmed.find('\\') != std::string::npos)
        {
            return false;
        }

        const size_t dotPos = trimmed.find_last_of('.');
        if (dotPos == std::string::npos || dotPos == 0 || dotPos + 1 >= trimmed.size())
        {
            return false;
        }

        return true;
    }

    struct GbDownloadToFileHeaderState
    {
        GB_NetworkDownloadedFileToPath* result = nullptr;
        GbDownloadProgressPointers progress;
        bool includeResponseHeaders = false;
        bool hasSeenStatusLine = false;
        std::string filePathUtf8;
        GbFileRangeChunkState* fileState = nullptr;
    };

    static size_t DownloadToFileHeaderCallback(char* buffer, size_t size, size_t nitems, void* userData)
    {
        size_t totalSize = 0;
        if (!TryComputeTotalSize(size, nitems, totalSize))
        {
            return 0;
        }
        if (userData == nullptr || buffer == nullptr || totalSize == 0)
        {
            return totalSize;
        }

        GbDownloadToFileHeaderState* state = static_cast<GbDownloadToFileHeaderState*>(userData);
        if (state == nullptr || state->result == nullptr)
        {
            return totalSize;
        }

        try
        {
            std::string headerLine(buffer, totalSize);
            headerLine = TrimCopy(headerLine);

            if (!headerLine.empty())
            {
                const std::string headerLower = ToLowerCopy(headerLine);
                if (headerLower.find("http/") == 0)
                {
                    if (state->hasSeenStatusLine)
                    {
                        // 进入新的响应段（例如重定向后的最终 200），避免将中间响应的 body 写入文件。
                        state->result->contentTypeUtf8.clear();
                        state->result->remoteFileNameUtf8.clear();
                        state->result->totalSizeKnown = false;
                        state->result->totalBytes = 0;

                        if (state->includeResponseHeaders)
                        {
                            state->result->responseHeadersUtf8.clear();
                        }

                        if (state->progress.enabled)
                        {
                            state->progress.totalBytesPtr->store(0, std::memory_order_relaxed);
                            state->progress.downloadedBytesPtr->store(0, std::memory_order_relaxed);
                        }

                        if (state->fileState != nullptr)
                        {
                            // 截断并重新打开文件，确保最终文件只包含最后一次响应的 body。
                            CloseFileRangeChunkState(*state->fileState);
                            if (!CreateOrTruncateFileUtf8(state->filePathUtf8, false, 0))
                            {
                                return 0;
                            }
                            state->fileState->expectedBytes = std::numeric_limits<size_t>::max();
                            state->fileState->writtenBytes = 0;
                            state->fileState->progress = state->progress;
                            std::string err;
                            if (!OpenFileRangeChunkForWriteUtf8(state->filePathUtf8, 0, *state->fileState, err))
                            {
                                return 0;
                            }
                        }
                    }

                    state->hasSeenStatusLine = true;
                }

                if (state->includeResponseHeaders)
                {
                    state->result->responseHeadersUtf8.push_back(headerLine);
                }
            }

            const std::string lower = ToLowerCopy(headerLine);
            if (lower.find("content-type:") == 0)
            {
                state->result->contentTypeUtf8 = TrimCopy(headerLine.substr(std::string("content-type:").size()));
            }
            else if (lower.find("content-length:") == 0)
            {
                const std::string valueText = TrimCopy(headerLine.substr(std::string("content-length:").size()));
                unsigned long long v = 0;
                size_t totalBytes = 0;
                if (TryParseUnsignedLongLong(valueText, v) && TryCastUnsignedLongLongToSize(v, totalBytes))
                {
                    state->result->totalSizeKnown = true;
                    state->result->totalBytes = totalBytes;
                    if (state->progress.enabled)
                    {
                        state->progress.totalBytesPtr->store(totalBytes, std::memory_order_relaxed);
                    }
                }
            }
            else if (lower.find("content-disposition:") == 0)
            {
                const std::string valueText = TrimCopy(headerLine.substr(std::string("content-disposition:").size()));
                std::string fileNameUtf8;
                if (ParseContentDispositionFileNameUtf8(valueText, fileNameUtf8))
                {
                    state->result->remoteFileNameUtf8 = fileNameUtf8;
                }
            }

            return totalSize;
        }
        catch (...)
        {
            return 0;
        }
    }

    struct GbSequentialFileWriteState
    {
        GbFileRangeChunkState* fileState = nullptr;
    };

    static size_t SequentialFileWriteCallback(char* ptr, size_t size, size_t nmemb, void* userData)
    {
        size_t totalSize = 0;
        if (!TryComputeTotalSize(size, nmemb, totalSize))
        {
            return 0;
        }
        if (userData == nullptr || ptr == nullptr || totalSize == 0)
        {
            return 0;
        }

        GbSequentialFileWriteState* state = static_cast<GbSequentialFileWriteState*>(userData);
        if (state == nullptr || state->fileState == nullptr)
        {
            return 0;
        }

        size_t newWrittenBytes = 0;
        if (!TryAddSize(state->fileState->writtenBytes, totalSize, newWrittenBytes))
        {
            return 0;
        }

        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(ptr);
        if (!WriteAllToFileRangeChunk(*state->fileState, bytes, totalSize))
        {
            return 0;
        }

        state->fileState->writtenBytes = newWrittenBytes;
        return totalSize;
    }

    static GB_NetworkDownloadedFileToPath DownloadFileToPathSingleThread(const std::string& urlUtf8, const std::string& filePathUtf8, const GB_NetworkRequestOptions& options, GbDownloadProgressPointers progress)
    {
        GB_NetworkDownloadedFileToPath result;
        result.filePathUtf8 = filePathUtf8;

        if (!EnsureCurlGlobalInit())
        {
            result.ok = false;
            result.curlErrorCode = static_cast<int>(CURLE_FAILED_INIT);
            result.errorMessageUtf8 = "curl_global_init failed";
            return result;
        }

        if (progress.enabled)
        {
            progress.totalBytesPtr->store(0, std::memory_order_relaxed);
            progress.downloadedBytesPtr->store(0, std::memory_order_relaxed);
        }

        // 目录检查与创建
        const std::string parentDirUtf8 = GetParentDirectoryUtf8(filePathUtf8);
        if (!parentDirUtf8.empty())
        {
            if (!EnsureDirectoryExistsUtf8(parentDirUtf8))
            {
                result.ok = false;
                result.curlErrorCode = static_cast<int>(CURLE_WRITE_ERROR);
                result.errorMessageUtf8 = "Failed to create parent directories";
                return result;
            }
        }

        // 覆盖/创建目标文件
        if (!CreateOrTruncateFileUtf8(filePathUtf8, false, 0))
        {
            result.ok = false;
            result.curlErrorCode = static_cast<int>(CURLE_WRITE_ERROR);
            result.errorMessageUtf8 = "Failed to create output file";
            return result;
        }

        GbFileRangeChunkState fileState;
        fileState.expectedBytes = std::numeric_limits<size_t>::max();
        fileState.writtenBytes = 0;
        fileState.progress = GbDownloadProgressPointers{};

        std::string openErr;
        if (!OpenFileRangeChunkForWriteUtf8(filePathUtf8, 0, fileState, openErr))
        {
            result.ok = false;
            result.curlErrorCode = static_cast<int>(CURLE_WRITE_ERROR);
            result.errorMessageUtf8 = openErr.empty() ? "Failed to open output file" : openErr;
            return result;
        }

        struct FileStateGuard
        {
            GbFileRangeChunkState* state = nullptr;
            ~FileStateGuard()
            {
                if (state != nullptr)
                {
                    CloseFileRangeChunkState(*state);
                }
            }
        };

        FileStateGuard fileGuard;
        fileGuard.state = &fileState;

        CURL* curlHandle = curl_easy_init();
        if (curlHandle == nullptr)
        {
            result.ok = false;
            result.errorMessageUtf8 = "curl_easy_init failed";
            result.curlErrorCode = static_cast<int>(CURLE_FAILED_INIT);
            TryDeleteFileUtf8(filePathUtf8);
            return result;
        }

        char errorBuffer[CURL_ERROR_SIZE] = { 0 };
        struct curl_slist* headers = nullptr;

        if (!ConfigureDownloadCurlCommon(curlHandle, urlUtf8, options, progress, errorBuffer, sizeof(errorBuffer), &headers))
        {
            curl_easy_cleanup(curlHandle);
            result.ok = false;
            result.errorMessageUtf8 = "Failed to configure curl options";
            TryDeleteFileUtf8(filePathUtf8);
            return result;
        }

        GbDownloadToFileHeaderState headerState;
        headerState.result = &result;
        headerState.progress = progress;
        headerState.includeResponseHeaders = options.includeResponseHeaders;
        headerState.filePathUtf8 = filePathUtf8;
        headerState.fileState = &fileState;

        GbSequentialFileWriteState writeState;
        writeState.fileState = &fileState;

        curl_easy_setopt(curlHandle, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curlHandle, CURLOPT_NOBODY, 0L);

        curl_easy_setopt(curlHandle, CURLOPT_HEADERFUNCTION, DownloadToFileHeaderCallback);
        curl_easy_setopt(curlHandle, CURLOPT_HEADERDATA, &headerState);

        curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, SequentialFileWriteCallback);
        curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &writeState);

        const CURLcode res = curl_easy_perform(curlHandle);

        long httpCode = 0;
        curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &httpCode);
        result.httpStatusCode = httpCode;

        char* effectiveUrl = nullptr;
        curl_easy_getinfo(curlHandle, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
        if (effectiveUrl != nullptr)
        {
            result.effectiveUrlUtf8 = effectiveUrl;
        }

        char* contentType = nullptr;
        curl_easy_getinfo(curlHandle, CURLINFO_CONTENT_TYPE, &contentType);
        if (contentType != nullptr && result.contentTypeUtf8.empty())
        {
            result.contentTypeUtf8 = contentType;
        }

#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x073700)
        if (!result.totalSizeKnown)
        {
            curl_off_t contentLength = -1;
            const CURLcode lenRes = curl_easy_getinfo(curlHandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &contentLength);
            if (lenRes == CURLE_OK && contentLength > 0)
            {
                size_t totalBytes = 0;
                if (TryCastUnsignedLongLongToSize(static_cast<unsigned long long>(contentLength), totalBytes))
                {
                    result.totalSizeKnown = true;
                    result.totalBytes = totalBytes;
                    if (progress.enabled)
                    {
                        progress.totalBytesPtr->store(totalBytes, std::memory_order_relaxed);
                    }
                }
            }
        }
#else
        if (!result.totalSizeKnown)
        {
            double contentLength = -1.0;
            const CURLcode lenRes = curl_easy_getinfo(curlHandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &contentLength);
            if (lenRes == CURLE_OK && contentLength > 0)
            {
                const unsigned long long contentLengthUll = static_cast<unsigned long long>(contentLength + 0.5);
                size_t totalBytes = 0;
                if (TryCastUnsignedLongLongToSize(contentLengthUll, totalBytes))
                {
                    result.totalSizeKnown = true;
                    result.totalBytes = totalBytes;
                    if (progress.enabled)
                    {
                        progress.totalBytesPtr->store(totalBytes, std::memory_order_relaxed);
                    }
                }
            }
        }
#endif

        if (headers != nullptr)
        {
            curl_slist_free_all(headers);
        }
        curl_easy_cleanup(curlHandle);

        result.curlErrorCode = static_cast<int>(res);
        if (res != CURLE_OK)
        {
            result.ok = false;
            result.errorMessageUtf8 = (errorBuffer[0] != '\0') ? errorBuffer : curl_easy_strerror(res);
            TryDeleteFileUtf8(filePathUtf8);
            return result;
        }

        if (result.remoteFileNameUtf8.empty())
        {
            const std::string baseUrl = !result.effectiveUrlUtf8.empty() ? result.effectiveUrlUtf8 : urlUtf8;
            result.remoteFileNameUtf8 = ExtractFileNameFromUrlUtf8(baseUrl);
        }

        if (httpCode >= 200 && httpCode < 300)
        {
            result.ok = true;
        }
        else
        {
            result.ok = false;
            result.errorMessageUtf8 = "HTTP status " + std::to_string(static_cast<long long>(httpCode));
            TryDeleteFileUtf8(filePathUtf8);
        }

        return result;
    }

    static GbParallelPrepareResult PrepareParallelDownloadToFilePlan(const std::string& urlUtf8, const GbProbeInfo& probe, const std::string& filePathUtf8, GbDownloadProgressPointers progress, GB_NetworkDownloadedFileToPath& outResult, std::string& outFinalUrl, std::vector<GbChunkRange>& outChunkRanges, std::vector<GbFileRangeChunkState>& outChunkStates)
    {
        outFinalUrl = !probe.effectiveUrlUtf8.empty() ? probe.effectiveUrlUtf8 : urlUtf8;

        if (!probe.ok || !probe.totalSizeKnown || !probe.acceptRangesBytes)
        {
            return GbParallelPrepareResult::FallbackToSingleThread;
        }

        const size_t totalBytes = probe.totalBytes;
        if (totalBytes == 0)
        {
            return GbParallelPrepareResult::FallbackToSingleThread;
        }

        outResult.filePathUtf8 = filePathUtf8;
        outResult.effectiveUrlUtf8 = outFinalUrl;
        outResult.contentTypeUtf8 = probe.contentTypeUtf8;
        outResult.remoteFileNameUtf8 = probe.fileNameUtf8;
        outResult.totalSizeKnown = true;
        outResult.totalBytes = totalBytes;

        if (progress.enabled)
        {
            progress.totalBytesPtr->store(totalBytes, std::memory_order_relaxed);
            progress.downloadedBytesPtr->store(0, std::memory_order_relaxed);
        }

        const std::string parentDirUtf8 = GetParentDirectoryUtf8(filePathUtf8);
        if (!parentDirUtf8.empty())
        {
            if (!EnsureDirectoryExistsUtf8(parentDirUtf8))
            {
                outResult.ok = false;
                outResult.curlErrorCode = static_cast<int>(CURLE_WRITE_ERROR);
                outResult.errorMessageUtf8 = "Failed to create parent directories";
                return GbParallelPrepareResult::FatalError;
            }
        }

        if (!CreateOrTruncateFileUtf8(filePathUtf8, true, totalBytes))
        {
            outResult.ok = false;
            outResult.curlErrorCode = static_cast<int>(CURLE_WRITE_ERROR);
            outResult.errorMessageUtf8 = "Failed to create output file";
            return GbParallelPrepareResult::FatalError;
        }

        const size_t partCount = ComputeParallelPartCount(totalBytes);
        if (!BuildChunkRanges(totalBytes, partCount, outChunkRanges))
        {
            return GbParallelPrepareResult::FallbackToSingleThread;
        }

        outChunkStates.clear();
        outChunkStates.resize(outChunkRanges.size());
        for (size_t i = 0; i < outChunkRanges.size(); i++)
        {
            const GbChunkRange& r = outChunkRanges[i];
            const size_t expectedBytes = r.end - r.begin + 1;
            outChunkStates[i].expectedBytes = expectedBytes;
            outChunkStates[i].writtenBytes = 0;
            outChunkStates[i].progress = progress;
        }

        return GbParallelPrepareResult::Ok;
    }

    static void MarkParallelDownloadToFileSuccess(const std::string& finalUrl, GB_NetworkDownloadedFileToPath& result)
    {
        result.ok = true;
        result.httpStatusCode = 200;
        result.curlErrorCode = static_cast<int>(CURLE_OK);
        result.effectiveUrlUtf8 = finalUrl;

        if (result.remoteFileNameUtf8.empty())
        {
            result.remoteFileNameUtf8 = ExtractFileNameFromUrlUtf8(finalUrl);
        }
    }

    struct GbMultiCurlFileChunkContext
    {
        CURL* easyHandle = nullptr;
        struct curl_slist* headers = nullptr;
        GbFileRangeChunkState* chunkState = nullptr;

        std::string rangeText;
        char errorBuffer[CURL_ERROR_SIZE] = { 0 };

        CURLcode curlCode = CURLE_OK;
        long httpStatusCode = 0;
        bool completed = false;
        std::string errorMessageUtf8;
    };

    static void CleanupMultiCurlFileContexts(CURLM* multiHandle, std::vector<GbMultiCurlFileChunkContext>& contexts)
    {
        for (size_t i = 0; i < contexts.size(); i++)
        {
            GbMultiCurlFileChunkContext& ctx = contexts[i];

            if (multiHandle != nullptr && ctx.easyHandle != nullptr)
            {
                // 按 libcurl 文档要求：先从 multi 中移除，再 cleanup easy
                (void)curl_multi_remove_handle(multiHandle, ctx.easyHandle);
            }

            if (ctx.easyHandle != nullptr)
            {
                curl_easy_cleanup(ctx.easyHandle);
                ctx.easyHandle = nullptr;
            }
            if (ctx.headers != nullptr)
            {
                curl_slist_free_all(ctx.headers);
                ctx.headers = nullptr;
            }
            if (ctx.chunkState != nullptr)
            {
                CloseFileRangeChunkState(*ctx.chunkState);
            }
        }
    }


    static GB_NetworkDownloadedFileToPath DownloadFileToPathMultiCurl(const std::string& urlUtf8, const std::string& filePathUtf8, const GB_NetworkRequestOptions& options, const GbProbeInfo& probe, GbDownloadProgressPointers progress)
    {
        if (!EnsureCurlGlobalInit())
        {
            GB_NetworkDownloadedFileToPath result;
            result.ok = false;
            result.curlErrorCode = static_cast<int>(CURLE_FAILED_INIT);
            result.errorMessageUtf8 = "curl_global_init failed";
            result.filePathUtf8 = filePathUtf8;
            return result;
        }

        GB_NetworkDownloadedFileToPath result;
        std::string finalUrl;
        std::vector<GbChunkRange> chunkRanges;
        std::vector<GbFileRangeChunkState> chunkStates;

        const GbParallelPrepareResult prepareRes = PrepareParallelDownloadToFilePlan(urlUtf8, probe, filePathUtf8, progress, result, finalUrl, chunkRanges, chunkStates);
        if (prepareRes == GbParallelPrepareResult::FatalError)
        {
            TryDeleteFileUtf8(filePathUtf8);
            return result;
        }
        if (prepareRes != GbParallelPrepareResult::Ok)
        {
            return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
        }

        CURLM* multiHandle = curl_multi_init();
        if (multiHandle == nullptr)
        {
            ResetDownloadProgress(progress);
            return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
        }

        struct MultiGuard
        {
            CURLM* handle = nullptr;
            ~MultiGuard()
            {
                if (handle != nullptr)
                {
                    curl_multi_cleanup(handle);
                }
            }
        };

        MultiGuard multiGuard;
        multiGuard.handle = multiHandle;

        const size_t numTransfers = chunkRanges.size();
        std::vector<GbMultiCurlFileChunkContext> contexts;
        contexts.resize(numTransfers);

        bool setupOk = true;
        for (size_t i = 0; i < numTransfers; i++)
        {
            const GbChunkRange& range = chunkRanges[i];
            GbFileRangeChunkState& state = chunkStates[i];

            GbMultiCurlFileChunkContext& ctx = contexts[i];
            std::memset(ctx.errorBuffer, 0, sizeof(ctx.errorBuffer));
            ctx.chunkState = &state;

            std::string openErr;
            if (!OpenFileRangeChunkForWriteUtf8(filePathUtf8, range.begin, state, openErr))
            {
                setupOk = false;
                break;
            }

            CURL* easyHandle = curl_easy_init();
            if (easyHandle == nullptr)
            {
                setupOk = false;
                break;
            }

            ctx.easyHandle = easyHandle;

            GbDownloadProgressPointers disabledProgress{};
            if (!ConfigureDownloadCurlCommon(easyHandle, finalUrl, options, disabledProgress, ctx.errorBuffer, sizeof(ctx.errorBuffer), &ctx.headers))
            {
                setupOk = false;
                break;
            }

            ctx.rangeText = std::to_string(static_cast<unsigned long long>(range.begin)) + "-" + std::to_string(static_cast<unsigned long long>(range.end));
            curl_easy_setopt(easyHandle, CURLOPT_RANGE, ctx.rangeText.c_str());
            curl_easy_setopt(easyHandle, CURLOPT_HTTPGET, 1L);
            curl_easy_setopt(easyHandle, CURLOPT_NOBODY, 0L);
            curl_easy_setopt(easyHandle, CURLOPT_HEADERFUNCTION, nullptr);
            curl_easy_setopt(easyHandle, CURLOPT_WRITEFUNCTION, RangeFileWriteCallback);
            curl_easy_setopt(easyHandle, CURLOPT_WRITEDATA, &state);
            curl_easy_setopt(easyHandle, CURLOPT_PRIVATE, &ctx);

            const CURLMcode addRes = curl_multi_add_handle(multiHandle, easyHandle);
            if (addRes != CURLM_OK)
            {
                setupOk = false;
                break;
            }
        }

        auto DrainDoneMessages = [&](bool& hasError, std::string& firstError) {
            int msgsInQueue = 0;
            while (true)
            {
                CURLMsg* msg = curl_multi_info_read(multiHandle, &msgsInQueue);
                if (msg == nullptr)
                {
                    break;
                }
                if (msg->msg != CURLMSG_DONE)
                {
                    continue;
                }

                CURL* easyHandle = msg->easy_handle;
                GbMultiCurlFileChunkContext* ctxPtr = nullptr;
                curl_easy_getinfo(easyHandle, CURLINFO_PRIVATE, &ctxPtr);

                if (ctxPtr != nullptr)
                {
                    ctxPtr->curlCode = msg->data.result;
                    ctxPtr->completed = true;

                    long httpCode = 0;
                    curl_easy_getinfo(easyHandle, CURLINFO_RESPONSE_CODE, &httpCode);
                    ctxPtr->httpStatusCode = httpCode;

                    if (ctxPtr->curlCode != CURLE_OK)
                    {
                        ctxPtr->errorMessageUtf8 = (ctxPtr->errorBuffer[0] != '\0') ? ctxPtr->errorBuffer : curl_easy_strerror(ctxPtr->curlCode);
                        if (!hasError)
                        {
                            hasError = true;
                            firstError = ctxPtr->errorMessageUtf8;
                        }
                    }
                    else if (ctxPtr->httpStatusCode != 0 && ctxPtr->httpStatusCode != 206)
                    {
                        ctxPtr->errorMessageUtf8 = "HTTP status " + std::to_string(static_cast<long long>(ctxPtr->httpStatusCode));
                        if (!hasError)
                        {
                            hasError = true;
                            firstError = ctxPtr->errorMessageUtf8;
                        }
                    }
                }
                else
                {
                    if (!hasError)
                    {
                        hasError = true;
                        firstError = "curl_easy_getinfo(CURLINFO_PRIVATE) failed";
                    }
                }

                curl_multi_remove_handle(multiHandle, easyHandle);
                curl_easy_cleanup(easyHandle);

                if (ctxPtr != nullptr)
                {
                    ctxPtr->easyHandle = nullptr;
                    if (ctxPtr->headers != nullptr)
                    {
                        curl_slist_free_all(ctxPtr->headers);
                        ctxPtr->headers = nullptr;
                    }
                }
            }
            };

        if (!setupOk)
        {
            CleanupMultiCurlFileContexts(multiHandle, contexts);
            ResetDownloadProgress(progress);
            return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
        }

        int stillRunning = 0;
        CURLMcode mc = CURLM_OK;
        do
        {
            mc = curl_multi_perform(multiHandle, &stillRunning);
        } while (mc == CURLM_CALL_MULTI_PERFORM);

        if (mc != CURLM_OK)
        {
            CleanupMultiCurlFileContexts(multiHandle, contexts);
            ResetDownloadProgress(progress);
            return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
        }

        bool hasError = false;
        std::string firstError;

        while (stillRunning && !hasError)
        {
            int numFds = 0;
            mc = curl_multi_wait(multiHandle, nullptr, 0, 1000, &numFds);
            if (mc != CURLM_OK)
            {
                hasError = true;
                firstError = "curl_multi_wait failed";
                break;
            }

            do
            {
                mc = curl_multi_perform(multiHandle, &stillRunning);
            } while (mc == CURLM_CALL_MULTI_PERFORM);

            if (mc != CURLM_OK)
            {
                hasError = true;
                firstError = "curl_multi_perform failed";
                break;
            }

            DrainDoneMessages(hasError, firstError);
        }

        if (!hasError)
        {
            DrainDoneMessages(hasError, firstError);
        }

        CleanupMultiCurlFileContexts(multiHandle, contexts);

        if (hasError)
        {
            ResetDownloadProgress(progress);
            return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
        }

        for (size_t i = 0; i < chunkStates.size(); i++)
        {
            const GbFileRangeChunkState& state = chunkStates[i];
            if (state.writtenBytes != state.expectedBytes)
            {
                ResetDownloadProgress(progress);
                return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
            }
        }

        MarkParallelDownloadToFileSuccess(finalUrl, result);
        return result;
    }

    static GbRangeThreadResult DownloadRangeChunkToFile(const std::string& urlUtf8, const GB_NetworkRequestOptions& options, const std::string& filePathUtf8, size_t rangeBegin, size_t rangeEnd, GbFileRangeChunkState& state)
    {
        GbRangeThreadResult result;

        std::string openErr;
        if (!OpenFileRangeChunkForWriteUtf8(filePathUtf8, rangeBegin, state, openErr))
        {
            result.curlCode = CURLE_WRITE_ERROR;
            result.errorMessageUtf8 = openErr.empty() ? "Failed to open output file" : openErr;
            return result;
        }

        struct FileGuard
        {
            GbFileRangeChunkState* state = nullptr;
            ~FileGuard()
            {
                if (state != nullptr)
                {
                    CloseFileRangeChunkState(*state);
                }
            }
        };

        FileGuard fg;
        fg.state = &state;

        CURL* curlHandle = curl_easy_init();
        if (curlHandle == nullptr)
        {
            result.curlCode = CURLE_FAILED_INIT;
            result.errorMessageUtf8 = "curl_easy_init failed";
            return result;
        }

        char errorBuffer[CURL_ERROR_SIZE] = { 0 };
        struct curl_slist* headers = nullptr;

        GbDownloadProgressPointers disabledProgress{};
        if (!ConfigureDownloadCurlCommon(curlHandle, urlUtf8, options, disabledProgress, errorBuffer, sizeof(errorBuffer), &headers))
        {
            if (headers != nullptr)
            {
                curl_slist_free_all(headers);
            }
            curl_easy_cleanup(curlHandle);
            result.curlCode = CURLE_FAILED_INIT;
            result.errorMessageUtf8 = "Failed to configure curl options";
            return result;
        }

        const std::string rangeText = std::to_string(static_cast<unsigned long long>(rangeBegin)) + "-" + std::to_string(static_cast<unsigned long long>(rangeEnd));
        curl_easy_setopt(curlHandle, CURLOPT_RANGE, rangeText.c_str());
        curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, RangeFileWriteCallback);
        curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(curlHandle, CURLOPT_HEADERFUNCTION, nullptr);

        const CURLcode res = curl_easy_perform(curlHandle);

        long httpCode = 0;
        curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &httpCode);
        result.httpStatusCode = httpCode;

        if (headers != nullptr)
        {
            curl_slist_free_all(headers);
        }

        curl_easy_cleanup(curlHandle);

        result.curlCode = res;
        if (res != CURLE_OK)
        {
            result.errorMessageUtf8 = (errorBuffer[0] != '\0') ? errorBuffer : curl_easy_strerror(res);
        }

        return result;
    }

    static GB_NetworkDownloadedFileToPath DownloadFileToPathMultiThread(const std::string& urlUtf8, const std::string& filePathUtf8, const GB_NetworkRequestOptions& options, const GbProbeInfo& probe, GbDownloadProgressPointers progress)
    {
        if (!EnsureCurlGlobalInit())
        {
            GB_NetworkDownloadedFileToPath result;
            result.ok = false;
            result.curlErrorCode = static_cast<int>(CURLE_FAILED_INIT);
            result.errorMessageUtf8 = "curl_global_init failed";
            result.filePathUtf8 = filePathUtf8;
            return result;
        }

        GB_NetworkDownloadedFileToPath result;
        std::string finalUrl;
        std::vector<GbChunkRange> chunkRanges;
        std::vector<GbFileRangeChunkState> chunkStates;

        const GbParallelPrepareResult prepareRes = PrepareParallelDownloadToFilePlan(urlUtf8, probe, filePathUtf8, progress, result, finalUrl, chunkRanges, chunkStates);
        if (prepareRes == GbParallelPrepareResult::FatalError)
        {
            TryDeleteFileUtf8(filePathUtf8);
            return result;
        }
        if (prepareRes != GbParallelPrepareResult::Ok)
        {
            return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
        }

        const size_t numThreads = chunkRanges.size();

        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        std::vector<GbRangeThreadResult> threadResults;
        threadResults.resize(numThreads);

        for (size_t i = 0; i < numThreads; i++)
        {
            const size_t threadIndex = i;
            const size_t rangeBegin = chunkRanges[i].begin;
            const size_t rangeEnd = chunkRanges[i].end;

            try
            {
                threads.emplace_back([&, threadIndex, rangeBegin, rangeEnd]() {
                    threadResults[threadIndex] = DownloadRangeChunkToFile(finalUrl, options, filePathUtf8, rangeBegin, rangeEnd, chunkStates[threadIndex]);
                    });
            }
            catch (...)
            {
                for (size_t t = 0; t < threads.size(); t++)
                {
                    threads[t].join();
                }

                ResetDownloadProgress(progress);
                return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
            }
        }

        for (size_t i = 0; i < threads.size(); i++)
        {
            threads[i].join();
        }

        for (size_t i = 0; i < threadResults.size(); i++)
        {
            const GbRangeThreadResult& tr = threadResults[i];
            if (tr.curlCode != CURLE_OK || (tr.httpStatusCode != 0 && tr.httpStatusCode != 206))
            {
                ResetDownloadProgress(progress);
                return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
            }
        }

        for (size_t i = 0; i < chunkStates.size(); i++)
        {
            const GbFileRangeChunkState& state = chunkStates[i];
            if (state.writtenBytes != state.expectedBytes)
            {
                ResetDownloadProgress(progress);
                return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
            }
        }

        MarkParallelDownloadToFileSuccess(finalUrl, result);
        return result;
    }
}


bool GB_CanConnectToInternet(unsigned int timeoutMs)
{
    if (timeoutMs == 0)
    {
        return false;
    }

#ifdef _WIN32
    WsaStartupGuard wsaGuard;
    if (!wsaGuard.IsOk())
    {
        return false;
    }
#endif

    struct ProbeEndpoint
    {
        const char* host;
        unsigned short port;
    };

    constexpr static ProbeEndpoint endpoints[] =
    {
        { "www.baidu.com", 443 },
        { "www.qq.com", 443 },
        { "1.1.1.1", 443 }
    };

    const auto startTime = std::chrono::steady_clock::now();
    const size_t numEndpoints = sizeof(endpoints) / sizeof(endpoints[0]);

    for (size_t i = 0; i < numEndpoints; i++)
    {
        unsigned int remainingMs = timeoutMs;

        const auto nowTime = std::chrono::steady_clock::now();
        const auto elapsedMs = static_cast<unsigned int>(std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - startTime).count());

        if (elapsedMs >= timeoutMs)
        {
            break;
        }

        remainingMs = timeoutMs - elapsedMs;

        if (ConnectTcpWithTimeout(endpoints[i].host, endpoints[i].port, remainingMs))
        {
            return true;
        }
    }

    return false;
}

GB_NetworkRequestOptions::GB_NetworkRequestOptions()
{
    const static std::string exeDirUtf8 = GB_GetExeDirectory();
    const static std::string testCertPath = exeDirUtf8 + GB_STR("cacert.pem");
    if (GB_IsFileExists(testCertPath))
    {
        caBundlePathUtf8 = testCertPath;
    }
}

void GB_ClearNetworkSystemProxyCache()
{
#ifdef _WIN32
    ClearWindowsSystemProxyCache();
#endif
}

GB_NetworkResponse GB_RequestUrlData(const std::string& urlUtf8, const GB_NetworkRequestOptions& options)
{
    GB_NetworkResponse response;

    if (urlUtf8.empty())
    {
        response.ok = false;
        response.errorMessageUtf8 = "URL is empty";
        return response;
    }

    const std::string schemeLower = GetUrlSchemeLower(urlUtf8);
    if (schemeLower != "http" && schemeLower != "https")
    {
        response.ok = false;
        response.errorMessageUtf8 = "Unsupported URL scheme (only http/https)";
        return response;
    }

    if (!EnsureCurlGlobalInit())
    {
        response.ok = false;
        response.errorMessageUtf8 = "curl_global_init failed";
        return response;
    }

    CURL* curlHandle = ::curl_easy_init();
    if (curlHandle == nullptr)
    {
        response.ok = false;
        response.errorMessageUtf8 = "curl_easy_init failed";
        return response;
    }

    struct CurlEasyHandleGuard
    {
        CURL* handle = nullptr;
        ~CurlEasyHandleGuard()
        {
            if (handle != nullptr)
            {
                ::curl_easy_cleanup(handle);
                handle = nullptr;
            }
        }
    };

    CurlEasyHandleGuard easyGuard;
    easyGuard.handle = curlHandle;

    char curlErrorBuffer[CURL_ERROR_SIZE];
    std::memset(curlErrorBuffer, 0, sizeof(curlErrorBuffer));
    ::curl_easy_setopt(curlHandle, CURLOPT_ERRORBUFFER, curlErrorBuffer);

    ::curl_easy_setopt(curlHandle, CURLOPT_URL, urlUtf8.c_str());
    ::curl_easy_setopt(curlHandle, CURLOPT_HTTPGET, 1L);

#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x075500)
    ::curl_easy_setopt(curlHandle, CURLOPT_PROTOCOLS_STR, "http,https");
    ::curl_easy_setopt(curlHandle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    ::curl_easy_setopt(curlHandle, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    ::curl_easy_setopt(curlHandle, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    ::curl_easy_setopt(curlHandle, CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(curlHandle, CURLOPT_FOLLOWLOCATION, options.followRedirects ? 1L : 0L);
    if (options.followRedirects)
    {
        ::curl_easy_setopt(curlHandle, CURLOPT_AUTOREFERER, 1L);
    }
    const long maxRedirects = (options.maxRedirects > 0) ? static_cast<long>(options.maxRedirects) : 0L;
    ::curl_easy_setopt(curlHandle, CURLOPT_MAXREDIRS, maxRedirects);
    ::curl_easy_setopt(curlHandle, CURLOPT_CONNECTTIMEOUT_MS, ClampUnsignedIntToCurlLong(options.connectTimeoutMs));
    ::curl_easy_setopt(curlHandle, CURLOPT_TIMEOUT_MS, ClampUnsignedIntToCurlLong(options.totalTimeoutMs));

    ::curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, &CurlWriteCallback);
    ::curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &response.body);

    if (options.includeResponseHeaders)
    {
        ::curl_easy_setopt(curlHandle, CURLOPT_HEADERFUNCTION, &CurlHeaderCallback);
        ::curl_easy_setopt(curlHandle, CURLOPT_HEADERDATA, &response.responseHeadersUtf8);
    }

    ::curl_easy_setopt(curlHandle, CURLOPT_SSL_VERIFYPEER, options.verifyTlsPeer ? 1L : 0L);
    ::curl_easy_setopt(curlHandle, CURLOPT_SSL_VERIFYHOST, options.verifyTlsHost ? 2L : 0L);
    if (!options.caBundlePathUtf8.empty())
    {
        ::curl_easy_setopt(curlHandle, CURLOPT_CAINFO, options.caBundlePathUtf8.c_str());
    }
    if (!options.caPathUtf8.empty())
    {
        ::curl_easy_setopt(curlHandle, CURLOPT_CAPATH, options.caPathUtf8.c_str());
    }

#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x072F00)
    if (options.enableHttp2)
    {
        ::curl_easy_setopt(curlHandle, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
    }
#endif

    std::string userAgentUtf8 = options.userAgentUtf8;
    if (userAgentUtf8.empty() && options.impersonateBrowser)
    {
        userAgentUtf8 = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    }
    if (!userAgentUtf8.empty())
    {
        ::curl_easy_setopt(curlHandle, CURLOPT_USERAGENT, userAgentUtf8.c_str());
    }

    if (!options.refererUtf8.empty())
    {
        ::curl_easy_setopt(curlHandle, CURLOPT_REFERER, options.refererUtf8.c_str());
    }

    struct CurlSlistGuard
    {
        curl_slist* list = nullptr;
        ~CurlSlistGuard()
        {
            if (list != nullptr)
            {
                ::curl_slist_free_all(list);
                list = nullptr;
            }
        }
    };

    CurlSlistGuard headerGuard;

    const auto tryAppendHeader = [&](const char* headerValue) -> bool {
        if (headerValue == nullptr)
        {
            return true;
        }

        curl_slist* newList = ::curl_slist_append(headerGuard.list, headerValue);
        if (newList == nullptr)
        {
            return false;
        }

        headerGuard.list = newList;
        return true;
        };

    if (options.impersonateBrowser)
    {
        if (!tryAppendHeader("Accept: */*") ||
            !tryAppendHeader("Accept-Language: zh-CN,zh;q=0.9,en;q=0.8"))
        {
            response.ok = false;
            response.curlErrorCode = static_cast<int>(CURLE_OUT_OF_MEMORY);
            response.errorMessageUtf8 = "Out of memory (curl_slist_append failed)";
            return response;
        }

        ::curl_easy_setopt(curlHandle, CURLOPT_ACCEPT_ENCODING, "");
    }

    for (size_t i = 0; i < options.headersUtf8.size(); i++)
    {
        const std::string& header = options.headersUtf8[i];
        if (!header.empty())
        {
            if (!tryAppendHeader(header.c_str()))
            {
                response.ok = false;
                response.curlErrorCode = static_cast<int>(CURLE_OUT_OF_MEMORY);
                response.errorMessageUtf8 = "Out of memory (curl_slist_append failed)";
                return response;
            }
        }
    }

    if (headerGuard.list != nullptr)
    {
        ::curl_easy_setopt(curlHandle, CURLOPT_HTTPHEADER, headerGuard.list);
    }

    ApplyProxySettings(curlHandle, urlUtf8, options.proxy);

    const CURLcode curlCode = ::curl_easy_perform(curlHandle);
    response.curlErrorCode = static_cast<int>(curlCode);
    if (curlCode != CURLE_OK)
    {
        response.ok = false;
        if (curlErrorBuffer[0] != 0)
        {
            response.errorMessageUtf8 = curlErrorBuffer;
        }
        else
        {
            response.errorMessageUtf8 = ::curl_easy_strerror(curlCode);
        }
    }

    long httpCode = 0;
    ::curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &httpCode);
    response.httpStatusCode = httpCode;

    char* effectiveUrl = nullptr;
    ::curl_easy_getinfo(curlHandle, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
    if (effectiveUrl != nullptr)
    {
        response.effectiveUrlUtf8 = effectiveUrl;
    }

    char* contentType = nullptr;
    ::curl_easy_getinfo(curlHandle, CURLINFO_CONTENT_TYPE, &contentType);
    if (contentType != nullptr)
    {
        response.contentTypeUtf8 = contentType;
    }

    if (curlCode == CURLE_OK)
    {
        if (httpCode == 0 || (httpCode >= 200 && httpCode < 400))
        {
            response.ok = true;
        }
        else
        {
            response.ok = false;
            response.errorMessageUtf8 = "HTTP status " + std::to_string(static_cast<long long>(httpCode));
        }
    }

    return response;
}

GB_NetworkDownloadedFile GB_DownloadFile(const std::string& urlUtf8, const GB_NetworkRequestOptions& options, GB_DownloadFileStrategy strategy, void* totalSizeAtomicPtr, void* downloadedSizeAtomicPtr)
{
    const GbDownloadProgressPointers progress = GetDownloadProgressPointers(totalSizeAtomicPtr, downloadedSizeAtomicPtr);

    if (progress.enabled)
    {
        progress.totalBytesPtr->store(0, std::memory_order_relaxed);
        progress.downloadedBytesPtr->store(0, std::memory_order_relaxed);
    }

    if (urlUtf8.empty())
    {
        GB_NetworkDownloadedFile result;
        result.ok = false;
        result.curlErrorCode = static_cast<int>(CURLE_URL_MALFORMAT);
        result.errorMessageUtf8 = "URL is empty";
        return result;
    }

    const std::string schemeLower = GetUrlSchemeLower(urlUtf8);
    if (schemeLower != "http" && schemeLower != "https")
    {
        GB_NetworkDownloadedFile result;
        result.ok = false;
        result.curlErrorCode = static_cast<int>(CURLE_UNSUPPORTED_PROTOCOL);
        result.errorMessageUtf8 = "Unsupported URL scheme (only http/https)";
        return result;
    }

    if (!EnsureCurlGlobalInit())
    {
        GB_NetworkDownloadedFile result;
        result.ok = false;
        result.curlErrorCode = static_cast<int>(CURLE_FAILED_INIT);
        result.errorMessageUtf8 = "curl_global_init failed";
        return result;
    }

    // responseHeadersUtf8 为单请求模型设计；并行分段下载不提供 header 聚合，显式回退到单线程。
    if (options.includeResponseHeaders)
    {
        return DownloadFileSingleThread(urlUtf8, options, progress);
    }

    // 探测是否支持分段与总大小（用于并行下载 + 文件名推断）
    const GbProbeInfo probe = ProbeUrlForRangeAndSize(urlUtf8, options);
    if (probe.ok && ShouldUseMultiThreadDownload(probe.totalBytes, probe.acceptRangesBytes))
    {
        if (strategy == GB_DownloadFileStrategy::MultiThread)
        {
            return DownloadFileMultiThread(urlUtf8, options, probe, progress);
        }
        return DownloadFileMultiCurl(urlUtf8, options, probe, progress);
    }

    return DownloadFileSingleThread(urlUtf8, options, progress);
}

bool GB_TryGetDownloadFileName(const std::string& urlUtf8, std::string& outFileNameUtf8, const GB_NetworkRequestOptions& options)
{
    outFileNameUtf8.clear();

    if (urlUtf8.empty())
    {
        return false;
    }

    const std::string schemeLower = GetUrlSchemeLower(urlUtf8);
    if (schemeLower != "http" && schemeLower != "https")
    {
        return false;
    }

    if (!EnsureCurlGlobalInit())
    {
        return false;
    }

    const GbProbeInfo probe = ProbeUrlForRangeAndSize(urlUtf8, options);

    std::string candidateFileNameUtf8;
    if (probe.ok)
    {
        candidateFileNameUtf8 = probe.fileNameUtf8;

        if (candidateFileNameUtf8.empty())
        {
            const std::string baseUrl = !probe.effectiveUrlUtf8.empty() ? probe.effectiveUrlUtf8 : urlUtf8;
            candidateFileNameUtf8 = ExtractFileNameFromUrlUtf8(baseUrl);
        }
    }
    else
    {
        candidateFileNameUtf8 = ExtractFileNameFromUrlUtf8(urlUtf8);
    }

    if (!LooksLikeFileNameWithExtensionUtf8(candidateFileNameUtf8))
    {
        return false;
    }

    outFileNameUtf8 = TrimCopy(candidateFileNameUtf8);
    return true;
}

GB_NetworkDownloadedFileToPath GB_DownloadFileToPath(const std::string& urlUtf8, const std::string& filePathUtf8, const GB_NetworkRequestOptions& options, GB_DownloadFileStrategy strategy, void* totalSizeAtomicPtr, void* downloadedSizeAtomicPtr)
{
    const GbDownloadProgressPointers progress = GetDownloadProgressPointers(totalSizeAtomicPtr, downloadedSizeAtomicPtr);

    if (progress.enabled)
    {
        progress.totalBytesPtr->store(0, std::memory_order_relaxed);
        progress.downloadedBytesPtr->store(0, std::memory_order_relaxed);
    }

    GB_NetworkDownloadedFileToPath result;
    result.filePathUtf8 = filePathUtf8;

    if (urlUtf8.empty())
    {
        result.ok = false;
        result.curlErrorCode = static_cast<int>(CURLE_URL_MALFORMAT);
        result.errorMessageUtf8 = "URL is empty";
        return result;
    }

    if (filePathUtf8.empty())
    {
        result.ok = false;
        result.curlErrorCode = static_cast<int>(CURLE_WRITE_ERROR);
        result.errorMessageUtf8 = "File path is empty";
        return result;
    }

    const std::string schemeLower = GetUrlSchemeLower(urlUtf8);
    if (schemeLower != "http" && schemeLower != "https")
    {
        result.ok = false;
        result.curlErrorCode = static_cast<int>(CURLE_UNSUPPORTED_PROTOCOL);
        result.errorMessageUtf8 = "Unsupported URL scheme (only http/https)";
        return result;
    }

    if (!EnsureCurlGlobalInit())
    {
        result.ok = false;
        result.curlErrorCode = static_cast<int>(CURLE_FAILED_INIT);
        result.errorMessageUtf8 = "curl_global_init failed";
        return result;
    }

    // includeResponseHeaders 为单请求模型设计；并行分段下载不做 header 聚合，强制回退单线程。
    if (options.includeResponseHeaders)
    {
        return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
    }

    const GbProbeInfo probe = ProbeUrlForRangeAndSize(urlUtf8, options);
    if (probe.ok && ShouldUseMultiThreadDownload(probe.totalBytes, probe.acceptRangesBytes))
    {
        if (strategy == GB_DownloadFileStrategy::MultiThread)
        {
            return DownloadFileToPathMultiThread(urlUtf8, filePathUtf8, options, probe, progress);
        }
        return DownloadFileToPathMultiCurl(urlUtf8, filePathUtf8, options, probe, progress);
    }

    return DownloadFileToPathSingleThread(urlUtf8, filePathUtf8, options, progress);
}

namespace
{
    static inline bool IsUrlAlpha(char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    static inline bool IsUrlSchemeChar(char c)
    {
        if ((c >= '0' && c <= '9') || IsUrlAlpha(c))
        {
            return true;
        }
        return (c == '+' || c == '-' || c == '.');
    }

    static bool IsValidUrlScheme(const std::string& scheme)
    {
        if (scheme.empty())
        {
            return false;
        }
        if (!IsUrlAlpha(scheme.front()))
        {
            return false;
        }
        for (size_t i = 1; i < scheme.size(); i++)
        {
            if (!IsUrlSchemeChar(scheme[i]))
            {
                return false;
            }
        }
        return true;
    }
    static inline bool IsUrlUnreservedByte(unsigned char c, GB_UrlOperator::UrlEncodingMode mode)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        {
            return true;
        }

        if (mode == GB_UrlOperator::UrlEncodingMode::FormUrlEncoded)
        {
            // WHATWG URL: application/x-www-form-urlencoded percent-encode set
            // 保留不编码：ALPHA / DIGIT / "*" / "-" / "." / "_"
            // 注意：在该模式下 "~" 会被编码为 %7E。
            return (c == '*' || c == '-' || c == '.' || c == '_');
        }

        // RFC 3986 unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~"
        return (c == '-' || c == '.' || c == '_' || c == '~');
    }

    static inline int HexToNibbleUrl(char c)
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F')
        {
            return 10 + (c - 'A');
        }
        return -1;
    }
    static std::string DecodePercentAndOptionalPlus(const char* data, size_t length, bool plusToSpace)
    {
        if (data == nullptr || length == 0)
        {
            return std::string();
        }

        bool hasPercent = false;
        bool hasPlus = false;

        for (size_t i = 0; i < length; i++)
        {
            const char c = data[i];
            if (c == '%')
            {
                hasPercent = true;
            }
            else if (plusToSpace && c == '+')
            {
                hasPlus = true;
            }

            if (hasPercent && (!plusToSpace || hasPlus))
            {
                break;
            }
        }

        if (!hasPercent && !hasPlus)
        {
            return std::string(data, length);
        }

        std::string result;
        result.reserve(length);

        for (size_t i = 0; i < length; i++)
        {
            const char c = data[i];

            if (plusToSpace && c == '+')
            {
                result.push_back(' ');
                continue;
            }

            if (c == '%' && i + 2 < length)
            {
                const int hi = HexToNibbleUrl(data[i + 1]);
                const int lo = HexToNibbleUrl(data[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    const char decoded = static_cast<char>((hi << 4) | lo);
                    result.push_back(decoded);
                    i += 2;
                    continue;
                }
            }

            result.push_back(c);
        }

        return result;
    }

    static std::string DecodePercentAndOptionalPlus(const std::string& text, bool plusToSpace)
    {
        return DecodePercentAndOptionalPlus(text.data(), text.size(), plusToSpace);
    }

    static std::string UrlEncodeInternal(const std::string& textUtf8, GB_UrlOperator::UrlEncodingMode mode)
    {
        std::string result;
        if (textUtf8.size() < (std::numeric_limits<size_t>::max() / 3))
        {
            result.reserve(textUtf8.size() * 3);
        }
        else
        {
            result.reserve(textUtf8.size());
        }

        auto AppendPercent = [&](unsigned char byteValue)
            {
                static const char kHex[] = "0123456789ABCDEF";
                result.push_back('%');
                result.push_back(kHex[(byteValue >> 4) & 0x0F]);
                result.push_back(kHex[(byteValue) & 0x0F]);
            };

        for (size_t i = 0; i < textUtf8.size(); i++)
        {
            const unsigned char c = static_cast<unsigned char>(textUtf8[i]);

            if (mode == GB_UrlOperator::UrlEncodingMode::FormUrlEncoded && c == ' ')
            {
                result.push_back('+');
                continue;
            }

            if (IsUrlUnreservedByte(c, mode))
            {
                result.push_back(static_cast<char>(c));
                continue;
            }

            AppendPercent(c);
        }

        return result;
    }
    static std::string UrlDecodeInternal(const char* data, size_t length, GB_UrlOperator::UrlEncodingMode mode)
    {
        const bool plusToSpace = (mode == GB_UrlOperator::UrlEncodingMode::FormUrlEncoded);
        return DecodePercentAndOptionalPlus(data, length, plusToSpace);
    }

    static std::string UrlDecodeInternal(const std::string& text, GB_UrlOperator::UrlEncodingMode mode)
    {
        return UrlDecodeInternal(text.data(), text.size(), mode);
    }

    struct GbQueryItemInternal
    {
        std::string keyUtf8;
        std::string valueUtf8;
        bool hasEquals = false;
    };

    struct GbQueryItemRaw
    {
        // 原始（未解码）的 key/value 子串（不含 '=' 与 '&'）
        std::string rawKey;
        std::string rawValue;
        bool hasEquals = false;

        // 解码后的 key/value（用于匹配/读取）
        std::string decodedKey;
        std::string decodedValue;

        // 本项前的分隔符（'&' 或 ';'）。首项忽略该字段。
        char separator = '&';
    };


    static inline char ToLowerAsciiChar(char c)
    {
        if (c >= 'A' && c <= 'Z')
        {
            return static_cast<char>(c - 'A' + 'a');
        }
        return c;
    }

    // URL key 的大小写不敏感比较：仅对 ASCII 字母做折叠（A-Z -> a-z）。
    // 说明：
    // - 这是“字节级”的比较，不做 Unicode 大小写折叠。
    // - 对于 raw key，可同时兼容 %2F 与 %2f 这类十六进制字符大小写差异。
    static bool AreUrlKeysEqual(const std::string& a, const std::string& b, bool keyCaseSensitive)
    {
        if (keyCaseSensitive)
        {
            return a == b;
        }

        if (a.size() != b.size())
        {
            return false;
        }

        for (size_t i = 0; i < a.size(); i++)
        {
            if (ToLowerAsciiChar(a[i]) != ToLowerAsciiChar(b[i]))
            {
                return false;
            }
        }

        return true;
    }

    static void SplitUrlQueryAndFragment(const std::string& urlUtf8,
        size_t& outPrefixEnd,
        bool& outHasQuery,
        size_t& outQueryBegin,
        size_t& outQueryEnd,
        bool& outHasFragment,
        size_t& outFragmentBegin)
    {
        outPrefixEnd = urlUtf8.size();
        outHasQuery = false;
        outQueryBegin = std::string::npos;
        outQueryEnd = std::string::npos;
        outHasFragment = false;
        outFragmentBegin = std::string::npos;

        const size_t fragmentPos = urlUtf8.find('#');
        if (fragmentPos != std::string::npos)
        {
            outHasFragment = true;
            outFragmentBegin = fragmentPos + 1;
            outPrefixEnd = fragmentPos;
        }

        const size_t queryPos = urlUtf8.find('?');
        if (queryPos != std::string::npos && queryPos < outPrefixEnd)
        {
            outHasQuery = true;
            outQueryBegin = queryPos + 1;
            outQueryEnd = outPrefixEnd;
            outPrefixEnd = queryPos;
        }
    }
    static std::vector<GbQueryItemInternal> ParseQueryStringInternal(const std::string& queryString, bool decode, GB_UrlOperator::UrlEncodingMode decodeMode)
    {
        std::vector<GbQueryItemInternal> items;

        const char* const data = queryString.data();
        const size_t totalLength = queryString.size();

        size_t begin = 0;
        while (begin <= totalLength)
        {
            size_t endPos = queryString.find_first_of("&;", begin);
            if (endPos == std::string::npos)
            {
                endPos = totalLength;
            }

            if (endPos > begin)
            {
                GbQueryItemInternal item;

                size_t eqPos = std::string::npos;
                for (size_t i = begin; i < endPos; i++)
                {
                    if (data[i] == '=')
                    {
                        eqPos = i;
                        break;
                    }
                }

                if (eqPos == std::string::npos)
                {
                    item.hasEquals = false;

                    if (decode)
                    {
                        item.keyUtf8 = UrlDecodeInternal(data + begin, endPos - begin, decodeMode);
                    }
                    else
                    {
                        item.keyUtf8.assign(data + begin, endPos - begin);
                    }

                    item.valueUtf8.clear();
                }
                else
                {
                    item.hasEquals = true;

                    const size_t keyBegin = begin;
                    const size_t keyLen = eqPos - begin;

                    const size_t valueBegin = eqPos + 1;
                    const size_t valueLen = endPos - valueBegin;

                    if (decode)
                    {
                        item.keyUtf8 = UrlDecodeInternal(data + keyBegin, keyLen, decodeMode);
                        item.valueUtf8 = UrlDecodeInternal(data + valueBegin, valueLen, decodeMode);
                    }
                    else
                    {
                        item.keyUtf8.assign(data + keyBegin, keyLen);
                        item.valueUtf8.assign(data + valueBegin, valueLen);
                    }
                }

                items.push_back(item);
            }

            if (endPos == totalLength)
            {
                break;
            }
            begin = endPos + 1;
        }

        return items;
    }
    static std::vector<GbQueryItemRaw> ParseQueryStringRaw(const std::string& queryString, GB_UrlOperator::UrlEncodingMode decodeMode)
    {
        std::vector<GbQueryItemRaw> items;

        const char* const data = queryString.data();
        const size_t totalLength = queryString.size();

        size_t begin = 0;
        char nextSeparator = '&';

        while (begin <= totalLength)
        {
            size_t endPos = queryString.find_first_of("&;", begin);
            char delimiter = '\0';
            if (endPos == std::string::npos)
            {
                endPos = totalLength;
            }
            else
            {
                delimiter = data[endPos];
            }

            if (endPos > begin)
            {
                GbQueryItemRaw item;
                item.separator = nextSeparator;

                size_t eqPos = std::string::npos;
                for (size_t i = begin; i < endPos; i++)
                {
                    if (data[i] == '=')
                    {
                        eqPos = i;
                        break;
                    }
                }

                if (eqPos == std::string::npos)
                {
                    item.hasEquals = false;
                    item.rawKey.assign(data + begin, endPos - begin);
                    item.rawValue.clear();

                    item.decodedKey = UrlDecodeInternal(data + begin, endPos - begin, decodeMode);
                    item.decodedValue.clear();
                }
                else
                {
                    item.hasEquals = true;

                    const size_t keyBegin = begin;
                    const size_t keyLen = eqPos - begin;

                    const size_t valueBegin = eqPos + 1;
                    const size_t valueLen = endPos - valueBegin;

                    item.rawKey.assign(data + keyBegin, keyLen);
                    item.rawValue.assign(data + valueBegin, valueLen);

                    item.decodedKey = UrlDecodeInternal(data + keyBegin, keyLen, decodeMode);
                    item.decodedValue = UrlDecodeInternal(data + valueBegin, valueLen, decodeMode);
                }

                items.push_back(item);
            }

            if (endPos == totalLength)
            {
                break;
            }

            nextSeparator = (delimiter == ';') ? ';' : '&';
            begin = endPos + 1;
        }

        return items;
    }

    static std::string BuildQueryStringRaw(const std::vector<GbQueryItemRaw>& items)
    {
        std::string result;

        size_t totalReserve = 0;
        if (!items.empty())
        {
            totalReserve += (items.size() - 1);
        }

        for (size_t i = 0; i < items.size(); i++)
        {
            const GbQueryItemRaw& item = items[i];
            totalReserve += item.rawKey.size();
            if (!item.hasEquals && item.rawValue.empty())
            {
                continue;
            }
            totalReserve += 1;
            totalReserve += item.rawValue.size();
        }
        result.reserve(totalReserve);

        for (size_t i = 0; i < items.size(); i++)
        {
            const GbQueryItemRaw& item = items[i];

            if (i > 0)
            {
                result.push_back((item.separator == ';') ? ';' : '&');
            }

            result += item.rawKey;

            if (!item.hasEquals && item.rawValue.empty())
            {
                continue;
            }

            result.push_back('=');
            result += item.rawValue;
        }

        return result;
    }

    static bool TryGetUrlPathRange(const std::string& urlUtf8, size_t& outPathBegin, size_t& outPathEndBeforeQueryAndFragment)
    {
        outPathBegin = 0;
        outPathEndBeforeQueryAndFragment = urlUtf8.size();

        size_t prefixEnd = 0;
        bool hasQuery = false;
        size_t queryBegin = 0;
        size_t queryEnd = 0;
        bool hasFragment = false;
        size_t fragmentBegin = 0;
        SplitUrlQueryAndFragment(urlUtf8, prefixEnd, hasQuery, queryBegin, queryEnd, hasFragment, fragmentBegin);

        outPathEndBeforeQueryAndFragment = prefixEnd;

        size_t posAfterScheme = 0;
        bool hasScheme = false;

        const size_t firstDelim = urlUtf8.find_first_of("/?#");
        const size_t schemeLimit = (firstDelim == std::string::npos) ? urlUtf8.size() : firstDelim;

        const size_t colonPos = urlUtf8.find(':');
        if (colonPos != std::string::npos && colonPos < schemeLimit)
        {
            const std::string scheme = urlUtf8.substr(0, colonPos);
            if (IsValidUrlScheme(scheme))
            {
                hasScheme = true;
                posAfterScheme = colonPos + 1;
            }
        }

        bool hasAuthority = false;
        size_t authorityBegin = std::string::npos;
        size_t authorityEnd = std::string::npos;

        if (hasScheme && posAfterScheme + 1 < urlUtf8.size() && urlUtf8.compare(posAfterScheme, 2, "//") == 0)
        {
            hasAuthority = true;
            authorityBegin = posAfterScheme + 2;
        }
        else if (!hasScheme && urlUtf8.size() >= 2 && urlUtf8.compare(0, 2, "//") == 0)
        {
            hasAuthority = true;
            authorityBegin = 2;
        }

        if (hasAuthority)
        {
            authorityEnd = urlUtf8.find_first_of("/?#", authorityBegin);
            if (authorityEnd == std::string::npos)
            {
                authorityEnd = urlUtf8.size();
            }
            if (authorityEnd > outPathEndBeforeQueryAndFragment)
            {
                authorityEnd = outPathEndBeforeQueryAndFragment;
            }

            outPathBegin = authorityEnd;
        }
        else
        {
            outPathBegin = hasScheme ? posAfterScheme : 0;
        }

        if (outPathBegin > outPathEndBeforeQueryAndFragment)
        {
            outPathBegin = outPathEndBeforeQueryAndFragment;
        }

        return true;
    }
    static bool AsciiEqualsAt(const std::string& text, size_t pos, const std::string& pattern, bool caseSensitive)
    {
        if (pattern.empty() || pos > text.size() || pattern.size() > text.size() - pos)
        {
            return false;
        }

        if (caseSensitive)
        {
            return text.compare(pos, pattern.size(), pattern) == 0;
        }

        for (size_t i = 0; i < pattern.size(); i++)
        {
            if (ToLowerAsciiChar(text[pos + i]) != ToLowerAsciiChar(pattern[i]))
            {
                return false;
            }
        }

        return true;
    }

    static void ReplaceAllInplace(std::string& text, const std::string& oldValue, const std::string& newValue, bool caseSensitive = true)
    {
        if (text.empty() || oldValue.empty() || oldValue == newValue)
        {
            return;
        }

        size_t firstPos = std::string::npos;
        if (caseSensitive)
        {
            firstPos = text.find(oldValue);
        }
        else
        {
            for (size_t i = 0; i + oldValue.size() <= text.size(); i++)
            {
                if (AsciiEqualsAt(text, i, oldValue, false))
                {
                    firstPos = i;
                    break;
                }
            }
        }

        if (firstPos == std::string::npos)
        {
            return;
        }

        size_t count = 0;
        if (caseSensitive)
        {
            size_t scanPos = firstPos;
            while (scanPos != std::string::npos)
            {
                count++;
                scanPos = text.find(oldValue, scanPos + oldValue.size());
            }
        }
        else
        {
            size_t scanPos = firstPos;
            while (scanPos + oldValue.size() <= text.size())
            {
                if (AsciiEqualsAt(text, scanPos, oldValue, false))
                {
                    count++;
                    scanPos += oldValue.size();
                }
                else
                {
                    scanPos++;
                }
            }
        }

        std::string result;
        if (newValue.size() > oldValue.size())
        {
            const size_t grow = (newValue.size() - oldValue.size()) * count;
            result.reserve(text.size() + grow);
        }
        else
        {
            result.reserve(text.size());
        }

        size_t lastPos = 0;
        if (caseSensitive)
        {
            size_t pos = firstPos;
            while (pos != std::string::npos)
            {
                result.append(text, lastPos, pos - lastPos);
                result += newValue;

                lastPos = pos + oldValue.size();
                pos = text.find(oldValue, lastPos);
            }
        }
        else
        {
            size_t pos = firstPos;
            while (pos + oldValue.size() <= text.size())
            {
                if (AsciiEqualsAt(text, pos, oldValue, false))
                {
                    result.append(text, lastPos, pos - lastPos);
                    result += newValue;
                    lastPos = pos + oldValue.size();
                    pos = lastPos;
                }
                else
                {
                    pos++;
                }
            }
        }

        result.append(text, lastPos, std::string::npos);
        text.swap(result);
    }
    static std::string ReplaceColonStyleParam(const std::string& path, const std::string& keyUtf8, const std::string& replacementUtf8, bool keyCaseSensitive)
    {
        if (keyUtf8.empty())
        {
            return path;
        }

        auto IsWordChar = [](char c) -> bool
            {
                return ((c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    (c == '_'));
            };

        std::string result;
        result.reserve(path.size() + replacementUtf8.size());

        size_t i = 0;
        while (i < path.size())
        {
            if (path[i] == ':' && (i == 0 || path[i - 1] == '/'))
            {
                if (AsciiEqualsAt(path, i + 1, keyUtf8, keyCaseSensitive))
                {
                    const size_t after = i + 1 + keyUtf8.size();
                    if (after == path.size() || !IsWordChar(path[after]))
                    {
                        result += replacementUtf8;
                        i = after;
                        continue;
                    }
                }
            }

            result.push_back(path[i]);
            i++;
        }

        return result;
    }

    static bool ParseAuthority(const std::string& authorityUtf8, std::string& outUserInfoUtf8, std::string& outHostUtf8, unsigned short& outPort, bool& outHasPort)
    {
        outUserInfoUtf8.clear();
        outHostUtf8.clear();
        outPort = 0;
        outHasPort = false;

        if (authorityUtf8.empty())
        {
            // 允许空 authority（例如 file:///path），此时 host 为空。
            return true;
        }

        std::string hostPort = authorityUtf8;
        const size_t atPos = authorityUtf8.rfind('@');
        if (atPos != std::string::npos)
        {
            outUserInfoUtf8 = authorityUtf8.substr(0, atPos);
            hostPort = (atPos + 1 < authorityUtf8.size()) ? authorityUtf8.substr(atPos + 1) : std::string();
        }

        if (hostPort.empty())
        {
            return true;
        }

        // IPv6 literal: [2001:db8::1]:8080
        if (hostPort.front() == '[')
        {
            const size_t closePos = hostPort.find(']');
            if (closePos == std::string::npos)
            {
                return false;
            }

            outHostUtf8 = hostPort.substr(1, closePos - 1);
            if (outHostUtf8.empty())
            {
                return false;
            }

            if (closePos + 1 == hostPort.size())
            {
                return true;
            }

            if (hostPort[closePos + 1] != ':')
            {
                return false;
            }

            if (closePos + 2 >= hostPort.size())
            {
                return false;
            }

            const std::string portText = hostPort.substr(closePos + 2);
            if (portText.empty())
            {
                return false;
            }

            for (size_t i = 0; i < portText.size(); i++)
            {
                if (portText[i] < '0' || portText[i] > '9')
                {
                    return false;
                }
            }

            try
            {
                const unsigned long portUl = std::stoul(portText);
                if (portUl > 65535UL)
                {
                    return false;
                }
                outPort = static_cast<unsigned short>(portUl);
                outHasPort = true;
            }
            catch (...)
            {
                return false;
            }

            return true;
        }

        // reg-name / IPv4: example.com:80
        const size_t lastColon = hostPort.rfind(':');
        if (lastColon != std::string::npos)
        {
            // authority 中（非 IPv6 literal）出现 ':'，应当解释为端口分隔符。
            if (lastColon == hostPort.size() - 1)
            {
                return false;
            }

            const std::string hostPart = hostPort.substr(0, lastColon);
            const std::string portText = hostPort.substr(lastColon + 1);

            if (hostPart.empty() || portText.empty())
            {
                return false;
            }

            for (size_t i = 0; i < portText.size(); i++)
            {
                if (portText[i] < '0' || portText[i] > '9')
                {
                    return false;
                }
            }

            try
            {
                const unsigned long portUl = std::stoul(portText);
                if (portUl > 65535UL)
                {
                    return false;
                }
                outHostUtf8 = hostPart;
                outPort = static_cast<unsigned short>(portUl);
                outHasPort = true;
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        outHostUtf8 = hostPort;
        return true;
    }
} // namespace


bool GB_UrlOperator::TryParseUrl(const std::string& urlUtf8, GB_UrlOperator::UrlComponents& outComponents)
{
    outComponents = GB_UrlOperator::UrlComponents();

    if (urlUtf8.empty())
    {
        return false;
    }

    size_t prefixEnd = 0;
    bool hasQuery = false;
    size_t queryBegin = 0;
    size_t queryEnd = 0;
    bool hasFragment = false;
    size_t fragmentBegin = 0;
    SplitUrlQueryAndFragment(urlUtf8, prefixEnd, hasQuery, queryBegin, queryEnd, hasFragment, fragmentBegin);

    if (hasQuery && queryBegin <= urlUtf8.size() && queryEnd <= urlUtf8.size() && queryEnd >= queryBegin)
    {
        outComponents.queryUtf8 = urlUtf8.substr(queryBegin, queryEnd - queryBegin);
    }

    if (hasFragment && fragmentBegin <= urlUtf8.size())
    {
        outComponents.fragmentUtf8 = urlUtf8.substr(fragmentBegin);
    }

    size_t posAfterScheme = 0;
    bool hasScheme = false;

    const size_t firstDelim = urlUtf8.find_first_of("/?#");
    const size_t schemeLimit = (firstDelim == std::string::npos) ? urlUtf8.size() : firstDelim;

    const size_t colonPos = urlUtf8.find(':');
    if (colonPos != std::string::npos && colonPos < schemeLimit)
    {
        const std::string scheme = urlUtf8.substr(0, colonPos);
        if (IsValidUrlScheme(scheme))
        {
            outComponents.schemeLower = ToLowerCopy(scheme);
            hasScheme = true;
            posAfterScheme = colonPos + 1;
        }
    }

    bool hasAuthority = false;
    size_t authorityBegin = std::string::npos;
    size_t authorityEnd = std::string::npos;

    if (hasScheme && posAfterScheme + 1 < urlUtf8.size() && urlUtf8.compare(posAfterScheme, 2, "//") == 0)
    {
        hasAuthority = true;
        authorityBegin = posAfterScheme + 2;
    }
    else if (!hasScheme && urlUtf8.size() >= 2 && urlUtf8.compare(0, 2, "//") == 0)
    {
        hasAuthority = true;
        authorityBegin = 2;
    }

    if (hasAuthority)
    {
        authorityEnd = urlUtf8.find_first_of("/?#", authorityBegin);
        if (authorityEnd == std::string::npos)
        {
            authorityEnd = urlUtf8.size();
        }

        outComponents.hasAuthority = true;

        const std::string authority = urlUtf8.substr(authorityBegin, authorityEnd - authorityBegin);
        if (!ParseAuthority(authority, outComponents.userInfoUtf8, outComponents.hostUtf8, outComponents.port, outComponents.hasPort))
        {
            return false;
        }
    }

    size_t pathBegin = 0;
    if (hasAuthority)
    {
        pathBegin = authorityEnd;
    }
    else
    {
        pathBegin = hasScheme ? posAfterScheme : 0;
    }

    if (pathBegin > prefixEnd)
    {
        pathBegin = prefixEnd;
    }

    if (prefixEnd >= pathBegin)
    {
        outComponents.pathUtf8 = urlUtf8.substr(pathBegin, prefixEnd - pathBegin);
    }

    return true;
}


std::string GB_UrlOperator::GetUrlBase(const std::string& urlUtf8)
{
    if (urlUtf8.empty())
    {
        return std::string();
    }

    const size_t qPos = urlUtf8.find('?');
    const size_t hPos = urlUtf8.find('#');

    size_t endPos = urlUtf8.size();
    if (qPos != std::string::npos)
    {
        endPos = qPos;
    }
    if (hPos != std::string::npos)
    {
        endPos = std::min(endPos, hPos);
    }

    return urlUtf8.substr(0, endPos);
}


std::string GB_UrlOperator::GetUrlHost(const std::string& urlUtf8)
{
    GB_UrlOperator::UrlComponents components;
    if (!GB_UrlOperator::TryParseUrl(urlUtf8, components))
    {
        return std::string();
    }
    return components.hostUtf8;
}


std::string GB_UrlOperator::UrlEncode(const std::string& textUtf8, GB_UrlOperator::UrlEncodingMode mode)
{
    return UrlEncodeInternal(textUtf8, mode);
}


std::string GB_UrlOperator::UrlDecode(const std::string& text, GB_UrlOperator::UrlEncodingMode mode)
{
    return UrlDecodeInternal(text, mode);
}


std::vector<GB_UrlOperator::UrlKeyValue> GB_UrlOperator::ParseUrlQueryKvp(const std::string& urlUtf8, bool decode, GB_UrlOperator::UrlEncodingMode decodeMode)
{
    size_t prefixEnd = 0;
    bool hasQuery = false;
    size_t queryBegin = 0;
    size_t queryEnd = 0;
    bool hasFragment = false;
    size_t fragmentBegin = 0;
    SplitUrlQueryAndFragment(urlUtf8, prefixEnd, hasQuery, queryBegin, queryEnd, hasFragment, fragmentBegin);

    if (!hasQuery || queryBegin > queryEnd || queryEnd > urlUtf8.size())
    {
        return {};
    }

    const std::string queryString = urlUtf8.substr(queryBegin, queryEnd - queryBegin);
    const std::vector<GbQueryItemInternal> items = ParseQueryStringInternal(queryString, decode, decodeMode);

    std::vector<GB_UrlOperator::UrlKeyValue> result;
    result.reserve(items.size());

    for (size_t i = 0; i < items.size(); i++)
    {
        GB_UrlOperator::UrlKeyValue kvp;
        kvp.keyUtf8 = items[i].keyUtf8;
        kvp.valueUtf8 = items[i].valueUtf8;
        result.push_back(kvp);
    }

    return result;
}


std::vector<std::string> GB_UrlOperator::GetUrlQueryValues(const std::string& urlUtf8, const std::string& keyUtf8, bool decode, GB_UrlOperator::UrlEncodingMode decodeMode, bool keyCaseSensitive)
{
    if (keyUtf8.empty())
    {
        return {};
    }

    const std::vector<GB_UrlOperator::UrlKeyValue> kvps = GB_UrlOperator::ParseUrlQueryKvp(urlUtf8, decode, decodeMode);

    std::vector<std::string> values;
    for (size_t i = 0; i < kvps.size(); i++)
    {
        if (AreUrlKeysEqual(kvps[i].keyUtf8, keyUtf8, keyCaseSensitive))
        {
            values.push_back(kvps[i].valueUtf8);
        }
    }

    return values;
}


bool GB_UrlOperator::TryGetUrlQueryValue(const std::string& urlUtf8, const std::string& keyUtf8, std::string& outValueUtf8, bool decode, GB_UrlOperator::UrlEncodingMode decodeMode, bool keyCaseSensitive)
{
    outValueUtf8.clear();

    if (keyUtf8.empty())
    {
        return false;
    }

    const std::vector<GB_UrlOperator::UrlKeyValue> kvps = GB_UrlOperator::ParseUrlQueryKvp(urlUtf8, decode, decodeMode);
    for (size_t i = 0; i < kvps.size(); i++)
    {
        if (AreUrlKeysEqual(kvps[i].keyUtf8, keyUtf8, keyCaseSensitive))
        {
            outValueUtf8 = kvps[i].valueUtf8;
            return true;
        }
    }

    return false;
}


std::string GB_UrlOperator::SetUrlQueryValue(const std::string& urlUtf8, const std::string& keyUtf8, const std::string& valueUtf8, GB_UrlOperator::UrlQuerySetMode setMode, GB_UrlOperator::UrlEncodingMode encodeMode, bool keyCaseSensitive)
{
    if (urlUtf8.empty())
    {
        return std::string();
    }

    if (keyUtf8.empty())
    {
        return urlUtf8;
    }

    size_t prefixEnd = 0;
    bool hasQuery = false;
    size_t queryBegin = 0;
    size_t queryEnd = 0;
    bool hasFragment = false;
    size_t fragmentBegin = 0;
    SplitUrlQueryAndFragment(urlUtf8, prefixEnd, hasQuery, queryBegin, queryEnd, hasFragment, fragmentBegin);

    const std::string prefix = urlUtf8.substr(0, prefixEnd);
    const std::string fragment = hasFragment ? urlUtf8.substr(fragmentBegin) : std::string();
    const std::string queryString = hasQuery ? urlUtf8.substr(queryBegin, queryEnd - queryBegin) : std::string();

    std::vector<GbQueryItemRaw> items = ParseQueryStringRaw(queryString, encodeMode);

    const std::string newRawKey = UrlEncodeInternal(keyUtf8, encodeMode);
    const std::string newRawValue = UrlEncodeInternal(valueUtf8, encodeMode);

    bool foundAny = false;

    // ReplaceAll 的语义是“URLSearchParams.set”：替换第一个匹配项并移除后续重复项。
    // 原实现使用 erase 循环会退化为 O(n^2)；这里改为一次线性扫描构建新数组。
    if (setMode != GB_UrlOperator::UrlQuerySetMode::Append && !items.empty())
    {
        std::vector<GbQueryItemRaw> newItems;
        newItems.reserve(items.size());

        bool replacedFirst = false;
        for (size_t i = 0; i < items.size(); i++)
        {
            GbQueryItemRaw item = items[i];
            if (!AreUrlKeysEqual(item.decodedKey, keyUtf8, keyCaseSensitive))
            {
                newItems.push_back(item);
                continue;
            }

            foundAny = true;

            if (setMode == GB_UrlOperator::UrlQuerySetMode::AddIfAbsent)
            {
                newItems.push_back(item);
                continue;
            }

            if (!replacedFirst)
            {
                item.rawValue = newRawValue;
                item.decodedValue = valueUtf8;
                item.hasEquals = true;
                replacedFirst = true;
                newItems.push_back(item);
                continue;
            }

            if (setMode == GB_UrlOperator::UrlQuerySetMode::ReplaceFirst)
            {
                // ReplaceFirst：后续重复项保持不变
                newItems.push_back(item);
            }
            // ReplaceAll：跳过后续重复项
        }

        items.swap(newItems);
    }

    if (setMode == GB_UrlOperator::UrlQuerySetMode::Append || !foundAny)
    {
        char joinSeparator = '&';
        if (queryString.find('&') == std::string::npos && queryString.find(';') != std::string::npos)
        {
            joinSeparator = ';';
        }
        if (!items.empty() && items.back().separator == ';')
        {
            joinSeparator = ';';
        }

        GbQueryItemRaw newItem;
        newItem.separator = joinSeparator;
        newItem.rawKey = newRawKey;
        newItem.rawValue = newRawValue;
        newItem.decodedKey = keyUtf8;
        newItem.decodedValue = valueUtf8;
        newItem.hasEquals = true;

        items.push_back(newItem);
    }

    const std::string newQuery = BuildQueryStringRaw(items);

    std::string result = prefix;
    if (!newQuery.empty())
    {
        result.push_back('?');
        result += newQuery;
    }
    if (hasFragment)
    {
        result.push_back('#');
        result += fragment;
    }

    return result;
}

std::string GB_UrlOperator::RemoveUrlQueryKey(const std::string& urlUtf8, const std::string& keyUtf8, bool decode, GB_UrlOperator::UrlEncodingMode decodeMode, bool keyCaseSensitive)
{
    if (urlUtf8.empty() || keyUtf8.empty())
    {
        return urlUtf8;
    }

    size_t prefixEnd = 0;
    bool hasQuery = false;
    size_t queryBegin = 0;
    size_t queryEnd = 0;
    bool hasFragment = false;
    size_t fragmentBegin = 0;
    SplitUrlQueryAndFragment(urlUtf8, prefixEnd, hasQuery, queryBegin, queryEnd, hasFragment, fragmentBegin);

    if (!hasQuery)
    {
        return urlUtf8;
    }

    const std::string prefix = urlUtf8.substr(0, prefixEnd);
    const std::string fragment = hasFragment ? urlUtf8.substr(fragmentBegin) : std::string();
    const std::string queryString = urlUtf8.substr(queryBegin, queryEnd - queryBegin);

    // 保持幂等：如果原本 query 为空，移除任意 key 都应返回原字符串（包含 '?'）。
    if (queryString.empty())
    {
        return urlUtf8;
    }

    std::vector<GbQueryItemRaw> items = ParseQueryStringRaw(queryString, decodeMode);

    std::vector<GbQueryItemRaw> kept;
    kept.reserve(items.size());

    for (size_t i = 0; i < items.size(); i++)
    {
        const std::string& candidateKey = decode ? items[i].decodedKey : items[i].rawKey;
        if (!AreUrlKeysEqual(candidateKey, keyUtf8, keyCaseSensitive))
        {
            kept.push_back(items[i]);
        }
    }

    // 如果没有删除任何项，直接返回原 URL，避免无意义的重建导致细微格式变化。
    if (kept.size() == items.size())
    {
        return urlUtf8;
    }

    const std::string newQuery = BuildQueryStringRaw(kept);

    std::string result = prefix;
    if (!newQuery.empty())
    {
        result.push_back('?');
        result += newQuery;
    }
    if (hasFragment)
    {
        result.push_back('#');
        result += fragment;
    }

    return result;
}


std::string GB_UrlOperator::ReplaceUrlPathParams(const std::string& urlUtf8, const std::vector<GB_UrlOperator::UrlKeyValue>& params, bool encodeValue, bool keyCaseSensitive)
{
    if (urlUtf8.empty() || params.empty())
    {
        return urlUtf8;
    }

    size_t pathBegin = 0;
    size_t pathEnd = 0;
    if (!TryGetUrlPathRange(urlUtf8, pathBegin, pathEnd))
    {
        return urlUtf8;
    }

    const std::string prefix = urlUtf8.substr(0, pathBegin);
    const std::string path = urlUtf8.substr(pathBegin, pathEnd - pathBegin);
    const std::string suffix = urlUtf8.substr(pathEnd);

    std::string newPath = path;

    for (size_t i = 0; i < params.size(); i++)
    {
        const GB_UrlOperator::UrlKeyValue& p = params[i];
        if (p.keyUtf8.empty())
        {
            continue;
        }

        const std::string replacement = encodeValue ? UrlEncodeInternal(p.valueUtf8, GB_UrlOperator::UrlEncodingMode::Rfc3986) : p.valueUtf8;

        // "{id}" 风格
        const std::string braceToken = "{" + p.keyUtf8 + "}";
        ReplaceAllInplace(newPath, braceToken, replacement, keyCaseSensitive);

        // ":id" 风格（路径段开头）
        newPath = ReplaceColonStyleParam(newPath, p.keyUtf8, replacement, keyCaseSensitive);
    }

    return prefix + newPath + suffix;
}