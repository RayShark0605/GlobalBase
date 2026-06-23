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

/** @brief 网络地址族。 */
enum class GB_SystemNetworkAddressFamily : uint16_t
{
    Unknown = 0,
    IPv4 = 1,
    IPv6 = 2
};

/** @brief 网络接口类型。 */
enum class GB_SystemNetworkInterfaceType : uint16_t
{
    Unknown = 0,
    Ethernet = 1,
    Wifi = 2,
    Loopback = 3,
    Tunnel = 4,
    Ppp = 5,
    Cellular = 6,
    Bluetooth = 7,
    Virtual = 8
};

/** @brief 网络接口操作状态。 */
enum class GB_SystemNetworkOperationalStatus : uint16_t
{
    Unknown = 0,
    Up = 1,
    Down = 2,
    Testing = 3,
    Dormant = 4,
    NotPresent = 5,
    LowerLayerDown = 6
};

/** @brief Windows 对网络的分层连通性判断。 */
enum class GB_SystemNetworkConnectivityLevel : uint16_t
{
    Unknown = 0,
    Disconnected = 1,
    InterfaceOnly = 2,
    LocalAccess = 3,
    InternetAccess = 4,
    CaptivePortalLikely = 5,
    Restricted = 6
};

/** @brief Windows Network List Manager 网络类别。 */
enum class GB_SystemNetworkCategory : uint16_t
{
    Unknown = 0,
    Public = 1,
    Private = 2,
    DomainAuthenticated = 3
};

/** @brief Windows Network Cost Manager 主成本类型。 */
enum class GB_SystemNetworkCostType : uint16_t
{
    Unknown = 0,
    Unrestricted = 1,
    Fixed = 2,
    Variable = 3
};

/** @brief Native Wi-Fi 接口状态。 */
enum class GB_SystemWifiInterfaceState : uint16_t
{
    Unknown = 0,
    NotReady = 1,
    Connected = 2,
    AdHocNetworkFormed = 3,
    Disconnecting = 4,
    Disconnected = 5,
    Associating = 6,
    Discovering = 7,
    Authenticating = 8
};

/** @brief 适合业务判断的 Wi-Fi 安全类型。 */
enum class GB_SystemWifiSecurityType : uint16_t
{
    Unknown = 0,
    Open = 1,
    Wep = 2,
    WpaPersonal = 3,
    Wpa2Personal = 4,
    Wpa3Personal = 5,
    WpaEnterprise = 6,
    Wpa2Enterprise = 7,
    Wpa3Enterprise = 8,
    Owe = 9
};

/** @brief 网络变化事件类型。 */
enum class GB_SystemNetworkEventType : uint16_t
{
    Unknown = 0,
    InitialSnapshot = 1,
    SnapshotChanged = 2,
    InterfaceChanged = 3,
    AddressChanged = 4,
    RouteChanged = 5,
    WifiChanged = 6,
    PeriodicRefresh = 7
};

/** @brief IP 地址及其前缀。 */
struct GB_SystemNetworkAddress
{
    GB_SystemNetworkAddressFamily family = GB_SystemNetworkAddressFamily::Unknown;
    std::string addressUtf8 = "";
    uint8_t prefixLength = 0;
    bool isDnsEligible = false;
    bool isTransient = false;
};

/** @brief 网络接口完整快照。所有 std::string 均为 UTF-8。 */
struct GB_SystemNetworkInterfaceInfo
{
    std::string interfaceIdUtf8 = "";
    uint64_t interfaceLuid = 0;
    uint32_t interfaceIndex = 0;
    uint32_t ipv6InterfaceIndex = 0;
    std::string interfaceAliasUtf8 = "";
    std::string friendlyNameUtf8 = "";
    std::string descriptionUtf8 = "";
    std::string dnsSuffixUtf8 = "";
    std::string macAddressUtf8 = "";
    GB_SystemNetworkInterfaceType interfaceType = GB_SystemNetworkInterfaceType::Unknown;
    GB_SystemNetworkOperationalStatus operationalStatus = GB_SystemNetworkOperationalStatus::Unknown;
    bool isPhysical = false;
    bool isVirtual = false;
    bool dhcpV4Enabled = false;
    bool dhcpV6Enabled = false;
    bool hasIpv4Address = false;
    bool hasIpv6Address = false;
    bool hasDefaultGateway = false;
    bool isDefaultRouteCandidate = false;
    uint32_t mtu = 0;
    uint64_t transmitLinkSpeedBitsPerSecond = 0;
    uint64_t receiveLinkSpeedBitsPerSecond = 0;
    uint64_t receivedBytes = 0;
    uint64_t transmittedBytes = 0;
    uint64_t receivedPackets = 0;
    uint64_t transmittedPackets = 0;
    uint64_t inputErrors = 0;
    uint64_t outputErrors = 0;
    uint64_t inputDiscards = 0;
    uint64_t outputDiscards = 0;
    uint32_t routeMetric = 0;
    std::vector<GB_SystemNetworkAddress> unicastAddresses;
    std::vector<std::string> gatewayAddressesUtf8;
    std::vector<std::string> dnsServerAddressesUtf8;
};

