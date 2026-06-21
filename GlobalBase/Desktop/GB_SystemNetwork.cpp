#include "GB_SystemNetwork.h"

#include "../GB_Utf8String.h"
#include "GB_ComScope.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <netlistmgr.h>
#include <wlanapi.h>
#include <wrl/client.h>
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Wlanapi.lib")
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace
{
const uint64_t DefaultSnapshotCacheAgeMilliseconds = 1000;

std::mutex snapshotCacheMutex;
std::mutex snapshotRefreshMutex;
GB_SystemNetworkSnapshot cachedSnapshot;
bool hasCachedSnapshot = false;

GB_SystemResult MakeUnsupportedPlatformResult(const std::string &operationName)
{
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, "当前平台不支持 Windows 网络能力。");
}

void AppendDiagnostic(std::string &diagnosticMessage, const std::string &message)
{
    if (message.empty())
    {
        return;
    }
    if (!diagnosticMessage.empty())
    {
        diagnosticMessage += " ";
    }
    diagnosticMessage += message;
}

template <typename ValueType>
void AppendUnique(std::vector<ValueType> &values, const ValueType &value)
{
    if (std::find(values.begin(), values.end(), value) == values.end())
    {
        values.push_back(value);
    }
}

std::string ToLowerAscii(const std::string &value)
{
    std::string result = value;
    for (size_t index = 0; index < result.size(); index++)
    {
        const unsigned char character = static_cast<unsigned char>(result[index]);
        if (character >= 'A' && character <= 'Z')
        {
            result[index] = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

bool LooksVirtual(const std::string &friendlyName, const std::string &description)
{
    const std::string text = ToLowerAscii(friendlyName + " " + description);
    static const char *const virtualMarkers[] = {"virtual", "hyper-v", "vmware", "vbox", "tap-", "tap ", "tun ", "wireguard", "loopback", "npcap"};
    for (size_t index = 0; index < sizeof(virtualMarkers) / sizeof(virtualMarkers[0]); index++)
    {
        if (text.find(virtualMarkers[index]) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

uint64_t ElapsedMilliseconds(const uint64_t newerTimestamp, const uint64_t olderTimestamp)
{
    return newerTimestamp >= olderTimestamp ? newerTimestamp - olderTimestamp : std::numeric_limits<uint64_t>::max();
}

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

class WlanHandleScope final
{
  public:
    ~WlanHandleScope() noexcept
    {
        Reset();
    }

    WlanHandleScope(const WlanHandleScope &) = delete;
    WlanHandleScope &operator=(const WlanHandleScope &) = delete;
    WlanHandleScope() = default;

    void Reset(HANDLE inputHandle = nullptr) noexcept
    {
        if (handle != nullptr)
        {
            (void)::WlanCloseHandle(handle, nullptr);
        }
        handle = inputHandle;
    }

    HANDLE Get() const noexcept
    {
        return handle;
    }

  private:
    HANDLE handle = nullptr;
};

template <typename ValueType>
class WlanMemoryScope final
{
  public:
    ~WlanMemoryScope() noexcept
    {
        Reset();
    }

    WlanMemoryScope(const WlanMemoryScope &) = delete;
    WlanMemoryScope &operator=(const WlanMemoryScope &) = delete;
    WlanMemoryScope() = default;

    void Reset(ValueType *inputValue = nullptr) noexcept
    {
        if (value != nullptr)
        {
            ::WlanFreeMemory(value);
        }
        value = inputValue;
    }

    ValueType *Get() const noexcept
    {
        return value;
    }

  private:
    ValueType *value = nullptr;
};

class BstrScope final
{
  public:
    ~BstrScope() noexcept
    {
        Reset();
    }

    BstrScope(const BstrScope &) = delete;
    BstrScope &operator=(const BstrScope &) = delete;
    BstrScope() = default;

    void Reset(BSTR inputValue = nullptr) noexcept
    {
        if (value != nullptr)
        {
            ::SysFreeString(value);
        }
        value = inputValue;
    }

    BSTR *Address() noexcept
    {
        Reset();
        return &value;
    }

    std::string ToUtf8() const
    {
        return value == nullptr ? std::string() : GB_WStringToUtf8(std::wstring(value, ::SysStringLen(value)));
    }

  private:
    BSTR value = nullptr;
};

class MibTableScope final
{
  public:
    ~MibTableScope() noexcept
    {
        Reset();
    }

    MibTableScope(const MibTableScope &) = delete;
    MibTableScope &operator=(const MibTableScope &) = delete;
    MibTableScope() = default;

    void Reset(void *inputTable = nullptr) noexcept
    {
        if (table != nullptr)
        {
            ::FreeMibTable(table);
        }
        table = inputTable;
    }

  private:
    void *table = nullptr;
};

GB_SystemResult OpenWlanHandle(WlanHandleScope &wlanHandle)
{
    DWORD negotiatedVersion = 0;
    HANDLE rawHandle = nullptr;
    const DWORD errorCode = ::WlanOpenHandle(2, nullptr, &negotiatedVersion, &rawHandle);
    if (errorCode != ERROR_SUCCESS)
    {
        GB_SystemResult result = GB_SystemResult::FromWin32Error(errorCode, "WlanOpenHandle", "打开 Native Wi-Fi 客户端句柄失败。");
        if (errorCode == ERROR_SERVICE_NOT_ACTIVE)
        {
            result.errorCode = GB_SystemErrorCode::InvalidState;
            result.message = "WLAN AutoConfig 服务未运行，Native Wi-Fi 能力不可用。";
        }
        return result;
    }
    wlanHandle.Reset(rawHandle);
    return GB_SystemResult::Succeeded("WlanOpenHandle");
}

std::string GuidToUtf8(const GUID &guid)
{
    wchar_t buffer[64] = {};
    const int length = ::StringFromGUID2(guid, buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
    return length <= 1 ? std::string() : GB_WStringToUtf8(std::wstring(buffer, static_cast<size_t>(length - 1)));
}

bool TryParseGuid(const std::string &textUtf8, GUID &guid)
{
    if (textUtf8.empty())
    {
        return false;
    }
    const std::wstring textWide = GB_Utf8ToWString(textUtf8);
    return SUCCEEDED(::CLSIDFromString(const_cast<wchar_t *>(textWide.c_str()), &guid));
}

std::string SockaddrToUtf8(const SOCKADDR *address)
{
    if (address == nullptr)
    {
        return std::string();
    }

    wchar_t buffer[INET6_ADDRSTRLEN + 32] = {};
    if (address->sa_family == AF_INET)
    {
        const SOCKADDR_IN *ipv4Address = reinterpret_cast<const SOCKADDR_IN *>(address);
        if (::InetNtopW(AF_INET, &ipv4Address->sin_addr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]))) == nullptr)
        {
            return std::string();
        }
    }
    else if (address->sa_family == AF_INET6)
    {
        const SOCKADDR_IN6 *ipv6Address = reinterpret_cast<const SOCKADDR_IN6 *>(address);
        if (::InetNtopW(AF_INET6, &ipv6Address->sin6_addr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]))) == nullptr)
        {
            return std::string();
        }
        if (ipv6Address->sin6_scope_id != 0)
        {
            const std::wstring scopedAddress = std::wstring(buffer) + L"%" + std::to_wstring(ipv6Address->sin6_scope_id);
            return GB_WStringToUtf8(scopedAddress);
        }
    }
    else
    {
        return std::string();
    }
    return GB_WStringToUtf8(std::wstring(buffer));
}

std::string BytesToHex(const unsigned char *bytes, const size_t byteCount, const char separator)
{
    static const char HexDigits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(byteCount * 3);
    for (size_t index = 0; index < byteCount; index++)
    {
        if (index != 0 && separator != '\0')
        {
            result.push_back(separator);
        }
        result.push_back(HexDigits[(bytes[index] >> 4) & 0x0F]);
        result.push_back(HexDigits[bytes[index] & 0x0F]);
    }
    return result;
}

std::string SsidToHex(const DOT11_SSID &ssid)
{
    const size_t length = (std::min)(static_cast<size_t>(ssid.uSSIDLength), sizeof(ssid.ucSSID));
    return BytesToHex(ssid.ucSSID, length, '\0');
}

std::string SsidToUtf8(const DOT11_SSID &ssid)
{
    const size_t length = (std::min)(static_cast<size_t>(ssid.uSSIDLength), sizeof(ssid.ucSSID));
    const std::string raw(reinterpret_cast<const char *>(ssid.ucSSID), length);
    return GB_IsUtf8(raw) ? raw : std::string();
}

GB_SystemNetworkInterfaceType MapInterfaceType(const ULONG interfaceType, const bool looksVirtual)
{
    if (looksVirtual)
    {
        return GB_SystemNetworkInterfaceType::Virtual;
    }
    switch (interfaceType)
    {
    case IF_TYPE_ETHERNET_CSMACD:
    case IF_TYPE_ISO88025_TOKENRING:
        return GB_SystemNetworkInterfaceType::Ethernet;
    case IF_TYPE_IEEE80211:
        return GB_SystemNetworkInterfaceType::Wifi;
    case IF_TYPE_SOFTWARE_LOOPBACK:
        return GB_SystemNetworkInterfaceType::Loopback;
    case IF_TYPE_TUNNEL:
        return GB_SystemNetworkInterfaceType::Tunnel;
    case IF_TYPE_PPP:
        return GB_SystemNetworkInterfaceType::Ppp;
#ifdef IF_TYPE_WWANPP
    case IF_TYPE_WWANPP:
#endif
#ifdef IF_TYPE_WWANPP2
    case IF_TYPE_WWANPP2:
#endif
        return GB_SystemNetworkInterfaceType::Cellular;
    default:
        return GB_SystemNetworkInterfaceType::Unknown;
    }
}

GB_SystemNetworkOperationalStatus MapOperationalStatus(const IF_OPER_STATUS status)
{
    switch (status)
    {
    case IfOperStatusUp:
        return GB_SystemNetworkOperationalStatus::Up;
    case IfOperStatusDown:
        return GB_SystemNetworkOperationalStatus::Down;
    case IfOperStatusTesting:
        return GB_SystemNetworkOperationalStatus::Testing;
    case IfOperStatusDormant:
        return GB_SystemNetworkOperationalStatus::Dormant;
    case IfOperStatusNotPresent:
        return GB_SystemNetworkOperationalStatus::NotPresent;
    case IfOperStatusLowerLayerDown:
        return GB_SystemNetworkOperationalStatus::LowerLayerDown;
    default:
        return GB_SystemNetworkOperationalStatus::Unknown;
    }
}

GB_SystemNetworkConnectivityLevel MapConnectivity(const NLM_CONNECTIVITY connectivity)
{
    const DWORD value = static_cast<DWORD>(connectivity);
    if ((value & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0)
    {
        return GB_SystemNetworkConnectivityLevel::InternetAccess;
    }
    if ((value & (NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_LOCALNETWORK | NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV6_SUBNET)) != 0)
    {
        return GB_SystemNetworkConnectivityLevel::LocalAccess;
    }
    return value == NLM_CONNECTIVITY_DISCONNECTED ? GB_SystemNetworkConnectivityLevel::Disconnected : GB_SystemNetworkConnectivityLevel::InterfaceOnly;
}

GB_SystemNetworkCategory MapNetworkCategory(const NLM_NETWORK_CATEGORY category)
{
    switch (category)
    {
    case NLM_NETWORK_CATEGORY_PUBLIC:
        return GB_SystemNetworkCategory::Public;
    case NLM_NETWORK_CATEGORY_PRIVATE:
        return GB_SystemNetworkCategory::Private;
    case NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED:
        return GB_SystemNetworkCategory::DomainAuthenticated;
    default:
        return GB_SystemNetworkCategory::Unknown;
    }
}

GB_SystemWifiInterfaceState MapWifiInterfaceState(const WLAN_INTERFACE_STATE state)
{
    switch (state)
    {
    case wlan_interface_state_not_ready:
        return GB_SystemWifiInterfaceState::NotReady;
    case wlan_interface_state_connected:
        return GB_SystemWifiInterfaceState::Connected;
    case wlan_interface_state_ad_hoc_network_formed:
        return GB_SystemWifiInterfaceState::AdHocNetworkFormed;
    case wlan_interface_state_disconnecting:
        return GB_SystemWifiInterfaceState::Disconnecting;
    case wlan_interface_state_disconnected:
        return GB_SystemWifiInterfaceState::Disconnected;
    case wlan_interface_state_associating:
        return GB_SystemWifiInterfaceState::Associating;
    case wlan_interface_state_discovering:
        return GB_SystemWifiInterfaceState::Discovering;
    case wlan_interface_state_authenticating:
        return GB_SystemWifiInterfaceState::Authenticating;
    default:
        return GB_SystemWifiInterfaceState::Unknown;
    }
}

GB_SystemWifiSecurityType MapWifiSecurity(const DOT11_AUTH_ALGORITHM authentication, const DOT11_CIPHER_ALGORITHM cipher)
{
    if (cipher == DOT11_CIPHER_ALGO_WEP || cipher == DOT11_CIPHER_ALGO_WEP40 || cipher == DOT11_CIPHER_ALGO_WEP104)
    {
        return GB_SystemWifiSecurityType::Wep;
    }
    if (authentication == DOT11_AUTH_ALGO_80211_OPEN && cipher == DOT11_CIPHER_ALGO_NONE)
    {
        return GB_SystemWifiSecurityType::Open;
    }
    if (authentication == DOT11_AUTH_ALGO_WPA_PSK)
    {
        return GB_SystemWifiSecurityType::WpaPersonal;
    }
    if (authentication == DOT11_AUTH_ALGO_WPA)
    {
        return GB_SystemWifiSecurityType::WpaEnterprise;
    }
    if (authentication == DOT11_AUTH_ALGO_RSNA_PSK)
    {
        return GB_SystemWifiSecurityType::Wpa2Personal;
    }
    if (authentication == DOT11_AUTH_ALGO_RSNA)
    {
        return GB_SystemWifiSecurityType::Wpa2Enterprise;
    }
    return GB_SystemWifiSecurityType::Unknown;
}

GB_SystemResult EnumerateInterfacesInternal(std::vector<GB_SystemNetworkInterfaceInfo> &interfaces)
{
    interfaces.clear();
    const ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_ALL_INTERFACES;
    ULONG bufferSize = 15 * 1024;
    std::vector<unsigned char> buffer(bufferSize);
    ULONG errorCode = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4; attempt++)
    {
        IP_ADAPTER_ADDRESSES *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
        errorCode = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &bufferSize);
        if (errorCode != ERROR_BUFFER_OVERFLOW)
        {
            break;
        }
        if (bufferSize == 0 || bufferSize > 64 * 1024 * 1024)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GetAdaptersAddresses", "系统请求的网卡枚举缓冲区大小不合理。");
        }
        buffer.resize(bufferSize);
    }
    if (errorCode != ERROR_SUCCESS)
    {
        return GB_SystemResult::FromWin32Error(errorCode, "GetAdaptersAddresses", "枚举本机网络接口失败。");
    }

    const IP_ADAPTER_ADDRESSES *adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES *>(buffer.data());
    for (; adapter != nullptr; adapter = adapter->Next)
    {
        GB_SystemNetworkInterfaceInfo info;
        info.interfaceIdUtf8 = adapter->AdapterName == nullptr ? std::string() : std::string(adapter->AdapterName);
        info.interfaceLuid = adapter->Luid.Value;
        info.interfaceIndex = adapter->IfIndex;
        info.ipv6InterfaceIndex = adapter->Ipv6IfIndex;
        info.interfaceAliasUtf8 = adapter->FriendlyName == nullptr ? std::string() : GB_WStringToUtf8(adapter->FriendlyName);
        info.friendlyNameUtf8 = info.interfaceAliasUtf8;
        info.descriptionUtf8 = adapter->Description == nullptr ? std::string() : GB_WStringToUtf8(adapter->Description);
        info.dnsSuffixUtf8 = adapter->DnsSuffix == nullptr ? std::string() : GB_WStringToUtf8(adapter->DnsSuffix);
        info.operationalStatus = MapOperationalStatus(adapter->OperStatus);
        info.isVirtual = LooksVirtual(info.friendlyNameUtf8, info.descriptionUtf8) || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType == IF_TYPE_TUNNEL;
        info.interfaceType = MapInterfaceType(adapter->IfType, info.isVirtual);
        info.isPhysical = adapter->PhysicalAddressLength != 0 && !info.isVirtual;
        info.macAddressUtf8 = BytesToHex(adapter->PhysicalAddress, adapter->PhysicalAddressLength, ':');
        info.dhcpV4Enabled = (adapter->Flags & IP_ADAPTER_DHCP_ENABLED) != 0;
        info.dhcpV6Enabled = adapter->Dhcpv6ClientDuidLength != 0;
        info.mtu = adapter->Mtu;
        info.transmitLinkSpeedBitsPerSecond = adapter->TransmitLinkSpeed;
        info.receiveLinkSpeedBitsPerSecond = adapter->ReceiveLinkSpeed;

        MIB_IF_ROW2 interfaceRow = {};
        interfaceRow.InterfaceLuid = adapter->Luid;
        if (::GetIfEntry2(&interfaceRow) == NO_ERROR)
        {
            info.mtu = interfaceRow.Mtu;
            info.transmitLinkSpeedBitsPerSecond = interfaceRow.TransmitLinkSpeed;
            info.receiveLinkSpeedBitsPerSecond = interfaceRow.ReceiveLinkSpeed;
            info.receivedBytes = interfaceRow.InOctets;
            info.transmittedBytes = interfaceRow.OutOctets;
            info.receivedPackets = interfaceRow.InUcastPkts + interfaceRow.InNUcastPkts;
            info.transmittedPackets = interfaceRow.OutUcastPkts + interfaceRow.OutNUcastPkts;
            info.inputErrors = interfaceRow.InErrors;
            info.outputErrors = interfaceRow.OutErrors;
            info.inputDiscards = interfaceRow.InDiscards;
            info.outputDiscards = interfaceRow.OutDiscards;
        }

        for (const IP_ADAPTER_UNICAST_ADDRESS *address = adapter->FirstUnicastAddress; address != nullptr; address = address->Next)
        {
            GB_SystemNetworkAddress networkAddress;
            networkAddress.addressUtf8 = SockaddrToUtf8(address->Address.lpSockaddr);
            networkAddress.prefixLength = address->OnLinkPrefixLength;
            networkAddress.family = address->Address.lpSockaddr != nullptr && address->Address.lpSockaddr->sa_family == AF_INET ? GB_SystemNetworkAddressFamily::IPv4 : (address->Address.lpSockaddr != nullptr && address->Address.lpSockaddr->sa_family == AF_INET6 ? GB_SystemNetworkAddressFamily::IPv6 : GB_SystemNetworkAddressFamily::Unknown);
            networkAddress.isDnsEligible = (address->Flags & IP_ADAPTER_ADDRESS_DNS_ELIGIBLE) != 0;
            networkAddress.isTransient = (address->Flags & IP_ADAPTER_ADDRESS_TRANSIENT) != 0;
            if (!networkAddress.addressUtf8.empty())
            {
                info.unicastAddresses.push_back(networkAddress);
                info.hasIpv4Address = info.hasIpv4Address || networkAddress.family == GB_SystemNetworkAddressFamily::IPv4;
                info.hasIpv6Address = info.hasIpv6Address || networkAddress.family == GB_SystemNetworkAddressFamily::IPv6;
            }
        }
        for (const IP_ADAPTER_GATEWAY_ADDRESS_LH *gateway = adapter->FirstGatewayAddress; gateway != nullptr; gateway = gateway->Next)
        {
            const std::string addressUtf8 = SockaddrToUtf8(gateway->Address.lpSockaddr);
            if (!addressUtf8.empty())
            {
                AppendUnique(info.gatewayAddressesUtf8, addressUtf8);
            }
        }
        for (const IP_ADAPTER_DNS_SERVER_ADDRESS_XP *dnsServer = adapter->FirstDnsServerAddress; dnsServer != nullptr; dnsServer = dnsServer->Next)
        {
            const std::string addressUtf8 = SockaddrToUtf8(dnsServer->Address.lpSockaddr);
            if (!addressUtf8.empty())
            {
                AppendUnique(info.dnsServerAddressesUtf8, addressUtf8);
            }
        }
        info.hasDefaultGateway = !info.gatewayAddressesUtf8.empty();
        std::sort(info.unicastAddresses.begin(), info.unicastAddresses.end(), [](const GB_SystemNetworkAddress &left, const GB_SystemNetworkAddress &right)
                  {
                    if (left.family != right.family)
                    {
                        return static_cast<uint16_t>(left.family) < static_cast<uint16_t>(right.family);
                    }
                    if (left.addressUtf8 != right.addressUtf8)
                    {
                        return left.addressUtf8 < right.addressUtf8;
                    }
                    return left.prefixLength < right.prefixLength; });
        std::sort(info.gatewayAddressesUtf8.begin(), info.gatewayAddressesUtf8.end());
        std::sort(info.dnsServerAddressesUtf8.begin(), info.dnsServerAddressesUtf8.end());
        interfaces.push_back(info);
    }

    std::unordered_map<uint64_t, size_t> interfaceIndexesByLuid;
    interfaceIndexesByLuid.reserve(interfaces.size());
    for (size_t interfaceIndex = 0; interfaceIndex < interfaces.size(); interfaceIndex++)
    {
        interfaceIndexesByLuid[interfaces[interfaceIndex].interfaceLuid] = interfaceIndex;
    }

    PMIB_IPFORWARD_TABLE2 routeTable = nullptr;
    if (::GetIpForwardTable2(AF_UNSPEC, &routeTable) == NO_ERROR && routeTable != nullptr)
    {
        MibTableScope routeTableScope;
        routeTableScope.Reset(routeTable);
        for (ULONG routeIndex = 0; routeIndex < routeTable->NumEntries; routeIndex++)
        {
            const MIB_IPFORWARD_ROW2 &route = routeTable->Table[routeIndex];
            if (route.DestinationPrefix.PrefixLength != 0)
            {
                continue;
            }
            const std::unordered_map<uint64_t, size_t>::const_iterator indexIter = interfaceIndexesByLuid.find(route.InterfaceLuid.Value);
            if (indexIter == interfaceIndexesByLuid.end())
            {
                continue;
            }
            GB_SystemNetworkInterfaceInfo &interfaceInfo = interfaces[indexIter->second];
            MIB_IPINTERFACE_ROW ipInterface = {};
            ipInterface.Family = route.DestinationPrefix.Prefix.si_family;
            ipInterface.InterfaceLuid = route.InterfaceLuid;
            ipInterface.InterfaceIndex = route.InterfaceIndex;
            const ULONG interfaceMetric = ::GetIpInterfaceEntry(&ipInterface) == NO_ERROR ? ipInterface.Metric : 0;
            const uint64_t combinedMetric = static_cast<uint64_t>(route.Metric) + interfaceMetric;
            const uint32_t boundedMetric = combinedMetric > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(combinedMetric);
            const bool wasDefaultRouteCandidate = interfaceInfo.isDefaultRouteCandidate;
            interfaceInfo.isDefaultRouteCandidate = true;
            if (!wasDefaultRouteCandidate || boundedMetric < interfaceInfo.routeMetric)
            {
                interfaceInfo.routeMetric = boundedMetric;
            }
        }
    }

        std::sort(interfaces.begin(), interfaces.end(), [](const GB_SystemNetworkInterfaceInfo &left, const GB_SystemNetworkInterfaceInfo &right)
            {
                return left.interfaceIdUtf8 < right.interfaceIdUtf8;
            });
    return GB_SystemResult::Succeeded("GB_SystemNetwork::EnumerateInterfaces");
}

