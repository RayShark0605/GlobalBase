#ifndef GLOBALBASE_SYSTEM_NETWORK_H_H
#define GLOBALBASE_SYSTEM_NETWORK_H_H

#include "../GlobalBasePort.h"
#include "GB_EventDispatcher.h"
#include "GB_SystemResult.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/**
 * @brief 网络地址族。
 */
enum class GB_SystemNetworkAddressFamily : uint16_t
{
    Unknown = 0, ///< 未知地址族或当前地址不是有效的 IP 地址。
    IPv4 = 1,    ///< IPv4 地址族。
    IPv6 = 2     ///< IPv6 地址族。
};

/**
 * @brief 网络接口类型。
 */
enum class GB_SystemNetworkInterfaceType : uint16_t
{
    Unknown = 0,   ///< 未知接口类型或暂未映射的系统接口类型。
    Ethernet = 1,  ///< 有线以太网接口。
    Wifi = 2,      ///< IEEE 802.11 无线局域网接口。
    Loopback = 3,  ///< 本机环回接口。
    Tunnel = 4,    ///< 隧道接口，例如 VPN、IPv6 隧道等。
    Ppp = 5,       ///< PPP 点对点接口。
    Cellular = 6,  ///< 蜂窝网络接口。
    Bluetooth = 7, ///< 蓝牙网络接口；优先依据 MIB_IF_ROW2::PhysicalMediumType 识别。
    Virtual = 8    ///< 虚拟接口，例如虚拟交换机、虚拟机网卡、TAP/TUN 设备等。
};

/**
 * @brief 网络接口操作状态。
 */
enum class GB_SystemNetworkOperationalStatus : uint16_t
{
    Unknown = 0,        ///< 未知操作状态。
    Up = 1,             ///< 接口已启用并处于可运行状态。
    Down = 2,           ///< 接口未运行或链路已断开。
    Testing = 3,        ///< 接口处于测试状态。
    Dormant = 4,        ///< 接口处于休眠或等待状态，可能需要外部事件触发。
    NotPresent = 5,     ///< 接口对应硬件当前不存在。
    LowerLayerDown = 6  ///< 下层接口不可用，例如隧道依赖的底层链路已断开。
};

/**
 * @brief Windows 对网络的分层连通性判断。
 */
enum class GB_SystemNetworkConnectivityLevel : uint16_t
{
    Unknown = 0,             ///< 无法判断连通性，通常表示系统 API 不可用或查询失败。
    Disconnected = 1,        ///< 未连接到任何有效网络。
    InterfaceOnly = 2,       ///< 仅检测到网络接口层连接，尚未确认本地网络访问能力。
    LocalAccess = 3,         ///< 具备本地子网或本地网络访问能力，但未确认互联网访问能力。
    InternetAccess = 4,      ///< 系统认为具备互联网访问能力。
    CaptivePortalLikely = 5, ///< 预留状态：可能存在强制门户或登录页限制。
    Restricted = 6           ///< 预留状态：网络访问受系统策略、费用、权限或其他限制影响。
};

/**
 * @brief Windows Network List Manager 网络类别。
 */
enum class GB_SystemNetworkCategory : uint16_t
{
    Unknown = 0,             ///< 未知网络类别或当前系统未返回类别信息。
    Public = 1,              ///< 公用网络。
    Private = 2,             ///< 专用网络。
    DomainAuthenticated = 3  ///< 域认证网络。
};

/**
 * @brief Windows Network Cost Manager 主成本类型。
 */
enum class GB_SystemNetworkCostType : uint16_t
{
    Unknown = 0,      ///< 未知成本类型。
    Unrestricted = 1, ///< 不受流量或费用限制的网络。
    Fixed = 2,        ///< 固定费用网络。
    Variable = 3      ///< 按流量或用量计费的网络。
};

/**
 * @brief Native Wi-Fi 接口状态。
 */
enum class GB_SystemWifiInterfaceState : uint16_t
{
    Unknown = 0,             ///< 未知状态。
    NotReady = 1,            ///< 无线接口尚未准备好。
    Connected = 2,           ///< 无线接口已连接。
    AdHocNetworkFormed = 3,  ///< 已形成 Ad-hoc 网络。
    Disconnecting = 4,       ///< 正在断开连接。
    Disconnected = 5,        ///< 已断开连接。
    Associating = 6,         ///< 正在关联接入点。
    Discovering = 7,         ///< 正在发现网络。
    Authenticating = 8       ///< 正在认证。
};