/** @brief NLM 网络信息。 */
struct GB_SystemConnectedNetworkInfo
{
    std::string networkIdUtf8 = "";
    std::string nameUtf8 = "";
    std::string descriptionUtf8 = "";
    GB_SystemNetworkCategory category = GB_SystemNetworkCategory::Unknown;
    GB_SystemNetworkConnectivityLevel connectivityLevel = GB_SystemNetworkConnectivityLevel::Unknown;
    bool isConnected = false;
    bool hasIpv4LocalAccess = false;
    bool hasIpv4InternetAccess = false;
    bool hasIpv6LocalAccess = false;
    bool hasIpv6InternetAccess = false;
};

/** @brief 当前主要 Internet 连接的成本信息。 */
struct GB_SystemNetworkCostInfo
{
    GB_SystemNetworkCostType costType = GB_SystemNetworkCostType::Unknown;
    bool isCostUnknown = true;
    bool isUnrestricted = false;
    bool isFixed = false;
    bool isVariable = false;
    bool isOverDataLimit = false;
    bool isApproachingDataLimit = false;
    bool isCongested = false;
    bool isRoaming = false;
    uint32_t nativeCostFlags = 0;
};

/** @brief 当前机器的网络总体快照。 */
struct GB_SystemNetworkSnapshot
{
    bool hasNetworkInterface = false;
    bool hasConnectedInterface = false;
    bool hasLocalNetworkAccess = false;
    bool hasInternetAccess = false;
    GB_SystemNetworkConnectivityLevel connectivityLevel = GB_SystemNetworkConnectivityLevel::Unknown;
    std::vector<std::string> activeNetworkNamesUtf8;
    std::string primaryInterfaceIdUtf8 = "";
    uint64_t primaryInterfaceLuid = 0;
    uint32_t primaryInterfaceIndex = 0;
    GB_SystemNetworkInterfaceType primaryInterfaceType = GB_SystemNetworkInterfaceType::Unknown;
    bool hasCostInfo = false;
    GB_SystemNetworkCostInfo costInfo;
    bool isMetered = false;
    bool isRoaming = false;
    uint64_t timestampMilliseconds = 0;
    std::vector<GB_SystemNetworkInterfaceInfo> interfaces;
    std::vector<GB_SystemConnectedNetworkInfo> connectedNetworks;
    std::string diagnosticMessageUtf8 = "";
};

/** @brief 当前 Wi-Fi 连接信息。SSID 字节不是合法 UTF-8 时 ssidUtf8 为空，ssidHexUtf8 仍可用于无损识别。 */
struct GB_SystemWifiConnectionInfo
{
    std::string interfaceIdUtf8 = "";
    std::string profileNameUtf8 = "";
    std::string ssidUtf8 = "";
    std::string ssidHexUtf8 = "";
    std::string bssidUtf8 = "";
    GB_SystemWifiInterfaceState interfaceState = GB_SystemWifiInterfaceState::Unknown;
    GB_SystemWifiSecurityType securityType = GB_SystemWifiSecurityType::Unknown;
    uint32_t signalQuality = 0;
    int32_t rssiDbm = 0;
    uint32_t receiveRateKbps = 0;
    uint32_t transmitRateKbps = 0;
    uint32_t phyType = 0;
    uint32_t bssType = 0;
    uint32_t authenticationAlgorithm = 0;
    uint32_t cipherAlgorithm = 0;
    bool isSecurityEnabled = false;
};

