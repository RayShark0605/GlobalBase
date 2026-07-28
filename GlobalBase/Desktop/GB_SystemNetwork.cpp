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
#include <utility>

#if defined(_WIN32)
#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <netlistmgr.h>
#include <oleauto.h>
#include <wlanapi.h>
#include <wrl/client.h>
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Wlanapi.lib")
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace
{
    std::mutex snapshotCacheMutex;
    std::mutex snapshotRefreshMutex;
    GB_SystemNetworkSnapshot cachedSnapshot;
    bool hasCachedSnapshot = false;

    GB_SystemResult MakeUnsupportedPlatformResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, "当前平台不支持 Windows 网络能力。");
    }

    void AppendDiagnostic(std::string& diagnosticMessage, const std::string& message)
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
    void AppendUnique(std::vector<ValueType>& values, const ValueType& value)
    {
        if (std::find(values.begin(), values.end(), value) == values.end())
        {
            values.push_back(value);
        }
    }

    std::string ToLowerAscii(const std::string& value)
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

    bool LooksVirtual(const std::string& friendlyName, const std::string& description)
    {
        const std::string text = ToLowerAscii(friendlyName + " " + description);
        static const char* const virtualMarkers[] = { "virtual", "hyper-v", "vmware", "vbox", "tap-", "tap ", "tun ", "wireguard", "loopback", "npcap" };
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

    bool IsCancellationRequested(const std::atomic<bool>* cancellationFlag)
    {
        return cancellationFlag != nullptr && cancellationFlag->load(std::memory_order_acquire);
    }

    GB_SystemResult MakeCancelledBeforeNativeCallResult(const std::string& operationName, const std::string& message)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, operationName, message);
    }