/**
 * @brief 适合业务判断的 Wi-Fi 安全类型。
 */
enum class GB_SystemWifiSecurityType : uint16_t
{
    Unknown = 0,        ///< 未知安全类型。
    Open = 1,           ///< 开放网络，不要求认证或加密。
    Wep = 2,            ///< WEP 安全机制。
    WpaPersonal = 3,    ///< WPA 个人版。
    Wpa2Personal = 4,   ///< WPA2 个人版。
    Wpa3Personal = 5,   ///< WPA3 个人版。
    WpaEnterprise = 6,  ///< WPA 企业版。
    Wpa2Enterprise = 7, ///< WPA2 企业版。
    Wpa3Enterprise = 8, ///< WPA3 企业版。
    Owe = 9             ///< Opportunistic Wireless Encryption。
};

/**
 * @brief 网络变化事件类型。
 */
enum class GB_SystemNetworkEventType : uint16_t
{
    Unknown = 0,         ///< 未知事件。
    InitialSnapshot = 1, ///< 监听器启动后派发的初始快照事件。
    SnapshotChanged = 2, ///< 网络快照发生变化，但事件原因不是单一接口、地址或路由变化。
    InterfaceChanged = 3,///< 网络接口列表、接口状态或接口属性发生变化。
    AddressChanged = 4,  ///< IP 地址、DNS 地址或地址相关属性发生变化。
    RouteChanged = 5,    ///< 默认路由、网关或路由度量发生变化。
    WifiChanged = 6,     ///< Wi-Fi 连接、配置文件或无线状态发生变化。
    PeriodicRefresh = 7  ///< 周期性刷新事件。
};

/**
 * @brief IP 地址及其前缀。
 */
struct GB_SystemNetworkAddress
{
    GB_SystemNetworkAddressFamily family = GB_SystemNetworkAddressFamily::Unknown; ///< IP 地址族。
    std::string addressUtf8 = "";                                                 ///< 地址文本，使用 UTF-8 编码，例如 "192.168.1.2" 或 "fe80::1%12"。
    uint8_t prefixLength = 0;                                                      ///< 网络前缀长度，IPv4 通常为 0~32，IPv6 通常为 0~128。
    bool isDnsEligible = false;                                                    ///< 该地址是否适合参与 DNS 注册或 DNS 相关用途。
    bool isTransient = false;                                                      ///< 该地址是否为临时地址，例如 IPv6 隐私扩展地址。
};

/**
 * @brief 网络接口完整快照。所有 std::string 均为 UTF-8。
 */