bool IsComUsable(const GB_ComScope &comScope)
{
    return comScope.IsInitialized() || static_cast<uint32_t>(comScope.GetInitializeHResult()) == 0x80010106u;
}

GB_SystemResult EnumerateConnectedNetworksInternal(std::vector<GB_SystemConnectedNetworkInfo> &networks)
{
    networks.clear();
    GB_ComScope comScope = GB_ComScope::InitializeMta("GB_SystemNetwork::EnumerateConnectedNetworks");
    if (!IsComUsable(comScope))
    {
        return comScope.GetInitializeResult();
    }

    ComPtr<INetworkListManager> manager;
    HRESULT hresult = ::CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(manager.GetAddressOf()));
    if (FAILED(hresult))
    {
        return GB_SystemResult::FromComHResult(static_cast<int32_t>(hresult), "CoCreateInstance(CLSID_NetworkListManager)", "创建 Network List Manager 失败。");
    }
    ComPtr<IEnumNetworks> enumerator;
    hresult = manager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, enumerator.GetAddressOf());
    if (FAILED(hresult))
    {
        return GB_SystemResult::FromComHResult(static_cast<int32_t>(hresult), "INetworkListManager::GetNetworks", "枚举已连接网络失败。");
    }

    for (;;)
    {
        ComPtr<INetwork> network;
        ULONG fetched = 0;
        hresult = enumerator->Next(1, network.GetAddressOf(), &fetched);
        if (hresult == S_FALSE || fetched == 0)
        {
            break;
        }
        if (FAILED(hresult))
        {
            return GB_SystemResult::FromComHResult(static_cast<int32_t>(hresult), "IEnumNetworks::Next", "读取已连接网络失败。");
        }

        GB_SystemConnectedNetworkInfo info;
        GUID networkId = {};
        if (SUCCEEDED(network->GetNetworkId(&networkId)))
        {
            info.networkIdUtf8 = GuidToUtf8(networkId);
        }
        BstrScope name;
        if (SUCCEEDED(network->GetName(name.Address())))
        {
            info.nameUtf8 = name.ToUtf8();
        }
        BstrScope description;
        if (SUCCEEDED(network->GetDescription(description.Address())))
        {
            info.descriptionUtf8 = description.ToUtf8();
        }
        NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
        if (SUCCEEDED(network->GetCategory(&category)))
        {
            info.category = MapNetworkCategory(category);
        }
        NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
        if (SUCCEEDED(network->GetConnectivity(&connectivity)))
        {
            const DWORD value = static_cast<DWORD>(connectivity);
            info.connectivityLevel = MapConnectivity(connectivity);
            info.hasIpv4LocalAccess = (value & (NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV4_LOCALNETWORK)) != 0;
            info.hasIpv4InternetAccess = (value & NLM_CONNECTIVITY_IPV4_INTERNET) != 0;
            info.hasIpv6LocalAccess = (value & (NLM_CONNECTIVITY_IPV6_SUBNET | NLM_CONNECTIVITY_IPV6_LOCALNETWORK)) != 0;
            info.hasIpv6InternetAccess = (value & NLM_CONNECTIVITY_IPV6_INTERNET) != 0;
        }
        VARIANT_BOOL connected = VARIANT_FALSE;
        if (SUCCEEDED(network->get_IsConnected(&connected)))
        {
            info.isConnected = connected == VARIANT_TRUE;
        }
        networks.push_back(info);
    }

        std::sort(networks.begin(), networks.end(), [](const GB_SystemConnectedNetworkInfo &left, const GB_SystemConnectedNetworkInfo &right)
            {
                return left.networkIdUtf8 < right.networkIdUtf8;
            });
    return GB_SystemResult::Succeeded("GB_SystemNetwork::EnumerateConnectedNetworks");
}