#if defined(_WIN32)
    using Microsoft::WRL::ComPtr;

    constexpr ULONG InitialAdapterAddressBufferSize = 15 * 1024;
    constexpr ULONG MaxAdapterAddressBufferSize = 64 * 1024 * 1024;
    constexpr int MaxAdapterAddressRetryCount = 8;

    DOT11_AUTH_ALGORITHM GetDot11AuthAlgorithmWpa3Enterprise192()
    {
#ifdef DOT11_AUTH_ALGO_WPA3_ENT_192
        return DOT11_AUTH_ALGO_WPA3_ENT_192;
#else
        return static_cast<DOT11_AUTH_ALGORITHM>(8);
#endif
    }

    DOT11_AUTH_ALGORITHM GetDot11AuthAlgorithmWpa3PersonalSae()
    {
#ifdef DOT11_AUTH_ALGO_WPA3_SAE
        return DOT11_AUTH_ALGO_WPA3_SAE;
#else
        return static_cast<DOT11_AUTH_ALGORITHM>(9);
#endif
    }

    DOT11_AUTH_ALGORITHM GetDot11AuthAlgorithmOwe()
    {
#ifdef DOT11_AUTH_ALGO_OWE
        return DOT11_AUTH_ALGO_OWE;
#else
        return static_cast<DOT11_AUTH_ALGORITHM>(10);
#endif
    }

    DOT11_AUTH_ALGORITHM GetDot11AuthAlgorithmWpa3Enterprise()
    {
#ifdef DOT11_AUTH_ALGO_WPA3_ENT
        return DOT11_AUTH_ALGO_WPA3_ENT;
#else
        return static_cast<DOT11_AUTH_ALGORITHM>(11);
#endif
    }

    bool IsWifiRadioOffError(const DWORD errorCode)
    {
#ifdef ERROR_NDIS_DOT11_POWER_STATE_INVALID
        return errorCode == ERROR_NDIS_DOT11_POWER_STATE_INVALID;
#else
        (void)errorCode;
        return false;
#endif
    }

    GB_SystemResult MakeWin32ErrorResult(const DWORD errorCode, const std::string& operationName, const std::string& message)
    {
        if (IsWifiRadioOffError(errorCode))
        {
            GB_SystemResult result = GB_SystemResult::FromWin32Error(errorCode, operationName, message + " 无线网卡可能处于关闭、飞行模式或电源不可用状态。");
            result.errorCode = GB_SystemErrorCode::InvalidState;
            return result;
        }
        return GB_SystemResult::FromWin32Error(errorCode, operationName, message);
    }

    GB_SystemResult MakeWifiWin32ErrorResult(const DWORD errorCode, const std::string& operationName, const std::string& message)
    {
        GB_SystemResult result = MakeWin32ErrorResult(errorCode, operationName, message);
        if (errorCode == ERROR_ACCESS_DENIED)
        {
            result.message += " 该 Native Wi-Fi API 在新版 Windows 中可能需要用户授予精确位置权限。";
        }
        return result;
    }

    class WlanHandleScope final
    {
    public:
        ~WlanHandleScope() noexcept
        {
            Reset();
        }

        WlanHandleScope(const WlanHandleScope&) = delete;
        WlanHandleScope& operator=(const WlanHandleScope&) = delete;
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

        WlanMemoryScope(const WlanMemoryScope&) = delete;
        WlanMemoryScope& operator=(const WlanMemoryScope&) = delete;
        WlanMemoryScope() = default;

        void Reset(ValueType* inputValue = nullptr) noexcept
        {
            if (value != nullptr)
            {
                ::WlanFreeMemory(value);
            }
            value = inputValue;
        }

        ValueType* Get() const noexcept
        {
            return value;
        }

    private:
        ValueType* value = nullptr;
    };

    class WlanNotificationScope final
    {
    public:
        ~WlanNotificationScope() noexcept
        {
            Reset();
        }

        WlanNotificationScope(const WlanNotificationScope&) = delete;
        WlanNotificationScope& operator=(const WlanNotificationScope&) = delete;
        WlanNotificationScope() = default;

        DWORD Register(HANDLE inputHandle, const DWORD notificationSource, WLAN_NOTIFICATION_CALLBACK callback, void* context, const bool ignoreDuplicateNotifications, DWORD* previousSource) noexcept
        {
            Reset();
            const DWORD errorCode = ::WlanRegisterNotification(inputHandle, notificationSource, ignoreDuplicateNotifications ? TRUE : FALSE, callback, context, nullptr, previousSource);
            if (errorCode == ERROR_SUCCESS)
            {
                handle = inputHandle;
                registered = true;
            }
            return errorCode;
        }

        void Reset() noexcept
        {
            if (registered && handle != nullptr)
            {
                (void)::WlanRegisterNotification(handle, WLAN_NOTIFICATION_SOURCE_NONE, FALSE, nullptr, nullptr, nullptr, nullptr);
            }
            handle = nullptr;
            registered = false;
        }

    private:
        HANDLE handle = nullptr;
        bool registered = false;
    };

    class BstrScope final
    {
    public:
        ~BstrScope() noexcept
        {
            Reset();
        }

        BstrScope(const BstrScope&) = delete;
        BstrScope& operator=(const BstrScope&) = delete;
        BstrScope() = default;

        void Reset(BSTR inputValue = nullptr) noexcept
        {
            if (value != nullptr)
            {
                ::SysFreeString(value);
            }
            value = inputValue;
        }

        BSTR* Address() noexcept
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

        MibTableScope(const MibTableScope&) = delete;
        MibTableScope& operator=(const MibTableScope&) = delete;
        MibTableScope() = default;

        void Reset(void* inputTable = nullptr) noexcept
        {
            if (table != nullptr)
            {
                ::FreeMibTable(table);
            }
            table = inputTable;
        }

    private:
        void* table = nullptr;
    };

    GB_SystemResult OpenWlanHandle(WlanHandleScope& wlanHandle)
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

    std::string GuidToUtf8(const GUID& guid)
    {
        wchar_t buffer[64] = {};
        const int length = ::StringFromGUID2(guid, buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
        return length <= 1 ? std::string() : GB_WStringToUtf8(std::wstring(buffer, static_cast<size_t>(length - 1)));
    }

    std::string TrimAsciiWhitespace(const std::string& text)
    {
        size_t beginIndex = 0;
        while (beginIndex < text.size() && static_cast<unsigned char>(text[beginIndex]) <= 0x20)
        {
            beginIndex++;
        }

        size_t endIndex = text.size();
        while (endIndex > beginIndex && static_cast<unsigned char>(text[endIndex - 1]) <= 0x20)
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    bool TryParseGuid(const std::string& textUtf8, GUID& guid)
    {
        const std::string trimmedTextUtf8 = TrimAsciiWhitespace(textUtf8);
        if (trimmedTextUtf8.empty())
        {
            return false;
        }

        const std::wstring textWide = GB_Utf8ToWString(trimmedTextUtf8);
        if (SUCCEEDED(::CLSIDFromString(textWide.c_str(), &guid)))
        {
            return true;
        }

        if (trimmedTextUtf8.front() == '{' || trimmedTextUtf8.back() == '}')
        {
            return false;
        }

        const std::wstring bracedTextWide = L"{" + textWide + L"}";
        return SUCCEEDED(::CLSIDFromString(bracedTextWide.c_str(), &guid));
    }

    std::string SockaddrToUtf8(const SOCKADDR* address)
    {
        if (address == nullptr)
        {
            return std::string();
        }

        wchar_t buffer[INET6_ADDRSTRLEN + 32] = {};
        if (address->sa_family == AF_INET)
        {
            const SOCKADDR_IN* ipv4Address = reinterpret_cast<const SOCKADDR_IN*>(address);
            if (::InetNtopW(AF_INET, &ipv4Address->sin_addr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]))) == nullptr)
            {
                return std::string();
            }
        }
        else if (address->sa_family == AF_INET6)
        {
            const SOCKADDR_IN6* ipv6Address = reinterpret_cast<const SOCKADDR_IN6*>(address);
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

    bool IsUsableUnicastAddress(const SOCKADDR* address)
    {
        if (address == nullptr)
        {
            return false;
        }
        if (address->sa_family == AF_INET)
        {
            const SOCKADDR_IN* ipv4Address = reinterpret_cast<const SOCKADDR_IN*>(address);
            const uint32_t hostOrderAddress = ntohl(ipv4Address->sin_addr.S_un.S_addr);
            if (hostOrderAddress == 0 || hostOrderAddress == 0xFFFFFFFFu)
            {
                return false;
            }
            if ((hostOrderAddress & 0xFF000000u) == 0x7F000000u)
            {
                return false;
            }
            if ((hostOrderAddress & 0xF0000000u) == 0xE0000000u)
            {
                return false;
            }
            return true;
        }
        if (address->sa_family == AF_INET6)
        {
            const SOCKADDR_IN6* ipv6Address = reinterpret_cast<const SOCKADDR_IN6*>(address);
            const IN6_ADDR& value = ipv6Address->sin6_addr;
            const bool isUnspecified = IN6_IS_ADDR_UNSPECIFIED(&value) != 0;
            const bool isLoopback = IN6_IS_ADDR_LOOPBACK(&value) != 0;
            const bool isMulticast = IN6_IS_ADDR_MULTICAST(&value) != 0;
            return !isUnspecified && !isLoopback && !isMulticast;
        }
        return false;
    }

    bool IsUsableGatewayAddress(const SOCKADDR* address)
    {
        return IsUsableUnicastAddress(address);
    }

    struct IpInterfaceMetricKey
    {
        IpInterfaceMetricKey(const ADDRESS_FAMILY inputFamily, const uint64_t inputInterfaceLuid)
            : family(inputFamily), interfaceLuid(inputInterfaceLuid)
        {
        }

        bool operator==(const IpInterfaceMetricKey& other) const
        {
            return family == other.family && interfaceLuid == other.interfaceLuid;
        }

        ADDRESS_FAMILY family;
        uint64_t interfaceLuid;
    };

    struct IpInterfaceMetricKeyHasher
    {
        size_t operator()(const IpInterfaceMetricKey& key) const noexcept
        {
            const size_t familyHash = std::hash<unsigned int>()(static_cast<unsigned int>(key.family));
            const size_t luidHash = std::hash<uint64_t>()(key.interfaceLuid);
            return luidHash ^ (familyHash + static_cast<size_t>(0x9E3779B9u) + (luidHash << 6) + (luidHash >> 2));
        }
    };

    std::string BytesToHex(const unsigned char* bytes, const size_t byteCount, const char separator)
    {
        if (bytes == nullptr || byteCount == 0)
        {
            return std::string();
        }

        static const char HexDigits[] = "0123456789ABCDEF";
        std::string result;
        result.reserve(separator == '\0' ? byteCount * 2 : byteCount * 3);
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

    std::string SsidToHex(const DOT11_SSID& ssid)
    {
        const size_t length = (std::min)(static_cast<size_t>(ssid.uSSIDLength), sizeof(ssid.ucSSID));
        return BytesToHex(ssid.ucSSID, length, '\0');
    }

    std::string SsidToUtf8(const DOT11_SSID& ssid)
    {
        const size_t length = (std::min)(static_cast<size_t>(ssid.uSSIDLength), sizeof(ssid.ucSSID));
        const std::string raw(reinterpret_cast<const char*>(ssid.ucSSID), length);
        return GB_IsUtf8(raw) ? raw : std::string();
    }

    uint32_t ClampSignalQuality(const ULONG signalQuality)
    {
        return signalQuality > 100 ? 100 : static_cast<uint32_t>(signalQuality);
    }

    int32_t SignalQualityToRssiDbm(const ULONG signalQuality)
    {
        const uint32_t boundedSignalQuality = ClampSignalQuality(signalQuality);
        return boundedSignalQuality == 0 ? -100 : (static_cast<int32_t>(boundedSignalQuality) / 2 - 100);
    }

    std::string MakeBssLookupKey(const std::string& ssidHexUtf8, const uint32_t bssType)
    {
        return ssidHexUtf8 + "#" + std::to_string(bssType);
    }

    bool IsRelevantNetworkInterface(const GB_SystemNetworkInterfaceInfo& info)
    {
        return info.interfaceType != GB_SystemNetworkInterfaceType::Loopback;
    }

    bool IsUsableConnectedInterface(const GB_SystemNetworkInterfaceInfo& info)
    {
        return IsRelevantNetworkInterface(info) && info.operationalStatus == GB_SystemNetworkOperationalStatus::Up && (info.hasIpv4Address || info.hasIpv6Address);
    }

    uint32_t GetInterfaceTypePreference(const GB_SystemNetworkInterfaceType interfaceType)
    {
        switch (interfaceType)
        {
        case GB_SystemNetworkInterfaceType::Ethernet:
            return 0;
        case GB_SystemNetworkInterfaceType::Wifi:
            return 1;
        case GB_SystemNetworkInterfaceType::Cellular:
            return 2;
        case GB_SystemNetworkInterfaceType::Ppp:
            return 3;
        case GB_SystemNetworkInterfaceType::Tunnel:
            return 4;
        case GB_SystemNetworkInterfaceType::Virtual:
            return 5;
        case GB_SystemNetworkInterfaceType::Bluetooth:
            return 6;
        default:
            return 7;
        }
    }

    uint64_t GetPrimaryInterfaceScore(const GB_SystemNetworkInterfaceInfo& info)
    {
        const uint64_t defaultRoutePenalty = info.isDefaultRouteCandidate ? 0 : (uint64_t(1) << 48);
        const uint64_t metric = info.isDefaultRouteCandidate ? info.routeMetric : std::numeric_limits<uint32_t>::max();
        return defaultRoutePenalty + (metric * 16) + GetInterfaceTypePreference(info.interfaceType);
    }

    GB_SystemNetworkInterfaceType MapInterfaceType(const ULONG interfaceType, const bool looksVirtual)
    {
        switch (interfaceType)
        {
        case IF_TYPE_SOFTWARE_LOOPBACK:
            return GB_SystemNetworkInterfaceType::Loopback;
        case IF_TYPE_TUNNEL:
            return GB_SystemNetworkInterfaceType::Tunnel;
        case IF_TYPE_IEEE80211:
            return looksVirtual ? GB_SystemNetworkInterfaceType::Virtual : GB_SystemNetworkInterfaceType::Wifi;
        case IF_TYPE_ETHERNET_CSMACD:
        case IF_TYPE_ISO88025_TOKENRING:
            return looksVirtual ? GB_SystemNetworkInterfaceType::Virtual : GB_SystemNetworkInterfaceType::Ethernet;
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
            return looksVirtual ? GB_SystemNetworkInterfaceType::Virtual : GB_SystemNetworkInterfaceType::Unknown;
        }
    }

    GB_SystemNetworkInterfaceType RefineInterfaceTypeByPhysicalMedium(const GB_SystemNetworkInterfaceType interfaceType, const NDIS_PHYSICAL_MEDIUM physicalMediumType, const bool looksVirtual)
    {
        if (looksVirtual || interfaceType == GB_SystemNetworkInterfaceType::Loopback || interfaceType == GB_SystemNetworkInterfaceType::Tunnel)
        {
            return interfaceType;
        }

        switch (physicalMediumType)
        {
        case NdisPhysicalMediumBluetooth:
            return GB_SystemNetworkInterfaceType::Bluetooth;
        case NdisPhysicalMediumWirelessLan:
        case NdisPhysicalMediumNative802_11:
            return GB_SystemNetworkInterfaceType::Wifi;
        case NdisPhysicalMediumWirelessWan:
            return GB_SystemNetworkInterfaceType::Cellular;
        default:
            return interfaceType;
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
        if (cipher == DOT11_CIPHER_ALGO_WEP || cipher == DOT11_CIPHER_ALGO_WEP40 || cipher == DOT11_CIPHER_ALGO_WEP104 || authentication == DOT11_AUTH_ALGO_80211_SHARED_KEY)
        {
            return GB_SystemWifiSecurityType::Wep;
        }
        if (authentication == GetDot11AuthAlgorithmWpa3PersonalSae())
        {
            return GB_SystemWifiSecurityType::Wpa3Personal;
        }
        if (authentication == GetDot11AuthAlgorithmWpa3Enterprise192() || authentication == GetDot11AuthAlgorithmWpa3Enterprise())
        {
            return GB_SystemWifiSecurityType::Wpa3Enterprise;
        }
        if (authentication == GetDot11AuthAlgorithmOwe())
        {
            return GB_SystemWifiSecurityType::Owe;
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

    GB_SystemResult EnumerateInterfacesInternal(std::vector<GB_SystemNetworkInterfaceInfo>& interfaces)
    {
        interfaces.clear();
        const ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_ALL_INTERFACES | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
        ULONG bufferSize = InitialAdapterAddressBufferSize;
        std::vector<unsigned char> buffer(bufferSize);
        ULONG errorCode = ERROR_BUFFER_OVERFLOW;
        for (int attempt = 0; attempt < MaxAdapterAddressRetryCount; attempt++)
        {
            IP_ADAPTER_ADDRESSES* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            bufferSize = static_cast<ULONG>(buffer.size());
            errorCode = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &bufferSize);
            if (errorCode != ERROR_BUFFER_OVERFLOW)
            {
                break;
            }
            if (bufferSize == 0 || bufferSize > MaxAdapterAddressBufferSize)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GetAdaptersAddresses", "系统请求的网卡枚举缓冲区大小不合理。");
            }
            const size_t requestedBufferSize = static_cast<size_t>(bufferSize);
            const size_t doubledBufferSize = buffer.size() <= static_cast<size_t>(MaxAdapterAddressBufferSize) / 2 ? buffer.size() * 2 : static_cast<size_t>(MaxAdapterAddressBufferSize);
            const size_t nextBufferSize = (std::min)(static_cast<size_t>(MaxAdapterAddressBufferSize), (std::max)(requestedBufferSize, doubledBufferSize));
            if (nextBufferSize <= buffer.size())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GetAdaptersAddresses", "系统请求的网卡枚举缓冲区无法继续扩展。");
            }
            buffer.resize(nextBufferSize);
        }
        if (errorCode == ERROR_NO_DATA)
        {
            return GB_SystemResult::Succeeded("GetAdaptersAddresses", "系统当前没有返回符合条件的网络地址。");
        }
        if (errorCode != ERROR_SUCCESS)
        {
            return MakeWin32ErrorResult(errorCode, "GetAdaptersAddresses", "枚举本机网络接口失败。");
        }

        const IP_ADAPTER_ADDRESSES* adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
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
#ifdef IP_ADAPTER_IPV6_MANAGE_ADDRESS_CONFIG
            info.dhcpV6Enabled = (adapter->Flags & IP_ADAPTER_IPV6_MANAGE_ADDRESS_CONFIG) != 0;
#else
            info.dhcpV6Enabled = adapter->Dhcpv6ClientDuidLength != 0;
#endif
            info.mtu = adapter->Mtu;
            info.transmitLinkSpeedBitsPerSecond = adapter->TransmitLinkSpeed;
            info.receiveLinkSpeedBitsPerSecond = adapter->ReceiveLinkSpeed;

            MIB_IF_ROW2 interfaceRow = {};
            interfaceRow.InterfaceLuid = adapter->Luid;
            if (::GetIfEntry2(&interfaceRow) == NO_ERROR)
            {
                info.interfaceType = RefineInterfaceTypeByPhysicalMedium(info.interfaceType, interfaceRow.PhysicalMediumType, info.isVirtual);
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

            for (const IP_ADAPTER_UNICAST_ADDRESS* address = adapter->FirstUnicastAddress; address != nullptr; address = address->Next)
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
                    if (IsUsableUnicastAddress(address->Address.lpSockaddr))
                    {
                        info.hasIpv4Address = info.hasIpv4Address || networkAddress.family == GB_SystemNetworkAddressFamily::IPv4;
                        info.hasIpv6Address = info.hasIpv6Address || networkAddress.family == GB_SystemNetworkAddressFamily::IPv6;
                    }
                }
            }
            for (const IP_ADAPTER_GATEWAY_ADDRESS_LH* gateway = adapter->FirstGatewayAddress; gateway != nullptr; gateway = gateway->Next)
            {
                const std::string addressUtf8 = SockaddrToUtf8(gateway->Address.lpSockaddr);
                if (!addressUtf8.empty() && IsUsableGatewayAddress(gateway->Address.lpSockaddr))
                {
                    AppendUnique(info.gatewayAddressesUtf8, addressUtf8);
                }
            }
            for (const IP_ADAPTER_DNS_SERVER_ADDRESS_XP* dnsServer = adapter->FirstDnsServerAddress; dnsServer != nullptr; dnsServer = dnsServer->Next)
            {
                const std::string addressUtf8 = SockaddrToUtf8(dnsServer->Address.lpSockaddr);
                if (!addressUtf8.empty())
                {
                    AppendUnique(info.dnsServerAddressesUtf8, addressUtf8);
                }
            }
            info.hasDefaultGateway = !info.gatewayAddressesUtf8.empty();
            std::sort(info.unicastAddresses.begin(), info.unicastAddresses.end(), [](const GB_SystemNetworkAddress& left, const GB_SystemNetworkAddress& right)
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
            std::unordered_map<IpInterfaceMetricKey, uint32_t, IpInterfaceMetricKeyHasher> interfaceMetricByKey;
            interfaceMetricByKey.reserve(interfaces.size() * 2);
            for (ULONG routeIndex = 0; routeIndex < routeTable->NumEntries; routeIndex++)
            {
                const MIB_IPFORWARD_ROW2& route = routeTable->Table[routeIndex];
                if (route.DestinationPrefix.PrefixLength != 0 || route.InterfaceLuid.Value == 0)
                {
                    continue;
                }
                if (route.DestinationPrefix.Prefix.si_family != AF_INET && route.DestinationPrefix.Prefix.si_family != AF_INET6)
                {
                    continue;
                }
                const std::unordered_map<uint64_t, size_t>::const_iterator indexIter = interfaceIndexesByLuid.find(route.InterfaceLuid.Value);
                if (indexIter == interfaceIndexesByLuid.end())
                {
                    continue;
                }
                const IpInterfaceMetricKey metricKey(route.DestinationPrefix.Prefix.si_family, route.InterfaceLuid.Value);
                std::unordered_map<IpInterfaceMetricKey, uint32_t, IpInterfaceMetricKeyHasher>::const_iterator metricIter = interfaceMetricByKey.find(metricKey);
                if (metricIter == interfaceMetricByKey.end())
                {
                    MIB_IPINTERFACE_ROW ipInterface = {};
                    ipInterface.Family = route.DestinationPrefix.Prefix.si_family;
                    ipInterface.InterfaceLuid = route.InterfaceLuid;
                    ipInterface.InterfaceIndex = route.InterfaceIndex;
                    const uint32_t interfaceMetric = ::GetIpInterfaceEntry(&ipInterface) == NO_ERROR ? ipInterface.Metric : 0;
                    metricIter = interfaceMetricByKey.emplace(metricKey, interfaceMetric).first;
                }
                GB_SystemNetworkInterfaceInfo& interfaceInfo = interfaces[indexIter->second];
                const uint64_t combinedMetric = static_cast<uint64_t>(route.Metric) + metricIter->second;
                const uint32_t boundedMetric = combinedMetric > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(combinedMetric);
                const bool wasDefaultRouteCandidate = interfaceInfo.isDefaultRouteCandidate;
                interfaceInfo.isDefaultRouteCandidate = true;
                if (!wasDefaultRouteCandidate || boundedMetric < interfaceInfo.routeMetric)
                {
                    interfaceInfo.routeMetric = boundedMetric;
                }
            }
        }

        std::sort(interfaces.begin(), interfaces.end(), [](const GB_SystemNetworkInterfaceInfo& left, const GB_SystemNetworkInterfaceInfo& right)
            {
                return left.interfaceIdUtf8 < right.interfaceIdUtf8;
            });
        return GB_SystemResult::Succeeded("GB_SystemNetwork::EnumerateInterfaces");
    }

    bool IsComUsable(const GB_ComScope& comScope)
    {
        return comScope.IsInitialized() || static_cast<uint32_t>(comScope.GetInitializeHResult()) == 0x80010106u;
    }

    struct NlmOverallConnectivityInfo
    {
        bool hasInfo = false;
        bool hasLocalNetworkAccess = false;
        bool hasInternetAccess = false;
        GB_SystemNetworkConnectivityLevel connectivityLevel = GB_SystemNetworkConnectivityLevel::Unknown;
    };

    void FillOverallConnectivityInfo(INetworkListManager* manager, NlmOverallConnectivityInfo& info)
    {
        info = NlmOverallConnectivityInfo();
        if (manager == nullptr)
        {
            return;
        }

        NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
        if (SUCCEEDED(manager->GetConnectivity(&connectivity)))
        {
            const DWORD value = static_cast<DWORD>(connectivity);
            info.hasInfo = true;
            info.connectivityLevel = MapConnectivity(connectivity);
            info.hasLocalNetworkAccess = (value & (NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_SUBNET | NLM_CONNECTIVITY_IPV6_LOCALNETWORK | NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
            info.hasInternetAccess = (value & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
        }

        VARIANT_BOOL internetConnected = VARIANT_FALSE;
        if (SUCCEEDED(manager->get_IsConnectedToInternet(&internetConnected)))
        {
            info.hasInfo = true;
            info.hasInternetAccess = info.hasInternetAccess || internetConnected == VARIANT_TRUE;
            info.hasLocalNetworkAccess = info.hasLocalNetworkAccess || internetConnected == VARIANT_TRUE;
            if (internetConnected == VARIANT_TRUE)
            {
                info.connectivityLevel = GB_SystemNetworkConnectivityLevel::InternetAccess;
            }
        }
    }

    void FillNetworkCostInfoFromNativeCost(const DWORD cost, GB_SystemNetworkCostInfo& costInfo)
    {
        const DWORD costLevel = cost & 0x0000FFFFu;
        const DWORD costFlags = cost & 0xFFFF0000u;
        costInfo.nativeCostFlags = cost;
        costInfo.isUnrestricted = (costLevel & NLM_CONNECTION_COST_UNRESTRICTED) != 0;
        costInfo.isFixed = (costLevel & NLM_CONNECTION_COST_FIXED) != 0;
        costInfo.isVariable = (costLevel & NLM_CONNECTION_COST_VARIABLE) != 0;
        costInfo.isCostUnknown = !costInfo.isUnrestricted && !costInfo.isFixed && !costInfo.isVariable;
        costInfo.isOverDataLimit = (costFlags & NLM_CONNECTION_COST_OVERDATALIMIT) != 0;
        costInfo.isApproachingDataLimit = (costFlags & NLM_CONNECTION_COST_APPROACHINGDATALIMIT) != 0;
        costInfo.isCongested = (costFlags & NLM_CONNECTION_COST_CONGESTED) != 0;
        costInfo.isRoaming = (costFlags & NLM_CONNECTION_COST_ROAMING) != 0;
        costInfo.costType = costInfo.isUnrestricted ? GB_SystemNetworkCostType::Unrestricted : (costInfo.isFixed ? GB_SystemNetworkCostType::Fixed : (costInfo.isVariable ? GB_SystemNetworkCostType::Variable : GB_SystemNetworkCostType::Unknown));
    }

    GB_SystemResult EnumerateConnectedNetworksFromManager(INetworkListManager* manager, std::vector<GB_SystemConnectedNetworkInfo>& networks)
    {
        networks.clear();
        if (manager == nullptr)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "INetworkListManager::GetNetworks", "Network List Manager 指针为空。");
        }

        ComPtr<IEnumNetworks> enumerator;
        HRESULT hresult = manager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, enumerator.GetAddressOf());
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
                networks.clear();
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

        std::sort(networks.begin(), networks.end(), [](const GB_SystemConnectedNetworkInfo& left, const GB_SystemConnectedNetworkInfo& right)
            {
                return left.networkIdUtf8 < right.networkIdUtf8;
            });
        return GB_SystemResult::Succeeded("GB_SystemNetwork::EnumerateConnectedNetworks");
    }

    GB_SystemResult QueryNetworkCostFromManager(INetworkListManager* manager, GB_SystemNetworkCostInfo& costInfo)
    {
        costInfo = GB_SystemNetworkCostInfo();
        if (manager == nullptr)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "INetworkCostManager::GetCost", "Network List Manager 指针为空。");
        }

        ComPtr<INetworkCostManager> costManager;
        HRESULT hresult = manager->QueryInterface(IID_PPV_ARGS(costManager.GetAddressOf()));
        if (FAILED(hresult))
        {
            hresult = ::CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(costManager.GetAddressOf()));
            if (FAILED(hresult))
            {
                return GB_SystemResult::FromComHResult(static_cast<int32_t>(hresult), "INetworkListManager::QueryInterface(INetworkCostManager)", "获取 Network Cost Manager 接口失败。");
            }
        }

        DWORD cost = NLM_CONNECTION_COST_UNKNOWN;
        hresult = costManager->GetCost(&cost, nullptr);
        if (FAILED(hresult))
        {
            return GB_SystemResult::FromComHResult(static_cast<int32_t>(hresult), "INetworkCostManager::GetCost", "读取当前网络成本失败。");
        }

        FillNetworkCostInfoFromNativeCost(cost, costInfo);
        return GB_SystemResult::Succeeded("GB_SystemNetwork::GetNetworkCost");
    }

    GB_SystemResult EnumerateConnectedNetworksInternal(std::vector<GB_SystemConnectedNetworkInfo>& networks, NlmOverallConnectivityInfo* overallConnectivityInfo)
    {
        networks.clear();
        if (overallConnectivityInfo != nullptr)
        {
            *overallConnectivityInfo = NlmOverallConnectivityInfo();
        }
        GB_ComScope comScope = GB_ComScope::InitializeMta("GB_SystemNetwork::EnumerateConnectedNetworks");
        if (!IsComUsable(comScope))
        {
            return comScope.GetInitializeResult();
        }

        ComPtr<INetworkListManager> manager;
        const HRESULT hresult = ::CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(manager.GetAddressOf()));
        if (FAILED(hresult))
        {
            return GB_SystemResult::FromComHResult(static_cast<int32_t>(hresult), "CoCreateInstance(CLSID_NetworkListManager)", "创建 Network List Manager 失败。");
        }
        if (overallConnectivityInfo != nullptr)
        {
            FillOverallConnectivityInfo(manager.Get(), *overallConnectivityInfo);
        }
        return EnumerateConnectedNetworksFromManager(manager.Get(), networks);
    }

    GB_SystemResult GetNetworkCostInternal(GB_SystemNetworkCostInfo& costInfo)
    {
        costInfo = GB_SystemNetworkCostInfo();
        GB_ComScope comScope = GB_ComScope::InitializeMta("GB_SystemNetwork::GetNetworkCost");
        if (!IsComUsable(comScope))
        {
            return comScope.GetInitializeResult();
        }

        ComPtr<INetworkListManager> manager;
        const HRESULT hresult = ::CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(manager.GetAddressOf()));
        if (FAILED(hresult))
        {
            return GB_SystemResult::FromComHResult(static_cast<int32_t>(hresult), "CoCreateInstance(CLSID_NetworkListManager)", "创建 Network List Manager 失败。");
        }
        return QueryNetworkCostFromManager(manager.Get(), costInfo);
    }

    void CollectNlmSnapshotData(std::vector<GB_SystemConnectedNetworkInfo>& networks, NlmOverallConnectivityInfo& overallConnectivityInfo, GB_SystemNetworkCostInfo& costInfo, bool& hasCostInfo, bool& connectedNetworksSucceeded, std::string& diagnosticMessage)
    {
        networks.clear();
        overallConnectivityInfo = NlmOverallConnectivityInfo();
        costInfo = GB_SystemNetworkCostInfo();
        hasCostInfo = false;
        connectedNetworksSucceeded = false;

        GB_ComScope comScope = GB_ComScope::InitializeMta("GB_SystemNetwork::RefreshSnapshot");
        if (!IsComUsable(comScope))
        {
            AppendDiagnostic(diagnosticMessage, "NLM 查询失败：" + comScope.GetInitializeResult().GetDisplayMessage());
            return;
        }

        ComPtr<INetworkListManager> manager;
        const HRESULT hresult = ::CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(manager.GetAddressOf()));
        if (FAILED(hresult))
        {
            const GB_SystemResult createResult = GB_SystemResult::FromComHResult(static_cast<int32_t>(hresult), "CoCreateInstance(CLSID_NetworkListManager)", "创建 Network List Manager 失败。");
            AppendDiagnostic(diagnosticMessage, "NLM 查询失败：" + createResult.GetDisplayMessage());
            return;
        }

        FillOverallConnectivityInfo(manager.Get(), overallConnectivityInfo);

        const GB_SystemResult networksResult = EnumerateConnectedNetworksFromManager(manager.Get(), networks);
        connectedNetworksSucceeded = networksResult.IsSucceeded();
        if (networksResult.IsFailed())
        {
            AppendDiagnostic(diagnosticMessage, "NLM 网络枚举失败：" + networksResult.GetDisplayMessage());
        }

        const GB_SystemResult costResult = QueryNetworkCostFromManager(manager.Get(), costInfo);
        hasCostInfo = costResult.IsSucceeded();
        if (costResult.IsFailed())
        {
            AppendDiagnostic(diagnosticMessage, "网络成本查询失败：" + costResult.GetDisplayMessage());
        }
    }

    GB_SystemResult QueryInternetAccessByNlm(bool& hasInternetAccess)
    {
        hasInternetAccess = false;
        GB_ComScope comScope = GB_ComScope::InitializeMta("GB_SystemNetwork::HasInternetAccess");
        if (!IsComUsable(comScope))
        {
            return comScope.GetInitializeResult().WithOperationName("GB_SystemNetwork::HasInternetAccess");
        }

        ComPtr<INetworkListManager> manager;
        const HRESULT createResult = ::CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(manager.GetAddressOf()));
        if (FAILED(createResult))
        {
            return GB_SystemResult::FromComHResult(static_cast<int32_t>(createResult), "CoCreateInstance(CLSID_NetworkListManager)", "创建 Network List Manager 失败。").WithOperationName("GB_SystemNetwork::HasInternetAccess");
        }

        VARIANT_BOOL internetConnected = VARIANT_FALSE;
        const HRESULT connectedResult = manager->get_IsConnectedToInternet(&internetConnected);
        if (FAILED(connectedResult))
        {
            return GB_SystemResult::FromComHResult(static_cast<int32_t>(connectedResult), "INetworkListManager::get_IsConnectedToInternet", "读取系统互联网连通性失败。").WithOperationName("GB_SystemNetwork::HasInternetAccess");
        }

        hasInternetAccess = internetConnected == VARIANT_TRUE;
        return GB_SystemResult::Succeeded("GB_SystemNetwork::HasInternetAccess");
    }

    GB_SystemResult BuildSnapshot(GB_SystemNetworkSnapshot& snapshot)
    {
        snapshot = GB_SystemNetworkSnapshot();
        GB_SystemResult result = EnumerateInterfacesInternal(snapshot.interfaces);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemNetwork::RefreshSnapshot");
        }

        NlmOverallConnectivityInfo overallConnectivityInfo;
        bool connectedNetworksSucceeded = false;
        CollectNlmSnapshotData(snapshot.connectedNetworks, overallConnectivityInfo, snapshot.costInfo, snapshot.hasCostInfo, connectedNetworksSucceeded, snapshot.diagnosticMessageUtf8);

        bool hasPrimaryInterface = false;
        uint64_t bestPrimaryScore = std::numeric_limits<uint64_t>::max();
        for (size_t index = 0; index < snapshot.interfaces.size(); index++)
        {
            const GB_SystemNetworkInterfaceInfo& info = snapshot.interfaces[index];
            const bool relevantInterface = IsRelevantNetworkInterface(info);
            snapshot.hasNetworkInterface = snapshot.hasNetworkInterface || relevantInterface;
            const bool connected = IsUsableConnectedInterface(info);
            snapshot.hasConnectedInterface = snapshot.hasConnectedInterface || connected;
            if (connected)
            {
                const uint64_t primaryScore = GetPrimaryInterfaceScore(info);
                if (!hasPrimaryInterface || primaryScore < bestPrimaryScore || (primaryScore == bestPrimaryScore && info.interfaceIdUtf8 < snapshot.primaryInterfaceIdUtf8))
                {
                    hasPrimaryInterface = true;
                    bestPrimaryScore = primaryScore;
                    snapshot.primaryInterfaceIdUtf8 = info.interfaceIdUtf8;
                    snapshot.primaryInterfaceLuid = info.interfaceLuid;
                    snapshot.primaryInterfaceIndex = info.interfaceIndex;
                    snapshot.primaryInterfaceType = info.interfaceType;
                }
            }
        }
        for (size_t index = 0; index < snapshot.connectedNetworks.size(); index++)
        {
            const GB_SystemConnectedNetworkInfo& network = snapshot.connectedNetworks[index];
            if (!network.nameUtf8.empty())
            {
                AppendUnique(snapshot.activeNetworkNamesUtf8, network.nameUtf8);
            }
            snapshot.hasLocalNetworkAccess = snapshot.hasLocalNetworkAccess || network.hasIpv4LocalAccess || network.hasIpv6LocalAccess || network.hasIpv4InternetAccess || network.hasIpv6InternetAccess;
            snapshot.hasInternetAccess = snapshot.hasInternetAccess || network.hasIpv4InternetAccess || network.hasIpv6InternetAccess;
        }
        if (overallConnectivityInfo.hasInfo)
        {
            snapshot.hasLocalNetworkAccess = snapshot.hasLocalNetworkAccess || overallConnectivityInfo.hasLocalNetworkAccess;
            snapshot.hasInternetAccess = snapshot.hasInternetAccess || overallConnectivityInfo.hasInternetAccess;
        }
        std::sort(snapshot.activeNetworkNamesUtf8.begin(), snapshot.activeNetworkNamesUtf8.end());
        if (!connectedNetworksSucceeded && !overallConnectivityInfo.hasInfo)
        {
            snapshot.hasLocalNetworkAccess = snapshot.hasConnectedInterface;
        }
        if (snapshot.hasInternetAccess)
        {
            snapshot.connectivityLevel = GB_SystemNetworkConnectivityLevel::InternetAccess;
        }
        else if (snapshot.hasLocalNetworkAccess)
        {
            snapshot.connectivityLevel = GB_SystemNetworkConnectivityLevel::LocalAccess;
        }
        else if (snapshot.hasConnectedInterface)
        {
            snapshot.connectivityLevel = GB_SystemNetworkConnectivityLevel::InterfaceOnly;
        }
        else
        {
            snapshot.connectivityLevel = GB_SystemNetworkConnectivityLevel::Disconnected;
        }
        snapshot.isMetered = snapshot.hasCostInfo && (snapshot.costInfo.isFixed || snapshot.costInfo.isVariable || snapshot.costInfo.isOverDataLimit || snapshot.costInfo.isApproachingDataLimit || snapshot.costInfo.isRoaming);
        snapshot.isRoaming = snapshot.hasCostInfo && snapshot.costInfo.isRoaming;
        snapshot.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
        return GB_SystemResult::Succeeded("GB_SystemNetwork::RefreshSnapshot", snapshot.diagnosticMessageUtf8.empty() ? std::string() : "已生成网络快照，部分可选信息不可用。");
    }

    bool TryReadCachedSnapshot(const uint64_t maxCacheAgeMilliseconds, GB_SystemNetworkSnapshot& snapshot)
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

    GB_SystemResult RefreshSnapshotAndCache(GB_SystemNetworkSnapshot& snapshot)
    {
        GB_SystemNetworkSnapshot refreshedSnapshot;
        GB_SystemResult result;
        try
        {
            result = BuildSnapshot(refreshedSnapshot);
        }
        catch (const std::bad_alloc&)
        {
            result = GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::RefreshSnapshot", "生成网络快照时内存不足。");
        }
        catch (const std::exception& exception)
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

GB_SystemResult GB_SystemNetwork::GetSnapshot(GB_SystemNetworkSnapshot& snapshot, const bool forceRefresh, const uint64_t maxCacheAgeMilliseconds)
{
#if !defined(_WIN32)
    snapshot = GB_SystemNetworkSnapshot();
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::GetSnapshot");
#else
    if (!forceRefresh && maxCacheAgeMilliseconds > 0 && TryReadCachedSnapshot(maxCacheAgeMilliseconds, snapshot))
    {
        return GB_SystemResult::Succeeded("GB_SystemNetwork::GetSnapshot");
    }
    std::lock_guard<std::mutex> refreshLock(snapshotRefreshMutex);
    if (!forceRefresh && maxCacheAgeMilliseconds > 0 && TryReadCachedSnapshot(maxCacheAgeMilliseconds, snapshot))
    {
        return GB_SystemResult::Succeeded("GB_SystemNetwork::GetSnapshot");
    }
    return RefreshSnapshotAndCache(snapshot);
#endif
}

GB_SystemResult GB_SystemNetwork::RefreshSnapshot(GB_SystemNetworkSnapshot& snapshot)
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

GB_SystemResult GB_SystemNetwork::EnumerateInterfaces(std::vector<GB_SystemNetworkInterfaceInfo>& interfaces)
{
#if !defined(_WIN32)
    interfaces.clear();
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::EnumerateInterfaces");
#else
    try
    {
        return EnumerateInterfacesInternal(interfaces);
    }
    catch (const std::bad_alloc&)
    {
        interfaces.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::EnumerateInterfaces", "枚举网络接口时内存不足。");
    }
    catch (const std::exception& exception)
    {
        interfaces.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::EnumerateInterfaces", std::string("枚举网络接口时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::EnumerateConnectedNetworks(std::vector<GB_SystemConnectedNetworkInfo>& networks)
{
#if !defined(_WIN32)
    networks.clear();
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::EnumerateConnectedNetworks");
#else
    try
    {
        return EnumerateConnectedNetworksInternal(networks, nullptr);
    }
    catch (const std::bad_alloc&)
    {
        networks.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::EnumerateConnectedNetworks", "枚举 NLM 网络时内存不足。");
    }
    catch (const std::exception& exception)
    {
        networks.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::EnumerateConnectedNetworks", std::string("枚举 NLM 网络时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::GetNetworkCost(GB_SystemNetworkCostInfo& costInfo)
{
#if !defined(_WIN32)
    costInfo = GB_SystemNetworkCostInfo();
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::GetNetworkCost");
#else
    try
    {
        return GetNetworkCostInternal(costInfo);
    }
    catch (const std::bad_alloc&)
    {
        costInfo = GB_SystemNetworkCostInfo();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::GetNetworkCost", "读取网络成本时内存不足。");
    }
    catch (const std::exception& exception)
    {
        costInfo = GB_SystemNetworkCostInfo();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::GetNetworkCost", std::string("读取网络成本时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::HasInternetAccess(bool& hasInternetAccess)
{
    hasInternetAccess = false;
#if !defined(_WIN32)
    return MakeUnsupportedPlatformResult("GB_SystemNetwork::HasInternetAccess");
#else
    try
    {
        GB_SystemResult directResult = QueryInternetAccessByNlm(hasInternetAccess);
        if (directResult.IsSucceeded())
        {
            return directResult;
        }

        GB_SystemNetworkSnapshot snapshot;
        GB_SystemResult snapshotResult = GetSnapshot(snapshot);
        if (snapshotResult.IsSucceeded())
        {
            hasInternetAccess = snapshot.hasInternetAccess;
            if (!snapshot.diagnosticMessageUtf8.empty())
            {
                return GB_SystemResult::Succeeded("GB_SystemNetwork::HasInternetAccess", snapshot.diagnosticMessageUtf8);
            }
            return GB_SystemResult::Succeeded("GB_SystemNetwork::HasInternetAccess");
        }

        return snapshotResult.WithOperationName("GB_SystemNetwork::HasInternetAccess");
    }
    catch (const std::bad_alloc&)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::HasInternetAccess", "判断互联网连通性时内存不足。");
    }
    catch (const std::exception& exception)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::HasInternetAccess", std::string("判断互联网连通性时发生异常：") + exception.what());
    }
#endif
}

namespace
{
#if defined(_WIN32)
    struct WlanOperationContext
    {
        std::mutex mutex;
        std::condition_variable condition;
        GUID interfaceGuid = {};
        bool hasTargetProfileName = false;
        std::wstring targetProfileName;
        bool scanComplete = false;
        bool scanFailed = false;
        bool connectionComplete = false;
        bool connectionFailed = false;
        bool disconnected = false;
        bool hasConnectionNotificationProfileName = false;
        std::wstring connectionNotificationProfileName;
        WLAN_REASON_CODE reasonCode = WLAN_REASON_CODE_SUCCESS;
    };

    struct WlanOperationState
    {
        bool scanComplete = false;
        bool scanFailed = false;
        bool connectionComplete = false;
        bool connectionFailed = false;
        bool disconnected = false;
        bool hasConnectionNotificationProfileName = false;
        std::wstring connectionNotificationProfileName;
        WLAN_REASON_CODE reasonCode = WLAN_REASON_CODE_SUCCESS;
    };

    WlanOperationState CopyWlanOperationState(WlanOperationContext& context)
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        WlanOperationState state;
        state.scanComplete = context.scanComplete;
        state.scanFailed = context.scanFailed;
        state.connectionComplete = context.connectionComplete;
        state.connectionFailed = context.connectionFailed;
        state.disconnected = context.disconnected;
        state.hasConnectionNotificationProfileName = context.hasConnectionNotificationProfileName;
        state.connectionNotificationProfileName = context.connectionNotificationProfileName;
        state.reasonCode = context.reasonCode;
        return state;
    }

    void WINAPI WlanOperationCallback(PWLAN_NOTIFICATION_DATA notificationData, PVOID contextPointer)
    {
        if (notificationData == nullptr || contextPointer == nullptr || notificationData->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM)
        {
            return;
        }
        WlanOperationContext* context = static_cast<WlanOperationContext*>(contextPointer);
        const GUID interfaceGuid = context->interfaceGuid;
        if (!::IsEqualGUID(notificationData->InterfaceGuid, interfaceGuid))
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
                    context->reasonCode = *static_cast<const WLAN_REASON_CODE*>(notificationData->pData);
                }
                notify = true;
                break;
            case wlan_notification_acm_connection_complete:
                if (notificationData->pData != nullptr && notificationData->dwDataSize >= sizeof(WLAN_CONNECTION_NOTIFICATION_DATA))
                {
                    const WLAN_CONNECTION_NOTIFICATION_DATA* connectionData = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA*>(notificationData->pData);
                    const std::wstring notificationProfileName = connectionData->strProfileName;
                    if (context->hasTargetProfileName && !notificationProfileName.empty() && ::CompareStringOrdinal(notificationProfileName.c_str(), -1, context->targetProfileName.c_str(), -1, FALSE) != CSTR_EQUAL)
                    {
                        break;
                    }
                    context->hasConnectionNotificationProfileName = true;
                    context->connectionNotificationProfileName = notificationProfileName;
                    context->reasonCode = connectionData->wlanReasonCode;
                    context->connectionFailed = context->reasonCode != WLAN_REASON_CODE_SUCCESS;
                }
                context->connectionComplete = true;
                notify = true;
                break;
            case wlan_notification_acm_connection_attempt_fail:
                if (notificationData->pData != nullptr && notificationData->dwDataSize >= sizeof(WLAN_CONNECTION_NOTIFICATION_DATA))
                {
                    const WLAN_CONNECTION_NOTIFICATION_DATA* connectionData = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA*>(notificationData->pData);
                    const std::wstring notificationProfileName = connectionData->strProfileName;
                    if (context->hasTargetProfileName && !notificationProfileName.empty() && ::CompareStringOrdinal(notificationProfileName.c_str(), -1, context->targetProfileName.c_str(), -1, FALSE) != CSTR_EQUAL)
                    {
                        break;
                    }
                    context->hasConnectionNotificationProfileName = true;
                    context->connectionNotificationProfileName = notificationProfileName;
                    context->reasonCode = connectionData->wlanReasonCode;
                }
                context->connectionFailed = true;
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

    GB_SystemResult ValidateWaitOptions(const uint32_t timeoutMilliseconds, const uint32_t cancellationPollMilliseconds, const std::string& operationName)
    {
        if (timeoutMilliseconds == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "timeoutMilliseconds 必须大于 0。");
        }
        if (cancellationPollMilliseconds == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "cancellationPollMilliseconds 必须大于 0。");
        }
        if (timeoutMilliseconds > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) || cancellationPollMilliseconds > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "等待时间参数过大，可能导致底层计时溢出。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    bool IsSameOrdinal(const std::wstring& left, const std::wstring& right)
    {
        return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, FALSE) == CSTR_EQUAL;
    }

    bool IsSameProfileNameUtf8(const std::string& profileNameUtf8, const std::wstring& expectedProfileNameWide)
    {
        return IsSameOrdinal(GB_Utf8ToWString(profileNameUtf8), expectedProfileNameWide);
    }

    bool IsSameProfileNameWide(const std::wstring& profileNameWide, const std::wstring& expectedProfileNameWide)
    {
        return IsSameOrdinal(profileNameWide, expectedProfileNameWide);
    }

    void SortWifiBssEntries(std::vector<GB_SystemWifiBssInfo>& bssEntries)
    {
        std::sort(bssEntries.begin(), bssEntries.end(), [](const GB_SystemWifiBssInfo& left, const GB_SystemWifiBssInfo& right)
            {
                if (left.rssiDbm != right.rssiDbm)
                {
                    return left.rssiDbm > right.rssiDbm;
                }
                if (left.channelNumber != right.channelNumber)
                {
                    return left.channelNumber < right.channelNumber;
                }
                return left.bssidUtf8 < right.bssidUtf8;
            });
    }

    void SortWifiNetworks(std::vector<GB_SystemWifiNetworkInfo>& networks)
    {
        std::sort(networks.begin(), networks.end(), [](const GB_SystemWifiNetworkInfo& left, const GB_SystemWifiNetworkInfo& right)
            {
                if (left.isConnected != right.isConnected)
                {
                    return left.isConnected;
                }
                if (left.isConnectable != right.isConnectable)
                {
                    return left.isConnectable;
                }
                if (left.signalQuality != right.signalQuality)
                {
                    return left.signalQuality > right.signalQuality;
                }
                if (left.ssidUtf8 != right.ssidUtf8)
                {
                    return left.ssidUtf8 < right.ssidUtf8;
                }
                if (left.ssidHexUtf8 != right.ssidHexUtf8)
                {
                    return left.ssidHexUtf8 < right.ssidHexUtf8;
                }
                if (left.securityType != right.securityType)
                {
                    return static_cast<uint16_t>(left.securityType) < static_cast<uint16_t>(right.securityType);
                }
                return left.profileNameUtf8 < right.profileNameUtf8;
            });
    }

    template <typename Predicate>
    GB_SystemResult WaitForWlanCondition(WlanOperationContext& context, const uint32_t timeoutMilliseconds, const uint32_t pollMilliseconds, const std::atomic<bool>* cancellationFlag, const Predicate& predicate, const std::string& operationName)
    {
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        std::unique_lock<std::mutex> lock(context.mutex);
        while (!predicate())
        {
            if (IsCancellationRequested(cancellationFlag))
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

    GB_SystemResult MakeWlanReasonFailure(const WLAN_REASON_CODE reasonCode, const std::string& operationName, const std::string& message)
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

    bool IsNoCurrentWifiConnectionError(const DWORD errorCode)
    {
        if (errorCode == ERROR_INVALID_STATE || errorCode == ERROR_NOT_FOUND || errorCode == ERROR_NOT_READY)
        {
            return true;
        }
#ifdef ERROR_NDIS_DOT11_POWER_STATE_INVALID
        if (errorCode == ERROR_NDIS_DOT11_POWER_STATE_INVALID)
        {
            return true;
        }
#endif
        return false;
    }

    GB_SystemResult QueryCurrentWifiConnection(HANDLE wlanHandle, const GUID& interfaceGuid, GB_SystemWifiConnectionInfo& connectionInfo, bool& found)
    {
        connectionInfo = GB_SystemWifiConnectionInfo();
        connectionInfo.interfaceIdUtf8 = GuidToUtf8(interfaceGuid);
        found = false;
        DWORD dataSize = 0;
        WLAN_OPCODE_VALUE_TYPE opcodeValueType = wlan_opcode_value_type_invalid;
        WLAN_CONNECTION_ATTRIBUTES* rawAttributes = nullptr;
        const DWORD errorCode = ::WlanQueryInterface(wlanHandle, &interfaceGuid, wlan_intf_opcode_current_connection, nullptr, &dataSize, reinterpret_cast<PVOID*>(&rawAttributes), &opcodeValueType);
        WlanMemoryScope<WLAN_CONNECTION_ATTRIBUTES> attributes;
        attributes.Reset(rawAttributes);
        if (IsNoCurrentWifiConnectionError(errorCode))
        {
            return GB_SystemResult::Succeeded("WlanQueryInterface(CurrentConnection)");
        }
        if (errorCode != ERROR_SUCCESS)
        {
            return MakeWifiWin32ErrorResult(errorCode, "WlanQueryInterface(CurrentConnection)", "查询当前 Wi-Fi 连接失败。");
        }
        if (attributes.Get() == nullptr)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "WlanQueryInterface(CurrentConnection)", "Native Wi-Fi 返回了空连接信息。");
        }

        const WLAN_CONNECTION_ATTRIBUTES& value = *attributes.Get();
        found = true;
        connectionInfo.profileNameUtf8 = GB_WStringToUtf8(value.strProfileName);
        connectionInfo.ssidUtf8 = SsidToUtf8(value.wlanAssociationAttributes.dot11Ssid);
        connectionInfo.ssidHexUtf8 = SsidToHex(value.wlanAssociationAttributes.dot11Ssid);
        connectionInfo.bssidUtf8 = BytesToHex(value.wlanAssociationAttributes.dot11Bssid, sizeof(value.wlanAssociationAttributes.dot11Bssid), ':');
        connectionInfo.interfaceState = MapWifiInterfaceState(value.isState);
        connectionInfo.signalQuality = ClampSignalQuality(value.wlanAssociationAttributes.wlanSignalQuality);
        connectionInfo.rssiDbm = SignalQualityToRssiDbm(value.wlanAssociationAttributes.wlanSignalQuality);
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

    GB_SystemResult QueryWifiInterfaceState(HANDLE wlanHandle, const GUID& interfaceGuid, GB_SystemWifiInterfaceState& interfaceState)
    {
        interfaceState = GB_SystemWifiInterfaceState::Unknown;
        DWORD dataSize = 0;
        WLAN_OPCODE_VALUE_TYPE opcodeValueType = wlan_opcode_value_type_invalid;
        WLAN_INTERFACE_STATE* rawState = nullptr;
        const DWORD errorCode = ::WlanQueryInterface(wlanHandle, &interfaceGuid, wlan_intf_opcode_interface_state, nullptr, &dataSize, reinterpret_cast<PVOID*>(&rawState), &opcodeValueType);
        WlanMemoryScope<WLAN_INTERFACE_STATE> state;
        state.Reset(rawState);
        if (errorCode != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(errorCode, "WlanQueryInterface(InterfaceState)", "查询 Wi-Fi 接口状态失败。");
        }
        if (state.Get() == nullptr)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "WlanQueryInterface(InterfaceState)", "Native Wi-Fi 返回了空接口状态。");
        }
        interfaceState = MapWifiInterfaceState(*state.Get());
        return GB_SystemResult::Succeeded("WlanQueryInterface(InterfaceState)");
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

    GB_SystemResult ValidateInterfaceId(const std::string& interfaceIdUtf8, GUID& interfaceGuid, const std::string& operationName)
    {
        try
        {
            if (!TryParseGuid(interfaceIdUtf8, interfaceGuid))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "interfaceIdUtf8 不是有效的 Wi-Fi 接口 GUID。");
            }
        }
        catch (const std::exception& exception)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, operationName, std::string("接口 GUID 的 UTF-8 转换失败：") + exception.what());
        }
        return GB_SystemResult::Succeeded(operationName);
    }
