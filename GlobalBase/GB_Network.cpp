#include "GB_Network.h"
#include "GB_Utf8String.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <curl/curl.h>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <winhttp.h>
#  pragma comment(lib, "Ws2_32.lib")
#  pragma comment(lib, "winhttp.lib")
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#endif

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

    static bool WaitForConnect(SocketHandle socketHandle, unsigned int timeoutMs)
    {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(socketHandle, &writeSet);

        fd_set exceptSet;
        FD_ZERO(&exceptSet);
        FD_SET(socketHandle, &exceptSet);

        timeval tv;
        tv.tv_sec = static_cast<long>(timeoutMs / 1000);
        tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);

#ifdef _WIN32
        const int selectResult = ::select(0, nullptr, &writeSet, &exceptSet, &tv);
#else
        const int selectResult = ::select(socketHandle + 1, nullptr, &writeSet, &exceptSet, &tv);
#endif
        if (selectResult <= 0)
        {
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
        const std::string portString = std::to_string(static_cast<unsigned int>(port));

        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
#ifdef AI_ADDRCONFIG
        hints.ai_flags = AI_ADDRCONFIG;
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

        for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next)
        {
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
            const bool inProgress =
                (lastError == WSAEWOULDBLOCK) ||
                (lastError == WSAEINPROGRESS) ||
                (lastError == WSAEALREADY);
#else
            const bool inProgress =
                (lastError == EINPROGRESS) ||
                (lastError == EALREADY);
#endif
            if (!inProgress)
            {
                CloseSocket(socketHandle);
                continue;
            }

            const bool ok = WaitForConnect(socketHandle, timeoutMs);
            CloseSocket(socketHandle);

            if (ok)
            {
                return true;
            }
        }

        return false;
    }

    class CurlGlobalGuard
    {
    public:
        CurlGlobalGuard()
        {
            m_ok = (::curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
        }

        ~CurlGlobalGuard()
        {
            if (m_ok)
            {
                ::curl_global_cleanup();
            }
        }

        bool IsOk() const
        {
            return m_ok;
        }

    private:
        bool m_ok = false;
    };

    static bool EnsureCurlGlobalInit()
    {
        static const CurlGlobalGuard guard;
        return guard.IsOk();
    }

    static size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userData)
    {
        if (ptr == nullptr || userData == nullptr)
        {
            return 0;
        }

        const size_t totalSize = size * nmemb;
        std::string* buffer = static_cast<std::string*>(userData);
        buffer->append(ptr, totalSize);
        return totalSize;
    }

    static size_t CurlHeaderCallback(char* buffer, size_t size, size_t nitems, void* userData)
    {
        if (buffer == nullptr || userData == nullptr)
        {
            return 0;
        }

        const size_t totalSize = size * nitems;
        std::vector<std::string>* headers = static_cast<std::vector<std::string>*>(userData);
        headers->emplace_back(buffer, totalSize);
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
        return TrimCopy(noProxyUtf8);
    }

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
        std::string selected;
        std::string fallback;
        bool foundSchemeSpecific = false;

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
                    const std::string normalized = NormalizeProxyToken(token);
                    if (!normalized.empty())
                    {
                        fallback = normalized;
                    }
                }
                else
                {
                    const std::string keyLower = ToLowerCopy(TrimCopy(token.substr(0, eqPos)));
                    const std::string value = NormalizeProxyToken(token.substr(eqPos + 1));
                    if (keyLower == schemeLower)
                    {
                        selected = value; // 可能为空（DIRECT）
                        foundSchemeSpecific = true;
                        break;
                    }
                }
            }

            begin = end + 1;
        }

        if (foundSchemeSpecific)
        {
            return selected;
        }
        return fallback;
    }

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

    static bool GetWindowsSystemProxyForUrlUtf8(const std::string& urlUtf8, std::string& proxyUtf8, std::string& bypassUtf8)
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

        if (!::WinHttpGetIEProxyConfigForCurrentUser(&ieProxyConfig))
        {
            return false;
        }

        ScopedGlobalFreeW proxyFree;
        ScopedGlobalFreeW bypassFree;
        ScopedGlobalFreeW pacUrlFree;
        proxyFree.ptr = ieProxyConfig.lpszProxy;
        bypassFree.ptr = ieProxyConfig.lpszProxyBypass;
        pacUrlFree.ptr = ieProxyConfig.lpszAutoConfigUrl;

        bool gotProxy = false;
        if (ieProxyConfig.lpszProxy != nullptr && ieProxyConfig.lpszProxy[0] != 0)
        {
            proxyUtf8 = GB_WStringToUtf8(ieProxyConfig.lpszProxy);
            gotProxy = !proxyUtf8.empty();
        }

        if (ieProxyConfig.lpszProxyBypass != nullptr && ieProxyConfig.lpszProxyBypass[0] != 0)
        {
            bypassUtf8 = GB_WStringToUtf8(ieProxyConfig.lpszProxyBypass);
        }

        const bool needsAuto = (ieProxyConfig.fAutoDetect != FALSE) || (ieProxyConfig.lpszAutoConfigUrl != nullptr);
        if (!needsAuto)
        {
            return gotProxy;
        }

        HINTERNET session = ::WinHttpOpen(L"GlobalBase/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr)
        {
            return gotProxy;
        }

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
        if (::WinHttpGetProxyForUrl(session, urlW.c_str(), &autoProxyOptions, &proxyInfo))
        {
            proxyInfoFree.ptr = proxyInfo.lpszProxy;
            bypassInfoFree.ptr = proxyInfo.lpszProxyBypass;

            if (proxyInfo.lpszProxy != nullptr && proxyInfo.lpszProxy[0] != 0)
            {
                proxyUtf8 = GB_WStringToUtf8(proxyInfo.lpszProxy);
                gotProxy = !proxyUtf8.empty();
            }

            if (proxyInfo.lpszProxyBypass != nullptr && proxyInfo.lpszProxyBypass[0] != 0)
            {
                bypassUtf8 = GB_WStringToUtf8(proxyInfo.lpszProxyBypass);
            }
        }

        ::WinHttpCloseHandle(session);
        return gotProxy;
    }