GB_SystemResult GetNetworkCostInternal(GB_SystemNetworkCostInfo &costInfo)
{
    costInfo = GB_SystemNetworkCostInfo();
    GB_ComScope comScope = GB_ComScope::InitializeMta("GB_SystemNetwork::GetNetworkCost");
    if (!IsComUsable(comScope))
    {
        return comScope.GetInitializeResult();
    }

    ComPtr<INetworkCostManager> costManager;
    const HRESULT createResult = ::CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(costManager.GetAddressOf()));
    if (FAILED(createResult))
    {
        return GB_SystemResult::FromComHResult(static_cast<int32_t>(createResult), "CoCreateInstance(INetworkCostManager)", "创建 Network Cost Manager 失败。");
    }
    DWORD cost = NLM_CONNECTION_COST_UNKNOWN;
    const HRESULT costResult = costManager->GetCost(&cost, nullptr);
    if (FAILED(costResult))
    {
        return GB_SystemResult::FromComHResult(static_cast<int32_t>(costResult), "INetworkCostManager::GetCost", "读取当前网络成本失败。");
    }

    costInfo.nativeCostFlags = cost;
    costInfo.isUnrestricted = (cost & NLM_CONNECTION_COST_UNRESTRICTED) != 0;
    costInfo.isFixed = (cost & NLM_CONNECTION_COST_FIXED) != 0;
    costInfo.isVariable = (cost & NLM_CONNECTION_COST_VARIABLE) != 0;
    costInfo.isCostUnknown = !costInfo.isUnrestricted && !costInfo.isFixed && !costInfo.isVariable;
    costInfo.isOverDataLimit = (cost & NLM_CONNECTION_COST_OVERDATALIMIT) != 0;
    costInfo.isApproachingDataLimit = (cost & NLM_CONNECTION_COST_APPROACHINGDATALIMIT) != 0;
    costInfo.isCongested = (cost & NLM_CONNECTION_COST_CONGESTED) != 0;
    costInfo.isRoaming = (cost & NLM_CONNECTION_COST_ROAMING) != 0;
    costInfo.costType = costInfo.isUnrestricted ? GB_SystemNetworkCostType::Unrestricted : (costInfo.isFixed ? GB_SystemNetworkCostType::Fixed : (costInfo.isVariable ? GB_SystemNetworkCostType::Variable : GB_SystemNetworkCostType::Unknown));
    return GB_SystemResult::Succeeded("GB_SystemNetwork::GetNetworkCost");
}

GB_SystemResult BuildSnapshot(GB_SystemNetworkSnapshot &snapshot)
{
    snapshot = GB_SystemNetworkSnapshot();
    GB_SystemResult result = EnumerateInterfacesInternal(snapshot.interfaces);
    if (result.IsFailed())
    {
        return result.WithOperationName("GB_SystemNetwork::RefreshSnapshot");
    }

    const GB_SystemResult networksResult = EnumerateConnectedNetworksInternal(snapshot.connectedNetworks);
    if (networksResult.IsFailed())
    {
        AppendDiagnostic(snapshot.diagnosticMessageUtf8, "NLM 查询失败：" + networksResult.GetDisplayMessage());
    }
    const GB_SystemResult costResult = GetNetworkCostInternal(snapshot.costInfo);
    snapshot.hasCostInfo = costResult.IsSucceeded();
    if (costResult.IsFailed())
    {
        AppendDiagnostic(snapshot.diagnosticMessageUtf8, "网络成本查询失败：" + costResult.GetDisplayMessage());
    }

    uint32_t bestMetric = std::numeric_limits<uint32_t>::max();
    for (size_t index = 0; index < snapshot.interfaces.size(); index++)
    {
        const GB_SystemNetworkInterfaceInfo &info = snapshot.interfaces[index];
        const bool relevantInterface = info.interfaceType != GB_SystemNetworkInterfaceType::Loopback;
        snapshot.hasNetworkInterface = snapshot.hasNetworkInterface || relevantInterface;
        const bool connected = relevantInterface && info.operationalStatus == GB_SystemNetworkOperationalStatus::Up && (info.hasIpv4Address || info.hasIpv6Address);
        snapshot.hasConnectedInterface = snapshot.hasConnectedInterface || connected;
        if (connected && info.isDefaultRouteCandidate && info.routeMetric < bestMetric)
        {
            bestMetric = info.routeMetric;
            snapshot.primaryInterfaceIdUtf8 = info.interfaceIdUtf8;
            snapshot.primaryInterfaceLuid = info.interfaceLuid;
            snapshot.primaryInterfaceIndex = info.interfaceIndex;
            snapshot.primaryInterfaceType = info.interfaceType;
        }
    }
    for (size_t index = 0; index < snapshot.connectedNetworks.size(); index++)
    {
        const GB_SystemConnectedNetworkInfo &network = snapshot.connectedNetworks[index];
        if (!network.nameUtf8.empty())
        {
            AppendUnique(snapshot.activeNetworkNamesUtf8, network.nameUtf8);
        }
        snapshot.hasLocalNetworkAccess = snapshot.hasLocalNetworkAccess || network.hasIpv4LocalAccess || network.hasIpv6LocalAccess || network.hasIpv4InternetAccess || network.hasIpv6InternetAccess;
        snapshot.hasInternetAccess = snapshot.hasInternetAccess || network.hasIpv4InternetAccess || network.hasIpv6InternetAccess;
    }
    if (networksResult.IsFailed())
    {
        snapshot.hasLocalNetworkAccess = snapshot.hasConnectedInterface;
    }
    snapshot.connectivityLevel = snapshot.hasInternetAccess ? GB_SystemNetworkConnectivityLevel::InternetAccess : (snapshot.hasLocalNetworkAccess ? GB_SystemNetworkConnectivityLevel::LocalAccess : (snapshot.hasConnectedInterface ? GB_SystemNetworkConnectivityLevel::InterfaceOnly : GB_SystemNetworkConnectivityLevel::Disconnected));
    snapshot.isMetered = snapshot.hasCostInfo && (snapshot.costInfo.isFixed || snapshot.costInfo.isVariable || snapshot.costInfo.isOverDataLimit || snapshot.costInfo.isApproachingDataLimit || snapshot.costInfo.isRoaming);
    snapshot.isRoaming = snapshot.hasCostInfo && snapshot.costInfo.isRoaming;
    snapshot.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
    return GB_SystemResult::Succeeded("GB_SystemNetwork::RefreshSnapshot", snapshot.diagnosticMessageUtf8.empty() ? std::string() : "已生成网络快照，部分可选信息不可用。");
}

bool TryReadCachedSnapshot(const uint64_t maxCacheAgeMilliseconds, GB_SystemNetworkSnapshot &snapshot)
{
    std::lock_guard<std::mutex> lock(snapshotCacheMutex);
    const uint64_t now = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
    if (!hasCachedSnapshot || ElapsedMilliseconds(now, cachedSnapshot.timestampMilliseconds) > maxCacheAgeMilliseconds)
    {
        return false;
    }
    snapshot = cachedSnapshot;
    return true;
}

GB_SystemResult RefreshSnapshotAndCache(GB_SystemNetworkSnapshot &snapshot)
{
    GB_SystemNetworkSnapshot refreshedSnapshot;
    GB_SystemResult result;
    try
    {
        result = BuildSnapshot(refreshedSnapshot);
    }
    catch (const std::bad_alloc &)
    {
        result = GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::RefreshSnapshot", "生成网络快照时内存不足。");
    }
    catch (const std::exception &exception)
    {
        result = GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::RefreshSnapshot", std::string("生成网络快照时发生异常：") + exception.what());
    }
    if (result.IsFailed())
    {
        snapshot = GB_SystemNetworkSnapshot();
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(snapshotCacheMutex);
        cachedSnapshot = refreshedSnapshot;
        hasCachedSnapshot = true;
    }
    snapshot = refreshedSnapshot;
    return result;
}
#endif
} // namespace