#endif
} // namespace

GB_SystemResult GB_SystemNetwork::EnumerateWifiInterfaces(std::vector<GB_SystemWifiInterfaceInfo>& interfaces)
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
        WLAN_INTERFACE_INFO_LIST* rawList = nullptr;
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
        interfaces.reserve(list.Get()->dwNumberOfItems);
        for (DWORD index = 0; index < list.Get()->dwNumberOfItems; index++)
        {
            const WLAN_INTERFACE_INFO& rawInfo = list.Get()->InterfaceInfo[index];
            GB_SystemWifiInterfaceInfo info;
            info.interfaceIdUtf8 = GuidToUtf8(rawInfo.InterfaceGuid);
            info.descriptionUtf8 = GB_WStringToUtf8(rawInfo.strInterfaceDescription);
            info.interfaceState = MapWifiInterfaceState(rawInfo.isState);
            DWORD dataSize = 0;
            WLAN_OPCODE_VALUE_TYPE opcodeValueType = wlan_opcode_value_type_invalid;
            WLAN_RADIO_STATE* rawRadioState = nullptr;
            const DWORD radioError = ::WlanQueryInterface(wlanHandle.Get(), &rawInfo.InterfaceGuid, wlan_intf_opcode_radio_state, nullptr, &dataSize, reinterpret_cast<PVOID*>(&rawRadioState), &opcodeValueType);
            WlanMemoryScope<WLAN_RADIO_STATE> radioState;
            radioState.Reset(rawRadioState);
            if (radioError == ERROR_SUCCESS && radioState.Get() != nullptr)
            {
                info.hasRadioState = true;
                for (DWORD phyIndex = 0; phyIndex < radioState.Get()->dwNumberOfPhys; phyIndex++)
                {
                    const WLAN_PHY_RADIO_STATE& phyState = radioState.Get()->PhyRadioState[phyIndex];
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
        std::sort(interfaces.begin(), interfaces.end(), [](const GB_SystemWifiInterfaceInfo& left, const GB_SystemWifiInterfaceInfo& right)
            {
                if (left.descriptionUtf8 != right.descriptionUtf8)
                {
                    return left.descriptionUtf8 < right.descriptionUtf8;
                }
                return left.interfaceIdUtf8 < right.interfaceIdUtf8;
            });
        return GB_SystemResult::Succeeded("GB_SystemNetwork::EnumerateWifiInterfaces");
    }
    catch (const std::bad_alloc&)
    {
        interfaces.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::EnumerateWifiInterfaces", "枚举 Wi-Fi 接口时内存不足。");
    }
    catch (const std::exception& exception)
    {
        interfaces.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::EnumerateWifiInterfaces", std::string("枚举 Wi-Fi 接口时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::GetCurrentWifiConnection(const std::string& interfaceIdUtf8, GB_SystemWifiConnectionInfo& connectionInfo, bool& found)
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
    catch (const std::bad_alloc&)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::GetCurrentWifiConnection", "查询当前 Wi-Fi 连接时内存不足。");
    }
    catch (const std::exception& exception)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::GetCurrentWifiConnection", std::string("查询当前 Wi-Fi 连接时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::ScanWifiNetworks(const std::string& interfaceIdUtf8, std::vector<GB_SystemWifiNetworkInfo>& networks, const GB_SystemWifiScanOptions& options)
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
    const std::string normalizedInterfaceIdUtf8 = GuidToUtf8(interfaceGuid);
    if (IsCancellationRequested(options.cancellationFlag))
    {
        return MakeCancelledBeforeNativeCallResult("GB_SystemNetwork::ScanWifiNetworks", "Wi-Fi 扫描在提交给系统前已被调用方取消。");
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
        std::string scanDiagnosticMessageUtf8;
        if (options.requestFreshScan)
        {
            WlanNotificationScope notificationScope;
            DWORD errorCode = notificationScope.Register(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_ACM, &WlanOperationCallback, &context, false, &previousSource);
            if (errorCode != ERROR_SUCCESS)
            {
                return MakeWin32ErrorResult(errorCode, "WlanRegisterNotification", "注册 Wi-Fi 扫描完成通知失败。");
            }
            errorCode = ::WlanScan(wlanHandle.Get(), &interfaceGuid, nullptr, nullptr, nullptr);
            if (errorCode != ERROR_SUCCESS)
            {
                return MakeWifiWin32ErrorResult(errorCode, "WlanScan", "请求 Wi-Fi 扫描失败。");
            }
            result = WaitForWlanCondition(context, options.timeoutMilliseconds, options.cancellationPollMilliseconds, options.cancellationFlag, [&context]()
                {
                    return context.scanComplete || context.scanFailed;
                }, "GB_SystemNetwork::ScanWifiNetworks");
            notificationScope.Reset();
            const WlanOperationState operationState = CopyWlanOperationState(context);
            if (result.IsFailed())
            {
                if (result.errorCode != GB_SystemErrorCode::Timeout)
                {
                    return result;
                }
                scanDiagnosticMessageUtf8 = "等待 Wi-Fi 扫描完成通知超时，已改为读取系统当前可用网络缓存。";
            }
            if (operationState.scanFailed)
            {
                return MakeWlanReasonFailure(operationState.reasonCode, "GB_SystemNetwork::ScanWifiNetworks", "Wi-Fi 扫描完成通知报告失败。");
            }
        }

        WLAN_AVAILABLE_NETWORK_LIST* rawList = nullptr;
        const DWORD listError = ::WlanGetAvailableNetworkList(wlanHandle.Get(), &interfaceGuid, WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_ADHOC_PROFILES | WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_MANUAL_HIDDEN_PROFILES, nullptr, &rawList);
        WlanMemoryScope<WLAN_AVAILABLE_NETWORK_LIST> list;
        list.Reset(rawList);
        if (listError != ERROR_SUCCESS)
        {
            return MakeWifiWin32ErrorResult(listError, "WlanGetAvailableNetworkList", "读取 Wi-Fi 扫描结果失败。");
        }
        if (list.Get() != nullptr)
        {
            networks.reserve(list.Get()->dwNumberOfItems);
        }
        for (DWORD index = 0; list.Get() != nullptr && index < list.Get()->dwNumberOfItems; index++)
        {
            const WLAN_AVAILABLE_NETWORK& rawNetwork = list.Get()->Network[index];
            GB_SystemWifiNetworkInfo info;
            info.interfaceIdUtf8 = normalizedInterfaceIdUtf8;
            info.profileNameUtf8 = GB_WStringToUtf8(rawNetwork.strProfileName);
            info.ssidUtf8 = SsidToUtf8(rawNetwork.dot11Ssid);
            info.ssidHexUtf8 = SsidToHex(rawNetwork.dot11Ssid);
            info.signalQuality = ClampSignalQuality(rawNetwork.wlanSignalQuality);
            info.strongestRssiDbm = SignalQualityToRssiDbm(rawNetwork.wlanSignalQuality);
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
            WLAN_BSS_LIST* rawBssList = nullptr;
            const DWORD bssError = ::WlanGetNetworkBssList(wlanHandle.Get(), &interfaceGuid, nullptr, dot11_BSS_type_any, FALSE, nullptr, &rawBssList);
            WlanMemoryScope<WLAN_BSS_LIST> bssList;
            bssList.Reset(rawBssList);
            if (bssError != ERROR_SUCCESS)
            {
                SortWifiNetworks(networks);
                GB_SystemResult detailResult = MakeWifiWin32ErrorResult(bssError, "WlanGetNetworkBssList", "读取 Wi-Fi BSS 详情失败。");
                const std::string message = scanDiagnosticMessageUtf8.empty() ? ("已返回可用 Wi-Fi 列表，但 BSS 详情不可用：" + detailResult.GetDisplayMessage()) : (scanDiagnosticMessageUtf8 + " 已返回可用 Wi-Fi 列表，但 BSS 详情不可用：" + detailResult.GetDisplayMessage());
                return GB_SystemResult::Succeeded("GB_SystemNetwork::ScanWifiNetworks", message);
            }

            for (size_t networkIndex = 0; networkIndex < networks.size(); networkIndex++)
            {
                networks[networkIndex].strongestRssiDbm = (std::numeric_limits<int32_t>::min)();
            }

            std::unordered_multimap<std::string, size_t> networkIndexesByBssKey;
            networkIndexesByBssKey.reserve(networks.size());
            for (size_t networkIndex = 0; networkIndex < networks.size(); networkIndex++)
            {
                networkIndexesByBssKey.emplace(MakeBssLookupKey(networks[networkIndex].ssidHexUtf8, networks[networkIndex].bssType), networkIndex);
            }
            for (DWORD bssIndex = 0; bssList.Get() != nullptr && bssIndex < bssList.Get()->dwNumberOfItems; bssIndex++)
            {
                const WLAN_BSS_ENTRY& rawBss = bssList.Get()->wlanBssEntries[bssIndex];
                const std::string ssidHex = SsidToHex(rawBss.dot11Ssid);
                const std::string bssKey = MakeBssLookupKey(ssidHex, static_cast<uint32_t>(rawBss.dot11BssType));
                const std::pair<std::unordered_multimap<std::string, size_t>::iterator, std::unordered_multimap<std::string, size_t>::iterator> range = networkIndexesByBssKey.equal_range(bssKey);
                for (std::unordered_multimap<std::string, size_t>::iterator iter = range.first; iter != range.second; iter++)
                {
                    GB_SystemWifiBssInfo bssInfo;
                    bssInfo.bssidUtf8 = BytesToHex(rawBss.dot11Bssid, sizeof(rawBss.dot11Bssid), ':');
                    bssInfo.rssiDbm = rawBss.lRssi;
                    bssInfo.signalQuality = ClampSignalQuality(rawBss.uLinkQuality);
                    bssInfo.centerFrequencyKhz = rawBss.ulChCenterFrequency;
                    bssInfo.channelNumber = FrequencyToChannel(rawBss.ulChCenterFrequency);
                    bssInfo.phyType = static_cast<uint32_t>(rawBss.dot11BssPhyType);
                    networks[iter->second].bssEntries.push_back(bssInfo);
                    networks[iter->second].strongestRssiDbm = (std::max)(networks[iter->second].strongestRssiDbm, bssInfo.rssiDbm);
                }
            }
            for (size_t networkIndex = 0; networkIndex < networks.size(); networkIndex++)
            {
                if (networks[networkIndex].bssEntries.empty())
                {
                    networks[networkIndex].strongestRssiDbm = SignalQualityToRssiDbm(networks[networkIndex].signalQuality);
                }
                else
                {
                    SortWifiBssEntries(networks[networkIndex].bssEntries);
                }
            }
        }
        SortWifiNetworks(networks);
        return GB_SystemResult::Succeeded("GB_SystemNetwork::ScanWifiNetworks", scanDiagnosticMessageUtf8);
    }
    catch (const std::bad_alloc&)
    {
        networks.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::ScanWifiNetworks", "读取 Wi-Fi 扫描结果时内存不足。");
    }
    catch (const std::exception& exception)
    {
        networks.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::ScanWifiNetworks", std::string("Wi-Fi 扫描时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::EnumerateWifiProfiles(const std::string& interfaceIdUtf8, std::vector<GB_SystemWifiProfileInfo>& profiles)
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
    const std::string normalizedInterfaceIdUtf8 = GuidToUtf8(interfaceGuid);
    try
    {
        WlanHandleScope wlanHandle;
        result = OpenWlanHandle(wlanHandle);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemNetwork::EnumerateWifiProfiles");
        }
        WLAN_PROFILE_INFO_LIST* rawList = nullptr;
        const DWORD errorCode = ::WlanGetProfileList(wlanHandle.Get(), &interfaceGuid, nullptr, &rawList);
        WlanMemoryScope<WLAN_PROFILE_INFO_LIST> list;
        list.Reset(rawList);
        if (errorCode != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(errorCode, "WlanGetProfileList", "枚举已保存 Wi-Fi Profile 失败。");
        }
        if (list.Get() != nullptr)
        {
            profiles.reserve(list.Get()->dwNumberOfItems);
        }
        for (DWORD index = 0; list.Get() != nullptr && index < list.Get()->dwNumberOfItems; index++)
        {
            GB_SystemWifiProfileInfo info;
            info.interfaceIdUtf8 = normalizedInterfaceIdUtf8;
            info.profileNameUtf8 = GB_WStringToUtf8(list.Get()->ProfileInfo[index].strProfileName);
            info.nativeFlags = list.Get()->ProfileInfo[index].dwFlags;
            profiles.push_back(info);
        }
        std::sort(profiles.begin(), profiles.end(), [](const GB_SystemWifiProfileInfo& left, const GB_SystemWifiProfileInfo& right)
            {
                if (left.interfaceIdUtf8 != right.interfaceIdUtf8)
                {
                    return left.interfaceIdUtf8 < right.interfaceIdUtf8;
                }
                return left.profileNameUtf8 < right.profileNameUtf8;
            });
        return GB_SystemResult::Succeeded("GB_SystemNetwork::EnumerateWifiProfiles");
    }
    catch (const std::bad_alloc&)
    {
        profiles.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::EnumerateWifiProfiles", "枚举 Wi-Fi Profile 时内存不足。");
    }
    catch (const std::exception& exception)
    {
        profiles.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::EnumerateWifiProfiles", std::string("枚举 Wi-Fi Profile 时发生异常：") + exception.what());
    }
#endif
}

GB_SystemResult GB_SystemNetwork::ConnectWifiByProfile(const std::string& interfaceIdUtf8, const std::string& profileNameUtf8, const GB_SystemWifiOperationOptions& options)
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
    if (IsCancellationRequested(options.cancellationFlag))
    {
        return MakeCancelledBeforeNativeCallResult("GB_SystemNetwork::ConnectWifiByProfile", "Wi-Fi 连接请求在提交给系统前已被调用方取消。");
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

        GB_SystemWifiConnectionInfo currentConnection;
        bool currentConnectionFound = false;
        const GB_SystemResult currentConnectionResult = QueryCurrentWifiConnection(wlanHandle.Get(), interfaceGuid, currentConnection, currentConnectionFound);
        if (currentConnectionResult.IsSucceeded() && currentConnectionFound && IsSameProfileNameUtf8(currentConnection.profileNameUtf8, profileNameWide))
        {
            return GB_SystemResult::Succeeded("GB_SystemNetwork::ConnectWifiByProfile", "目标 Wi-Fi Profile 已处于连接状态。");
        }

        WlanOperationContext context;
        context.interfaceGuid = interfaceGuid;
        context.hasTargetProfileName = true;
        context.targetProfileName = profileNameWide;
        DWORD previousSource = 0;
        WlanNotificationScope notificationScope;
        DWORD errorCode = notificationScope.Register(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_ACM, &WlanOperationCallback, &context, false, &previousSource);
        if (errorCode != ERROR_SUCCESS)
        {
            return MakeWin32ErrorResult(errorCode, "WlanRegisterNotification", "注册 Wi-Fi 连接结果通知失败。");
        }
        WLAN_CONNECTION_PARAMETERS parameters = {};
        parameters.wlanConnectionMode = wlan_connection_mode_profile;
        parameters.strProfile = profileNameWide.c_str();
        parameters.dot11BssType = dot11_BSS_type_any;
        errorCode = ::WlanConnect(wlanHandle.Get(), &interfaceGuid, &parameters, nullptr);
        if (errorCode != ERROR_SUCCESS)
        {
            return MakeWin32ErrorResult(errorCode, "WlanConnect", "提交 Wi-Fi Profile 连接请求失败。");
        }
        result = WaitForWlanCondition(context, options.timeoutMilliseconds, options.cancellationPollMilliseconds, options.cancellationFlag, [&context]()
            {
                return context.connectionComplete || context.connectionFailed;
            }, "GB_SystemNetwork::ConnectWifiByProfile");
        notificationScope.Reset();
        const WlanOperationState operationState = CopyWlanOperationState(context);
        if (result.IsFailed())
        {
            if (result.errorCode == GB_SystemErrorCode::Timeout)
            {
                GB_SystemWifiConnectionInfo connectionInfo;
                bool found = false;
                const GB_SystemResult queryResult = QueryCurrentWifiConnection(wlanHandle.Get(), interfaceGuid, connectionInfo, found);
                if (queryResult.IsSucceeded() && found && IsSameProfileNameUtf8(connectionInfo.profileNameUtf8, profileNameWide))
                {
                    InvalidateSnapshotCache();
                    return GB_SystemResult::Succeeded("GB_SystemNetwork::ConnectWifiByProfile", "等待 Wi-Fi 连接通知超时，但已确认当前连接为目标 Profile。");
                }
                return result.WithMessage("等待 Wi-Fi 连接通知超时；Native Wi-Fi 连接请求可能仍在系统后台继续，未主动断开当前接口。");
            }
            if (result.errorCode == GB_SystemErrorCode::Cancelled)
            {
                return result.WithMessage("Wi-Fi 连接等待已被调用方取消；Native Wi-Fi 连接请求可能仍在系统后台继续，未主动断开当前接口。");
            }
            return result;
        }
        if (operationState.hasConnectionNotificationProfileName && !IsSameProfileNameWide(operationState.connectionNotificationProfileName, profileNameWide))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_SystemNetwork::ConnectWifiByProfile", "收到 Wi-Fi 连接通知，但通知中的 Profile 与目标 Profile 不一致。");
        }
        if (operationState.connectionFailed || operationState.reasonCode != WLAN_REASON_CODE_SUCCESS)
        {
            return MakeWlanReasonFailure(operationState.reasonCode, "GB_SystemNetwork::ConnectWifiByProfile", "Wi-Fi 连接未达到成功状态。");
        }
        if (operationState.hasConnectionNotificationProfileName)
        {
            InvalidateSnapshotCache();
            return GB_SystemResult::Succeeded("GB_SystemNetwork::ConnectWifiByProfile");
        }

        GB_SystemWifiConnectionInfo connectionInfo;
        bool found = false;
        result = QueryCurrentWifiConnection(wlanHandle.Get(), interfaceGuid, connectionInfo, found);
        if (result.IsFailed())
        {
            if (result.errorCode == GB_SystemErrorCode::PermissionDenied)
            {
                InvalidateSnapshotCache();
                return GB_SystemResult::Succeeded("GB_SystemNetwork::ConnectWifiByProfile", "已收到 Wi-Fi 连接成功通知；由于系统权限限制，无法读取当前连接作二次校验。");
            }
            return result.WithOperationName("GB_SystemNetwork::ConnectWifiByProfile");
        }
        if (!found || !IsSameProfileNameUtf8(connectionInfo.profileNameUtf8, profileNameWide))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "GB_SystemNetwork::ConnectWifiByProfile", "收到连接成功通知后，当前连接 Profile 与目标 Profile 不一致。");
        }
        InvalidateSnapshotCache();
        return GB_SystemResult::Succeeded("GB_SystemNetwork::ConnectWifiByProfile");
    }
    catch (const std::bad_alloc&)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::ConnectWifiByProfile", "连接 Wi-Fi 时内存不足。");
    }
    catch (const std::exception& exception)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, "GB_SystemNetwork::ConnectWifiByProfile", std::string("连接 Wi-Fi 时发生异常：") + exception.what());
    }
