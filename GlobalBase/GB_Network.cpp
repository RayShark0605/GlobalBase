#include "GB_Network.h"
#include "GB_Utf8String.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
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
        buffer->append(ptr, totalSize);
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

        const bool needsAuto = (ieProxyConfig.fAutoDetect != FALSE) || (ieProxyConfig.lpszAutoConfigUrl != nullptr);
        if (!needsAuto)
        {
            // IE 配置读取成功。此时 proxyUtf8 可能为空（DIRECT），也应返回 true。
            return true;
        }

        HINTERNET session = ::WinHttpOpen(L"GlobalBase/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session != nullptr)
        {
            // 避免 WPAD/PAC 等场景下长时间阻塞
            ::WinHttpSetTimeouts(session, 2000, 2000, 2000, 2000);
        }

        if (session == nullptr)
        {
            // 自动代理需要 WinHTTP session，但创建失败时回退到“显式代理”（若有）。
            return gotExplicitProxy;
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

        bool gotAutoResult = false;
        if (::WinHttpGetProxyForUrl(session, urlW.c_str(), &autoProxyOptions, &proxyInfo))
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

        ::WinHttpCloseHandle(session);

        if (gotAutoResult)
        {
            return true;
        }

        // 自动代理失败时：若存在显式代理，则仍认为获取成功；否则交由上层决定回退策略
        return gotExplicitProxy;
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

            if (GetWindowsSystemProxyForUrlUtf8(urlUtf8, systemProxyUtf8, systemBypassUtf8))
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
            size_t idx = 0;
            const unsigned long long v = std::stoull(TrimCopy(text), &idx, 10);
            if (idx == 0)
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

    static std::string PercentDecode(const std::string& text)
    {
        std::string result;
        result.reserve(text.size());

        auto HexToNibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
            };

        for (size_t i = 0; i < text.size(); i++)
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

        std::vector<std::string> parts;
        {
            std::string current;
            for (size_t i = 0; i < headerValue.size(); i++)
            {
                const char c = headerValue[i];
                if (c == ';')
                {
                    parts.push_back(TrimCopy(current));
                    current.clear();
                }
                else
                {
                    current.push_back(c);
                }
            }
            if (!current.empty())
            {
                parts.push_back(TrimCopy(current));
            }
        }

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
        const size_t totalSize = size * nitems;
        if (userData == nullptr || buffer == nullptr || totalSize == 0)
        {
            return totalSize;
        }

        GbDownloadHeaderState* state = static_cast<GbDownloadHeaderState*>(userData);
        if (state == nullptr || state->result == nullptr)
        {
            return totalSize;
        }

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
            if (TryParseUnsignedLongLong(valueText, v))
            {
                state->result->totalSizeKnown = true;
                state->result->totalBytes = static_cast<size_t>(v);

                if (state->progress.enabled)
                {
                    state->progress.totalBytesPtr->store(state->result->totalBytes, std::memory_order_relaxed);
                }

                if (!state->hasReserved && state->result->totalBytes > 0)
                {
                    // 只 reserve，不 resize，避免先填充 0
                    state->result->data.reserve(state->result->totalBytes);
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

    struct GbWriteState
    {
        GB_ByteBuffer* data = nullptr;
        GbDownloadProgressPointers progress;
    };

    static size_t DownloadWriteCallback(void* ptr, size_t size, size_t nmemb, void* userData)
    {
        const size_t totalSize = size * nmemb;
        if (userData == nullptr || ptr == nullptr || totalSize == 0)
        {
            return 0;
        }

        GbWriteState* state = static_cast<GbWriteState*>(userData);
        if (state == nullptr || state->data == nullptr)
        {
            return 0;
        }

        const unsigned char* bytes = static_cast<const unsigned char*>(ptr);
        const size_t oldSize = state->data->size();
        state->data->resize(oldSize + totalSize);
        std::memcpy(state->data->data() + oldSize, bytes, totalSize);

        if (state->progress.enabled)
        {
            state->progress.downloadedBytesPtr->fetch_add(totalSize, std::memory_order_relaxed);
        }

        return totalSize;
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

        const size_t total = (dltotal > 0) ? static_cast<size_t>(dltotal) : 0;
        const size_t now = (dlnow > 0) ? static_cast<size_t>(dlnow) : 0;

        progress->totalBytesPtr->store(total, std::memory_order_relaxed);
        progress->downloadedBytesPtr->store(now, std::memory_order_relaxed);

        return 0;
    }

    static bool ConfigureDownloadCurlCommon(
        CURL* curlHandle,
        const std::string& urlUtf8,
        const GB_NetworkRequestOptions& options,
        GbDownloadProgressPointers& progress,
        char* errorBuffer,
        size_t errorBufferSize,
        struct curl_slist** outHeaders)
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
        curl_easy_setopt(curlHandle, CURLOPT_TIMEOUT_MS, static_cast<long>(options.totalTimeoutMs));
        curl_easy_setopt(curlHandle, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options.connectTimeoutMs));
        // 只允许 http/https
#if defined(CURLOPT_PROTOCOLS_STR)
        curl_easy_setopt(curlHandle, CURLOPT_PROTOCOLS_STR, "http,https");
#elif defined(CURLOPT_PROTOCOLS)
        curl_easy_setopt(curlHandle, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
#if defined(CURLOPT_REDIR_PROTOCOLS_STR)
        curl_easy_setopt(curlHandle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#elif defined(CURLOPT_REDIR_PROTOCOLS)
        curl_easy_setopt(curlHandle, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

        if (options.followRedirects)
        {
            curl_easy_setopt(curlHandle, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curlHandle, CURLOPT_MAXREDIRS, static_cast<long>(options.maxRedirects));
        }
        else
        {
            curl_easy_setopt(curlHandle, CURLOPT_FOLLOWLOCATION, 0L);
        }
        // 多线程/超时场景下避免信号（Unix 下尤其重要）
        curl_easy_setopt(curlHandle, CURLOPT_NOSIGNAL, 1L);

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

#ifdef CURL_HTTP_VERSION_2TLS
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
            headers = curl_slist_append(headers, "Accept: */*");
            headers = curl_slist_append(headers, "Connection: keep-alive");
        }

        for (size_t i = 0; i < options.headersUtf8.size(); i++)
        {
            headers = curl_slist_append(headers, options.headersUtf8[i].c_str());
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
            curl_easy_setopt(curlHandle, CURLOPT_XFERINFOFUNCTION, DownloadXferInfoCallback);
            curl_easy_setopt(curlHandle, CURLOPT_XFERINFODATA, &progress);
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
        const size_t totalSize = size * nitems;
        if (userData == nullptr || buffer == nullptr || totalSize == 0)
        {
            return totalSize;
        }

        GbProbeHeaderState* state = static_cast<GbProbeHeaderState*>(userData);
        if (state == nullptr || state->info == nullptr)
        {
            return totalSize;
        }

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
            const size_t slashPos = valueText.find('/');
            if (slashPos != std::string::npos && slashPos + 1 < valueText.size())
            {
                const std::string totalText = TrimCopy(valueText.substr(slashPos + 1));
                unsigned long long v = 0;
                if (totalText != "*" && TryParseUnsignedLongLong(totalText, v))
                {
                    state->info->totalSizeKnown = true;
                    state->info->totalBytes = static_cast<size_t>(v);
                }
            }
        }
        else if (lower.find("content-length:") == 0)
        {
            const std::string valueText = TrimCopy(headerLine.substr(std::string("content-length:").size()));
            unsigned long long v = 0;
            if (TryParseUnsignedLongLong(valueText, v))
            {
                state->info->totalSizeKnown = true;
                state->info->totalBytes = static_cast<size_t>(v);
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

    struct GbProbeWriteState
    {
        size_t receivedBytes = 0;
        size_t maxBytes = 1024;
    };

    static size_t ProbeWriteCallback(void* ptr, size_t size, size_t nmemb, void* userData)
    {
        (void)ptr;

        const size_t totalSize = size * nmemb;
        if (userData == nullptr)
        {
            return totalSize;
        }

        GbProbeWriteState* state = static_cast<GbProbeWriteState*>(userData);
        if (state == nullptr)
        {
            return totalSize;
        }

        state->receivedBytes += totalSize;

        // 如果服务端忽略 Range 且开始狂发 body，这里尽快中止
        if (state->receivedBytes > state->maxBytes)
        {
            return 0;
        }

        return totalSize;
    }

    static GbProbeInfo ProbeUrlForRangeAndSize(const std::string& urlUtf8, const GB_NetworkRequestOptions& options)
    {
        EnsureCurlGlobalInit();

        GbProbeInfo info;

        // 先 HEAD：无 body 成本
        {
            CURL* curlHandle = curl_easy_init();
            if (curlHandle != nullptr)
            {
                char errorBuffer[CURL_ERROR_SIZE] = { 0 };
                struct curl_slist* headers = nullptr;

                GbDownloadProgressPointers progress;
                ConfigureDownloadCurlCommon(curlHandle, urlUtf8, options, progress, errorBuffer, sizeof(errorBuffer), &headers);

                GbProbeHeaderState headerState;
                headerState.info = &info;

                curl_easy_setopt(curlHandle, CURLOPT_NOBODY, 1L);
                curl_easy_setopt(curlHandle, CURLOPT_HEADERFUNCTION, ProbeHeaderCallback);
                curl_easy_setopt(curlHandle, CURLOPT_HEADERDATA, &headerState);
                curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, nullptr);

                CURLcode res = curl_easy_perform(curlHandle);

                long httpCode = 0;
                curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &httpCode);
                info.ok = (res == CURLE_OK);

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

                // HEAD 成功且已经有 size + ranges 信息就直接返回
                if (info.ok && info.totalSizeKnown)
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

            GbDownloadProgressPointers progress;
            ConfigureDownloadCurlCommon(curlHandle, urlUtf8, options, progress, errorBuffer, sizeof(errorBuffer), &headers);

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

            // res 可能因为我们主动中止而不是 OK；但 headers 可能已经足够了
            (void)res;
            info.ok = true;

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

    static size_t RangeWriteCallback(void* ptr, size_t size, size_t nmemb, void* userData)
    {
        const size_t totalSize = size * nmemb;
        if (userData == nullptr || ptr == nullptr || totalSize == 0)
        {
            return 0;
        }

        GbRangeChunkState* state = static_cast<GbRangeChunkState*>(userData);
        if (state == nullptr || state->bufferBase == nullptr)
        {
            return 0;
        }

        if (state->writtenBytes + totalSize > state->expectedBytes)
        {
            // 服务端忽略 Range 时，可能会超出预期，直接失败让上层回退
            return 0;
        }

        std::memcpy(state->bufferBase + state->baseOffset + state->writtenBytes, ptr, totalSize);
        state->writtenBytes += totalSize;

        if (state->progress.enabled)
        {
            state->progress.downloadedBytesPtr->fetch_add(totalSize, std::memory_order_relaxed);
        }

        return totalSize;
    }

    struct GbRangeThreadResult
    {
        CURLcode curlCode = CURLE_OK;
        std::string errorMessageUtf8 = "";
    };

    static GbRangeThreadResult DownloadRangeChunk(
        const std::string& urlUtf8,
        const GB_NetworkRequestOptions& options,
        size_t rangeBegin,
        size_t rangeEnd,
        GbRangeChunkState& state)
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

        GbDownloadProgressPointers progress;
        ConfigureDownloadCurlCommon(curlHandle, urlUtf8, options, progress, errorBuffer, sizeof(errorBuffer), &headers);

        const std::string rangeText = std::to_string(static_cast<unsigned long long>(rangeBegin)) + "-" + std::to_string(static_cast<unsigned long long>(rangeEnd));
        curl_easy_setopt(curlHandle, CURLOPT_RANGE, rangeText.c_str());

        curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, RangeWriteCallback);
        curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(curlHandle, CURLOPT_HEADERFUNCTION, nullptr);

        const CURLcode res = curl_easy_perform(curlHandle);

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

        return result;
    }

    static GB_NetworkDownloadedFile DownloadFileSingleThread(
        const std::string& urlUtf8,
        const GB_NetworkRequestOptions& options,
        GbDownloadProgressPointers progress)
    {
        EnsureCurlGlobalInit();

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

    static GB_NetworkDownloadedFile DownloadFileMultiThread(
        const std::string& urlUtf8,
        const GB_NetworkRequestOptions& options,
        const GbProbeInfo& probe,
        GbDownloadProgressPointers progress)
    {
        GB_NetworkDownloadedFile result;

        if (!probe.totalSizeKnown || probe.totalBytes == 0)
        {
            return DownloadFileSingleThread(urlUtf8, options, progress);
        }

        const size_t totalBytes = probe.totalBytes;

        if (progress.enabled)
        {
            progress.totalBytesPtr->store(totalBytes, std::memory_order_relaxed);
            progress.downloadedBytesPtr->store(0, std::memory_order_relaxed);
        }

        result.totalSizeKnown = true;
        result.totalBytes = totalBytes;
        result.fileNameUtf8 = !probe.fileNameUtf8.empty() ? probe.fileNameUtf8 : ExtractFileNameFromUrlUtf8(!probe.effectiveUrlUtf8.empty() ? probe.effectiveUrlUtf8 : urlUtf8);
        result.contentTypeUtf8 = probe.contentTypeUtf8;

        result.data.resize(totalBytes);

        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0)
        {
            hw = 4;
        }
        size_t numThreads = static_cast<size_t>(hw);
        if (numThreads > 8)
        {
            numThreads = 8;
        }
        if (numThreads < 2)
        {
            numThreads = 2;
        }

        const size_t kMinChunkSize = 4ULL * 1024ULL * 1024ULL;
        if (totalBytes / numThreads < kMinChunkSize)
        {
            numThreads = std::max<size_t>(2, totalBytes / kMinChunkSize);
        }
        if (numThreads < 2)
        {
            return DownloadFileSingleThread(urlUtf8, options, progress);
        }

        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        std::vector<GbRangeThreadResult> threadResults;
        threadResults.resize(numThreads);

        std::vector<GbRangeChunkState> chunkStates;
        chunkStates.resize(numThreads);

        const std::string finalUrl = !probe.effectiveUrlUtf8.empty() ? probe.effectiveUrlUtf8 : urlUtf8;

        size_t begin = 0;
        for (size_t i = 0; i < numThreads; i++)
        {
            size_t end = 0;
            if (i + 1 == numThreads)
            {
                end = totalBytes - 1;
            }
            else
            {
                const size_t chunkSize = totalBytes / numThreads;
                end = begin + chunkSize - 1;
            }

            GbRangeChunkState& state = chunkStates[i];
            state.bufferBase = result.data.data();
            state.baseOffset = begin;
            state.expectedBytes = end - begin + 1;
            state.writtenBytes = 0;
            state.progress = progress;

            const size_t threadIndex = i;
            const size_t rangeBegin = begin;
            const size_t rangeEnd = end;

            threads.emplace_back([&, threadIndex, rangeBegin, rangeEnd]() {
                threadResults[threadIndex] = DownloadRangeChunk(finalUrl, options, rangeBegin, rangeEnd, chunkStates[threadIndex]);
                });

            begin = end + 1;
        }

        for (size_t i = 0; i < threads.size(); i++)
        {
            threads[i].join();
        }

        for (size_t i = 0; i < threadResults.size(); i++)
        {
            const GbRangeThreadResult& tr = threadResults[i];
            if (tr.curlCode != CURLE_OK)
            {
                if (progress.enabled)
                {
                    progress.downloadedBytesPtr->store(0, std::memory_order_relaxed);
                    progress.totalBytesPtr->store(0, std::memory_order_relaxed);
                }
                return DownloadFileSingleThread(urlUtf8, options, progress);
            }
        }

        result.ok = true;
        result.httpStatusCode = 206; // 多个 206 合成的“完成态”
        result.effectiveUrlUtf8 = finalUrl;

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

    // 端点顺序：先不依赖 DNS 的 IP，再尝试常见域名
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

#if defined(CURLOPT_PROTOCOLS_STR)
    ::curl_easy_setopt(curlHandle, CURLOPT_PROTOCOLS_STR, "http,https");
#elif defined(CURLOPT_PROTOCOLS)
    ::curl_easy_setopt(curlHandle, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

#if defined(CURLOPT_REDIR_PROTOCOLS_STR)
    ::curl_easy_setopt(curlHandle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#elif defined(CURLOPT_REDIR_PROTOCOLS)
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

GB_NetworkDownloadedFile GB_DownloadFile(const std::string& urlUtf8, const GB_NetworkRequestOptions& options, void* totalSizeAtomicPtr, void* downloadedSizeAtomicPtr)
{
    const GbDownloadProgressPointers progress = GetDownloadProgressPointers(totalSizeAtomicPtr, downloadedSizeAtomicPtr);

    const std::string schemeLower = GetUrlSchemeLower(urlUtf8);
    if (schemeLower != "http" && schemeLower != "https")
    {
        GB_NetworkDownloadedFile result;
        result.ok = false;
        result.errorMessageUtf8 = "Unsupported URL scheme (only http/https)";
        return result;
    }

    // 探测是否支持分段与总大小（用于并行下载 + 文件名推断）
    const GbProbeInfo probe = ProbeUrlForRangeAndSize(urlUtf8, options);

    if (probe.ok && ShouldUseMultiThreadDownload(probe.totalBytes, probe.acceptRangesBytes))
    {
        return DownloadFileMultiThread(urlUtf8, options, probe, progress);
    }

    return DownloadFileSingleThread(urlUtf8, options, progress);
}