GB_SystemResult GB_SystemNetwork::GetSnapshot(GB_SystemNetworkSnapshot &snapshot, const bool forceRefresh, const uint64_t maxCacheAgeMilliseconds)
{
#if !defined(_WIN32)
    snapshot = GB_SystemNetworkSnapshot();
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::GetSnapshot");
#else
    const uint64_t effectiveMaxAge = maxCacheAgeMilliseconds == 0 ? DefaultSnapshotCacheAgeMilliseconds : maxCacheAgeMilliseconds;
    if (!forceRefresh && TryReadCachedSnapshot(effectiveMaxAge, snapshot))
    {
        return GB_SystemResult::Succeeded("GB_SystemNetwork::GetSnapshot");
    }
    std::lock_guard<std::mutex> refreshLock(snapshotRefreshMutex);
    if (!forceRefresh && TryReadCachedSnapshot(effectiveMaxAge, snapshot))
    {
        return GB_SystemResult::Succeeded("GB_SystemNetwork::GetSnapshot");
    }
    return RefreshSnapshotAndCache(snapshot);
#endif
}

GB_SystemResult GB_SystemNetwork::RefreshSnapshot(GB_SystemNetworkSnapshot &snapshot)
{
#if !defined(_WIN32)
    snapshot = GB_SystemNetworkSnapshot();
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::RefreshSnapshot");
#else
    std::lock_guard<std::mutex> refreshLock(snapshotRefreshMutex);
    return RefreshSnapshotAndCache(snapshot);
#endif
}

void GB_SystemNetwork::InvalidateSnapshotCache()
{
    std::lock_guard<std::mutex> lock(snapshotCacheMutex);
    hasCachedSnapshot = false;
    cachedSnapshot = GB_SystemNetworkSnapshot();
}

GB_SystemResult GB_SystemNetwork::EnumerateInterfaces(std::vector<GB_SystemNetworkInterfaceInfo> &interfaces)
{
#if !defined(_WIN32)
    interfaces.clear();
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::EnumerateInterfaces");
#else
    try
    {
        return EnumerateInterfacesInternal(interfaces);
    }
    catch (const std::bad_alloc &)
    {
        interfaces.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::EnumerateInterfaces", "枚举网络接口时内存不足。");
    }
    catch (const std::exception &exception)
    {
        interfaces.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::EnumerateInterfaces", std::string("枚举网络接口时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::EnumerateConnectedNetworks(std::vector<GB_SystemConnectedNetworkInfo> &networks)
{
#if !defined(_WIN32)
    networks.clear();
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::EnumerateConnectedNetworks");
#else
    try
    {
        return EnumerateConnectedNetworksInternal(networks);
    }
    catch (const std::bad_alloc &)
    {
        networks.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::EnumerateConnectedNetworks", "枚举 NLM 网络时内存不足。");
    }
    catch (const std::exception &exception)
    {
        networks.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::EnumerateConnectedNetworks", std::string("枚举 NLM 网络时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::GetNetworkCost(GB_SystemNetworkCostInfo &costInfo)
{
#if !defined(_WIN32)
    costInfo = GB_SystemNetworkCostInfo();
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::GetNetworkCost");
#else
    try
    {
        return GetNetworkCostInternal(costInfo);
    }
    catch (const std::bad_alloc &)
    {
        costInfo = GB_SystemNetworkCostInfo();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::GetNetworkCost", "读取网络成本时内存不足。");
    }
    catch (const std::exception &exception)
    {
        costInfo = GB_SystemNetworkCostInfo();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::GetNetworkCost", std::string("读取网络成本时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::HasInternetAccess(bool &hasInternetAccess)
{
    hasInternetAccess = false;
    GB_SystemNetworkSnapshot snapshot;
    GB_SystemResult result = GetSnapshot(snapshot);
    if (result.IsSucceeded())
    {
        hasInternetAccess = snapshot.hasInternetAccess;
    }
    return result.WithOperationName("GB_SystemNetwork::HasInternetAccess");
}

namespace
{
#if defined(_WIN32)
struct WlanOperationContext
{
    std::mutex mutex;
    std::condition_variable condition;
    GUID interfaceGuid = {};
    bool scanComplete = false;
    bool scanFailed = false;
    bool connectionComplete = false;
    bool connectionFailed = false;
    bool disconnected = false;
    WLAN_REASON_CODE reasonCode = WLAN_REASON_CODE_SUCCESS;
};

void WINAPI WlanOperationCallback(PWLAN_NOTIFICATION_DATA notificationData, PVOID contextPointer)
{
    if (notificationData == nullptr || contextPointer == nullptr || notificationData->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM)
    {
        return;
    }
    WlanOperationContext *context = static_cast<WlanOperationContext *>(contextPointer);
    if (!::IsEqualGUID(notificationData->InterfaceGuid, context->interfaceGuid))
    {
        return;
    }

    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        switch (notificationData->NotificationCode)
        {
        case wlan_notification_acm_scan_complete:
            context->scanComplete = true;
            notify = true;
            break;
        case wlan_notification_acm_scan_fail:
            context->scanFailed = true;
            if (notificationData->pData != nullptr && notificationData->dwDataSize >= sizeof(WLAN_REASON_CODE))
            {
                context->reasonCode = *static_cast<const WLAN_REASON_CODE *>(notificationData->pData);
            }
            notify = true;
            break;
        case wlan_notification_acm_connection_complete:
            context->connectionComplete = true;
            if (notificationData->pData != nullptr && notificationData->dwDataSize >= sizeof(WLAN_CONNECTION_NOTIFICATION_DATA))
            {
                context->reasonCode = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA *>(notificationData->pData)->wlanReasonCode;
                context->connectionFailed = context->reasonCode != WLAN_REASON_CODE_SUCCESS;
            }
            notify = true;
            break;
        case wlan_notification_acm_connection_attempt_fail:
            context->connectionFailed = true;
            if (notificationData->pData != nullptr && notificationData->dwDataSize >= sizeof(WLAN_CONNECTION_NOTIFICATION_DATA))
            {
                context->reasonCode = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA *>(notificationData->pData)->wlanReasonCode;
            }
            notify = true;
            break;
        case wlan_notification_acm_disconnected:
            context->disconnected = true;
            notify = true;
            break;
        default:
            break;
        }
    }
    if (notify)
    {
        context->condition.notify_all();
    }
}

GB_SystemResult ValidateWaitOptions(const uint32_t timeoutMilliseconds, const uint32_t cancellationPollMilliseconds, const std::string &operationName)
{
    if (timeoutMilliseconds == 0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "timeoutMilliseconds 必须大于 0。");
    }
    if (cancellationPollMilliseconds == 0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "cancellationPollMilliseconds 必须大于 0。");
    }
    return GB_SystemResult::Succeeded(operationName);
}

template <typename Predicate>
GB_SystemResult WaitForWlanCondition(WlanOperationContext &context, const uint32_t timeoutMilliseconds, const uint32_t pollMilliseconds, const std::atomic<bool> *cancellationFlag, const Predicate &predicate, const std::string &operationName)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    std::unique_lock<std::mutex> lock(context.mutex);
    while (!predicate())
    {
        if (cancellationFlag != nullptr && cancellationFlag->load(std::memory_order_acquire))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, operationName, "Wi-Fi 操作已被调用方取消。");
        }
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, operationName, "等待 Native Wi-Fi 通知超时。");
        }
        const std::chrono::milliseconds remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        context.condition.wait_for(lock, (std::min)(remaining, std::chrono::milliseconds(pollMilliseconds)));
    }
    return GB_SystemResult::Succeeded(operationName);
}

GB_SystemResult MakeWlanReasonFailure(const WLAN_REASON_CODE reasonCode, const std::string &operationName, const std::string &message)
{
    wchar_t reasonBuffer[1024] = {};
    const DWORD textResult = ::WlanReasonCodeToString(reasonCode, static_cast<DWORD>(sizeof(reasonBuffer) / sizeof(reasonBuffer[0])), reasonBuffer, nullptr);
    std::string fullMessage = message + " WLAN reason code=" + std::to_string(static_cast<uint32_t>(reasonCode)) + "。";
    GB_SystemResult result = GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, fullMessage);
    if (textResult == ERROR_SUCCESS)
    {
        result.nativeMessage = GB_WStringToUtf8(reasonBuffer);
    }
    return result;
}

GB_SystemResult QueryCurrentWifiConnection(HANDLE wlanHandle, const GUID &interfaceGuid, GB_SystemWifiConnectionInfo &connectionInfo, bool &found)
{
    connectionInfo = GB_SystemWifiConnectionInfo();
    connectionInfo.interfaceIdUtf8 = GuidToUtf8(interfaceGuid);
    found = false;
    DWORD dataSize = 0;
    WLAN_OPCODE_VALUE_TYPE opcodeValueType = wlan_opcode_value_type_invalid;
    WLAN_CONNECTION_ATTRIBUTES *rawAttributes = nullptr;
    const DWORD errorCode = ::WlanQueryInterface(wlanHandle, &interfaceGuid, wlan_intf_opcode_current_connection, nullptr, &dataSize, reinterpret_cast<PVOID *>(&rawAttributes), &opcodeValueType);
    WlanMemoryScope<WLAN_CONNECTION_ATTRIBUTES> attributes;
    attributes.Reset(rawAttributes);
    if (errorCode == ERROR_INVALID_STATE || errorCode == ERROR_NOT_FOUND)
    {
        return GB_SystemResult::Succeeded("WlanQueryInterface(CurrentConnection)");
    }
    if (errorCode != ERROR_SUCCESS)
    {
        return GB_SystemResult::FromWin32Error(errorCode, "WlanQueryInterface(CurrentConnection)", "查询当前 Wi-Fi 连接失败。");
    }
    if (attributes.Get() == nullptr)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "WlanQueryInterface(CurrentConnection)", "Native Wi-Fi 返回了空连接信息。");
    }

    const WLAN_CONNECTION_ATTRIBUTES &value = *attributes.Get();
    found = true;
    connectionInfo.profileNameUtf8 = GB_WStringToUtf8(value.strProfileName);
    connectionInfo.ssidUtf8 = SsidToUtf8(value.wlanAssociationAttributes.dot11Ssid);
    connectionInfo.ssidHexUtf8 = SsidToHex(value.wlanAssociationAttributes.dot11Ssid);
    connectionInfo.bssidUtf8 = BytesToHex(value.wlanAssociationAttributes.dot11Bssid, sizeof(value.wlanAssociationAttributes.dot11Bssid), ':');
    connectionInfo.interfaceState = MapWifiInterfaceState(value.isState);
    connectionInfo.signalQuality = value.wlanAssociationAttributes.wlanSignalQuality;
    connectionInfo.rssiDbm = static_cast<int32_t>(value.wlanAssociationAttributes.wlanSignalQuality) / 2 - 100;
    connectionInfo.receiveRateKbps = value.wlanAssociationAttributes.ulRxRate;
    connectionInfo.transmitRateKbps = value.wlanAssociationAttributes.ulTxRate;
    connectionInfo.phyType = static_cast<uint32_t>(value.wlanAssociationAttributes.dot11PhyType);
    connectionInfo.bssType = static_cast<uint32_t>(value.wlanAssociationAttributes.dot11BssType);
    connectionInfo.authenticationAlgorithm = static_cast<uint32_t>(value.wlanSecurityAttributes.dot11AuthAlgorithm);
    connectionInfo.cipherAlgorithm = static_cast<uint32_t>(value.wlanSecurityAttributes.dot11CipherAlgorithm);
    connectionInfo.isSecurityEnabled = value.wlanSecurityAttributes.bSecurityEnabled != FALSE;
    connectionInfo.securityType = MapWifiSecurity(value.wlanSecurityAttributes.dot11AuthAlgorithm, value.wlanSecurityAttributes.dot11CipherAlgorithm);
    return GB_SystemResult::Succeeded("WlanQueryInterface(CurrentConnection)");
}

uint32_t FrequencyToChannel(const uint32_t frequencyKhz)
{
    const uint32_t frequencyMhz = frequencyKhz / 1000;
    if (frequencyMhz == 2484)
    {
        return 14;
    }
    if (frequencyMhz >= 2412 && frequencyMhz <= 2472)
    {
        return (frequencyMhz - 2407) / 5;
    }
    if (frequencyMhz >= 5005 && frequencyMhz <= 5895)
    {
        return (frequencyMhz - 5000) / 5;
    }
    if (frequencyMhz >= 5955 && frequencyMhz <= 7115)
    {
        return (frequencyMhz - 5950) / 5;
    }
    return 0;
}

