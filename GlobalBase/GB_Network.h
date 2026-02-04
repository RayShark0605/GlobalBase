#ifndef GLOBALBASE_NETWORK_H
#define GLOBALBASE_NETWORK_H

#include "GlobalBasePort.h"
#include "GB_BaseTypes.h"
#include <string>
#include <vector>

/**
 * @brief 判断当前机器是否能连接到 Internet。
 *
 * @remarks
 * - 返回 true 仅表示“至少能连通某个外部端点”，不保证所有网站均可访问（例如被代理/防火墙限制）。
 * - 返回 false 可能是网络断开、DNS 异常、目标端口被拦截等原因。
 *
 * @param timeoutMs 总超时时间（毫秒）。建议 1000~5000。
 * @return true 表示可以连接到 Internet；false 表示无法连接。
 */
GLOBALBASE_PORT bool GB_CanConnectToInternet(unsigned int timeoutMs = 3000);

/**
 * @brief 网络代理类型。
 */
enum class GB_NetworkProxyType
{
    Http,            // HTTP 代理
    Https,           // HTTPS 代理（如果 libcurl 支持）
    Socks4,          // SOCKS4
    Socks4a,         // SOCKS4a（带域名解析）
    Socks5,          // SOCKS5
    Socks5Hostname   // SOCKS5（在代理端解析域名，等价于 socks5h）
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
    bool useSystemProxy = true;                       // 是否使用系统代理配置（Windows: WinHTTP/IE；Linux: 环境变量）

    bool enableProxy = false;                         // 是否启用自定义代理（useSystemProxy=false 时生效）
    GB_NetworkProxyType proxyType = GB_NetworkProxyType::Http; // 自定义代理类型

    std::string proxyHostUtf8 = "";                   // 代理主机（UTF-8）。可为 "host"、"host:port" 或带 scheme 的 "http://host:port"
    unsigned short proxyPort = 0;                     // 代理端口（0 表示从 proxyHostUtf8 解析，或由系统决定）
    std::string proxyUserNameUtf8 = "";              // 代理用户名（UTF-8）
    std::string proxyPasswordUtf8 = "";              // 代理密码（UTF-8）

    std::string noProxyUtf8 = "";                     // 不走代理的主机列表（CURLOPT_NOPROXY）。逗号分隔，如 "localhost,127.0.0.1,*.company.com"

    bool proxyTunnel = true;                          // HTTPS 目标是否对 HTTP/HTTPS 代理启用隧道（CONNECT）
};

/**
 * @brief URL 请求选项（当前实现主要用于 HTTP/HTTPS GET）。
 */
struct GB_NetworkRequestOptions
{
    GB_NetworkProxySettings proxy;                    // 网络代理设置

    bool impersonateBrowser = true;                   // 是否尽量伪装成浏览器（补充常见请求头等）
    std::string userAgentUtf8 = "";                   // User-Agent。为空且 impersonateBrowser=true 时会自动设置默认 UA
    std::string refererUtf8 = "";                     // Referer。为空表示不设置

    std::vector<std::string> headersUtf8;             // 额外的 HTTP 头（UTF-8），每项形如 "Header-Name: value"

    bool followRedirects = true;                      // 是否自动跟随重定向（3xx）
    int maxRedirects = 10;                            // 最大重定向次数（followRedirects=true 时有效）

    unsigned int connectTimeoutMs = 10000;            // 连接超时（毫秒）
    unsigned int totalTimeoutMs = 0;                  // 总超时（毫秒，包含连接 + 传输），如果是下载大文件建议设置成 0

    bool enableHttp2 = true;                          // 是否允许 libcurl 通过 ALPN 等方式协商 HTTP/2（若当前构建支持）

    bool verifyTlsPeer = true;                        // 是否校验证书链（CURLOPT_SSL_VERIFYPEER）
    bool verifyTlsHost = true;                        // 是否校验证书主机名（CURLOPT_SSL_VERIFYHOST）

    std::string caBundlePathUtf8 = "";                // 自定义 CA 证书文件（PEM）。为空则使用 libcurl 默认策略
    std::string caPathUtf8 = "";                      // 自定义 CA 证书目录（PEM）。为空则使用 libcurl 默认策略