#endif
}



GB_SystemResult GB_SystemNetwork::DisconnectWifi(const std::string& interfaceIdUtf8, const GB_SystemWifiOperationOptions& options)
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
    if (IsCancellationRequested(options.cancellationFlag))
    {
        return MakeCancelledBeforeNativeCallResult("GB_SystemNetwork::DisconnectWifi", "Wi-Fi 断开请求在提交给系统前已被调用方取消。");
    }
    try
    {
        WlanHandleScope wlanHandle;
        result = OpenWlanHandle(wlanHandle);
        if (result.IsFailed())
        {
            return result.WithOperationName("GB_SystemNetwork::DisconnectWifi");
        }
        WlanOperationContext context;
        context.interfaceGuid = interfaceGuid;
        DWORD previousSource = 0;
        WlanNotificationScope notificationScope;
        DWORD errorCode = notificationScope.Register(wlanHandle.Get(), WLAN_NOTIFICATION_SOURCE_ACM, &WlanOperationCallback, &context, false, &previousSource);
        if (errorCode != ERROR_SUCCESS)
        {
            return MakeWin32ErrorResult(errorCode, "WlanRegisterNotification", "注册 Wi-Fi 断开通知失败。");
        }
        errorCode = ::WlanDisconnect(wlanHandle.Get(), &interfaceGuid, nullptr);
        if (IsNoCurrentWifiConnectionError(errorCode))
        {
            InvalidateSnapshotCache();
            return GB_SystemResult::Succeeded("GB_SystemNetwork::DisconnectWifi", "目标 Wi-Fi 接口本来就未连接。");
        }
        if (errorCode != ERROR_SUCCESS)
        {
            return MakeWin32ErrorResult(errorCode, "WlanDisconnect", "提交 Wi-Fi 断开请求失败。");
        }
        result = WaitForWlanCondition(context, options.timeoutMilliseconds, options.cancellationPollMilliseconds, options.cancellationFlag, [&context]()
            {
                return context.disconnected;
            }, "GB_SystemNetwork::DisconnectWifi");
        notificationScope.Reset();
        if (result.IsFailed() && result.errorCode == GB_SystemErrorCode::Timeout)
        {
            GB_SystemWifiInterfaceState interfaceState = GB_SystemWifiInterfaceState::Unknown;
            const GB_SystemResult queryResult = QueryWifiInterfaceState(wlanHandle.Get(), interfaceGuid, interfaceState);
            if (queryResult.IsSucceeded() && interfaceState == GB_SystemWifiInterfaceState::Disconnected)
            {
                InvalidateSnapshotCache();
                return GB_SystemResult::Succeeded("GB_SystemNetwork::DisconnectWifi", "等待 Wi-Fi 断开通知超时，但已确认目标接口处于未连接状态。");
            }
        }
        if (result.IsSucceeded())
        {
            InvalidateSnapshotCache();
        }
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetwork::DisconnectWifi", "断开 Wi-Fi 时内存不足。");
    }
    catch (const std::exception& exception)
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
    case GB_SystemWifiSecurityType::Owe:
        return "Owe";
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

    uint64_t HashString(uint64_t hash, const std::string& value)
    {
        for (size_t index = 0; index < value.size(); index++)
        {
            hash = HashCombine(hash, static_cast<unsigned char>(value[index]));
        }
        return hash;
    }

    uint64_t HashInterfaceStructure(const GB_SystemNetworkSnapshot& snapshot)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t index = 0; index < snapshot.interfaces.size(); index++)
        {
            const GB_SystemNetworkInterfaceInfo& info = snapshot.interfaces[index];
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

    uint64_t HashAddresses(const GB_SystemNetworkSnapshot& snapshot)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t index = 0; index < snapshot.interfaces.size(); index++)
        {
            const GB_SystemNetworkInterfaceInfo& info = snapshot.interfaces[index];
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

    uint64_t HashRoutes(const GB_SystemNetworkSnapshot& snapshot)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t index = 0; index < snapshot.interfaces.size(); index++)
        {
            const GB_SystemNetworkInterfaceInfo& info = snapshot.interfaces[index];
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

    uint64_t HashConnectivity(const GB_SystemNetworkSnapshot& snapshot)
    {
        uint64_t hash = HashCombine(0, static_cast<uint64_t>(snapshot.connectivityLevel));
        hash = HashCombine(hash, snapshot.hasInternetAccess ? 1 : 0);
        hash = HashCombine(hash, snapshot.hasLocalNetworkAccess ? 1 : 0);
        hash = HashString(hash, snapshot.primaryInterfaceIdUtf8);
        return hash;
    }

    uint64_t HashNetworkNames(const GB_SystemNetworkSnapshot& snapshot)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t index = 0; index < snapshot.activeNetworkNamesUtf8.size(); index++)
        {
            hash = HashString(hash, snapshot.activeNetworkNamesUtf8[index]);
        }
        return hash;
    }

    uint64_t HashCost(const GB_SystemNetworkSnapshot& snapshot)
    {
        uint64_t hash = HashCombine(0, snapshot.hasCostInfo ? 1 : 0);
        return HashCombine(hash, snapshot.hasCostInfo ? snapshot.costInfo.nativeCostFlags : 0);
    }
} // namespace