GB_SystemResult ValidateInterfaceId(const std::string &interfaceIdUtf8, GUID &interfaceGuid, const std::string &operationName)
{
    try
    {
        if (!TryParseGuid(interfaceIdUtf8, interfaceGuid))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "interfaceIdUtf8 不是有效的 Wi-Fi 接口 GUID。");
        }
    }
    catch (const std::exception &exception)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, operationName, std::string("接口 GUID 的 UTF-8 转换失败：") + exception.what());
    }
    return GB_SystemResult::Succeeded(operationName);
}
#endif
} // namespace

GB_SystemResult GB_SystemNetwork::EnumerateWifiInterfaces(std::vector<GB_SystemWifiInterfaceInfo> &interfaces)
{
    interfaces.clear();
#if !defined(_WIN32)
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::EnumerateWifiInterfaces");
#else
    try
    {
        WlanHandleScope wlanHandle;
        GB_SystemResult result = OpenWlanHandle(wlanHandle);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemNetwork::EnumerateWifiInterfaces");
        }
        WLAN_INTERFACE_INFO_LIST *rawList = nullptr;
        const DWORD errorCode = ::WlanEnumInterfaces(wlanHandle.Get(), nullptr, &rawList);
        WlanMemoryScope<WLAN_INTERFACE_INFO_LIST> list;
        list.Reset(rawList);
        if (errorCode != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(errorCode, "WlanEnumInterfaces", "枚举 Wi-Fi 接口失败。");
        }
        if (list.Get() == nullptr)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "WlanEnumInterfaces", "Native Wi-Fi 返回了空接口列表。");
        }
        for (DWORD index = 0; index < list.Get()->dwNumberOfItems; index++)
        {
            const WLAN_INTERFACE_INFO &rawInfo = list.Get()->InterfaceInfo[index];
            GB_SystemWifiInterfaceInfo info;
            info.interfaceIdUtf8 = GuidToUtf8(rawInfo.InterfaceGuid);
            info.descriptionUtf8 = GB_WStringToUtf8(rawInfo.strInterfaceDescription);
            info.interfaceState = MapWifiInterfaceState(rawInfo.isState);
            DWORD dataSize = 0;
            WLAN_OPCODE_VALUE_TYPE opcodeValueType = wlan_opcode_value_type_invalid;
            WLAN_RADIO_STATE *rawRadioState = nullptr;
            const DWORD radioError = ::WlanQueryInterface(wlanHandle.Get(), &rawInfo.InterfaceGuid, wlan_intf_opcode_radio_state, nullptr, &dataSize, reinterpret_cast<PVOID *>(&rawRadioState), &opcodeValueType);
            WlanMemoryScope<WLAN_RADIO_STATE> radioState;
            radioState.Reset(rawRadioState);
            if (radioError == ERROR_SUCCESS && radioState.Get() != nullptr)
            {
                info.hasRadioState = true;
                for (DWORD phyIndex = 0; phyIndex < radioState.Get()->dwNumberOfPhys; phyIndex++)
                {
                    const WLAN_PHY_RADIO_STATE &phyState = radioState.Get()->PhyRadioState[phyIndex];
                    info.isRadioOn = info.isRadioOn || (phyState.dot11HardwareRadioState == dot11_radio_state_on && phyState.dot11SoftwareRadioState == dot11_radio_state_on);
                }
            }
            result = QueryCurrentWifiConnection(wlanHandle.Get(), rawInfo.InterfaceGuid, info.currentConnection, info.hasCurrentConnection);
            if (result.IsFailed() && result.errorCode != GB_SystemErrorCode::PermissionDenied)
            {
                return result.WithOperationName("GB_SystemNetwork::EnumerateWifiInterfaces");
            }
            if (result.IsFailed())
            {
                info.diagnosticMessageUtf8 = result.GetDisplayMessage();
            }
            interfaces.push_back(info);
        }
        return GB_SystemResult::Succeeded("GB_SystemNetwork::EnumerateWifiInterfaces");
    }
    catch (const std::bad_alloc &)
    {
        interfaces.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::EnumerateWifiInterfaces", "枚举 Wi-Fi 接口时内存不足。");
    }
    catch (const std::exception &exception)
    {
        interfaces.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::EnumerateWifiInterfaces", std::string("枚举 Wi-Fi 接口时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::GetCurrentWifiConnection(const std::string &interfaceIdUtf8, GB_SystemWifiConnectionInfo &connectionInfo, bool &found)
{
    connectionInfo = GB_SystemWifiConnectionInfo();
    found = false;
#if !defined(_WIN32)
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::GetCurrentWifiConnection");
#else
    GUID interfaceGuid = {};
    GB_SystemResult result = ValidateInterfaceId(interfaceIdUtf8, interfaceGuid, "GB_SystemNetwork::GetCurrentWifiConnection");
    if (result.IsFailed())
    {
        return result;
    }
    WlanHandleScope wlanHandle;
    try
    {
        result = OpenWlanHandle(wlanHandle);
        return result.IsFailed() ? result.WithOperationName("GB_SystemNetwork::GetCurrentWifiConnection") : QueryCurrentWifiConnection(wlanHandle.Get(), interfaceGuid, connectionInfo, found).WithOperationName("GB_SystemNetwork::GetCurrentWifiConnection");
    }
    catch (const std::bad_alloc &)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::GetCurrentWifiConnection", "查询当前 Wi-Fi 连接时内存不足。");
    }
    catch (const std::exception &exception)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::GetCurrentWifiConnection", std::string("查询当前 Wi-Fi 连接时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::ScanWifiNetworks(const std::string &interfaceIdUtf8, std::vector<GB_SystemWifiNetworkInfo> &networks, const GB_SystemWifiScanOptions &options)
{
    networks.clear();
#if !defined(_WIN32)
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::ScanWifiNetworks");
#else
    GB_SystemResult result = options.requestFreshScan ? ValidateWaitOptions(options.timeoutMilliseconds, options.cancellationPollMilliseconds, "GB_SystemNetwork::ScanWifiNetworks") : GB_SystemResult::Succeeded("GB_SystemNetwork::ScanWifiNetworks");
    if (result.IsFailed())
    {
        return result;
    }
    GUID interfaceGuid = {};
    result = ValidateInterfaceId(interfaceIdUtf8, interfaceGuid, "GB_SystemNetwork::ScanWifiNetworks");
    if (result.IsFailed())
    {
        return result;
    }
    try
    {
        WlanHandleScope wlanHandle;
        result = OpenWlanHandle(wlanHandle);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemNetwork::ScanWifiNetworks");
        }
        WlanOperationContext context;
        context.interfaceGuid = interfaceGuid;
        DWORD previousSource = 0;
        if (options.requestFreshScan)
        {
            DWORD errorCode = ::WlanRegisterNotification(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_ACM, TRUE, &WlanOperationCallback, &context, nullptr, &previousSource);
            if (errorCode != ERROR_SUCCESS)
            {
                return GB_SystemResult::FromWin32Error(errorCode, "WlanRegisterNotification", "注册 Wi-Fi 扫描完成通知失败。");
            }
            errorCode = ::WlanScan(wlanHandle.Get(), &interfaceGuid, nullptr, nullptr, nullptr);
            if (errorCode != ERROR_SUCCESS)
            {
                (void)::WlanRegisterNotification(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
                return GB_SystemResult::FromWin32Error(errorCode, "WlanScan", "请求 Wi-Fi 扫描失败；新版 Windows 可能要求精确位置权限。");
            }
            result = WaitForWlanCondition(context, options.timeoutMilliseconds, options.cancellationPollMilliseconds, options.cancellationFlag, [&context]()
                {
                    return context.scanComplete || context.scanFailed;
                }, "GB_SystemNetwork::ScanWifiNetworks");
            (void)::WlanRegisterNotification(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
            if (result.IsFailed())
            {
                return result;
            }
            if (context.scanFailed)
            {
                return MakeWlanReasonFailure(context.reasonCode, "GB_SystemNetwork::ScanWifiNetworks", "Wi-Fi 扫描完成通知报告失败。");
            }
        }

        WLAN_AVAILABLE_NETWORK_LIST *rawList = nullptr;
        const DWORD listError = ::WlanGetAvailableNetworkList(wlanHandle.Get(), &interfaceGuid, WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_ADHOC_PROFILES | WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_MANUAL_HIDDEN_PROFILES, nullptr, &rawList);
        WlanMemoryScope<WLAN_AVAILABLE_NETWORK_LIST> list;
        list.Reset(rawList);
        if (listError != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(listError, "WlanGetAvailableNetworkList", "读取 Wi-Fi 扫描结果失败；新版 Windows 可能要求精确位置权限。");
        }
        for (DWORD index = 0; list.Get() != nullptr && index < list.Get()->dwNumberOfItems; index++)
        {
            const WLAN_AVAILABLE_NETWORK &rawNetwork = list.Get()->Network[index];
            GB_SystemWifiNetworkInfo info;
            info.interfaceIdUtf8 = interfaceIdUtf8;
            info.profileNameUtf8 = GB_WStringToUtf8(rawNetwork.strProfileName);
            info.ssidUtf8 = SsidToUtf8(rawNetwork.dot11Ssid);
            info.ssidHexUtf8 = SsidToHex(rawNetwork.dot11Ssid);
            info.signalQuality = rawNetwork.wlanSignalQuality;
            info.strongestRssiDbm = static_cast<int32_t>(rawNetwork.wlanSignalQuality) / 2 - 100;
            info.bssType = static_cast<uint32_t>(rawNetwork.dot11BssType);
            info.authenticationAlgorithm = static_cast<uint32_t>(rawNetwork.dot11DefaultAuthAlgorithm);
            info.cipherAlgorithm = static_cast<uint32_t>(rawNetwork.dot11DefaultCipherAlgorithm);
            info.securityType = MapWifiSecurity(rawNetwork.dot11DefaultAuthAlgorithm, rawNetwork.dot11DefaultCipherAlgorithm);
            info.isSecurityEnabled = rawNetwork.bSecurityEnabled != FALSE;
            info.hasSavedProfile = (rawNetwork.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0;
            info.isConnected = (rawNetwork.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
            info.isConnectable = rawNetwork.bNetworkConnectable != FALSE;
            info.notConnectableReason = rawNetwork.wlanNotConnectableReason;
            networks.push_back(info);
        }

        if (options.includeBssDetails)
        {
            for (size_t networkIndex = 0; networkIndex < networks.size(); networkIndex++)
            {
                networks[networkIndex].strongestRssiDbm = (std::numeric_limits<int32_t>::min)();
            }
            WLAN_BSS_LIST *rawBssList = nullptr;
            const DWORD bssError = ::WlanGetNetworkBssList(wlanHandle.Get(), &interfaceGuid, nullptr, dot11_BSS_type_any, FALSE, nullptr, &rawBssList);
            WlanMemoryScope<WLAN_BSS_LIST> bssList;
            bssList.Reset(rawBssList);
            if (bssError != ERROR_SUCCESS)
            {
                return GB_SystemResult::FromWin32Error(bssError, "WlanGetNetworkBssList", "读取 Wi-Fi BSS 详情失败；该能力可能要求精确位置权限。");
            }
            for (DWORD bssIndex = 0; bssList.Get() != nullptr && bssIndex < bssList.Get()->dwNumberOfItems; bssIndex++)
            {
                const WLAN_BSS_ENTRY &rawBss = bssList.Get()->wlanBssEntries[bssIndex];
                const std::string ssidHex = SsidToHex(rawBss.dot11Ssid);
                std::vector<GB_SystemWifiNetworkInfo>::iterator networkIter = std::find_if(networks.begin(), networks.end(), [&ssidHex, &rawBss](const GB_SystemWifiNetworkInfo &item)
                    {
                        return item.ssidHexUtf8 == ssidHex && item.bssType == static_cast<uint32_t>(rawBss.dot11BssType);
                    });
                if (networkIter == networks.end())
                {
                    continue;
                }
                GB_SystemWifiBssInfo bssInfo;
                bssInfo.bssidUtf8 = BytesToHex(rawBss.dot11Bssid, sizeof(rawBss.dot11Bssid), ':');
                bssInfo.rssiDbm = rawBss.lRssi;
                bssInfo.signalQuality = rawBss.uLinkQuality;
                bssInfo.centerFrequencyKhz = rawBss.ulChCenterFrequency;
                bssInfo.channelNumber = FrequencyToChannel(rawBss.ulChCenterFrequency);
                bssInfo.phyType = static_cast<uint32_t>(rawBss.dot11BssPhyType);
                networkIter->bssEntries.push_back(bssInfo);
                networkIter->strongestRssiDbm = (std::max)(networkIter->strongestRssiDbm, bssInfo.rssiDbm);
            }
            for (size_t networkIndex = 0; networkIndex < networks.size(); networkIndex++)
            {
                if (networks[networkIndex].bssEntries.empty())
                {
                    networks[networkIndex].strongestRssiDbm = static_cast<int32_t>(networks[networkIndex].signalQuality) / 2 - 100;
                }
            }
        }
        return GB_SystemResult::Succeeded("GB_SystemNetwork::ScanWifiNetworks");
    }
    catch (const std::bad_alloc &)
    {
        networks.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::ScanWifiNetworks", "读取 Wi-Fi 扫描结果时内存不足。");
    }
    catch (const std::exception &exception)
    {
        networks.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::ScanWifiNetworks", std::string("Wi-Fi 扫描时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::EnumerateWifiProfiles(const std::string &interfaceIdUtf8, std::vector<GB_SystemWifiProfileInfo> &profiles)
{
    profiles.clear();
#if !defined(_WIN32)
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::EnumerateWifiProfiles");
#else
    GUID interfaceGuid = {};
    GB_SystemResult result = ValidateInterfaceId(interfaceIdUtf8, interfaceGuid, "GB_SystemNetwork::EnumerateWifiProfiles");
    if (result.IsFailed())
    {
        return result;
    }
    try
    {
        WlanHandleScope wlanHandle;
        result = OpenWlanHandle(wlanHandle);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemNetwork::EnumerateWifiProfiles");
        }
        WLAN_PROFILE_INFO_LIST *rawList = nullptr;
        const DWORD errorCode = ::WlanGetProfileList(wlanHandle.Get(), &interfaceGuid, nullptr, &rawList);
        WlanMemoryScope<WLAN_PROFILE_INFO_LIST> list;
        list.Reset(rawList);
        if (errorCode != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(errorCode, "WlanGetProfileList", "枚举已保存 Wi-Fi Profile 失败。");
        }
        for (DWORD index = 0; list.Get() != nullptr && index < list.Get()->dwNumberOfItems; index++)
        {
            GB_SystemWifiProfileInfo info;
            info.interfaceIdUtf8 = interfaceIdUtf8;
            info.profileNameUtf8 = GB_WStringToUtf8(list.Get()->ProfileInfo[index].strProfileName);
            info.nativeFlags = list.Get()->ProfileInfo[index].dwFlags;
            profiles.push_back(info);
        }
        return GB_SystemResult::Succeeded("GB_SystemNetwork::EnumerateWifiProfiles");
    }
    catch (const std::bad_alloc &)
    {
        profiles.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::EnumerateWifiProfiles", "枚举 Wi-Fi Profile 时内存不足。");
    }
    catch (const std::exception &exception)
    {
        profiles.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::EnumerateWifiProfiles", std::string("枚举 Wi-Fi Profile 时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::ConnectWifiByProfile(const std::string &interfaceIdUtf8, const std::string &profileNameUtf8, const GB_SystemWifiOperationOptions &options)
{
#if !defined(_WIN32)
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::ConnectWifiByProfile");
#else
    if (profileNameUtf8.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemNetwork::ConnectWifiByProfile", "profileNameUtf8 不能为空。");
    }
    GB_SystemResult result = ValidateWaitOptions(options.timeoutMilliseconds, options.cancellationPollMilliseconds, "GB_SystemNetwork::ConnectWifiByProfile");
    GUID interfaceGuid = {};
    if (result.IsSucceeded())
    {
        result = ValidateInterfaceId(interfaceIdUtf8, interfaceGuid, "GB_SystemNetwork::ConnectWifiByProfile");
    }
    if (result.IsFailed())
    {
        return result;
    }
    try
    {
        const std::wstring profileNameWide = GB_Utf8ToWString(profileNameUtf8);
        WlanHandleScope wlanHandle;
        result = OpenWlanHandle(wlanHandle);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemNetwork::ConnectWifiByProfile");
        }
        WlanOperationContext context;
        context.interfaceGuid = interfaceGuid;
        DWORD previousSource = 0;
        DWORD errorCode = ::WlanRegisterNotification(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_ACM, TRUE, &WlanOperationCallback, &context, nullptr, &previousSource);
        if (errorCode != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(errorCode, "WlanRegisterNotification", "注册 Wi-Fi 连接结果通知失败。");
        }
        WLAN_CONNECTION_PARAMETERS parameters = {};
        parameters.wlanConnectionMode = wlan_connection_mode_profile;
        parameters.strProfile = profileNameWide.c_str();
        parameters.dot11BssType = dot11_BSS_type_any;
        errorCode = ::WlanConnect(wlanHandle.Get(), &interfaceGuid, &parameters, nullptr);
        if (errorCode != ERROR_SUCCESS)
        {
            (void)::WlanRegisterNotification(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
            return GB_SystemResult::FromWin32Error(errorCode, "WlanConnect", "提交 Wi-Fi Profile 连接请求失败。");
        }
        result = WaitForWlanCondition(context, options.timeoutMilliseconds, options.cancellationPollMilliseconds, options.cancellationFlag, [&context]()
            {
                return context.connectionComplete || context.connectionFailed;
            }, "GB_SystemNetwork::ConnectWifiByProfile");
        (void)::WlanRegisterNotification(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
        if (result.IsFailed())
        {
            if (result.errorCode == GB_SystemErrorCode::Cancelled || result.errorCode == GB_SystemErrorCode::Timeout)
            {
                (void)::WlanDisconnect(wlanHandle.Get(), &interfaceGuid, nullptr);
            }
            return result;
        }
        if (context.connectionFailed || context.reasonCode != WLAN_REASON_CODE_SUCCESS)
        {
            return MakeWlanReasonFailure(context.reasonCode, "GB_SystemNetwork::ConnectWifiByProfile", "Wi-Fi 连接未达到成功状态。");
        }
        GB_SystemWifiConnectionInfo connectionInfo;
        bool found = false;
        result = QueryCurrentWifiConnection(wlanHandle.Get(), interfaceGuid, connectionInfo, found);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemNetwork::ConnectWifiByProfile");
        }
        if (!found || ::CompareStringOrdinal(GB_Utf8ToWString(connectionInfo.profileNameUtf8).c_str(), -1, profileNameWide.c_str(), -1, TRUE) != CSTR_EQUAL)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_SystemNetwork::ConnectWifiByProfile", "收到连接成功通知后，当前连接 Profile 与目标 Profile 不一致。");
        }
        InvalidateSnapshotCache();
        return GB_SystemResult::Succeeded("GB_SystemNetwork::ConnectWifiByProfile");
    }
    catch (const std::bad_alloc &)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::ConnectWifiByProfile", "连接 Wi-Fi 时内存不足。");
    }
    catch (const std::exception &exception)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::ConnectWifiByProfile", std::string("连接 Wi-Fi 时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::DisconnectWifi(const std::string &interfaceIdUtf8, const GB_SystemWifiOperationOptions &options)
{
#if !defined(_WIN32)
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::DisconnectWifi");
#else
    GB_SystemResult result = ValidateWaitOptions(options.timeoutMilliseconds, options.cancellationPollMilliseconds, "GB_SystemNetwork::DisconnectWifi");
    GUID interfaceGuid = {};
    if (result.IsSucceeded())
    {
        result = ValidateInterfaceId(interfaceIdUtf8, interfaceGuid, "GB_SystemNetwork::DisconnectWifi");
    }
    if (result.IsFailed())
    {
        return result;
    }
    try
    {
        WlanHandleScope wlanHandle;
        result = OpenWlanHandle(wlanHandle);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemNetwork::DisconnectWifi");
        }
        GB_SystemWifiConnectionInfo currentConnection;
        bool found = false;
        result = QueryCurrentWifiConnection(wlanHandle.Get(), interfaceGuid, currentConnection, found);
        if (result.IsFailed() || !found)
        {
            return result.IsFailed() ? result.WithOperationName("GB_SystemNetwork::DisconnectWifi") : GB_SystemResult::Succeeded("GB_SystemNetwork::DisconnectWifi", "目标 Wi-Fi 接口本来就未连接。");
        }
        WlanOperationContext context;
        context.interfaceGuid = interfaceGuid;
        DWORD previousSource = 0;
        DWORD errorCode = ::WlanRegisterNotification(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_ACM, TRUE, &WlanOperationCallback, &context, nullptr, &previousSource);
        if (errorCode != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(errorCode, "WlanRegisterNotification", "注册 Wi-Fi 断开通知失败。");
        }
        errorCode = ::WlanDisconnect(wlanHandle.Get(), &interfaceGuid, nullptr);
        if (errorCode != ERROR_SUCCESS)
        {
            (void)::WlanRegisterNotification(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
            return GB_SystemResult::FromWin32Error(errorCode, "WlanDisconnect", "提交 Wi-Fi 断开请求失败。");
        }
    result = WaitForWlanCondition(context, options.timeoutMilliseconds, options.cancellationPollMilliseconds, options.cancellationFlag, [&context]()
        {
            return context.disconnected;
        }, "GB_SystemNetwork::DisconnectWifi");
        (void)::WlanRegisterNotification(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
        if (result.IsSucceeded())
        {
            InvalidateSnapshotCache();
        }
        return result;
    }
    catch (const std::bad_alloc &)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::DisconnectWifi", "断开 Wi-Fi 时内存不足。");
    }
    catch (const std::exception &exception)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::DisconnectWifi", std::string("断开 Wi-Fi 时发生异常：") + exception.what());
    }
#endif
}

std::string GB_SystemNetwork::GetAddressFamilyName(const GB_SystemNetworkAddressFamily family)
{
    switch (family)
    {
    case GB_SystemNetworkAddressFamily::IPv4:
        return "IPv4";
    case GB_SystemNetworkAddressFamily::IPv6:
        return "IPv6";
    default:
        return "Unknown";
    }
}

std::string GB_SystemNetwork::GetInterfaceTypeName(const GB_SystemNetworkInterfaceType interfaceType)
{
    switch (interfaceType)
    {
    case GB_SystemNetworkInterfaceType::Ethernet:
        return "Ethernet";
    case GB_SystemNetworkInterfaceType::Wifi:
        return "Wifi";
    case GB_SystemNetworkInterfaceType::Loopback:
        return "Loopback";
    case GB_SystemNetworkInterfaceType::Tunnel:
        return "Tunnel";
    case GB_SystemNetworkInterfaceType::Ppp:
        return "Ppp";
    case GB_SystemNetworkInterfaceType::Cellular:
        return "Cellular";
    case GB_SystemNetworkInterfaceType::Bluetooth:
        return "Bluetooth";
    case GB_SystemNetworkInterfaceType::Virtual:
        return "Virtual";
    default:
        return "Unknown";
    }
}

std::string GB_SystemNetwork::GetOperationalStatusName(const GB_SystemNetworkOperationalStatus status)
{
    switch (status)
    {
    case GB_SystemNetworkOperationalStatus::Up:
        return "Up";
    case GB_SystemNetworkOperationalStatus::Down:
        return "Down";
    case GB_SystemNetworkOperationalStatus::Testing:
        return "Testing";
    case GB_SystemNetworkOperationalStatus::Dormant:
        return "Dormant";
    case GB_SystemNetworkOperationalStatus::NotPresent:
        return "NotPresent";
    case GB_SystemNetworkOperationalStatus::LowerLayerDown:
        return "LowerLayerDown";
    default:
        return "Unknown";
    }
}

std::string GB_SystemNetwork::GetConnectivityLevelName(const GB_SystemNetworkConnectivityLevel level)
{
    switch (level)
    {
    case GB_SystemNetworkConnectivityLevel::Disconnected:
        return "Disconnected";
    case GB_SystemNetworkConnectivityLevel::InterfaceOnly:
        return "InterfaceOnly";
    case GB_SystemNetworkConnectivityLevel::LocalAccess:
        return "LocalAccess";
    case GB_SystemNetworkConnectivityLevel::InternetAccess:
        return "InternetAccess";
    case GB_SystemNetworkConnectivityLevel::CaptivePortalLikely:
        return "CaptivePortalLikely";
    case GB_SystemNetworkConnectivityLevel::Restricted:
        return "Restricted";
    default:
        return "Unknown";
    }
}

std::string GB_SystemNetwork::GetNetworkCategoryName(const GB_SystemNetworkCategory category)
{
    switch (category)
    {
    case GB_SystemNetworkCategory::Public:
        return "Public";
    case GB_SystemNetworkCategory::Private:
        return "Private";
    case GB_SystemNetworkCategory::DomainAuthenticated:
        return "DomainAuthenticated";
    default:
        return "Unknown";
    }
}

std::string GB_SystemNetwork::GetNetworkCostTypeName(const GB_SystemNetworkCostType costType)
{
    switch (costType)
    {
    case GB_SystemNetworkCostType::Unrestricted:
        return "Unrestricted";
    case GB_SystemNetworkCostType::Fixed:
        return "Fixed";
    case GB_SystemNetworkCostType::Variable:
        return "Variable";
    default:
        return "Unknown";
    }
}

std::string GB_SystemNetwork::GetWifiInterfaceStateName(const GB_SystemWifiInterfaceState state)
{
    switch (state)
    {
    case GB_SystemWifiInterfaceState::NotReady:
        return "NotReady";
    case GB_SystemWifiInterfaceState::Connected:
        return "Connected";
    case GB_SystemWifiInterfaceState::AdHocNetworkFormed:
        return "AdHocNetworkFormed";
    case GB_SystemWifiInterfaceState::Disconnecting:
        return "Disconnecting";
    case GB_SystemWifiInterfaceState::Disconnected:
        return "Disconnected";
    case GB_SystemWifiInterfaceState::Associating:
        return "Associating";
    case GB_SystemWifiInterfaceState::Discovering:
        return "Discovering";
    case GB_SystemWifiInterfaceState::Authenticating:
        return "Authenticating";
    default:
        return "Unknown";
    }
}

std::string GB_SystemNetwork::GetWifiSecurityTypeName(const GB_SystemWifiSecurityType securityType)
{
    switch (securityType)
    {
    case GB_SystemWifiSecurityType::Open:
        return "Open";
    case GB_SystemWifiSecurityType::Wep:
        return "Wep";
    case GB_SystemWifiSecurityType::WpaPersonal:
        return "WpaPersonal";
    case GB_SystemWifiSecurityType::Wpa2Personal:
        return "Wpa2Personal";
    case GB_SystemWifiSecurityType::Wpa3Personal:
        return "Wpa3Personal";
    case GB_SystemWifiSecurityType::WpaEnterprise:
        return "WpaEnterprise";
    case GB_SystemWifiSecurityType::Wpa2Enterprise:
        return "Wpa2Enterprise";
    case GB_SystemWifiSecurityType::Wpa3Enterprise:
        return "Wpa3Enterprise";
    default:
        return "Unknown";
    }
}

std::string GB_SystemNetwork::GetEventTypeName(const GB_SystemNetworkEventType eventType)
{
    switch (eventType)
    {
    case GB_SystemNetworkEventType::InitialSnapshot:
        return "InitialSnapshot";
    case GB_SystemNetworkEventType::SnapshotChanged:
        return "SnapshotChanged";
    case GB_SystemNetworkEventType::InterfaceChanged:
        return "InterfaceChanged";
    case GB_SystemNetworkEventType::AddressChanged:
        return "AddressChanged";
    case GB_SystemNetworkEventType::RouteChanged:
        return "RouteChanged";
    case GB_SystemNetworkEventType::WifiChanged:
        return "WifiChanged";
    case GB_SystemNetworkEventType::PeriodicRefresh:
        return "PeriodicRefresh";
    default:
        return "Unknown";
    }
}

namespace
{
uint64_t HashCombine(uint64_t hash, const uint64_t value)
{
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash;
}

uint64_t HashString(uint64_t hash, const std::string &value)
{
    for (size_t index = 0; index < value.size(); index++)
    {
        hash = HashCombine(hash, static_cast<unsigned char>(value[index]));
    }
    return hash;
}

uint64_t HashInterfaceStructure(const GB_SystemNetworkSnapshot &snapshot)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t index = 0; index < snapshot.interfaces.size(); index++)
    {
        const GB_SystemNetworkInterfaceInfo &info = snapshot.interfaces[index];
        hash = HashString(hash, info.interfaceIdUtf8);
        hash = HashCombine(hash, static_cast<uint64_t>(info.interfaceType));
        hash = HashCombine(hash, static_cast<uint64_t>(info.operationalStatus));
        hash = HashCombine(hash, info.interfaceIndex);
        hash = HashCombine(hash, info.mtu);
        hash = HashString(hash, info.interfaceAliasUtf8);
        hash = HashString(hash, info.friendlyNameUtf8);
        hash = HashString(hash, info.descriptionUtf8);
        hash = HashString(hash, info.macAddressUtf8);
        hash = HashCombine(hash, info.isVirtual ? 1 : 0);
    }
    return hash;
}

uint64_t HashAddresses(const GB_SystemNetworkSnapshot &snapshot)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t index = 0; index < snapshot.interfaces.size(); index++)
    {
        const GB_SystemNetworkInterfaceInfo &info = snapshot.interfaces[index];
        hash = HashString(hash, info.interfaceIdUtf8);
        for (size_t addressIndex = 0; addressIndex < info.unicastAddresses.size(); addressIndex++)
        {
            hash = HashString(hash, info.unicastAddresses[addressIndex].addressUtf8);
            hash = HashCombine(hash, info.unicastAddresses[addressIndex].prefixLength);
        }
        for (size_t dnsIndex = 0; dnsIndex < info.dnsServerAddressesUtf8.size(); dnsIndex++)
        {
            hash = HashString(hash, info.dnsServerAddressesUtf8[dnsIndex]);
        }
    }
    return hash;
}

uint64_t HashRoutes(const GB_SystemNetworkSnapshot &snapshot)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t index = 0; index < snapshot.interfaces.size(); index++)
    {
        const GB_SystemNetworkInterfaceInfo &info = snapshot.interfaces[index];
        hash = HashString(hash, info.interfaceIdUtf8);
        hash = HashCombine(hash, info.isDefaultRouteCandidate ? 1 : 0);
        hash = HashCombine(hash, info.routeMetric);
        for (size_t gatewayIndex = 0; gatewayIndex < info.gatewayAddressesUtf8.size(); gatewayIndex++)
        {
            hash = HashString(hash, info.gatewayAddressesUtf8[gatewayIndex]);
        }
    }
    return hash;
}