struct GB_SystemNetworkInterfaceInfo
{
    std::string interfaceIdUtf8 = "";                                          ///< 系统适配器 ID，通常为 Windows 适配器名称/GUID 字符串。
    uint64_t interfaceLuid = 0;                                                ///< Windows 网络接口 LUID，用于稳定关联适配器、接口统计和路由记录。
    uint32_t interfaceIndex = 0;                                               ///< IPv4 接口索引。
    uint32_t ipv6InterfaceIndex = 0;                                           ///< IPv6 接口索引。
    std::string interfaceAliasUtf8 = "";                                      ///< 系统接口别名，通常是面向用户显示的适配器名称。
    std::string friendlyNameUtf8 = "";                                       ///< 友好名称；当前实现与 interfaceAliasUtf8 保持一致。
    std::string descriptionUtf8 = "";                                        ///< 适配器描述信息，通常来自设备驱动或系统设备描述。
    std::string dnsSuffixUtf8 = "";                                          ///< 接口 DNS 后缀。
    std::string macAddressUtf8 = "";                                         ///< 物理地址/MAC 地址文本，格式为十六进制冒号分隔；无物理地址时为空。
    GB_SystemNetworkInterfaceType interfaceType = GB_SystemNetworkInterfaceType::Unknown; ///< 业务归一化后的接口类型。
    GB_SystemNetworkOperationalStatus operationalStatus = GB_SystemNetworkOperationalStatus::Unknown; ///< 接口当前操作状态。
    bool isPhysical = false;                                                   ///< 是否被识别为物理接口；无物理地址或被判定为虚拟接口时为 false。
    bool isVirtual = false;                                                    ///< 是否被识别为虚拟接口、环回接口或隧道接口。
    bool dhcpV4Enabled = false;                                                ///< IPv4 DHCP 是否启用。
    bool dhcpV6Enabled = false;                                                ///< IPv6 地址自动管理或 DHCPv6 相关能力是否启用。
    bool hasIpv4Address = false;                                               ///< 是否存在可用 IPv4 单播地址；未指定、环回、多播等特殊地址不计入。
    bool hasIpv6Address = false;                                               ///< 是否存在可用 IPv6 单播地址；未指定、环回、多播等特殊地址不计入。
    bool hasDefaultGateway = false;                                            ///< 是否配置了默认网关地址。
    bool isDefaultRouteCandidate = false;                                      ///< 是否拥有 IPv4 或 IPv6 默认路由候选项。
    uint32_t mtu = 0;                                                          ///< 接口 MTU，单位为字节；无法获取时为 0。
    uint64_t transmitLinkSpeedBitsPerSecond = 0;                               ///< 发送链路速率，单位为 bit/s；无法获取时为 0。
    uint64_t receiveLinkSpeedBitsPerSecond = 0;                                ///< 接收链路速率，单位为 bit/s；无法获取时为 0。
    uint64_t receivedBytes = 0;                                                ///< 接口累计接收字节数。
    uint64_t transmittedBytes = 0;                                             ///< 接口累计发送字节数。
    uint64_t receivedPackets = 0;                                              ///< 接口累计接收包数量。
    uint64_t transmittedPackets = 0;                                           ///< 接口累计发送包数量。
    uint64_t inputErrors = 0;                                                  ///< 接口累计输入错误数量。
    uint64_t outputErrors = 0;                                                 ///< 接口累计输出错误数量。
    uint64_t inputDiscards = 0;                                                ///< 接口累计输入丢弃数量。
    uint64_t outputDiscards = 0;                                               ///< 接口累计输出丢弃数量。
    uint32_t routeMetric = 0;                                                  ///< 默认路由综合度量，数值越小优先级越高；无默认路由时为 0。
    std::vector<GB_SystemNetworkAddress> unicastAddresses;                     ///< 接口单播 IP 地址列表。
    std::vector<std::string> gatewayAddressesUtf8;                             ///< 接口网关地址列表，地址文本使用 UTF-8 编码。
    std::vector<std::string> dnsServerAddressesUtf8;                           ///< 接口 DNS 服务器地址列表，地址文本使用 UTF-8 编码。
};

/**
 * @brief NLM 网络信息。
 */
struct GB_SystemConnectedNetworkInfo
{
    std::string networkIdUtf8 = "";                                           ///< NLM 网络 ID，通常为 GUID 字符串。
    std::string nameUtf8 = "";                                                ///< NLM 网络名称。
    std::string descriptionUtf8 = "";                                         ///< NLM 网络描述。
    GB_SystemNetworkCategory category = GB_SystemNetworkCategory::Unknown;     ///< NLM 网络类别。
    GB_SystemNetworkConnectivityLevel connectivityLevel = GB_SystemNetworkConnectivityLevel::Unknown; ///< 该网络的连通性级别。
    bool isConnected = false;                                                  ///< NLM 是否认为该网络处于连接状态。
    bool hasIpv4LocalAccess = false;                                           ///< 是否具备 IPv4 本地网络访问能力。
    bool hasIpv4InternetAccess = false;                                        ///< 是否具备 IPv4 互联网访问能力。
    bool hasIpv6LocalAccess = false;                                           ///< 是否具备 IPv6 本地网络访问能力。
    bool hasIpv6InternetAccess = false;                                        ///< 是否具备 IPv6 互联网访问能力。
};

/**
 * @brief 当前主要 Internet 连接的成本信息。
 */
struct GB_SystemNetworkCostInfo
{
    GB_SystemNetworkCostType costType = GB_SystemNetworkCostType::Unknown; ///< 归一化后的主成本类型。
    bool isCostUnknown = true;                                             ///< 是否无法确定网络成本。
    bool isUnrestricted = false;                                           ///< 是否为不受限网络。
    bool isFixed = false;                                                  ///< 是否为固定费用网络。
    bool isVariable = false;                                               ///< 是否为按用量计费网络。
    bool isOverDataLimit = false;                                          ///< 是否已超过数据额度限制。
    bool isApproachingDataLimit = false;                                   ///< 是否接近数据额度限制。
    bool isCongested = false;                                              ///< 网络是否拥塞。
    bool isRoaming = false;                                                ///< 是否处于漫游状态。
    uint32_t nativeCostFlags = 0;                                          ///< Windows Network Cost Manager 返回的原始成本标志位。
};