/** @brief Wi-Fi 接口信息。 */
struct GB_SystemWifiInterfaceInfo
{
    std::string interfaceIdUtf8 = "";
    std::string descriptionUtf8 = "";
    GB_SystemWifiInterfaceState interfaceState = GB_SystemWifiInterfaceState::Unknown;
    bool hasRadioState = false;
    bool isRadioOn = false;
    bool hasCurrentConnection = false;
    GB_SystemWifiConnectionInfo currentConnection;
    std::string diagnosticMessageUtf8 = "";
};

/** @brief Wi-Fi BSS 详细信息；仅在扫描选项显式要求时返回。 */
struct GB_SystemWifiBssInfo
{
    std::string bssidUtf8 = "";
    int32_t rssiDbm = 0;
    uint32_t signalQuality = 0;
    uint32_t centerFrequencyKhz = 0;
    uint32_t channelNumber = 0;
    uint32_t phyType = 0;
};

/** @brief 可用 Wi-Fi 网络信息。 */
struct GB_SystemWifiNetworkInfo
{
    std::string interfaceIdUtf8 = "";
    std::string profileNameUtf8 = "";
    std::string ssidUtf8 = "";
    std::string ssidHexUtf8 = "";
    GB_SystemWifiSecurityType securityType = GB_SystemWifiSecurityType::Unknown;
    uint32_t signalQuality = 0;
    int32_t strongestRssiDbm = 0;
    uint32_t bssType = 0;
    uint32_t authenticationAlgorithm = 0;
    uint32_t cipherAlgorithm = 0;
    bool isSecurityEnabled = false;
    bool hasSavedProfile = false;
    bool isConnected = false;
    bool isConnectable = false;
    uint32_t notConnectableReason = 0;
    std::vector<GB_SystemWifiBssInfo> bssEntries;
};

/** @brief 已保存 Wi-Fi Profile 信息；不包含 Profile XML 或密码。 */
struct GB_SystemWifiProfileInfo
{
    std::string interfaceIdUtf8 = "";
    std::string profileNameUtf8 = "";
    uint32_t nativeFlags = 0;
};

/** @brief Wi-Fi 扫描选项。 */
struct GB_SystemWifiScanOptions
{
    bool requestFreshScan = true;
    bool includeBssDetails = false;
    uint32_t timeoutMilliseconds = 8000;
    uint32_t cancellationPollMilliseconds = 50;
    const std::atomic<bool>* cancellationFlag = nullptr;
};

/** @brief Wi-Fi 连接或断开等待选项。 */
struct GB_SystemWifiOperationOptions
{
    uint32_t timeoutMilliseconds = 15000;
    uint32_t cancellationPollMilliseconds = 50;
    const std::atomic<bool>* cancellationFlag = nullptr;
};

/** @brief 网络变化监听器选项。 */
struct GB_SystemNetworkWatcherOptions
{
    uint32_t debounceMilliseconds = 250;
    uint32_t periodicRefreshMilliseconds = 30000; ///< 为 0 时关闭定时刷新，仅响应系统通知。
    size_t maxDispatchQueueSize = 64;
    bool emitInitialSnapshot = true;
};

/** @brief 网络变化事件；快照字段只包含状态信息，不包含 Wi-Fi 扫描结果。 */
struct GB_SystemNetworkEvent
{
    GB_SystemNetworkEventType eventType = GB_SystemNetworkEventType::Unknown;
    std::string eventName = "";
    std::string sourceName = "";
    uint64_t timestampMilliseconds = 0;
    bool interfacesChanged = false;
    bool addressesChanged = false;
    bool routesChanged = false;
    bool connectivityChanged = false;
    bool networkNamesChanged = false;
    bool costChanged = false;
    bool wifiChanged = false;
    GB_SystemNetworkSnapshot previousSnapshot;
    GB_SystemNetworkSnapshot currentSnapshot;
};

/**
 * @brief Windows 本机网络状态与 Wi-Fi 接入能力。
 *
 * @remarks
 * - 所有 std::string 输入输出均约定为 UTF-8；SSID 原始字节另以十六进制字段无损表达。
 * - 本类不实现 HTTP、下载、TLS、代理请求和持久网络配置修改；这些能力应由 GB_Network 或专门模块承担。
 * - Wi-Fi 扫描和当前连接信息在新版 Windows 中可能受精确位置权限限制，ERROR_ACCESS_DENIED 会保留在 GB_SystemResult 中。
 */