uint64_t HashConnectivity(const GB_SystemNetworkSnapshot &snapshot)
{
    uint64_t hash = HashCombine(0, static_cast<uint64_t>(snapshot.connectivityLevel));
    hash = HashCombine(hash, snapshot.hasInternetAccess ? 1 : 0);
    hash = HashCombine(hash, snapshot.hasLocalNetworkAccess ? 1 : 0);
    hash = HashString(hash, snapshot.primaryInterfaceIdUtf8);
    return hash;
}

uint64_t HashNetworkNames(const GB_SystemNetworkSnapshot &snapshot)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t index = 0; index < snapshot.activeNetworkNamesUtf8.size(); index++)
    {
        hash = HashString(hash, snapshot.activeNetworkNamesUtf8[index]);
    }
    return hash;
}

uint64_t HashCost(const GB_SystemNetworkSnapshot &snapshot)
{
    uint64_t hash = HashCombine(0, snapshot.hasCostInfo ? 1 : 0);
    return HashCombine(hash, snapshot.hasCostInfo ? snapshot.costInfo.nativeCostFlags : 0);
}
} // namespace

class GB_SystemNetworkWatcher::Impl final
{
  public:
    explicit Impl(const GB_SystemNetworkWatcherOptions &inputOptions)
        : options(inputOptions), eventDispatcher(GB_EventDispatcher::MakeQueuedOptions(inputOptions.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropOldest, "GB_SystemNetworkWatcher"))
    {
        callbackSetupResult = eventDispatcher.SubscribeAll([this](const GB_Event &event)
            {
                DispatchTypedCallback(event);
            }, typedSubscriptionToken);
    }