/**
 * @brief 当前机器的网络总体快照。
 */
struct GB_SystemNetworkSnapshot
{
    bool hasNetworkInterface = false;                                      ///< 是否枚举到除环回外的网络接口。
    bool hasConnectedInterface = false;                                    ///< 是否存在 Up 状态且具有可用 IP 地址的非环回接口。
    bool hasLocalNetworkAccess = false;                                    ///< 是否具备本地网络访问能力。
    bool hasInternetAccess = false;                                        ///< 是否具备互联网访问能力。
    GB_SystemNetworkConnectivityLevel connectivityLevel = GB_SystemNetworkConnectivityLevel::Unknown; ///< 综合连通性级别。
    std::vector<std::string> activeNetworkNamesUtf8;                       ///< 当前连接中的 NLM 网络名称列表。
    std::string primaryInterfaceIdUtf8 = "";                              ///< 推断出的主接口 ID；不存在主接口时为空。
    uint64_t primaryInterfaceLuid = 0;                                     ///< 推断出的主接口 LUID；不存在主接口时为 0。
    uint32_t primaryInterfaceIndex = 0;                                    ///< 推断出的主接口 IPv4 索引；不存在主接口时为 0。
    GB_SystemNetworkInterfaceType primaryInterfaceType = GB_SystemNetworkInterfaceType::Unknown; ///< 推断出的主接口类型。
    bool hasCostInfo = false;                                              ///< 是否成功获取网络成本信息。
    GB_SystemNetworkCostInfo costInfo;                                     ///< 当前主要 Internet 连接的成本信息。
    bool isMetered = false;                                                ///< 当前网络是否属于计费或受流量额度限制的网络。
    bool isRoaming = false;                                                ///< 当前网络是否处于漫游状态。
    uint64_t timestampMilliseconds = 0;                                    ///< 快照采集时间戳，单位为毫秒。
    std::vector<GB_SystemNetworkInterfaceInfo> interfaces;                 ///< 当前接口完整列表。
    std::vector<GB_SystemConnectedNetworkInfo> connectedNetworks;          ///< 当前 NLM 已连接网络列表。
    std::string diagnosticMessageUtf8 = "";                               ///< 采集过程中的非致命诊断信息。
};

/**
 * @brief 当前 Wi-Fi 连接信息。SSID 字节不是合法 UTF-8 时 ssidUtf8 为空，ssidHexUtf8 仍可用于无损识别。
 */
struct GB_SystemWifiConnectionInfo
{
    std::string interfaceIdUtf8 = "";                                      ///< Wi-Fi 接口 ID，使用 Native Wi-Fi 接口 GUID 文本。
    std::string profileNameUtf8 = "";                                     ///< 已连接 Wi-Fi 配置文件名称。
    std::string ssidUtf8 = "";                                            ///< 已连接网络 SSID；仅当原始 SSID 是合法 UTF-8 时非空。
    std::string ssidHexUtf8 = "";                                         ///< SSID 原始字节的十六进制文本，用于无损识别非 UTF-8 SSID。
    std::string bssidUtf8 = "";                                           ///< 当前接入点 BSSID，格式为十六进制冒号分隔字符串。
    GB_SystemWifiInterfaceState interfaceState = GB_SystemWifiInterfaceState::Unknown; ///< Wi-Fi 接口状态。
    GB_SystemWifiSecurityType securityType = GB_SystemWifiSecurityType::Unknown; ///< 当前连接安全类型。
    uint32_t signalQuality = 0;                                            ///< 信号质量百分比，范围通常为 0~100。
    int32_t rssiDbm = 0;                                                   ///< 信号强度估计值，单位为 dBm。
    uint32_t receiveRateKbps = 0;                                          ///< 当前接收速率，单位为 Kbps。
    uint32_t transmitRateKbps = 0;                                         ///< 当前发送速率，单位为 Kbps。
    uint32_t phyType = 0;                                                  ///< Native Wi-Fi 返回的物理层类型原始值。
    uint32_t bssType = 0;                                                  ///< Native Wi-Fi 返回的 BSS 类型原始值。
    uint32_t authenticationAlgorithm = 0;                                  ///< Native Wi-Fi 返回的认证算法原始值。
    uint32_t cipherAlgorithm = 0;                                          ///< Native Wi-Fi 返回的加密算法原始值。
    bool isSecurityEnabled = false;                                        ///< 当前连接是否启用安全机制。
};

/**
 * @brief Wi-Fi 接口信息。
 */