class GLOBALBASE_PORT GB_SystemNetwork final
{
public:
    GB_SystemNetwork() = delete;
    ~GB_SystemNetwork() = delete;

    static GB_SystemResult GetSnapshot(GB_SystemNetworkSnapshot& snapshot, bool forceRefresh = false, uint64_t maxCacheAgeMilliseconds = 1000); ///< maxCacheAgeMilliseconds 为 0 时不读取缓存。
    static GB_SystemResult RefreshSnapshot(GB_SystemNetworkSnapshot& snapshot);
    static void InvalidateSnapshotCache();

    static GB_SystemResult EnumerateInterfaces(std::vector<GB_SystemNetworkInterfaceInfo>& interfaces);
    static GB_SystemResult EnumerateConnectedNetworks(std::vector<GB_SystemConnectedNetworkInfo>& networks);
    static GB_SystemResult GetNetworkCost(GB_SystemNetworkCostInfo& costInfo);
    static GB_SystemResult HasInternetAccess(bool& hasInternetAccess);

    static GB_SystemResult EnumerateWifiInterfaces(std::vector<GB_SystemWifiInterfaceInfo>& interfaces);
    static GB_SystemResult GetCurrentWifiConnection(const std::string& interfaceIdUtf8, GB_SystemWifiConnectionInfo& connectionInfo, bool& found);
    static GB_SystemResult ScanWifiNetworks(const std::string& interfaceIdUtf8, std::vector<GB_SystemWifiNetworkInfo>& networks, const GB_SystemWifiScanOptions& options = GB_SystemWifiScanOptions());
    static GB_SystemResult EnumerateWifiProfiles(const std::string& interfaceIdUtf8, std::vector<GB_SystemWifiProfileInfo>& profiles);
    static GB_SystemResult ConnectWifiByProfile(const std::string& interfaceIdUtf8, const std::string& profileNameUtf8, const GB_SystemWifiOperationOptions& options = GB_SystemWifiOperationOptions());
    static GB_SystemResult DisconnectWifi(const std::string& interfaceIdUtf8, const GB_SystemWifiOperationOptions& options = GB_SystemWifiOperationOptions());

    static std::string GetAddressFamilyName(GB_SystemNetworkAddressFamily family);
    static std::string GetInterfaceTypeName(GB_SystemNetworkInterfaceType interfaceType);
    static std::string GetOperationalStatusName(GB_SystemNetworkOperationalStatus status);
    static std::string GetConnectivityLevelName(GB_SystemNetworkConnectivityLevel level);
    static std::string GetNetworkCategoryName(GB_SystemNetworkCategory category);
    static std::string GetNetworkCostTypeName(GB_SystemNetworkCostType costType);
    static std::string GetWifiInterfaceStateName(GB_SystemWifiInterfaceState state);
    static std::string GetWifiSecurityTypeName(GB_SystemWifiSecurityType securityType);
    static std::string GetEventTypeName(GB_SystemNetworkEventType eventType);
};

/** @brief Windows 网络变化监听器。 */
class GLOBALBASE_PORT GB_SystemNetworkWatcher final
{
public:
    using NetworkEventCallback = std::function<void(const GB_SystemNetworkEvent& event)>;

    GB_SystemNetworkWatcher();
    explicit GB_SystemNetworkWatcher(const GB_SystemNetworkWatcherOptions& options);
    ~GB_SystemNetworkWatcher() noexcept;

    GB_SystemNetworkWatcher(const GB_SystemNetworkWatcher&) = delete;
    GB_SystemNetworkWatcher& operator=(const GB_SystemNetworkWatcher&) = delete;

    GB_SystemResult Start();
    GB_SystemResult Stop();
    bool IsRunning() const;
    void SetNetworkEventCallback(const NetworkEventCallback& callback);
    GB_EventDispatcher& GetEventDispatcher();
    uint64_t GetCoalescedNativeEventCount() const;
    uint64_t GetRefreshFailureCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_NETWORK_H_H