    ~Impl() noexcept
    {
        (void)Stop();
    }

    GB_SystemResult Start()
    {
#if !defined(_WIN32)
        return MakeUnsupportedPlatformResult("GB_SystemNetworkWatcher::Start");
#else
        if (options.debounceMilliseconds == 0 || options.maxDispatchQueueSize == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemNetworkWatcher::Start", "debounceMilliseconds 和 maxDispatchQueueSize 必须大于 0。");
        }
        if (callbackSetupResult.IsFailed())
        {
            return callbackSetupResult.WithOperationName("GB_SystemNetworkWatcher::Start");
        }
        std::unique_lock<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (running)
            {
                return GB_SystemResult::Succeeded("GB_SystemNetworkWatcher::Start");
            }
        }
        if (workerThread.joinable())
        {
            workerThread.join();
        }
        GB_SystemResult result = GB_SystemNetwork::RefreshSnapshot(currentSnapshot);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemNetworkWatcher::Start");
        }
        result = eventDispatcher.Start();
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemNetworkWatcher::Start");
        }

        HANDLE newInterfaceHandle = nullptr;
        HANDLE newAddressHandle = nullptr;
        HANDLE newRouteHandle = nullptr;
        DWORD errorCode = ::NotifyIpInterfaceChange(AF_UNSPEC, &Impl::InterfaceCallback, this, FALSE, &newInterfaceHandle);
        if (errorCode == NO_ERROR)
        {
            errorCode = ::NotifyUnicastIpAddressChange(AF_UNSPEC, &Impl::AddressCallback, this, FALSE, &newAddressHandle);
        }
        if (errorCode == NO_ERROR)
        {
            errorCode = ::NotifyRouteChange2(AF_UNSPEC, &Impl::RouteCallback, this, FALSE, &newRouteHandle);
        }
        if (errorCode != NO_ERROR)
        {
            if (newInterfaceHandle != nullptr)
            {
                (void)::CancelMibChangeNotify2(newInterfaceHandle);
            }
            if (newAddressHandle != nullptr)
            {
                (void)::CancelMibChangeNotify2(newAddressHandle);
            }
            if (newRouteHandle != nullptr)
            {
                (void)::CancelMibChangeNotify2(newRouteHandle);
            }
            (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return GB_SystemResult::FromWin32Error(errorCode, "GB_SystemNetworkWatcher::Start", "注册 NetIO 网络变化通知失败。");
        }

        HANDLE newWlanHandle = nullptr;
        DWORD negotiatedVersion = 0;
        if (::WlanOpenHandle(2, nullptr, &negotiatedVersion, &newWlanHandle) == ERROR_SUCCESS)
        {
            if (::WlanRegisterNotification(newWlanHandle, WLAN_NOTIFICATION_SOURCE_ACM, TRUE, &Impl::WifiCallback, this, nullptr, nullptr) != ERROR_SUCCESS)
            {
                (void)::WlanCloseHandle(newWlanHandle, nullptr);
                newWlanHandle = nullptr;
            }
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            interfaceNotificationHandle = newInterfaceHandle;
            addressNotificationHandle = newAddressHandle;
            routeNotificationHandle = newRouteHandle;
            wlanHandle = newWlanHandle;
            stopRequested = false;
            running = true;
            pendingReasons = (options.emitInitialSnapshot ? InitialReason : 0) | InterfaceReason;
        }
        try
        {
            workerThread = std::thread(&Impl::WorkerMain, this);
        }
        catch (const std::system_error &exception)
        {
            operationLock.unlock();
            (void)Stop();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetworkWatcher::Start", std::string("创建网络监听工作线程失败：") + exception.what());
        }
        catch (const std::bad_alloc &)
        {
            operationLock.unlock();
            (void)Stop();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetworkWatcher::Start", "创建网络监听工作线程时内存不足。");
        }
        condition.notify_all();
        return GB_SystemResult::Succeeded("GB_SystemNetworkWatcher::Start");
#endif
    }

    GB_SystemResult Stop()
    {
#if !defined(_WIN32)
        (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
        return GB_SystemResult::Succeeded("GB_SystemNetworkWatcher::Stop");
#else
        std::unique_lock<std::mutex> operationLock(operationMutex);
        HANDLE localInterfaceHandle = nullptr;
        HANDLE localAddressHandle = nullptr;
        HANDLE localRouteHandle = nullptr;
        HANDLE localWlanHandle = nullptr;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!running && !workerThread.joinable())
            {
                (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
                return GB_SystemResult::Succeeded("GB_SystemNetworkWatcher::Stop");
            }
            running = false;
            stopRequested = true;
            localInterfaceHandle = interfaceNotificationHandle;
            localAddressHandle = addressNotificationHandle;
            localRouteHandle = routeNotificationHandle;
            localWlanHandle = wlanHandle;
            interfaceNotificationHandle = nullptr;
            addressNotificationHandle = nullptr;
            routeNotificationHandle = nullptr;
            wlanHandle = nullptr;
        }
        if (localInterfaceHandle != nullptr)
        {
            (void)::CancelMibChangeNotify2(localInterfaceHandle);
        }
        if (localAddressHandle != nullptr)
        {
            (void)::CancelMibChangeNotify2(localAddressHandle);
        }
        if (localRouteHandle != nullptr)
        {
            (void)::CancelMibChangeNotify2(localRouteHandle);
        }
        if (localWlanHandle != nullptr)
        {
            (void)::WlanRegisterNotification(localWlanHandle, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
            (void)::WlanCloseHandle(localWlanHandle, nullptr);
        }
        condition.notify_all();
        if (workerThread.joinable())
        {
            workerThread.join();
        }
        return eventDispatcher.Stop(GB_EventDispatcherStopMode::Drain).WithOperationName("GB_SystemNetworkWatcher::Stop");
#endif
    }

    bool IsRunning() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return running && !stopRequested;
    }
    void SetCallback(const NetworkEventCallback &callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        networkEventCallback = callback;
    }
    GB_EventDispatcher &GetDispatcher()
    {
        return eventDispatcher;
    }
    uint64_t GetCoalescedCount() const
    {
        return coalescedNativeEventCount.load(std::memory_order_acquire);
    }
    uint64_t GetFailureCount() const
    {
        return refreshFailureCount.load(std::memory_order_acquire);
    }

  private:
    enum Reason : uint32_t
    {
        InterfaceReason = 1,
        AddressReason = 2,
        RouteReason = 4,
        WifiReason = 8,
        PeriodicReason = 16,
        InitialReason = 32
    };

    void Signal(const uint32_t reason)
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!running || stopRequested)
            {
                return;
            }
            if (pendingReasons != 0)
            {
                coalescedNativeEventCount.fetch_add(1, std::memory_order_relaxed);
            }
            pendingReasons |= reason;
        }
        condition.notify_all();
    }