struct GB_SystemWifiInterfaceInfo
{
    std::string interfaceIdUtf8 = "";                                      ///< Wi-Fi 接口 ID，使用 Native Wi-Fi 接口 GUID 文本。
    std::string descriptionUtf8 = "";                                     ///< Wi-Fi 接口描述信息。
    GB_SystemWifiInterfaceState interfaceState = GB_SystemWifiInterfaceState::Unknown; ///< Wi-Fi 接口状态。
    bool hasRadioState = false;                                            ///< 是否成功读取无线电状态。
    bool isRadioOn = false;                                                ///< 无线电是否处于开启状态；仅在 hasRadioState 为 true 时可靠。
    bool hasCurrentConnection = false;                                     ///< 是否成功读取到当前连接信息。
    GB_SystemWifiConnectionInfo currentConnection;                         ///< 当前连接信息；未连接或读取失败时保持默认值。
    std::string diagnosticMessageUtf8 = "";                               ///< Wi-Fi 接口采集过程中的非致命诊断信息。
};

/**
 * @brief Wi-Fi BSS 详细信息；仅在扫描选项显式要求时返回。
 */
struct GB_SystemWifiBssInfo
{
    std::string bssidUtf8 = "";                                            ///< BSSID/MAC 地址，格式为十六进制冒号分隔字符串。
    int32_t rssiDbm = 0;                                                    ///< 接收信号强度，单位为 dBm。
    uint32_t signalQuality = 0;                                             ///< 信号质量百分比，范围通常为 0~100。
    uint32_t centerFrequencyKhz = 0;                                        ///< 信道中心频率，单位为 KHz。
    uint32_t channelNumber = 0;                                             ///< 估算信道号，无法估算时为 0。
    uint32_t phyType = 0;                                                   ///< Native Wi-Fi 返回的物理层类型原始值。
};

/**
 * @brief 可用 Wi-Fi 网络信息。
 */
struct GB_SystemWifiNetworkInfo
{
    std::string interfaceIdUtf8 = "";                                      ///< Wi-Fi 接口 ID，使用 Native Wi-Fi 接口 GUID 文本。
    std::string profileNameUtf8 = "";                                     ///< 匹配的本机 Wi-Fi 配置文件名称；无保存配置文件时为空。
    std::string ssidUtf8 = "";                                            ///< Wi-Fi SSID；仅当原始 SSID 是合法 UTF-8 时非空。
    std::string ssidHexUtf8 = "";                                         ///< SSID 原始字节的十六进制文本，用于无损识别非 UTF-8 SSID。
    GB_SystemWifiSecurityType securityType = GB_SystemWifiSecurityType::Unknown; ///< 网络安全类型。
    uint32_t signalQuality = 0;                                            ///< 信号质量百分比，范围通常为 0~100。
    int32_t strongestRssiDbm = 0;                                          ///< includeBssDetails 为 true 且 BSS 详情可用时取真实最强 RSSI；否则由 signalQuality 按经验公式估算。
    uint32_t bssType = 0;                                                  ///< Native Wi-Fi 返回的 BSS 类型原始值。
    uint32_t authenticationAlgorithm = 0;                                  ///< Native Wi-Fi 返回的认证算法原始值。
    uint32_t cipherAlgorithm = 0;                                          ///< Native Wi-Fi 返回的加密算法原始值。
    bool isSecurityEnabled = false;                                        ///< 网络是否启用安全机制。
    bool hasSavedProfile = false;                                          ///< 本机是否保存了可用于该网络的 Wi-Fi 配置文件。
    bool isConnected = false;                                              ///< 当前接口是否已连接到该网络。
    bool isConnectable = false;                                            ///< 系统当前是否认为该网络可连接。
    uint32_t notConnectableReason = 0;                                     ///< Native Wi-Fi 返回的不可连接原因原始值；可连接时通常为 0。
    std::vector<GB_SystemWifiBssInfo> bssEntries;                          ///< 该网络对应的 BSS/接入点列表；未请求或读取失败时为空。
};

/**
 * @brief 已保存 Wi-Fi Profile 信息；不包含 Profile XML 或密码。
 */
struct GB_SystemWifiProfileInfo
{
    std::string interfaceIdUtf8 = "";                                      ///< Wi-Fi 接口 ID，使用 Native Wi-Fi 接口 GUID 文本。
    std::string profileNameUtf8 = "";                                     ///< Wi-Fi 配置文件名称。
    uint32_t nativeFlags = 0;                                               ///< Native Wi-Fi 返回的配置文件标志原始值。
};