class GB_SystemNetworkWatcher::Impl final
{
public:
    explicit Impl(const GB_SystemNetworkWatcherOptions& inputOptions)
        : options(inputOptions), eventDispatcher(GB_EventDispatcher::MakeQueuedOptions(inputOptions.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropOldest, "GB_SystemNetworkWatcher"))
    {
        callbackSetupResult = eventDispatcher.SubscribeAll([this](const GB_Event& event)
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
        if (options.maxDispatchQueueSize == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, "GB_SystemNetworkWatcher::Start", "maxDispatchQueueSize 必须大于 0。");
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
            if (::WlanRegisterNotification(newWlanHandle, WLAN_NOTIFICATION_SOURCE_ACM, FALSE, &Impl::WifiCallback, this, nullptr, nullptr) != ERROR_SUCCESS)
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
            pendingReasons = options.emitInitialSnapshot ? InitialReason : 0;
        }
        try
        {
            workerThread = std::thread(&Impl::WorkerMain, this);
        }
        catch (const std::system_error& exception)
        {
            operationLock.unlock();
            (void)Stop();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, "GB_SystemNetworkWatcher::Start", std::string("创建网络监听工作线程失败：") + exception.what());
        }
        catch (const std::bad_alloc&)
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
            (void)::WlanRegisterNotification(localWlanHandle, WLAN_NOTIFICATION_SOURCE_NONE, FALSE, nullptr, nullptr, nullptr, nullptr);
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
    void SetCallback(const NetworkEventCallback& callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        networkEventCallback = callback;
    }
    GB_EventDispatcher& GetDispatcher()
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
            static_cast<Impl*>(context)->Signal(InterfaceReason);
        }
    }
    static VOID CALLBACK AddressCallback(PVOID context, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE)
    {
        if (context != nullptr)
        {
            static_cast<Impl*>(context)->Signal(AddressReason);
        }
    }
    static VOID CALLBACK RouteCallback(PVOID context, PMIB_IPFORWARD_ROW2, MIB_NOTIFICATION_TYPE)
    {
        if (context != nullptr)
        {
            static_cast<Impl*>(context)->Signal(RouteReason);
        }
    }
    static VOID WINAPI WifiCallback(PWLAN_NOTIFICATION_DATA, PVOID context)
    {
        if (context != nullptr)
        {
            static_cast<Impl*>(context)->Signal(WifiReason);
        }
    }