#if defined(_WIN32)
    static VOID CALLBACK InterfaceCallback(PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE)
    {
        if (context != nullptr)
        {
            static_cast<Impl *>(context)->Signal(InterfaceReason);
        }
    }
    static VOID CALLBACK AddressCallback(PVOID context, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE)
    {
        if (context != nullptr)
        {
            static_cast<Impl *>(context)->Signal(AddressReason);
        }
    }
    static VOID CALLBACK RouteCallback(PVOID context, PMIB_IPFORWARD_ROW2, MIB_NOTIFICATION_TYPE)
    {
        if (context != nullptr)
        {
            static_cast<Impl *>(context)->Signal(RouteReason);
        }
    }
    static VOID WINAPI WifiCallback(PWLAN_NOTIFICATION_DATA, PVOID context)
    {
        if (context != nullptr)
        {
            static_cast<Impl *>(context)->Signal(WifiReason);
        }
    }
#endif

    void WorkerMain()
    {
        std::chrono::steady_clock::time_point nextPeriodic = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.periodicRefreshMilliseconds == 0 ? std::numeric_limits<uint32_t>::max() : options.periodicRefreshMilliseconds);
        for (;;)
        {
            uint32_t reasons = 0;
            {
                std::unique_lock<std::mutex> lock(stateMutex);
                condition.wait_until(lock, nextPeriodic, [this]()
                    {
                        return stopRequested || pendingReasons != 0;
                    });
                if (stopRequested)
                {
                    break;
                }
                if (pendingReasons == 0)
                {
                    pendingReasons = PeriodicReason;
                }
                reasons = pendingReasons;
                pendingReasons = 0;
            }
            if ((reasons & InitialReason) == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(options.debounceMilliseconds));
                std::lock_guard<std::mutex> lock(stateMutex);
                reasons |= pendingReasons;
                pendingReasons = 0;
            }
            try
            {
                GB_SystemNetworkSnapshot refreshedSnapshot;
                const bool initialSnapshotOnly = reasons == InitialReason;
                GB_SystemResult result = initialSnapshotOnly ? GB_SystemResult::Succeeded("GB_SystemNetworkWatcher::InitialSnapshot") : GB_SystemNetwork::RefreshSnapshot(refreshedSnapshot);
                if (initialSnapshotOnly)
                {
                    refreshedSnapshot = currentSnapshot;
                }
                if (result.IsFailed())
                {
                    refreshFailureCount.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                GB_SystemNetworkEvent networkEvent;
                networkEvent.eventType = (reasons & InitialReason) != 0 ? GB_SystemNetworkEventType::InitialSnapshot : ((reasons & WifiReason) != 0 ? GB_SystemNetworkEventType::WifiChanged : GB_SystemNetworkEventType::SnapshotChanged);
                networkEvent.eventName = "SystemNetwork." + GB_SystemNetwork::GetEventTypeName(networkEvent.eventType);
                networkEvent.sourceName = "NetIO/NativeWifi";
                networkEvent.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
                networkEvent.previousSnapshot = currentSnapshot;
                networkEvent.currentSnapshot = refreshedSnapshot;
                networkEvent.interfacesChanged = HashInterfaceStructure(currentSnapshot) != HashInterfaceStructure(refreshedSnapshot);
                networkEvent.addressesChanged = HashAddresses(currentSnapshot) != HashAddresses(refreshedSnapshot);
                networkEvent.routesChanged = HashRoutes(currentSnapshot) != HashRoutes(refreshedSnapshot);
                networkEvent.connectivityChanged = HashConnectivity(currentSnapshot) != HashConnectivity(refreshedSnapshot);
                networkEvent.networkNamesChanged = HashNetworkNames(currentSnapshot) != HashNetworkNames(refreshedSnapshot);
                networkEvent.costChanged = HashCost(currentSnapshot) != HashCost(refreshedSnapshot);
                networkEvent.wifiChanged = (reasons & WifiReason) != 0;
                currentSnapshot = refreshedSnapshot;
                if ((reasons & InitialReason) == 0 && (reasons & WifiReason) == 0)
                {
                    if (reasons == InterfaceReason)
                    {
                        networkEvent.eventType = GB_SystemNetworkEventType::InterfaceChanged;
                    }
                    else if (reasons == AddressReason)
                    {
                        networkEvent.eventType = GB_SystemNetworkEventType::AddressChanged;
                    }
                    else if (reasons == RouteReason)
                    {
                        networkEvent.eventType = GB_SystemNetworkEventType::RouteChanged;
                    }
                    else if (reasons == PeriodicReason)
                    {
                        networkEvent.eventType = GB_SystemNetworkEventType::PeriodicRefresh;
                    }
                    networkEvent.eventName = "SystemNetwork." + GB_SystemNetwork::GetEventTypeName(networkEvent.eventType);
                }
                const bool shouldPublish = (reasons & InitialReason) != 0 || networkEvent.interfacesChanged || networkEvent.addressesChanged || networkEvent.routesChanged || networkEvent.connectivityChanged || networkEvent.networkNamesChanged || networkEvent.costChanged || networkEvent.wifiChanged;
                if (shouldPublish)
                {
                    (void)eventDispatcher.Post(networkEvent.eventName, GB_Variant(networkEvent), networkEvent.sourceName);
                }
            }
            catch (const std::bad_alloc &)
            {
                refreshFailureCount.fetch_add(1, std::memory_order_relaxed);
            }
            catch (const std::exception &)
            {
                refreshFailureCount.fetch_add(1, std::memory_order_relaxed);
            }
            nextPeriodic = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.periodicRefreshMilliseconds == 0 ? std::numeric_limits<uint32_t>::max() : options.periodicRefreshMilliseconds);
        }
    }

    void DispatchTypedCallback(const GB_Event &event)
    {
        const GB_SystemNetworkEvent *networkEvent = event.payload.AnyCast<GB_SystemNetworkEvent>();
        if (networkEvent == nullptr)
        {
            return;
        }
        NetworkEventCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callback = networkEventCallback;
        }
        if (callback)
        {
            callback(*networkEvent);
        }
    }

  private:
    GB_SystemNetworkWatcherOptions options;
    GB_EventDispatcher eventDispatcher;
    GB_SystemResult callbackSetupResult;
    GB_EventSubscriptionToken typedSubscriptionToken;
    mutable std::mutex operationMutex;
    mutable std::mutex stateMutex;
    std::mutex callbackMutex;
    std::condition_variable condition;
    std::thread workerThread;
    NetworkEventCallback networkEventCallback;
    GB_SystemNetworkSnapshot currentSnapshot;
    bool running = false;
    bool stopRequested = false;
    uint32_t pendingReasons = 0;
    std::atomic<uint64_t> coalescedNativeEventCount{0};
    std::atomic<uint64_t> refreshFailureCount{0};
#if defined(_WIN32)
    HANDLE interfaceNotificationHandle = nullptr;
    HANDLE addressNotificationHandle = nullptr;
    HANDLE routeNotificationHandle = nullptr;
    HANDLE wlanHandle = nullptr;
#endif
};

GB_SystemNetworkWatcher::GB_SystemNetworkWatcher()
    : GB_SystemNetworkWatcher(GB_SystemNetworkWatcherOptions())
{
}
GB_SystemNetworkWatcher::GB_SystemNetworkWatcher(const GB_SystemNetworkWatcherOptions &options)
    : impl(new Impl(options))
{
}
GB_SystemNetworkWatcher::~GB_SystemNetworkWatcher() noexcept = default;
GB_SystemResult GB_SystemNetworkWatcher::Start()
{
    return impl->Start();
}
GB_SystemResult GB_SystemNetworkWatcher::Stop()
{
    return impl->Stop();
}
bool GB_SystemNetworkWatcher::IsRunning() const
{
    return impl->IsRunning();
}
void GB_SystemNetworkWatcher::SetNetworkEventCallback(const NetworkEventCallback &callback)
{
    impl->SetCallback(callback);
}
GB_EventDispatcher &GB_SystemNetworkWatcher::GetEventDispatcher()
{
    return impl->GetDispatcher();
}
uint64_t GB_SystemNetworkWatcher::GetCoalescedNativeEventCount() const
{
    return impl->GetCoalescedCount();
}
uint64_t GB_SystemNetworkWatcher::GetRefreshFailureCount() const
{
    return impl->GetFailureCount();
}