#endif

    static void ApplyProxySettings(CURL* curlHandle, const std::string& urlUtf8, const GB_NetworkProxySettings& proxySettings)
    {
        if (curlHandle == nullptr)
        {
            return;
        }

        if (proxySettings.useSystemProxy)
        {
#ifdef _WIN32
            std::string systemProxyUtf8;
            std::string systemBypassUtf8;
            if (GetWindowsSystemProxyForUrlUtf8(urlUtf8, systemProxyUtf8, systemBypassUtf8))
            {
                const std::string schemeLower = GetUrlSchemeLower(urlUtf8);
                const std::string selectedProxy = PickProxyFromProtocolList(systemProxyUtf8, schemeLower);
                if (!selectedProxy.empty())
                {
                    ::curl_easy_setopt(curlHandle, CURLOPT_PROXY, selectedProxy.c_str());
                }

                const std::string rawBypass = NormalizeNoProxyList(systemBypassUtf8);
                const bool hadLocalBypass = (ToLowerCopy(rawBypass).find("<local>") != std::string::npos);

                std::string bypass = RemoveLocalBypassToken(rawBypass);
                if (hadLocalBypass)
                {
                    if (!bypass.empty())
                    {
                        bypass += ",";
                    }
                    bypass += "localhost,127.0.0.1";
                }

                bypass = NormalizeNoProxyList(bypass);
                if (!bypass.empty())
                {
                    ::curl_easy_setopt(curlHandle, CURLOPT_NOPROXY, bypass.c_str());
                }
            }
#endif
            return;
        }

        if (!proxySettings.enableProxy)
        {
            ::curl_easy_setopt(curlHandle, CURLOPT_PROXY, "");
            ::curl_easy_setopt(curlHandle, CURLOPT_NOPROXY, "*");
            return;
        }

        if (!proxySettings.proxyHostUtf8.empty())
        {
            ::curl_easy_setopt(curlHandle, CURLOPT_PROXY, proxySettings.proxyHostUtf8.c_str());
        }

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

        if (!proxySettings.proxyUserNameUtf8.empty())
        {
            ::curl_easy_setopt(curlHandle, CURLOPT_PROXYUSERNAME, proxySettings.proxyUserNameUtf8.c_str());
        }

        if (!proxySettings.proxyPasswordUtf8.empty())
        {
            ::curl_easy_setopt(curlHandle, CURLOPT_PROXYPASSWORD, proxySettings.proxyPasswordUtf8.c_str());
        }

        if (proxySettings.proxyTunnel)
        {
            ::curl_easy_setopt(curlHandle, CURLOPT_HTTPPROXYTUNNEL, 1L);
        }

        const std::string noProxy = NormalizeNoProxyList(proxySettings.noProxyUtf8);
        if (!noProxy.empty())
        {
            ::curl_easy_setopt(curlHandle, CURLOPT_NOPROXY, noProxy.c_str());
        }
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

    // 端点顺序：先不依赖 DNS 的 IP，再尝试常见域名
    const ProbeEndpoint endpoints[] =
    {
        { "1.1.1.1", 443 },
        { "www.baidu.com", 443 },
        { "www.qq.com", 443 }
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

GB_NetworkResponse GB_RequestUrlData(const std::string& urlUtf8, const GB_NetworkRequestOptions& options)
{
    GB_NetworkResponse response;

    if (urlUtf8.empty())
    {
        response.ok = false;
        response.errorMessageUtf8 = "URL is empty";
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

    ::curl_easy_setopt(curlHandle, CURLOPT_URL, urlUtf8.c_str());
    ::curl_easy_setopt(curlHandle, CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(curlHandle, CURLOPT_FOLLOWLOCATION, options.followRedirects ? 1L : 0L);
    ::curl_easy_setopt(curlHandle, CURLOPT_MAXREDIRS, static_cast<long>(options.maxRedirects));
    ::curl_easy_setopt(curlHandle, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options.connectTimeoutMs));
    ::curl_easy_setopt(curlHandle, CURLOPT_TIMEOUT_MS, static_cast<long>(options.totalTimeoutMs));

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

#ifdef CURL_HTTP_VERSION_2TLS
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
    if (options.impersonateBrowser)
    {
        headerGuard.list = ::curl_slist_append(headerGuard.list, "Accept: */*");
        headerGuard.list = ::curl_slist_append(headerGuard.list, "Accept-Language: zh-CN,zh;q=0.9,en;q=0.8");
        ::curl_easy_setopt(curlHandle, CURLOPT_ACCEPT_ENCODING, "");
    }

    for (size_t i = 0; i < options.headersUtf8.size(); i++)
    {
        const std::string& header = options.headersUtf8[i];
        if (!header.empty())
        {
            headerGuard.list = ::curl_slist_append(headerGuard.list, header.c_str());
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
        response.errorMessageUtf8 = ::curl_easy_strerror(curlCode);
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
