#include "Desktop/GB_SystemNetwork.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
void Require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void RequireSucceeded(const GB_SystemResult &result, const std::string &operation)
{
    Require(result.IsSucceeded(), operation + ": " + result.ToString());
}
} // namespace

int RunGB_SystemNetworkTests()
{
    try
    {
        std::vector<GB_SystemNetworkInterfaceInfo> interfaces;
        RequireSucceeded(GB_SystemNetwork::EnumerateInterfaces(interfaces), "EnumerateInterfaces");
        std::set<std::string> interfaceIds;
        for (size_t index = 0; index < interfaces.size(); index++)
        {
            const GB_SystemNetworkInterfaceInfo &info = interfaces[index];
            Require(!info.interfaceIdUtf8.empty(), "Network interface ID must not be empty");
            Require(interfaceIds.insert(info.interfaceIdUtf8).second, "Network interface IDs must be unique");
            for (size_t addressIndex = 0; addressIndex < info.unicastAddresses.size(); addressIndex++)
            {
                const GB_SystemNetworkAddress &address = info.unicastAddresses[addressIndex];
                Require(!address.addressUtf8.empty(), "Network address must not be empty");
                Require(address.family == GB_SystemNetworkAddressFamily::IPv4 || address.family == GB_SystemNetworkAddressFamily::IPv6, "Network address family must be known");
                Require(address.prefixLength <= (address.family == GB_SystemNetworkAddressFamily::IPv4 ? 32 : 128), "Network prefix length is invalid");
            }
        }

        GB_SystemNetworkSnapshot firstSnapshot;
        GB_SystemNetworkSnapshot cachedSnapshot;
        RequireSucceeded(GB_SystemNetwork::RefreshSnapshot(firstSnapshot), "RefreshSnapshot");
        RequireSucceeded(GB_SystemNetwork::GetSnapshot(cachedSnapshot), "GetSnapshot");
        Require(firstSnapshot.timestampMilliseconds == cachedSnapshot.timestampMilliseconds, "GetSnapshot should reuse a fresh cache entry");
        Require(firstSnapshot.interfaces.size() == cachedSnapshot.interfaces.size(), "Cached snapshot interface count mismatch");
        if (!firstSnapshot.primaryInterfaceIdUtf8.empty())
        {
            const bool primaryFound = std::find_if(firstSnapshot.interfaces.begin(), firstSnapshot.interfaces.end(), [&firstSnapshot](const GB_SystemNetworkInterfaceInfo &info)
                {
                    return info.interfaceIdUtf8 == firstSnapshot.primaryInterfaceIdUtf8 && info.isDefaultRouteCandidate;
                }) != firstSnapshot.interfaces.end();
            Require(primaryFound, "Primary interface must refer to a default-route candidate");
        }

        std::vector<GB_SystemConnectedNetworkInfo> connectedNetworks;
        RequireSucceeded(GB_SystemNetwork::EnumerateConnectedNetworks(connectedNetworks), "EnumerateConnectedNetworks");
        GB_SystemNetworkCostInfo costInfo;
        RequireSucceeded(GB_SystemNetwork::GetNetworkCost(costInfo), "GetNetworkCost");

        bool hasInternetAccess = false;
        RequireSucceeded(GB_SystemNetwork::HasInternetAccess(hasInternetAccess), "HasInternetAccess");
        Require(hasInternetAccess == cachedSnapshot.hasInternetAccess, "Internet access result must match cached snapshot");

        std::vector<GB_SystemWifiInterfaceInfo> wifiInterfaces;
        const GB_SystemResult wifiResult = GB_SystemNetwork::EnumerateWifiInterfaces(wifiInterfaces);
        Require(wifiResult.IsSucceeded() || wifiResult.errorCode == GB_SystemErrorCode::InvalidState || wifiResult.errorCode == GB_SystemErrorCode::PermissionDenied, "Wi-Fi enumeration returned an unexpected result: " + wifiResult.ToString());
        for (size_t index = 0; index < wifiInterfaces.size(); index++)
        {
            Require(!wifiInterfaces[index].interfaceIdUtf8.empty(), "Wi-Fi interface GUID must not be empty");
        }
        std::vector<GB_SystemWifiNetworkInfo> invalidScanNetworks;
        GB_SystemWifiScanOptions invalidScanOptions;
        invalidScanOptions.timeoutMilliseconds = 0;
        Require(GB_SystemNetwork::ScanWifiNetworks("invalid-guid", invalidScanNetworks, invalidScanOptions).errorCode == GB_SystemErrorCode::InvalidArgument, "Invalid Wi-Fi scan parameters should be rejected");

        GB_SystemNetworkWatcherOptions watcherOptions;
        watcherOptions.debounceMilliseconds = 50;
        watcherOptions.periodicRefreshMilliseconds = 0;
        watcherOptions.emitInitialSnapshot = true;
        GB_SystemNetworkWatcher watcher(watcherOptions);
        RequireSucceeded(watcher.Start(), "Network watcher Start");
        Require(watcher.IsRunning(), "Network watcher should be running");
        RequireSucceeded(watcher.Start(), "Network watcher repeated Start");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        RequireSucceeded(watcher.Stop(), "Network watcher Stop");
        Require(!watcher.IsRunning(), "Network watcher should be stopped");
        RequireSucceeded(watcher.Stop(), "Network watcher repeated Stop");

        std::cout << "GB_SystemNetwork tests passed. Interfaces=" << interfaces.size() << ", WifiInterfaces=" << wifiInterfaces.size() << std::endl;
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "GB_SystemNetwork tests failed: " << exception.what() << std::endl;
        return 1;
    }
}