/**
 * @brief Wi-Fi 扫描选项。
 */
struct GB_SystemWifiScanOptions
{
    bool requestFreshScan = true;                                           ///< 是否主动请求一次新的 Native Wi-Fi 扫描；false 表示只读取系统当前缓存，避免额外扫描延迟。
    bool includeBssDetails = false;                                         ///< 是否读取 BSSID/RSSI/信道等 BSS 详情；新版 Windows 可能要求精确位置权限，失败时仍会尽量返回可用网络列表。
    uint32_t timeoutMilliseconds = 5000;                                    ///< 等待扫描完成通知的最长时间，单位为毫秒；超时后改读系统当前缓存，不主动中断系统扫描。
    uint32_t cancellationPollMilliseconds = 50;                             ///< 轮询 cancellationFlag 的间隔，单位为毫秒；必须大于 0。
    const std::atomic<bool>* cancellationFlag = nullptr;                    ///< 可选取消标志；若在提交扫描前已取消，则不会触发新的系统扫描。
};

/**
 * @brief Wi-Fi 连接或断开等待选项。
 */
struct GB_SystemWifiOperationOptions
{
    uint32_t timeoutMilliseconds = 15000;                                   ///< 等待 Native Wi-Fi 结果通知的最长时间，单位为毫秒；超时只停止等待，不强制回滚系统连接请求。
    uint32_t cancellationPollMilliseconds = 50;                             ///< 轮询 cancellationFlag 的间隔，单位为毫秒；必须大于 0。
    const std::atomic<bool>* cancellationFlag = nullptr;                    ///< 可选取消标志；取消只停止等待，不保证取消已经提交给系统的 Native Wi-Fi 请求。
};

/**
 * @brief 网络变化监听器选项。
 */
struct GB_SystemNetworkWatcherOptions
{
    uint32_t debounceMilliseconds = 250;                                    ///< 防抖间隔，单位为毫秒；为 0 时关闭防抖，收到系统通知后尽快刷新快照。
    uint32_t periodicRefreshMilliseconds = 30000;                           ///< 周期性刷新间隔，单位为毫秒；为 0 时关闭定时刷新，仅响应系统通知。
    size_t maxDispatchQueueSize = 64;                                       ///< 事件分发队列最大长度；队列满时丢弃最旧事件，避免通知风暴造成无界内存增长。
    bool emitInitialSnapshot = true;                                        ///< Start 后是否立即派发一次 InitialSnapshot 事件。
    bool emitUnchangedPeriodicRefresh = true;                               ///< 定时刷新即使快照内容未变化也派发 PeriodicRefresh；false 表示定时刷新只作为兜底变更检测。
};

/**
 * @brief 网络变化事件；快照字段只包含状态信息，不包含 Wi-Fi 扫描结果。
 */
struct GB_SystemNetworkEvent
{
    GB_SystemNetworkEventType eventType = GB_SystemNetworkEventType::Unknown; ///< 事件类型。
    std::string eventName = "";                                             ///< 事件名称，格式通常为 "SystemNetwork.<事件类型>"。
    std::string sourceName = "";                                            ///< 事件来源名称，例如 "NetIO/NativeWifi"。
    uint64_t timestampMilliseconds = 0;                                      ///< 事件生成时间戳，单位为毫秒。
    bool interfacesChanged = false;                                         ///< 与上一快照相比，接口结构、接口状态或接口属性是否发生变化。
    bool addressesChanged = false;                                          ///< 与上一快照相比，IP 地址、DNS 或地址相关属性是否发生变化。
    bool routesChanged = false;                                             ///< 与上一快照相比，默认路由、网关或路由度量是否发生变化。
    bool connectivityChanged = false;                                       ///< 与上一快照相比，综合连通性、互联网访问能力或主接口是否发生变化。
    bool networkNamesChanged = false;                                       ///< 与上一快照相比，当前连接网络名称集合是否发生变化。
    bool costChanged = false;                                               ///< 与上一快照相比，网络成本或计费状态是否发生变化。
    bool wifiChanged = false;                                               ///< 是否由 Native Wi-Fi 通知触发或涉及 Wi-Fi 状态变化。
    GB_SystemNetworkSnapshot previousSnapshot;                              ///< 变化前的网络快照。
    GB_SystemNetworkSnapshot currentSnapshot;                               ///< 变化后的网络快照。
};