#endif

    void WorkerMain()
    {
        const bool hasPeriodicRefresh = options.periodicRefreshMilliseconds != 0;
        std::chrono::steady_clock::time_point nextPeriodic = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.periodicRefreshMilliseconds);
        for (;;)
        {
            uint32_t reasons = 0;
            {
                std::unique_lock<std::mutex> lock(stateMutex);
                if (hasPeriodicRefresh)
                {
                    condition.wait_until(lock, nextPeriodic, [this]()
                        {
                            return stopRequested || pendingReasons != 0;
                        });
                }
                else
                {
                    condition.wait(lock, [this]()
                        {
                            return stopRequested || pendingReasons != 0;
                        });
                }
                if (stopRequested)
                {
                    break;
                }
                if (pendingReasons == 0 && hasPeriodicRefresh)
                {
                    pendingReasons = PeriodicReason;
                }
                reasons = pendingReasons;
                pendingReasons = 0;
            }
            if ((reasons & InitialReason) == 0 && options.debounceMilliseconds != 0)
            {
                const std::chrono::steady_clock::time_point debounceDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.debounceMilliseconds);
                std::unique_lock<std::mutex> lock(stateMutex);
                while (!stopRequested && std::chrono::steady_clock::now() < debounceDeadline)
                {
                    condition.wait_until(lock, debounceDeadline);
                }
                if (stopRequested)
                {
                    break;
                }
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
                const bool hasSnapshotChange = networkEvent.interfacesChanged || networkEvent.addressesChanged || networkEvent.routesChanged || networkEvent.connectivityChanged || networkEvent.networkNamesChanged || networkEvent.costChanged;
                if ((reasons & InitialReason) != 0)
                {
                    networkEvent.eventType = GB_SystemNetworkEventType::InitialSnapshot;
                }
                else if ((reasons & WifiReason) != 0)
                {
                    networkEvent.eventType = GB_SystemNetworkEventType::WifiChanged;
                }
                else if (reasons == InterfaceReason)
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
                else if (reasons == PeriodicReason && !hasSnapshotChange)
                {
                    networkEvent.eventType = GB_SystemNetworkEventType::PeriodicRefresh;
                }
                else
                {
                    networkEvent.eventType = GB_SystemNetworkEventType::SnapshotChanged;
                }
                networkEvent.eventName = "SystemNetwork." + GB_SystemNetwork::GetEventTypeName(networkEvent.eventType);
                currentSnapshot = refreshedSnapshot;
                const bool shouldPublish = (reasons & InitialReason) != 0 || ((reasons & PeriodicReason) != 0 && options.emitUnchangedPeriodicRefresh) || hasSnapshotChange || networkEvent.wifiChanged;
                if (shouldPublish)
                {
                    (void)eventDispatcher.Post(networkEvent.eventName, GB_Variant(networkEvent), networkEvent.sourceName);
                }
            }
            catch (const std::bad_alloc&)
            {
                refreshFailureCount.fetch_add(1, std::memory_order_relaxed);
            }
            catch (const std::exception&)
            {
                refreshFailureCount.fetch_add(1, std::memory_order_relaxed);
            }
            if (hasPeriodicRefresh)
            {
                nextPeriodic = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.periodicRefreshMilliseconds);
            }
        }
    }

    void DispatchTypedCallback(const GB_Event& event)
    {
        const GB_SystemNetworkEvent* networkEvent = event.payload.AnyCast<GB_SystemNetworkEvent>();
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
    std::atomic<uint64_t> coalescedNativeEventCount{ 0 };
    std::atomic<uint64_t> refreshFailureCount{ 0 };
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
GB_SystemNetworkWatcher::GB_SystemNetworkWatcher(const GB_SystemNetworkWatcherOptions& options)
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
void GB_SystemNetworkWatcher::SetNetworkEventCallback(const NetworkEventCallback& callback)
{
    impl->SetCallback(callback);
}
GB_EventDispatcher& GB_SystemNetworkWatcher::GetEventDispatcher()
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