    bool includeResponseHeaders = false;              // 是否收集响应头原始行（遇到重定向可能包含多段）
};

/**
 * @brief URL 请求结果。
 *
 * @remarks
 * - body 为“原始字节流”，不保证是 UTF-8 文本；可能包含 '\0'。
 * - ok 的判定规则：
 *   - libcurl 成功且 HTTP 状态码为 2xx/3xx（或非 HTTP 协议时状态码为 0）。
 */
struct GB_NetworkResponse
{
    bool ok = false;                                  // 是否成功（综合 curl 返回码与 HTTP 状态码）
    long httpStatusCode = 0;                          // HTTP 状态码（非 HTTP 协议时通常为 0）
    std::string effectiveUrlUtf8 = "";                // 最终生效的 URL（跟随重定向后）
    std::string contentTypeUtf8 = "";                 // Content-Type 响应头（可能为空）

    std::string body = "";                            // 响应体（原始字节流）
    std::vector<std::string> responseHeadersUtf8;     // 原始响应头行（includeResponseHeaders=true 时填充）

    std::string errorMessageUtf8 = "";                // 错误信息（UTF-8）
    int curlErrorCode = 0;                            // libcurl 错误码（CURLE_XXX 的数值）
};

/**
 * @brief 根据 URL 发起请求并获取返回数据（当前实现为 HTTP/HTTPS GET）。
 *
 * 为降低误用风险，内部会限制协议为 http/https，并限制重定向协议为 http/https。
 *
 * @param urlUtf8 目标 URL（UTF-8）。
 * @param options 请求选项。
 * @return GB_NetworkResponse 请求结果。
 */
GLOBALBASE_PORT GB_NetworkResponse GB_RequestUrlData(const std::string& urlUtf8, const GB_NetworkRequestOptions& options = GB_NetworkRequestOptions());

/**
 * @brief URL 文件下载结果。
 *
 * @remarks
 * - data 为文件的原始字节流。
 * - fileNameUtf8 会尽量从响应头 Content-Disposition 中解析（filename / filename*），否则会从最终 URL 推断。
 * - totalSizeKnown=false 表示服务端未提供可用的总大小信息（例如 Transfer-Encoding: chunked、缺少 Content-Length 等）。
 */
struct GB_NetworkDownloadedFile
{
    bool ok = false;
    long httpStatusCode = 0;
    std::string effectiveUrlUtf8 = "";
    std::string contentTypeUtf8 = "";
    std::string fileNameUtf8 = "";

    bool totalSizeKnown = false;
    size_t totalBytes = 0;

    GB_ByteBuffer data;

    std::vector<std::string> responseHeadersUtf8;

    std::string errorMessageUtf8 = "";
    int curlErrorCode = 0;
};

/**
 * @brief 尝试从下载链接推断即将下载的文件名（包含扩展名）。
 *
 * @remarks
 * - 优先从响应头 Content-Disposition 中解析（filename / filename*），否则从最终生效 URL 推断。
 * - 如果无法获得“看起来像文件名”的结果（例如 URL 末尾无文件段且响应头也不提供），返回 false。
 * - outFileNameUtf8 仅为文件名，不包含路径分隔符。
 *
 * @param urlUtf8 目标 URL（UTF-8）。
 * @param outFileNameUtf8 输出文件名（UTF-8）。
 * @param options 请求选项（影响代理、重定向、UA 等）。
 * @return 成功返回 true，否则返回 false。
 */
GLOBALBASE_PORT bool GB_TryGetDownloadFileName(const std::string& urlUtf8, std::string& outFileNameUtf8, const GB_NetworkRequestOptions& options = GB_NetworkRequestOptions());

/**
 * @brief URL 文件下载到指定文件路径的结果。
 *
 * @remarks
 * - 与 GB_NetworkDownloadedFile 不同，本结构体不包含内存中的 data，而是直接写入 filePathUtf8。
 * - remoteFileNameUtf8 会尽量从响应头 Content-Disposition 中解析（filename / filename*），否则会从最终 URL 推断。
 */