/**
 * @brief Windows 本机网络状态与 Wi-Fi 接入能力。
 *
 * @remarks
 * - 所有 std::string 输入输出均约定为 UTF-8；SSID 原始字节另以十六进制字段无损表达。
 * - 本类不实现 HTTP、下载、TLS、代理请求和持久网络配置修改；这些能力应由 GB_Network 或专门模块承担。
 * - Wi-Fi 扫描和当前连接信息在新版 Windows 中可能受精确位置权限限制，ERROR_ACCESS_DENIED 会保留在 GB_SystemResult 中。
 * - Wi-Fi 接口 ID 使用 Native Wi-Fi 接口 GUID；传入接口 ID 时兼容带大括号和不带大括号两种文本形式。
 */
class GLOBALBASE_PORT GB_SystemNetwork final
{
public:
    /** @brief 禁止构造实例；该类仅提供静态工具函数。 */
    GB_SystemNetwork() = delete;

    /** @brief 禁止析构实例；该类仅提供静态工具函数。 */
    ~GB_SystemNetwork() = delete;

    /** @brief 获取当前网络状态快照。@param snapshot 输出网络快照。@param forceRefresh 是否强制刷新。@param maxCacheAgeMilliseconds 可接受的缓存最大年龄，单位为毫秒；为 0 时不读取缓存。 */
    static GB_SystemResult GetSnapshot(GB_SystemNetworkSnapshot& snapshot, bool forceRefresh = false, uint64_t maxCacheAgeMilliseconds = 1000);

    /** @brief 强制刷新当前网络状态快照，不读取缓存。@param snapshot 输出网络快照。 */
    static GB_SystemResult RefreshSnapshot(GB_SystemNetworkSnapshot& snapshot);

    /** @brief 清空网络快照缓存，使下一次 GetSnapshot 重新采集系统状态。 */
    static void InvalidateSnapshotCache();

    /** @brief 枚举本机网络接口。@param interfaces 输出接口列表。 */
    static GB_SystemResult EnumerateInterfaces(std::vector<GB_SystemNetworkInterfaceInfo>& interfaces);

    /** @brief 枚举 NLM 当前已连接网络。@param networks 输出已连接网络列表。 */
    static GB_SystemResult EnumerateConnectedNetworks(std::vector<GB_SystemConnectedNetworkInfo>& networks);

    /** @brief 获取当前主要 Internet 连接的成本信息。@param costInfo 输出网络成本信息。 */
    static GB_SystemResult GetNetworkCost(GB_SystemNetworkCostInfo& costInfo);

    /** @brief 判断当前系统是否具备互联网访问能力。@param hasInternetAccess 输出判断结果。 */
    static GB_SystemResult HasInternetAccess(bool& hasInternetAccess);

    /** @brief 枚举本机 Wi-Fi 接口及其状态。@param interfaces 输出 Wi-Fi 接口列表。 */
    static GB_SystemResult EnumerateWifiInterfaces(std::vector<GB_SystemWifiInterfaceInfo>& interfaces);

    /** @brief 获取指定 Wi-Fi 接口的当前连接信息。@param interfaceIdUtf8 Wi-Fi 接口 ID。@param connectionInfo 输出连接信息。@param found 输出是否找到当前连接。 */
    static GB_SystemResult GetCurrentWifiConnection(const std::string& interfaceIdUtf8, GB_SystemWifiConnectionInfo& connectionInfo, bool& found);

    /** @brief 扫描或读取指定 Wi-Fi 接口的可用网络列表。@param interfaceIdUtf8 Wi-Fi 接口 ID。@param networks 输出可用网络列表。@param options 扫描选项。 */
    static GB_SystemResult ScanWifiNetworks(const std::string& interfaceIdUtf8, std::vector<GB_SystemWifiNetworkInfo>& networks, const GB_SystemWifiScanOptions& options = GB_SystemWifiScanOptions());

    /** @brief 枚举指定 Wi-Fi 接口已保存的配置文件。@param interfaceIdUtf8 Wi-Fi 接口 ID。@param profiles 输出配置文件列表。 */
    static GB_SystemResult EnumerateWifiProfiles(const std::string& interfaceIdUtf8, std::vector<GB_SystemWifiProfileInfo>& profiles);

