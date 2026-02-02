#ifndef GLOBALBASE_NETWORK_H_H
#define GLOBALBASE_NETWORK_H_H

#include "GlobalBasePort.h"
#include <string>
#include <vector>

/**
 * @brief 判断当前机器是否能连接到 Internet。
 * 
 * 默认探测端点（可在 cpp 内按实际环境调整）：
 * - 1.1.1.1:443（不依赖 DNS，通常较快）
 * - www.baidu.com:443
 * - www.qq.com:443
 *
 * 注意：
 * - 返回 true 仅表示“至少能连通某个外部端点”，不保证所有网站均可访问（例如被代理/防火墙限制）。
 * - 返回 false 可能是网络断开、DNS 异常、目标端口被拦截等原因。
 *
 * @param timeoutMs 总超时时间（毫秒）。建议 1000~5000。
 * @return true 表示可以连接到 Internet；false 表示无法连接。
 */
GLOBALBASE_PORT bool GB_CanConnectToInternet(unsigned int timeoutMs = 3000);

// 网络代理类型。
enum class GB_NetworkProxyType
{
    Http,
    Https,
    Socks4,
    Socks4a,
    Socks5,
    Socks5Hostname
};

/**
 * @brief 代理设置。
 *
 * 设计目标：
 * - useSystemProxy=true：尽量使用系统默认代理。
 *   - Windows：优先尝试从 WinHTTP/IE 获取（含 WPAD/PAC）。
 *   - Linux：通常由环境变量 http_proxy/https_proxy/all_proxy/no_proxy 决定（libcurl 默认会遵循）。
 * - useSystemProxy=false：完全由本结构体决定。
 *   - enableProxy=false 时，会显式禁用所有代理（包括环境变量代理）。
 */
struct GB_NetworkProxySettings
{
    bool useSystemProxy = true;

    bool enableProxy = false;
    GB_NetworkProxyType proxyType = GB_NetworkProxyType::Http;

    std::string proxyHostUtf8 = "";
    unsigned short proxyPort = 0;
    std::string proxyUserNameUtf8 = "";
    std::string proxyPasswordUtf8 = "";

    /*
    * @brief 不走代理的主机列表。
    * 逗号分隔，例如："localhost,127.0.0.1,*.company.com"。
    */
    std::string noProxyUtf8 = "";

    /*
    * @brief 是否对 HTTPS 目标启用 HTTP 代理隧道（CONNECT）。
    * 对于企业网络较常见的 HTTP 代理访问 HTTPS 站点，一般需要为 true。
    */
    bool proxyTunnel = true;
};

/**
 * @brief URL 请求选项（当前实现主要用于 HTTP/HTTPS GET）。
 */
struct GB_NetworkRequestOptions
{
	// 网络代理设置。
    GB_NetworkProxySettings proxy;

    // 是否尽量伪装成浏览器。
    bool impersonateBrowser = true;

    // 定义 User-Agent。为空时，如果 impersonateBrowser=true，会自动设置一个默认 UA。
    std::string userAgentUtf8 = "";

    // Referer 头。为空表示不设置。
    std::string refererUtf8 = "";

    // 额外的 HTTP 头（UTF-8 字符串，形如 "Header-Name: value"）。
    std::vector<std::string> headersUtf8;

    bool followRedirects = true;
    int maxRedirects = 10;

    unsigned int connectTimeoutMs = 10000;
    unsigned int totalTimeoutMs = 30000;

    // 是否允许 libcurl 通过 ALPN 等方式协商 HTTP/2（如果当前构建支持）。
    bool enableHttp2 = true;

    bool verifyTlsPeer = true;
    bool verifyTlsHost = true;

    // 自定义 CA 证书文件（PEM）。为空则使用 libcurl 默认策略。
    std::string caBundlePathUtf8 = "";

    // 自定义 CA 证书目录（PEM）。为空则使用 libcurl 默认策略。
    std::string caPathUtf8 = "";

    // 是否收集响应头（原始行）。
    bool includeResponseHeaders = false;
};

/**
 * @brief URL 请求结果。
 *
 * 注意：
 * - body 为“原始字节流”，不保证是 UTF-8 文本；可能包含 '\0'。
 * - ok 的判定规则：
 *   - libcurl 成功且 HTTP 状态码为 2xx/3xx（或非 HTTP 协议时状态码为 0）
 */
struct GB_NetworkResponse
{
    bool ok = false;
    long httpStatusCode = 0;
    std::string effectiveUrlUtf8 = "";
    std::string contentTypeUtf8 = "";

    std::string body = "";
    std::vector<std::string> responseHeadersUtf8;

    std::string errorMessageUtf8 = "";
    int curlErrorCode = 0;
};

/**
 * @brief 根据 URL 发起请求并获取返回数据（当前实现为 HTTP/HTTPS GET）。
 *
 * @param urlUtf8 目标 URL（UTF-8）。
 * @param options 请求选项。
 * @return GB_NetworkResponse 请求结果。
 */
GLOBALBASE_PORT GB_NetworkResponse GB_RequestUrlData(const std::string& urlUtf8, const GB_NetworkRequestOptions& options = GB_NetworkRequestOptions());


#endif