struct GB_NetworkDownloadedFileToPath
{
    bool ok = false;
    long httpStatusCode = 0;
    std::string effectiveUrlUtf8 = "";
    std::string contentTypeUtf8 = "";
    std::string remoteFileNameUtf8 = "";

    std::string filePathUtf8 = "";

    bool totalSizeKnown = false;
    size_t totalBytes = 0;

    std::vector<std::string> responseHeadersUtf8;

    std::string errorMessageUtf8 = "";
    int curlErrorCode = 0;
};

/**
 * @brief 文件下载策略。
 *
 * @remarks
 * - MultiCurl：使用 libcurl multi 接口在单线程中并行驱动多个 Range 请求（默认）。
 * - MultiThread：使用多个线程 + 多个 easy handle 并行 Range 下载。
 */
enum class GB_DownloadFileStrategy
{
    MultiCurl,
    MultiThread
};

/**
 * @brief 根据 URL 下载文件并返回文件字节流。
 *
 * @remarks
 * - 为降低误用风险，内部会限制协议为 http/https，并限制重定向协议为 http/https。
 * - 如果 totalSizeAtomicPtr 和 downloadedSizeAtomicPtr 同时非空，则会按“字节数”持续更新进度：
 *   - *totalSizeAtomicPtr：总大小（未知时为 0）
 *   - *downloadedSizeAtomicPtr：已下载大小
 * - 当文件较大且服务端支持 Range（Accept-Ranges: bytes 或可用的 Content-Range/Content-Length）时，会尝试并行分段下载以提升速度；
 *   否则自动回退到单线程下载。
 * - 当 options.includeResponseHeaders=true 时，为避免并行分段下载造成的响应头聚合歧义，内部会强制使用单线程下载。
 *
 * @param urlUtf8 目标 URL（UTF-8）。
 * @param options 请求选项。
 * @param strategy 下载策略。
 * @param totalSizeAtomicPtr 可选进度输出指针（实际类型为 std::atomic_size_t*）。
 * @param downloadedSizeAtomicPtr 可选进度输出指针（实际类型为 std::atomic_size_t*）。
 * @return 下载结果。
 */
GLOBALBASE_PORT GB_NetworkDownloadedFile GB_DownloadFile(const std::string& urlUtf8, const GB_NetworkRequestOptions& options = GB_NetworkRequestOptions(), GB_DownloadFileStrategy strategy = GB_DownloadFileStrategy::MultiCurl, void* totalSizeAtomicPtr = nullptr, void* downloadedSizeAtomicPtr = nullptr);

/**
 * @brief 根据 URL 下载文件并直接写入到指定路径（避免大文件占用过多内存）。
 *
 * @remarks
 * - filePathUtf8 若已存在文件则会覆盖。
 * - 若 filePathUtf8 的父目录不存在，会递归创建。
 * - 当文件较大且服务端支持 Range 时，会尝试并行分段下载（MultiCurl 或 MultiThread），否则回退到单线程下载。
 * - 当 options.includeResponseHeaders=true 时，为避免并行分段下载造成的响应头聚合歧义，内部会强制使用单线程下载。
 *
 * @param urlUtf8 目标 URL（UTF-8）。
 * @param filePathUtf8 目标文件路径（UTF-8）。
 * @param options 请求选项。
 * @param strategy 下载策略。
 * @param totalSizeAtomicPtr 可选进度输出指针（实际类型为 std::atomic_size_t*）。
 * @param downloadedSizeAtomicPtr 可选进度输出指针（实际类型为 std::atomic_size_t*）。
 * @return 下载结果。
 */
GLOBALBASE_PORT GB_NetworkDownloadedFileToPath GB_DownloadFileToPath(const std::string& urlUtf8, const std::string& filePathUtf8, const GB_NetworkRequestOptions& options = GB_NetworkRequestOptions(), GB_DownloadFileStrategy strategy = GB_DownloadFileStrategy::MultiCurl, void* totalSizeAtomicPtr = nullptr, void* downloadedSizeAtomicPtr = nullptr);

#endif