    /** @brief 使用指定配置文件连接 Wi-Fi。@param interfaceIdUtf8 Wi-Fi 接口 ID。@param profileNameUtf8 配置文件名称。@param options 等待和取消选项。 */
    static GB_SystemResult ConnectWifiByProfile(const std::string& interfaceIdUtf8, const std::string& profileNameUtf8, const GB_SystemWifiOperationOptions& options = GB_SystemWifiOperationOptions());

    /** @brief 断开指定 Wi-Fi 接口的当前连接。@param interfaceIdUtf8 Wi-Fi 接口 ID。@param options 等待和取消选项。 */
    static GB_SystemResult DisconnectWifi(const std::string& interfaceIdUtf8, const GB_SystemWifiOperationOptions& options = GB_SystemWifiOperationOptions());

    /** @brief 获取地址族名称。@param family 地址族枚举值。 */
    static std::string GetAddressFamilyName(GB_SystemNetworkAddressFamily family);

    /** @brief 获取接口类型名称。@param interfaceType 接口类型枚举值。 */
    static std::string GetInterfaceTypeName(GB_SystemNetworkInterfaceType interfaceType);

    /** @brief 获取接口操作状态名称。@param status 操作状态枚举值。 */
    static std::string GetOperationalStatusName(GB_SystemNetworkOperationalStatus status);

    /** @brief 获取连通性级别名称。@param level 连通性级别枚举值。 */
    static std::string GetConnectivityLevelName(GB_SystemNetworkConnectivityLevel level);

    /** @brief 获取网络类别名称。@param category 网络类别枚举值。 */
    static std::string GetNetworkCategoryName(GB_SystemNetworkCategory category);

    /** @brief 获取网络成本类型名称。@param costType 网络成本类型枚举值。 */
    static std::string GetNetworkCostTypeName(GB_SystemNetworkCostType costType);

    /** @brief 获取 Wi-Fi 接口状态名称。@param state Wi-Fi 接口状态枚举值。 */
    static std::string GetWifiInterfaceStateName(GB_SystemWifiInterfaceState state);

    /** @brief 获取 Wi-Fi 安全类型名称。@param securityType Wi-Fi 安全类型枚举值。 */
    static std::string GetWifiSecurityTypeName(GB_SystemWifiSecurityType securityType);

    /** @brief 获取网络事件类型名称。@param eventType 网络事件类型枚举值。 */
    static std::string GetEventTypeName(GB_SystemNetworkEventType eventType);
};

/**
 * @brief Windows 网络变化监听器。
 */
class GLOBALBASE_PORT GB_SystemNetworkWatcher final
{
public:
    /** @brief 网络事件回调函数类型。 */
    using NetworkEventCallback = std::function<void(const GB_SystemNetworkEvent& event)>;

    /** @brief 使用默认选项构造网络监听器。 */
    GB_SystemNetworkWatcher();

    /** @brief 使用指定选项构造网络监听器。@param options 监听器选项。 */
    explicit GB_SystemNetworkWatcher(const GB_SystemNetworkWatcherOptions& options);

    /** @brief 析构网络监听器；析构时会停止监听线程并释放系统通知句柄。 */
    ~GB_SystemNetworkWatcher() noexcept;

    /** @brief 禁止复制构造，避免多个对象管理同一底层监听状态。 */
    GB_SystemNetworkWatcher(const GB_SystemNetworkWatcher&) = delete;

    /** @brief 禁止复制赋值，避免多个对象管理同一底层监听状态。 */
    GB_SystemNetworkWatcher& operator=(const GB_SystemNetworkWatcher&) = delete;

    /** @brief 启动网络变化监听。 */
    GB_SystemResult Start();

    /** @brief 停止网络变化监听并等待内部线程退出。 */
    GB_SystemResult Stop();

    /** @brief 判断监听器是否正在运行。 */
    bool IsRunning() const;

    /** @brief 设置网络事件回调；传入空回调可清除当前回调。@param callback 网络事件回调。 */
    void SetNetworkEventCallback(const NetworkEventCallback& callback);

    /** @brief 获取内部事件分发器，供高级调用方自行订阅事件。 */
    GB_EventDispatcher& GetEventDispatcher();

    /** @brief 获取被合并的原生系统通知数量。 */
    uint64_t GetCoalescedNativeEventCount() const;

    /** @brief 获取网络快照刷新失败次数。 */
    uint64_t GetRefreshFailureCount() const;

private:
    class Impl;                 ///< 内部实现类，用于隐藏平台相关句柄、线程、同步对象和事件分发器。
    std::unique_ptr<Impl> impl; ///< 内部实现对象所有权。
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_NETWORK_H_H
