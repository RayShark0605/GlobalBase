#include "GB_SystemBluetooth.h"
#include "GB_SystemDevice.h"
#include "../GB_Utf8String.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bluetoothapis.h>
#ifdef _MSC_VER
#  pragma comment(lib, "Bthprops.lib")
#endif
#endif

namespace
{
    static bool ContainsNullCharacter(const std::string& text)
    {
        return text.find('\0') != std::string::npos;
    }

    static char ToLowerAsciiChar(const char character)
    {
        if (character >= 'A' && character <= 'Z')
        {
            return static_cast<char>(character - 'A' + 'a');
        }

        return character;
    }

    static bool ContainsAsciiNoCase(const std::string& text, const std::string& needle)
    {
        if (needle.empty())
        {
            return true;
        }
        if (text.size() < needle.size())
        {
            return false;
        }

        return std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](const char left, const char right)
            {
                return ToLowerAsciiChar(left) == ToLowerAsciiChar(right);
            }) != text.end();
    }

    static bool StartsWithAsciiNoCase(const std::string& text, const std::string& prefix)
    {
        if (prefix.size() > text.size())
        {
            return false;
        }

        for (size_t index = 0; index < prefix.size(); index++)
        {
            if (ToLowerAsciiChar(text[index]) != ToLowerAsciiChar(prefix[index]))
            {
                return false;
            }
        }

        return true;
    }

    static int HexValue(const char character)
    {
        if (character >= '0' && character <= '9')
        {
            return static_cast<int>(character - '0');
        }
        if (character >= 'a' && character <= 'f')
        {
            return static_cast<int>(character - 'a') + 10;
        }
        if (character >= 'A' && character <= 'F')
        {
            return static_cast<int>(character - 'A') + 10;
        }

        return -1;
    }

    static bool IsAsciiWhitespace(const char character)
    {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n' || character == '\f' || character == '\v';
    }

    static std::string TrimAsciiWhitespace(const std::string& text)
    {
        size_t beginIndex = 0;
        while (beginIndex < text.size() && IsAsciiWhitespace(text[beginIndex]))
        {
            beginIndex++;
        }

        size_t endIndex = text.size();
        while (endIndex > beginIndex && IsAsciiWhitespace(text[endIndex - 1]))
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    static bool IsHexPairAt(const std::string& text, const size_t index)
    {
        return index + 1 < text.size() && HexValue(text[index]) >= 0 && HexValue(text[index + 1]) >= 0;
    }

    static bool TryParseBluetoothAddressValue(const std::string& address, uint64_t& addressValue)
    {
        addressValue = 0;
        if (address.empty() || ContainsNullCharacter(address))
        {
            return false;
        }

        const std::string trimmedAddress = TrimAsciiWhitespace(address);
        if (trimmedAddress.empty())
        {
            return false;
        }

        char digits[13] = {};
        if (trimmedAddress.size() == 12)
        {
            for (size_t index = 0; index < trimmedAddress.size(); index++)
            {
                if (HexValue(trimmedAddress[index]) < 0)
                {
                    return false;
                }

                digits[index] = trimmedAddress[index];
            }
        }
        else if (trimmedAddress.size() == 17)
        {
            const char separator = trimmedAddress[2];
            if (separator != ':' && separator != '-')
            {
                return false;
            }

            size_t digitIndex = 0;
            for (size_t groupIndex = 0; groupIndex < 6; groupIndex++)
            {
                const size_t textIndex = groupIndex * 3;
                if (!IsHexPairAt(trimmedAddress, textIndex))
                {
                    return false;
                }
                if (groupIndex < 5 && trimmedAddress[textIndex + 2] != separator)
                {
                    return false;
                }

                digits[digitIndex] = trimmedAddress[textIndex];
                digitIndex++;
                digits[digitIndex] = trimmedAddress[textIndex + 1];
                digitIndex++;
            }
        }
        else
        {
            return false;
        }

        uint64_t parsedValue = 0;
        for (size_t index = 0; index < 12; index++)
        {
            const int value = HexValue(digits[index]);
            if (value < 0)
            {
                return false;
            }

            parsedValue = (parsedValue << 4) | static_cast<uint64_t>(value);
        }

        if (parsedValue == 0)
        {
            return false;
        }

        addressValue = parsedValue;
        return true;
    }

    static std::string FormatBluetoothAddressValue(const uint64_t addressValue)
    {
        char buffer[18] = {};
        std::snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
            static_cast<unsigned int>((addressValue >> 40) & 0xffu),
            static_cast<unsigned int>((addressValue >> 32) & 0xffu),
            static_cast<unsigned int>((addressValue >> 24) & 0xffu),
            static_cast<unsigned int>((addressValue >> 16) & 0xffu),
            static_cast<unsigned int>((addressValue >> 8) & 0xffu),
            static_cast<unsigned int>(addressValue & 0xffu));
        return std::string(buffer);
    }

    static std::string NormalizeBluetoothAddress(const std::string& address)
    {
        uint64_t addressValue = 0;
        if (!TryParseBluetoothAddressValue(address, addressValue))
        {
            return std::string();
        }

        return FormatBluetoothAddressValue(addressValue);
    }

    static bool IsValidBluetoothDeviceKind(const GB_BluetoothDeviceKind deviceKind)
    {
        switch (deviceKind)
        {
        case GB_BluetoothDeviceKind::Unknown:
        case GB_BluetoothDeviceKind::Classic:
        case GB_BluetoothDeviceKind::LowEnergy:
        case GB_BluetoothDeviceKind::DualMode:
            return true;
        default:
            break;
        }

        return false;
    }

    static bool IsValidPairStatus(const GB_BluetoothPairStatus pairStatus)
    {
        switch (pairStatus)
        {
        case GB_BluetoothPairStatus::Unknown:
        case GB_BluetoothPairStatus::Unpaired:
        case GB_BluetoothPairStatus::Paired:
        case GB_BluetoothPairStatus::CannotPair:
            return true;
        default:
            break;
        }

        return false;
    }

    static bool IsValidConnectionStatus(const GB_BluetoothConnectionStatus connectionStatus)
    {
        switch (connectionStatus)
        {
        case GB_BluetoothConnectionStatus::Unknown:
        case GB_BluetoothConnectionStatus::Disconnected:
        case GB_BluetoothConnectionStatus::Connected:
            return true;
        default:
            break;
        }

        return false;
    }

    static bool IsValidBluetoothEventType(const GB_BluetoothEventType eventType)
    {
        switch (eventType)
        {
        case GB_BluetoothEventType::Unknown:
        case GB_BluetoothEventType::DeviceAdded:
        case GB_BluetoothEventType::DeviceUpdated:
        case GB_BluetoothEventType::DeviceRemoved:
        case GB_BluetoothEventType::RadioChanged:
            return true;
        default:
            break;
        }

        return false;
    }

    static std::string GetBluetoothEventName(const GB_BluetoothEventType eventType)
    {
        return std::string("SystemBluetooth.") + GB_SystemBluetooth::GetEventTypeName(eventType);
    }

    static GB_SystemResult MakeUnsupportedBleResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, u8"当前 C++14 实现未接入 Windows Runtime BLE 枚举；需要后续通过 WinRT ABI 或独立 C++17 实现单元补充，不能用经典蓝牙 API 伪装 BLE 结果。");
    }

#if defined(_WIN32)
    class BluetoothRadioFindScope
    {
    public:
        explicit BluetoothRadioFindScope(HBLUETOOTH_RADIO_FIND findHandle) : findHandle(findHandle)
        {
        }

        ~BluetoothRadioFindScope()
        {
            if (findHandle != nullptr)
            {
                (void)::BluetoothFindRadioClose(findHandle);
                findHandle = nullptr;
            }
        }

        BluetoothRadioFindScope(const BluetoothRadioFindScope&) = delete;
        BluetoothRadioFindScope& operator=(const BluetoothRadioFindScope&) = delete;

        bool IsValid() const
        {
            return findHandle != nullptr;
        }

        HBLUETOOTH_RADIO_FIND Get() const
        {
            return findHandle;
        }

    private:
        HBLUETOOTH_RADIO_FIND findHandle = nullptr;
    };

    class BluetoothDeviceFindScope
    {
    public:
        explicit BluetoothDeviceFindScope(HBLUETOOTH_DEVICE_FIND findHandle) : findHandle(findHandle)
        {
        }

        ~BluetoothDeviceFindScope()
        {
            if (findHandle != nullptr)
            {
                (void)::BluetoothFindDeviceClose(findHandle);
                findHandle = nullptr;
            }
        }

        BluetoothDeviceFindScope(const BluetoothDeviceFindScope&) = delete;
        BluetoothDeviceFindScope& operator=(const BluetoothDeviceFindScope&) = delete;

        bool IsValid() const
        {
            return findHandle != nullptr;
        }

        HBLUETOOTH_DEVICE_FIND Get() const
        {
            return findHandle;
        }

    private:
        HBLUETOOTH_DEVICE_FIND findHandle = nullptr;
    };

    class BluetoothRadioHandleEntry
    {
    public:
        BluetoothRadioHandleEntry()
        {
        }

        BluetoothRadioHandleEntry(HANDLE radioHandle, const GB_BluetoothRadioInfo& radioInfo) : radioHandle(radioHandle), radioInfo(radioInfo)
        {
        }

        ~BluetoothRadioHandleEntry()
        {
            Close();
        }

        BluetoothRadioHandleEntry(const BluetoothRadioHandleEntry&) = delete;
        BluetoothRadioHandleEntry& operator=(const BluetoothRadioHandleEntry&) = delete;

        BluetoothRadioHandleEntry(BluetoothRadioHandleEntry&& other) noexcept
        {
            MoveFrom(other);
        }

        BluetoothRadioHandleEntry& operator=(BluetoothRadioHandleEntry&& other) noexcept
        {
            if (this != &other)
            {
                Close();
                MoveFrom(other);
            }

            return *this;
        }

        HANDLE GetHandle() const
        {
            return radioHandle;
        }

        const GB_BluetoothRadioInfo& GetInfo() const
        {
            return radioInfo;
        }

        bool IsValid() const
        {
            return radioHandle != nullptr && radioHandle != INVALID_HANDLE_VALUE;
        }

    private:
        void Close() noexcept
        {
            if (radioHandle != nullptr && radioHandle != INVALID_HANDLE_VALUE)
            {
                (void)::CloseHandle(radioHandle);
                radioHandle = nullptr;
            }
        }

        void MoveFrom(BluetoothRadioHandleEntry& other) noexcept
        {
            radioHandle = other.radioHandle;
            radioInfo = other.radioInfo;
            other.radioHandle = nullptr;
            other.radioInfo = GB_BluetoothRadioInfo();
        }

    private:
        HANDLE radioHandle = nullptr;
        GB_BluetoothRadioInfo radioInfo;
    };

    static BLUETOOTH_ADDRESS MakeBluetoothAddress(const uint64_t addressValue)
    {
        BLUETOOTH_ADDRESS address = {};
        address.ullLong = addressValue;
        return address;
    }

    static std::string FormatBluetoothAddress(const BLUETOOTH_ADDRESS& address)
    {
        return FormatBluetoothAddressValue(static_cast<uint64_t>(address.ullLong));
    }

    static std::string WideStringToUtf8Safe(const std::wstring& text)
    {
        try
        {
            return GB_WStringToUtf8(text);
        }
        catch (...)
        {
            return std::string();
        }
    }

    static std::string WideNullTerminatedStringToUtf8(const wchar_t* text)
    {
        if (text == nullptr || text[0] == L'\0')
        {
            return std::string();
        }

        return WideStringToUtf8Safe(std::wstring(text));
    }

    static std::string GuidToString(const GUID& guid)
    {
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", static_cast<unsigned long>(guid.Data1), static_cast<unsigned int>(guid.Data2), static_cast<unsigned int>(guid.Data3), static_cast<unsigned int>(guid.Data4[0]), static_cast<unsigned int>(guid.Data4[1]), static_cast<unsigned int>(guid.Data4[2]), static_cast<unsigned int>(guid.Data4[3]), static_cast<unsigned int>(guid.Data4[4]), static_cast<unsigned int>(guid.Data4[5]), static_cast<unsigned int>(guid.Data4[6]), static_cast<unsigned int>(guid.Data4[7]));
        return std::string(buffer);
    }

    static std::string SystemTimeToLocalString(const SYSTEMTIME& systemTime)
    {
        if (systemTime.wYear == 0)
        {
            return std::string();
        }

        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u.%03u", static_cast<unsigned int>(systemTime.wYear), static_cast<unsigned int>(systemTime.wMonth), static_cast<unsigned int>(systemTime.wDay), static_cast<unsigned int>(systemTime.wHour), static_cast<unsigned int>(systemTime.wMinute), static_cast<unsigned int>(systemTime.wSecond), static_cast<unsigned int>(systemTime.wMilliseconds));
        return std::string(buffer);
    }

    static GB_SystemResult MapBluetoothWin32Result(const DWORD win32Error, const std::string& operationName, const std::string& message)
    {
        if (win32Error == ERROR_SUCCESS)
        {
            return GB_SystemResult::Succeeded(operationName, message);
        }
        if (win32Error == ERROR_CANCELLED)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, operationName, message);
        }
        if (win32Error == ERROR_NOT_FOUND || win32Error == ERROR_NO_MORE_ITEMS)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, message);
        }
        if (win32Error == ERROR_ACCESS_DENIED)
        {
            return GB_SystemResult::FromWin32Error(win32Error, operationName, message).WithMessage(message);
        }
        if (win32Error == ERROR_BUSY)
        {
            return GB_SystemResult::FromWin32Error(win32Error, operationName, message).WithMessage(message);
        }

        return GB_SystemResult::FromWin32Error(win32Error, operationName, message);
    }

    static GB_SystemResult BuildRadioInfo(HANDLE radioHandle, GB_BluetoothRadioInfo& radio)
    {
        radio = GB_BluetoothRadioInfo();
        if (radioHandle == nullptr || radioHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::GetRadios", u8"蓝牙无线电句柄无效。");
        }

        BLUETOOTH_RADIO_INFO nativeRadioInfo = {};
        nativeRadioInfo.dwSize = sizeof(nativeRadioInfo);
        const DWORD radioResult = ::BluetoothGetRadioInfo(radioHandle, &nativeRadioInfo);
        if (radioResult != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(radioResult, u8"GB_SystemBluetooth::GetRadios", u8"BluetoothGetRadioInfo 读取蓝牙无线电信息失败。");
        }

        radio.address = FormatBluetoothAddress(nativeRadioInfo.address);
        radio.radioId = radio.address;
        radio.nativeDeviceId = radio.address;
        radio.name = WideNullTerminatedStringToUtf8(nativeRadioInfo.szName);
        radio.classOfDevice = nativeRadioInfo.ulClassofDevice;
        radio.manufacturer = nativeRadioInfo.manufacturer;
        radio.isClassicSupported = true;
        radio.isLowEnergySupported = false;
        radio.isConnectable = ::BluetoothIsConnectable(radioHandle) != FALSE;
        radio.isDiscoverable = ::BluetoothIsDiscoverable(radioHandle) != FALSE;
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetRadios");
    }

    static GB_SystemResult BuildBluetoothBooleanApiFailureResult(const std::string& operationName, const std::string& message)
    {
        const DWORD lastError = ::GetLastError();
        if (lastError != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(static_cast<uint32_t>(lastError), operationName, message);
        }

        return GB_SystemResult::Failed(GB_SystemErrorCode::NativeApiFailed, operationName, message);
    }

    static GB_SystemResult SetRadioIncomingConnections(HANDLE radioHandle, const bool enabled, const std::string& operationName)
    {
        if (radioHandle == nullptr || radioHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"蓝牙无线电句柄无效。");
        }

        const bool currentEnabled = ::BluetoothIsConnectable(radioHandle) != FALSE;
        if (currentEnabled == enabled)
        {
            return GB_SystemResult::Succeeded(operationName);
        }

        ::SetLastError(ERROR_SUCCESS);
        if (::BluetoothEnableIncomingConnections(radioHandle, enabled ? TRUE : FALSE) == FALSE)
        {
            if ((::BluetoothIsConnectable(radioHandle) != FALSE) == enabled)
            {
                return GB_SystemResult::Succeeded(operationName);
            }

            return BuildBluetoothBooleanApiFailureResult(operationName, enabled ? u8"BluetoothEnableIncomingConnections 打开入站连接失败。" : u8"BluetoothEnableIncomingConnections 关闭入站连接失败。");
        }

        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult SetRadioDiscovery(HANDLE radioHandle, const bool enabled, const std::string& operationName)
    {
        if (radioHandle == nullptr || radioHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"蓝牙无线电句柄无效。");
        }

        const bool currentEnabled = ::BluetoothIsDiscoverable(radioHandle) != FALSE;
        if (currentEnabled == enabled)
        {
            return GB_SystemResult::Succeeded(operationName);
        }

        ::SetLastError(ERROR_SUCCESS);
        if (::BluetoothEnableDiscovery(radioHandle, enabled ? TRUE : FALSE) == FALSE)
        {
            if ((::BluetoothIsDiscoverable(radioHandle) != FALSE) == enabled)
            {
                return GB_SystemResult::Succeeded(operationName);
            }

            return BuildBluetoothBooleanApiFailureResult(operationName, enabled ? u8"BluetoothEnableDiscovery 打开可发现性失败。" : u8"BluetoothEnableDiscovery 关闭可发现性失败。");
        }

        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult EnumerateRadioHandles(std::vector<BluetoothRadioHandleEntry>& radioHandles)
    {
        radioHandles.clear();

        BLUETOOTH_FIND_RADIO_PARAMS radioParams = {};
        radioParams.dwSize = sizeof(radioParams);

        HANDLE firstRadioHandle = nullptr;
        HBLUETOOTH_RADIO_FIND findHandle = ::BluetoothFindFirstRadio(&radioParams, &firstRadioHandle);
        if (findHandle == nullptr)
        {
            const DWORD lastError = ::GetLastError();
            if (lastError == ERROR_SUCCESS || lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_SERVICE_DOES_NOT_EXIST)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetRadios", u8"当前系统未发现蓝牙无线电。");
            }

            return GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemBluetooth::GetRadios", u8"BluetoothFindFirstRadio 枚举蓝牙无线电失败。");
        }

        BluetoothRadioFindScope findScope(findHandle);
        HANDLE currentRadioHandle = firstRadioHandle;
        while (currentRadioHandle != nullptr && currentRadioHandle != INVALID_HANDLE_VALUE)
        {
            GB_BluetoothRadioInfo radioInfo;
            GB_SystemResult infoResult = BuildRadioInfo(currentRadioHandle, radioInfo);
            if (infoResult.IsFailed())
            {
                (void)::CloseHandle(currentRadioHandle);
                radioHandles.clear();
                return infoResult;
            }

            try
            {
                radioHandles.push_back(BluetoothRadioHandleEntry(currentRadioHandle, radioInfo));
            }
            catch (...)
            {
                (void)::CloseHandle(currentRadioHandle);
                radioHandles.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetRadios", u8"保存蓝牙无线电句柄时内存不足。");
            }

            currentRadioHandle = nullptr;
            if (::BluetoothFindNextRadio(findScope.Get(), &currentRadioHandle) == FALSE)
            {
                const DWORD nextError = ::GetLastError();
                if (nextError == ERROR_SUCCESS || nextError == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }

                radioHandles.clear();
                return GB_SystemResult::FromWin32Error(nextError, u8"GB_SystemBluetooth::GetRadios", u8"BluetoothFindNextRadio 枚举下一个蓝牙无线电失败。");
            }
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetRadios");
    }

    static GB_SystemResult FindTargetRadioHandles(const std::string& radioAddress, std::vector<BluetoothRadioHandleEntry>& radioHandles, std::vector<BluetoothRadioHandleEntry*>& targetRadioHandles, const std::string& operationName)
    {
        radioHandles.clear();
        targetRadioHandles.clear();

        std::string targetRadioAddress;
        if (!radioAddress.empty())
        {
            targetRadioAddress = NormalizeBluetoothAddress(radioAddress);
            if (targetRadioAddress.empty())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"radioAddress 不是有效蓝牙地址。");
            }
        }

        GB_SystemResult radioResult = EnumerateRadioHandles(radioHandles);
        if (radioResult.IsFailed())
        {
            return radioResult.WithOperationName(operationName);
        }
        if (radioHandles.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"当前系统未发现蓝牙无线电。");
        }

        try
        {
            targetRadioHandles.reserve(radioHandles.size());
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"保存目标蓝牙无线电句柄时内存不足。");
        }

        for (size_t index = 0; index < radioHandles.size(); index++)
        {
            if (!targetRadioAddress.empty() && radioHandles[index].GetInfo().address != targetRadioAddress)
            {
                continue;
            }

            targetRadioHandles.push_back(&radioHandles[index]);
        }

        if (targetRadioHandles.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"未找到指定 radioAddress 对应的本机蓝牙无线电。");
        }

        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult ValidateClassicDeviceQueryOptions(const GB_BluetoothClassicDeviceQueryOptions& options)
    {
        if (!options.includeAuthenticated && !options.includeRemembered && !options.includeUnknown && !options.includeConnected)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::GetClassicDevices", u8"经典蓝牙查询至少需要启用一种返回类型。");
        }
        if (!options.radioAddress.empty() && !GB_SystemBluetooth::IsValidAddress(options.radioAddress))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::GetClassicDevices", u8"radioAddress 不是有效蓝牙地址。");
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetClassicDevices");
    }

    static UCHAR NormalizeInquiryTimeoutMultiplier(const uint8_t timeoutMultiplier)
    {
        if (timeoutMultiplier == 0)
        {
            return 1;
        }
        if (timeoutMultiplier > 48)
        {
            return 48;
        }

        return static_cast<UCHAR>(timeoutMultiplier);
    }

    static BLUETOOTH_DEVICE_SEARCH_PARAMS BuildSearchParams(HANDLE radioHandle, const GB_BluetoothClassicDeviceQueryOptions& options)
    {
        BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams = {};
        searchParams.dwSize = sizeof(searchParams);
        searchParams.fReturnAuthenticated = options.includeAuthenticated ? TRUE : FALSE;
        searchParams.fReturnRemembered = options.includeRemembered ? TRUE : FALSE;
        searchParams.fReturnUnknown = options.includeUnknown ? TRUE : FALSE;
        searchParams.fReturnConnected = options.includeConnected ? TRUE : FALSE;
        searchParams.fIssueInquiry = options.requestFreshInquiry ? TRUE : FALSE;
        searchParams.cTimeoutMultiplier = options.requestFreshInquiry ? NormalizeInquiryTimeoutMultiplier(options.inquiryTimeoutMultiplier) : 0;
        searchParams.hRadio = radioHandle;
        return searchParams;
    }

    static void FillClassOfDeviceFields(GB_BluetoothDeviceInfo& device)
    {
        device.serviceClass = (device.classOfDevice >> 13) & 0x7ffu;
        device.majorDeviceClass = (device.classOfDevice >> 8) & 0x1fu;
        device.minorDeviceClass = (device.classOfDevice >> 2) & 0x3fu;
    }

    static GB_BluetoothDeviceInfo ConvertDeviceInfo(const BLUETOOTH_DEVICE_INFO& nativeDeviceInfo, const GB_BluetoothRadioInfo& radioInfo)
    {
        GB_BluetoothDeviceInfo device;
        device.address = FormatBluetoothAddress(nativeDeviceInfo.Address);
        device.deviceId = device.address;
        device.nativeDeviceId = device.address;
        device.radioId = radioInfo.radioId;
        device.radioAddress = radioInfo.address;
        device.name = WideNullTerminatedStringToUtf8(nativeDeviceInfo.szName);
        device.deviceKind = GB_BluetoothDeviceKind::Classic;
        device.isRemembered = nativeDeviceInfo.fRemembered != FALSE;
        device.isAuthenticated = nativeDeviceInfo.fAuthenticated != FALSE;
        device.isConnected = nativeDeviceInfo.fConnected != FALSE;
        device.pairStatus = device.isAuthenticated ? GB_BluetoothPairStatus::Paired : GB_BluetoothPairStatus::Unpaired;
        device.connectionStatus = device.isConnected ? GB_BluetoothConnectionStatus::Connected : GB_BluetoothConnectionStatus::Disconnected;
        device.isClassicSupported = true;
        device.isLowEnergySupported = false;
        device.classOfDevice = nativeDeviceInfo.ulClassofDevice;
        device.lastSeenTimeLocal = SystemTimeToLocalString(nativeDeviceInfo.stLastSeen);
        device.lastUsedTimeLocal = SystemTimeToLocalString(nativeDeviceInfo.stLastUsed);
        device.sourceName = u8"Win32Bluetooth";
        FillClassOfDeviceFields(device);
        return device;
    }

    static GB_SystemResult ReadInstalledServices(HANDLE radioHandle, const BLUETOOTH_DEVICE_INFO& nativeDeviceInfo, std::vector<std::string>& serviceGuids)
    {
        serviceGuids.clear();
        if (radioHandle == nullptr || radioHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::ReadInstalledServices", u8"蓝牙无线电句柄无效。");
        }

        DWORD serviceCount = 0;
        DWORD serviceResult = ::BluetoothEnumerateInstalledServices(radioHandle, &nativeDeviceInfo, &serviceCount, nullptr);
        if (serviceResult == ERROR_NOT_FOUND || serviceResult == ERROR_SERVICE_DOES_NOT_EXIST)
        {
            return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::ReadInstalledServices");
        }
        if (serviceResult != ERROR_SUCCESS && serviceResult != ERROR_MORE_DATA)
        {
            return GB_SystemResult::FromWin32Error(serviceResult, u8"GB_SystemBluetooth::ReadInstalledServices", u8"BluetoothEnumerateInstalledServices 查询经典蓝牙设备已安装服务数量失败。");
        }
        if (serviceCount == 0)
        {
            return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::ReadInstalledServices");
        }
        if (serviceCount > 1024)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, u8"GB_SystemBluetooth::ReadInstalledServices", u8"系统返回的经典蓝牙服务数量异常，已拒绝分配过大的服务 GUID 缓冲区。");
        }

        try
        {
            for (int retryIndex = 0; retryIndex < 2; retryIndex++)
            {
                std::vector<GUID> nativeServiceGuids(static_cast<size_t>(serviceCount));
                DWORD returnedServiceCount = serviceCount;
                serviceResult = ::BluetoothEnumerateInstalledServices(radioHandle, &nativeDeviceInfo, &returnedServiceCount, nativeServiceGuids.data());
                if (serviceResult == ERROR_SUCCESS)
                {
                    serviceGuids.reserve(static_cast<size_t>(returnedServiceCount));
                    const DWORD validServiceCount = returnedServiceCount < static_cast<DWORD>(nativeServiceGuids.size()) ? returnedServiceCount : static_cast<DWORD>(nativeServiceGuids.size());
                    for (DWORD index = 0; index < validServiceCount; index++)
                    {
                        serviceGuids.push_back(GuidToString(nativeServiceGuids[index]));
                    }

                    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::ReadInstalledServices");
                }

                if (serviceResult == ERROR_MORE_DATA && returnedServiceCount > serviceCount && returnedServiceCount <= 1024)
                {
                    serviceCount = returnedServiceCount;
                    continue;
                }

                if (serviceResult == ERROR_NOT_FOUND || serviceResult == ERROR_SERVICE_DOES_NOT_EXIST)
                {
                    serviceGuids.clear();
                    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::ReadInstalledServices");
                }

                serviceGuids.clear();
                return GB_SystemResult::FromWin32Error(serviceResult, u8"GB_SystemBluetooth::ReadInstalledServices", u8"BluetoothEnumerateInstalledServices 读取经典蓝牙设备已安装服务失败。");
            }
        }
        catch (...)
        {
            serviceGuids.clear();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::ReadInstalledServices", u8"保存经典蓝牙服务 GUID 时内存不足。");
        }

        serviceGuids.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, u8"GB_SystemBluetooth::ReadInstalledServices", u8"BluetoothEnumerateInstalledServices 多次返回更多数据，服务 GUID 数量不稳定。");
    }

    static GB_SystemResult TryGetNativeClassicDeviceInfoByAddress(HANDLE radioHandle, const GB_BluetoothRadioInfo& radioInfo, const std::string& targetAddress, const bool includeInstalledServices, BLUETOOTH_DEVICE_INFO& nativeDeviceInfo, GB_BluetoothDeviceInfo* publicDevice, bool& found)
    {
        found = false;
        nativeDeviceInfo = {};
        nativeDeviceInfo.dwSize = sizeof(nativeDeviceInfo);

        uint64_t addressValue = 0;
        if (!TryParseBluetoothAddressValue(targetAddress, addressValue))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::TryGetNativeClassicDeviceInfoByAddress", u8"蓝牙设备地址格式无效。");
        }

        nativeDeviceInfo.Address = MakeBluetoothAddress(addressValue);
        const DWORD getResult = ::BluetoothGetDeviceInfo(radioHandle, &nativeDeviceInfo);
        if (getResult == ERROR_NOT_FOUND || getResult == ERROR_FILE_NOT_FOUND || getResult == ERROR_NO_MORE_ITEMS)
        {
            return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::TryGetNativeClassicDeviceInfoByAddress");
        }
        if (getResult != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(getResult, u8"GB_SystemBluetooth::TryGetNativeClassicDeviceInfoByAddress", u8"BluetoothGetDeviceInfo 按地址读取经典蓝牙设备信息失败。");
        }

        if (publicDevice != nullptr)
        {
            *publicDevice = ConvertDeviceInfo(nativeDeviceInfo, radioInfo);
            if (includeInstalledServices)
            {
                GB_SystemResult serviceResult = ReadInstalledServices(radioHandle, nativeDeviceInfo, publicDevice->installedServiceGuids);
                if (serviceResult.IsFailed())
                {
                    return serviceResult.WithOperationName(u8"GB_SystemBluetooth::TryGetNativeClassicDeviceInfoByAddress");
                }
            }
        }

        found = true;
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::TryGetNativeClassicDeviceInfoByAddress");
    }

    static GB_SystemResult FindNativeClassicDeviceForRadio(HANDLE radioHandle, const GB_BluetoothRadioInfo& radioInfo, const GB_BluetoothClassicDeviceQueryOptions& options, const std::string& targetAddress, BLUETOOTH_DEVICE_INFO& nativeDeviceInfo, GB_BluetoothDeviceInfo* publicDevice, bool& found)
    {
        found = false;
        nativeDeviceInfo = {};
        nativeDeviceInfo.dwSize = sizeof(nativeDeviceInfo);

        if (!options.requestFreshInquiry)
        {
            GB_SystemResult fastLookupResult = TryGetNativeClassicDeviceInfoByAddress(radioHandle, radioInfo, targetAddress, options.includeInstalledServices, nativeDeviceInfo, publicDevice, found);
            if (fastLookupResult.IsFailed())
            {
                return fastLookupResult.WithOperationName(u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio");
            }
            if (found)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio");
            }
        }

        BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams = BuildSearchParams(radioHandle, options);
        HBLUETOOTH_DEVICE_FIND findHandle = ::BluetoothFindFirstDevice(&searchParams, &nativeDeviceInfo);
        if (findHandle == nullptr)
        {
            const DWORD lastError = ::GetLastError();
            if (lastError == ERROR_SUCCESS || lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_NOT_FOUND)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio");
            }

            return GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio", u8"BluetoothFindFirstDevice 查找经典蓝牙设备失败。");
        }

        BluetoothDeviceFindScope findScope(findHandle);
        while (true)
        {
            const std::string currentAddress = FormatBluetoothAddress(nativeDeviceInfo.Address);
            if (currentAddress == targetAddress)
            {
                if (publicDevice != nullptr)
                {
                    *publicDevice = ConvertDeviceInfo(nativeDeviceInfo, radioInfo);
                    if (options.includeInstalledServices)
                    {
                        GB_SystemResult serviceResult = ReadInstalledServices(radioHandle, nativeDeviceInfo, publicDevice->installedServiceGuids);
                        if (serviceResult.IsFailed())
                        {
                            return serviceResult.WithOperationName(u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio");
                        }
                    }
                }

                found = true;
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio");
            }

            BLUETOOTH_DEVICE_INFO nextDeviceInfo = {};
            nextDeviceInfo.dwSize = sizeof(nextDeviceInfo);
            if (::BluetoothFindNextDevice(findScope.Get(), &nextDeviceInfo) == FALSE)
            {
                const DWORD nextError = ::GetLastError();
                if (nextError == ERROR_SUCCESS || nextError == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }

                return GB_SystemResult::FromWin32Error(nextError, u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio", u8"BluetoothFindNextDevice 查找下一个经典蓝牙设备失败。");
            }
            nativeDeviceInfo = nextDeviceInfo;
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio");
    }

    static GB_SystemResult EnumerateClassicDevicesForRadio(HANDLE radioHandle, const GB_BluetoothRadioInfo& radioInfo, const GB_BluetoothClassicDeviceQueryOptions& options, std::vector<GB_BluetoothDeviceInfo>& devices, std::unordered_set<std::string>& seenAddresses)
    {
        BLUETOOTH_DEVICE_INFO nativeDeviceInfo = {};
        nativeDeviceInfo.dwSize = sizeof(nativeDeviceInfo);

        BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams = BuildSearchParams(radioHandle, options);
        HBLUETOOTH_DEVICE_FIND findHandle = ::BluetoothFindFirstDevice(&searchParams, &nativeDeviceInfo);
        if (findHandle == nullptr)
        {
            const DWORD lastError = ::GetLastError();
            if (lastError == ERROR_SUCCESS || lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_NOT_FOUND)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetClassicDevices");
            }

            return GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemBluetooth::GetClassicDevices", u8"BluetoothFindFirstDevice 枚举经典蓝牙设备失败。");
        }

        BluetoothDeviceFindScope findScope(findHandle);
        while (true)
        {
            GB_BluetoothDeviceInfo device = ConvertDeviceInfo(nativeDeviceInfo, radioInfo);
            const std::string seenKey = device.radioAddress + "|" + device.address;
            if (seenAddresses.insert(seenKey).second)
            {
                if (options.includeInstalledServices)
                {
                    GB_SystemResult serviceResult = ReadInstalledServices(radioHandle, nativeDeviceInfo, device.installedServiceGuids);
                    if (serviceResult.IsFailed())
                    {
                        return serviceResult.WithOperationName(u8"GB_SystemBluetooth::GetClassicDevices");
                    }
                }
                try
                {
                    devices.push_back(device);
                }
                catch (...)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetClassicDevices", u8"保存经典蓝牙设备信息时内存不足。");
                }
            }

            BLUETOOTH_DEVICE_INFO nextDeviceInfo = {};
            nextDeviceInfo.dwSize = sizeof(nextDeviceInfo);
            if (::BluetoothFindNextDevice(findScope.Get(), &nextDeviceInfo) == FALSE)
            {
                const DWORD nextError = ::GetLastError();
                if (nextError == ERROR_SUCCESS || nextError == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }

                return GB_SystemResult::FromWin32Error(nextError, u8"GB_SystemBluetooth::GetClassicDevices", u8"BluetoothFindNextDevice 枚举下一个经典蓝牙设备失败。");
            }
            nativeDeviceInfo = nextDeviceInfo;
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetClassicDevices");
    }

    static GB_SystemResult ResolveClassicDeviceAddress(const GB_BluetoothDeviceId& deviceId, std::string& address)
    {
        address.clear();
        if (!IsValidBluetoothDeviceKind(deviceId.deviceKind))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::ResolveClassicDeviceAddress", u8"deviceKind 不是有效蓝牙设备类型。");
        }

        if (deviceId.deviceKind == GB_BluetoothDeviceKind::LowEnergy)
        {
            return MakeUnsupportedBleResult(u8"GB_SystemBluetooth::ResolveClassicDeviceAddress");
        }

        const std::string sourceAddress = !deviceId.address.empty() ? deviceId.address : deviceId.nativeDeviceId;
        if (sourceAddress.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::ResolveClassicDeviceAddress", u8"经典蓝牙设备操作需要提供 address。");
        }

        address = NormalizeBluetoothAddress(sourceAddress);
        if (address.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::ResolveClassicDeviceAddress", u8"蓝牙设备地址格式无效。");
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::ResolveClassicDeviceAddress");
    }

    static GB_BluetoothClassicDeviceQueryOptions MakeLookupOptions(bool requestFreshInquiry)
    {
        GB_BluetoothClassicDeviceQueryOptions options;
        options.includeAuthenticated = true;
        options.includeRemembered = true;
        options.includeUnknown = true;
        options.includeConnected = true;
        options.requestFreshInquiry = requestFreshInquiry;
        options.inquiryTimeoutMultiplier = 4;
        options.includeInstalledServices = false;
        return options;
    }
#endif
}

GB_SystemResult GB_SystemBluetooth::GetRadios(std::vector<GB_BluetoothRadioInfo>& radios)
{
    radios.clear();
#if !defined(_WIN32)
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::GetRadios", u8"当前平台不支持 Windows 蓝牙无线电枚举。");
#else
    std::vector<BluetoothRadioHandleEntry> radioHandles;
    GB_SystemResult result = EnumerateRadioHandles(radioHandles);
    if (result.IsFailed())
    {
        return result.WithOperationName(u8"GB_SystemBluetooth::GetRadios");
    }

    try
    {
        radios.reserve(radioHandles.size());
        for (size_t index = 0; index < radioHandles.size(); index++)
        {
            radios.push_back(radioHandles[index].GetInfo());
        }
    }
    catch (...)
    {
        radios.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetRadios", u8"保存蓝牙无线电信息时内存不足。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetRadios");
#endif
}

GB_SystemResult GB_SystemBluetooth::GetDefaultRadio(GB_BluetoothRadioInfo& radio, bool& found)
{
    radio = GB_BluetoothRadioInfo();
    found = false;

    std::vector<GB_BluetoothRadioInfo> radios;
    GB_SystemResult result = GetRadios(radios);
    if (result.IsFailed())
    {
        return result.WithOperationName(u8"GB_SystemBluetooth::GetDefaultRadio");
    }

    if (radios.empty())
    {
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetDefaultRadio", u8"当前系统未发现蓝牙无线电。");
    }

    radio = radios.front();
    found = true;
    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetDefaultRadio");
}

GB_SystemResult GB_SystemBluetooth::IsBluetoothAvailable(bool& available)
{
    available = false;

    std::vector<GB_BluetoothRadioInfo> radios;
    GB_SystemResult result = GetRadios(radios);
    if (result.IsFailed())
    {
        return result.WithOperationName(u8"GB_SystemBluetooth::IsBluetoothAvailable");
    }

    available = !radios.empty();
    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::IsBluetoothAvailable");
}

GB_SystemResult GB_SystemBluetooth::SetRadioConnectable(const std::string& radioAddress, const bool enabled)
{
#if !defined(_WIN32)
    (void)radioAddress;
    (void)enabled;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::SetRadioConnectable", u8"当前平台不支持 Windows 蓝牙入站连接状态设置。");
#else
    std::vector<BluetoothRadioHandleEntry> radioHandles;
    std::vector<BluetoothRadioHandleEntry*> targetRadioHandles;
    GB_SystemResult targetResult = FindTargetRadioHandles(radioAddress, radioHandles, targetRadioHandles, u8"GB_SystemBluetooth::SetRadioConnectable");
    if (targetResult.IsFailed())
    {
        return targetResult;
    }

    for (size_t index = 0; index < targetRadioHandles.size(); index++)
    {
        HANDLE radioHandle = targetRadioHandles[index]->GetHandle();
        if (!enabled && ::BluetoothIsDiscoverable(radioHandle) != FALSE)
        {
            GB_SystemResult discoveryResult = SetRadioDiscovery(radioHandle, false, u8"GB_SystemBluetooth::SetRadioConnectable");
            if (discoveryResult.IsFailed())
            {
                return discoveryResult;
            }
        }

        GB_SystemResult connectableResult = SetRadioIncomingConnections(radioHandle, enabled, u8"GB_SystemBluetooth::SetRadioConnectable");
        if (connectableResult.IsFailed())
        {
            return connectableResult;
        }
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::SetRadioConnectable");
#endif
}

GB_SystemResult GB_SystemBluetooth::SetRadioDiscoverable(const std::string& radioAddress, const bool enabled)
{
#if !defined(_WIN32)
    (void)radioAddress;
    (void)enabled;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::SetRadioDiscoverable", u8"当前平台不支持 Windows 蓝牙可发现状态设置。");
#else
    std::vector<BluetoothRadioHandleEntry> radioHandles;
    std::vector<BluetoothRadioHandleEntry*> targetRadioHandles;
    GB_SystemResult targetResult = FindTargetRadioHandles(radioAddress, radioHandles, targetRadioHandles, u8"GB_SystemBluetooth::SetRadioDiscoverable");
    if (targetResult.IsFailed())
    {
        return targetResult;
    }

    for (size_t index = 0; index < targetRadioHandles.size(); index++)
    {
        HANDLE radioHandle = targetRadioHandles[index]->GetHandle();
        if (enabled && ::BluetoothIsConnectable(radioHandle) == FALSE)
        {
            GB_SystemResult connectableResult = SetRadioIncomingConnections(radioHandle, true, u8"GB_SystemBluetooth::SetRadioDiscoverable");
            if (connectableResult.IsFailed())
            {
                return connectableResult;
            }
        }

        GB_SystemResult discoveryResult = SetRadioDiscovery(radioHandle, enabled, u8"GB_SystemBluetooth::SetRadioDiscoverable");
        if (discoveryResult.IsFailed())
        {
            return discoveryResult;
        }
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::SetRadioDiscoverable");
#endif
}

GB_SystemResult GB_SystemBluetooth::GetClassicDevices(std::vector<GB_BluetoothDeviceInfo>& devices, const GB_BluetoothClassicDeviceQueryOptions& options)
{
    devices.clear();
#if !defined(_WIN32)
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::GetClassicDevices", u8"当前平台不支持 Windows 经典蓝牙设备枚举。");
#else
    GB_SystemResult validateResult = ValidateClassicDeviceQueryOptions(options);
    if (validateResult.IsFailed())
    {
        return validateResult.WithOperationName(u8"GB_SystemBluetooth::GetClassicDevices");
    }

    std::vector<BluetoothRadioHandleEntry> radioHandles;
    GB_SystemResult radioResult = EnumerateRadioHandles(radioHandles);
    if (radioResult.IsFailed())
    {
        return radioResult.WithOperationName(u8"GB_SystemBluetooth::GetClassicDevices");
    }

    const std::string targetRadioAddress = NormalizeBluetoothAddress(options.radioAddress);
    if (!options.radioAddress.empty() && targetRadioAddress.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::GetClassicDevices", u8"radioAddress 不是有效蓝牙地址。");
    }

    bool matchedRadio = options.radioAddress.empty();
    std::unordered_set<std::string> seenAddresses;
    try
    {
        seenAddresses.reserve(radioHandles.size() * 8u + 8u);
        devices.reserve(radioHandles.size() * 8u);
    }
    catch (...)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetClassicDevices", u8"预分配经典蓝牙设备结果缓冲区时内存不足。");
    }

    for (size_t index = 0; index < radioHandles.size(); index++)
    {
        if (!options.radioAddress.empty() && radioHandles[index].GetInfo().address != targetRadioAddress)
        {
            continue;
        }

        matchedRadio = true;
        GB_SystemResult enumerateResult = EnumerateClassicDevicesForRadio(radioHandles[index].GetHandle(), radioHandles[index].GetInfo(), options, devices, seenAddresses);
        if (enumerateResult.IsFailed())
        {
            devices.clear();
            return enumerateResult.WithOperationName(u8"GB_SystemBluetooth::GetClassicDevices");
        }
    }

    if (!matchedRadio)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_SystemBluetooth::GetClassicDevices", u8"未找到指定 radioAddress 对应的本机蓝牙无线电。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetClassicDevices");
#endif
}

GB_SystemResult GB_SystemBluetooth::GetLowEnergyDevices(std::vector<GB_BluetoothDeviceInfo>& devices)
{
    devices.clear();
    return MakeUnsupportedBleResult(u8"GB_SystemBluetooth::GetLowEnergyDevices");
}

GB_SystemResult GB_SystemBluetooth::GetClassicDeviceByAddress(const std::string& address, GB_BluetoothDeviceInfo& device, bool& found, const GB_BluetoothClassicDeviceQueryOptions& options)
{
    device = GB_BluetoothDeviceInfo();
    found = false;

    const std::string normalizedAddress = NormalizeBluetoothAddress(address);
    if (normalizedAddress.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::GetClassicDeviceByAddress", u8"蓝牙设备地址格式无效。");
    }
#if !defined(_WIN32)
    (void)options;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::GetClassicDeviceByAddress", u8"当前平台不支持 Windows 经典蓝牙设备查询。");
#else
    GB_SystemResult validateResult = ValidateClassicDeviceQueryOptions(options);
    if (validateResult.IsFailed())
    {
        return validateResult.WithOperationName(u8"GB_SystemBluetooth::GetClassicDeviceByAddress");
    }

    GB_BluetoothClassicDeviceQueryOptions lookupOptions = options;
    lookupOptions.includeAuthenticated = true;
    lookupOptions.includeRemembered = true;
    lookupOptions.includeUnknown = true;
    lookupOptions.includeConnected = true;

    std::vector<BluetoothRadioHandleEntry> radioHandles;
    GB_SystemResult radioResult = EnumerateRadioHandles(radioHandles);
    if (radioResult.IsFailed())
    {
        return radioResult.WithOperationName(u8"GB_SystemBluetooth::GetClassicDeviceByAddress");
    }

    const std::string targetRadioAddress = NormalizeBluetoothAddress(lookupOptions.radioAddress);
    bool matchedRadio = lookupOptions.radioAddress.empty();
    for (size_t index = 0; index < radioHandles.size(); index++)
    {
        if (!lookupOptions.radioAddress.empty() && radioHandles[index].GetInfo().address != targetRadioAddress)
        {
            continue;
        }

        matchedRadio = true;
        BLUETOOTH_DEVICE_INFO nativeDeviceInfo = {};
        bool currentFound = false;
        GB_SystemResult findResult = FindNativeClassicDeviceForRadio(radioHandles[index].GetHandle(), radioHandles[index].GetInfo(), lookupOptions, normalizedAddress, nativeDeviceInfo, &device, currentFound);
        if (findResult.IsFailed())
        {
            device = GB_BluetoothDeviceInfo();
            return findResult.WithOperationName(u8"GB_SystemBluetooth::GetClassicDeviceByAddress");
        }
        if (currentFound)
        {
            found = true;
            return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetClassicDeviceByAddress");
        }
    }

    if (!matchedRadio)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_SystemBluetooth::GetClassicDeviceByAddress", u8"未找到指定 radioAddress 对应的本机蓝牙无线电。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetClassicDeviceByAddress", u8"未找到指定经典蓝牙设备。");
#endif
}

GB_SystemResult GB_SystemBluetooth::IsDeviceConnected(const GB_BluetoothDeviceId& deviceId, bool& connected)
{
    connected = false;
#if !defined(_WIN32)
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::IsDeviceConnected", u8"当前平台不支持 Windows 蓝牙连接状态查询。");
#else
    std::string address;
    GB_SystemResult resolveResult = ResolveClassicDeviceAddress(deviceId, address);
    if (resolveResult.IsFailed())
    {
        return resolveResult.WithOperationName(u8"GB_SystemBluetooth::IsDeviceConnected");
    }

    GB_BluetoothDeviceInfo device;
    bool found = false;
    GB_SystemResult lookupResult = GetClassicDeviceByAddress(address, device, found, MakeLookupOptions(false));
    if (lookupResult.IsFailed())
    {
        return lookupResult.WithOperationName(u8"GB_SystemBluetooth::IsDeviceConnected");
    }
    if (!found)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_SystemBluetooth::IsDeviceConnected", u8"未找到指定经典蓝牙设备，无法判断连接状态。");
    }

    connected = device.isConnected;
    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::IsDeviceConnected");
#endif
}

GB_SystemResult GB_SystemBluetooth::PairDevice(const GB_BluetoothDeviceId& deviceId, const GB_BluetoothPairingOptions& options)
{
#if !defined(_WIN32)
    (void)deviceId;
    (void)options;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::PairDevice", u8"当前平台不支持 Windows 蓝牙配对。");
#else
    std::string address;
    GB_SystemResult resolveResult = ResolveClassicDeviceAddress(deviceId, address);
    if (resolveResult.IsFailed())
    {
        return resolveResult.WithOperationName(u8"GB_SystemBluetooth::PairDevice");
    }

    if (!GB_IsUtf8(options.pinCodeUtf8) || ContainsNullCharacter(options.pinCodeUtf8))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::PairDevice", u8"pinCodeUtf8 必须是合法 UTF-8 且不能包含空字符。");
    }
    if (!options.pinCodeUtf8.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::PairDevice", u8"当前实现使用 BluetoothAuthenticateDeviceEx 发起系统配对，尚未实现 PIN 输入或自定义认证回调。");
    }
    if (!options.allowSystemPairingUi)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::PairDevice", u8"当前基础配对实现需要允许系统配对 UI 或系统授权流程。");
    }

    std::vector<BluetoothRadioHandleEntry> radioHandles;
    GB_SystemResult radioResult = EnumerateRadioHandles(radioHandles);
    if (radioResult.IsFailed())
    {
        return radioResult.WithOperationName(u8"GB_SystemBluetooth::PairDevice");
    }
    if (radioHandles.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_SystemBluetooth::PairDevice", u8"当前系统未发现蓝牙无线电。");
    }

    BLUETOOTH_DEVICE_INFO nativeDeviceInfo = {};
    HANDLE matchedRadioHandle = nullptr;
    GB_BluetoothDeviceInfo publicDevice;
    const GB_BluetoothClassicDeviceQueryOptions lookupOptions = MakeLookupOptions(true);
    for (size_t index = 0; index < radioHandles.size(); index++)
    {
        bool currentFound = false;
        GB_SystemResult findResult = FindNativeClassicDeviceForRadio(radioHandles[index].GetHandle(), radioHandles[index].GetInfo(), lookupOptions, address, nativeDeviceInfo, &publicDevice, currentFound);
        if (findResult.IsFailed())
        {
            return findResult.WithOperationName(u8"GB_SystemBluetooth::PairDevice");
        }
        if (currentFound)
        {
            matchedRadioHandle = radioHandles[index].GetHandle();
            break;
        }
    }

    if (matchedRadioHandle == nullptr)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_SystemBluetooth::PairDevice", u8"未找到指定经典蓝牙设备；请确认设备处于可发现或已记住状态。");
    }
    if (publicDevice.isAuthenticated)
    {
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::PairDevice", u8"指定经典蓝牙设备已经完成配对。");
    }

    const DWORD pairResult = ::BluetoothAuthenticateDeviceEx(nullptr, matchedRadioHandle, &nativeDeviceInfo, nullptr, MITMProtectionNotRequiredBonding);
    if (pairResult == ERROR_SUCCESS)
    {
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::PairDevice", u8"经典蓝牙配对请求已成功完成。");
    }
    if (pairResult == ERROR_NO_MORE_ITEMS)
    {
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::PairDevice", u8"系统返回设备已经完成认证。");
    }

    return MapBluetoothWin32Result(pairResult, u8"GB_SystemBluetooth::PairDevice", u8"BluetoothAuthenticateDeviceEx 配对经典蓝牙设备失败。");
#endif
}

GB_SystemResult GB_SystemBluetooth::RemoveDevice(const GB_BluetoothDeviceId& deviceId)
{
#if !defined(_WIN32)
    (void)deviceId;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::RemoveDevice", u8"当前平台不支持 Windows 蓝牙设备移除。");
#else
    std::string address;
    GB_SystemResult resolveResult = ResolveClassicDeviceAddress(deviceId, address);
    if (resolveResult.IsFailed())
    {
        return resolveResult.WithOperationName(u8"GB_SystemBluetooth::RemoveDevice");
    }

    uint64_t addressValue = 0;
    if (!TryParseBluetoothAddressValue(address, addressValue))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::RemoveDevice", u8"蓝牙设备地址格式无效。");
    }

    BLUETOOTH_ADDRESS nativeAddress = MakeBluetoothAddress(addressValue);
    const DWORD removeResult = ::BluetoothRemoveDevice(&nativeAddress);
    if (removeResult == ERROR_SUCCESS)
    {
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::RemoveDevice", u8"经典蓝牙设备配对记录已移除。");
    }

    return MapBluetoothWin32Result(removeResult, u8"GB_SystemBluetooth::RemoveDevice", u8"BluetoothRemoveDevice 移除经典蓝牙设备失败。");
#endif
}

bool GB_SystemBluetooth::IsValidAddress(const std::string& address)
{
    uint64_t addressValue = 0;
    return TryParseBluetoothAddressValue(address, addressValue);
}

std::string GB_SystemBluetooth::NormalizeAddress(const std::string& address)
{
    return NormalizeBluetoothAddress(address);
}

std::string GB_SystemBluetooth::GetDeviceKindName(const GB_BluetoothDeviceKind deviceKind)
{
    if (!IsValidBluetoothDeviceKind(deviceKind))
    {
        return "Invalid";
    }

    switch (deviceKind)
    {
    case GB_BluetoothDeviceKind::Unknown:
        return "Unknown";
    case GB_BluetoothDeviceKind::Classic:
        return "Classic";
    case GB_BluetoothDeviceKind::LowEnergy:
        return "LowEnergy";
    case GB_BluetoothDeviceKind::DualMode:
        return "DualMode";
    default:
        break;
    }

    return "Invalid";
}

std::string GB_SystemBluetooth::GetPairStatusName(const GB_BluetoothPairStatus pairStatus)
{
    if (!IsValidPairStatus(pairStatus))
    {
        return "Invalid";
    }

    switch (pairStatus)
    {
    case GB_BluetoothPairStatus::Unknown:
        return "Unknown";
    case GB_BluetoothPairStatus::Unpaired:
        return "Unpaired";
    case GB_BluetoothPairStatus::Paired:
        return "Paired";
    case GB_BluetoothPairStatus::CannotPair:
        return "CannotPair";
    default:
        break;
    }

    return "Invalid";
}

std::string GB_SystemBluetooth::GetConnectionStatusName(const GB_BluetoothConnectionStatus connectionStatus)
{
    if (!IsValidConnectionStatus(connectionStatus))
    {
        return "Invalid";
    }

    switch (connectionStatus)
    {
    case GB_BluetoothConnectionStatus::Unknown:
        return "Unknown";
    case GB_BluetoothConnectionStatus::Disconnected:
        return "Disconnected";
    case GB_BluetoothConnectionStatus::Connected:
        return "Connected";
    default:
        break;
    }

    return "Invalid";
}

std::string GB_SystemBluetooth::GetEventTypeName(const GB_BluetoothEventType eventType)
{
    if (!IsValidBluetoothEventType(eventType))
    {
        return "Invalid";
    }

    switch (eventType)
    {
    case GB_BluetoothEventType::Unknown:
        return "Unknown";
    case GB_BluetoothEventType::DeviceAdded:
        return "DeviceAdded";
    case GB_BluetoothEventType::DeviceUpdated:
        return "DeviceUpdated";
    case GB_BluetoothEventType::DeviceRemoved:
        return "DeviceRemoved";
    case GB_BluetoothEventType::RadioChanged:
        return "RadioChanged";
    default:
        break;
    }

    return "Invalid";
}

class GB_SystemBluetoothWatcher::Impl
{
public:
    explicit Impl(const GB_SystemBluetoothWatcherOptions& inputOptions) : options(NormalizeOptions(inputOptions)), typedDispatcher(GB_EventDispatcher::MakeQueuedOptions(options.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropOldest, u8"GB_SystemBluetoothWatcher.Typed")), publicDispatcher(GB_EventDispatcher::MakeQueuedOptions(options.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropOldest, u8"GB_SystemBluetoothWatcher.Public")), acceptingDeviceEvents(false)
    {
        (void)typedDispatcher.SubscribeAll([this](const GB_Event& event)
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
        std::lock_guard<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (running)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::Start", u8"蓝牙监听器已经启动。");
            }
        }

        GB_SystemResult result = typedDispatcher.Start();
        if (result.IsFailed())
        {
            return result.WithOperationName(u8"GB_SystemBluetoothWatcher::Start");
        }
        result = publicDispatcher.Start();
        if (result.IsFailed())
        {
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return result.WithOperationName(u8"GB_SystemBluetoothWatcher::Start");
        }

        acceptingDeviceEvents.store(true, std::memory_order_release);
        deviceWatcher.SetDeviceEventCallback([this](const GB_SystemDeviceEvent& deviceEvent)
            {
                HandleSystemDeviceEvent(deviceEvent);
            });

        result = deviceWatcher.Start();
        if (result.IsFailed())
        {
            acceptingDeviceEvents.store(false, std::memory_order_release);
            deviceWatcher.SetDeviceEventCallback(GB_SystemDeviceWatcher::DeviceEventCallback());
            (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return result.WithOperationName(u8"GB_SystemBluetoothWatcher::Start");
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            running = true;
        }
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::Start");
    }

    GB_SystemResult Stop()
    {
        std::lock_guard<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!running)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::Stop", u8"蓝牙监听器未启动。");
            }
            running = false;
        }

        acceptingDeviceEvents.store(false, std::memory_order_release);
        deviceWatcher.SetDeviceEventCallback(GB_SystemDeviceWatcher::DeviceEventCallback());
        GB_SystemResult deviceStopResult = deviceWatcher.Stop();
        (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Drain);
        (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Drain);
        if (deviceStopResult.IsFailed())
        {
            return deviceStopResult.WithOperationName(u8"GB_SystemBluetoothWatcher::Stop");
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::Stop");
    }

    bool IsRunning() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return running;
    }

    void SetBluetoothEventCallback(const GB_SystemBluetoothWatcher::BluetoothEventCallback& callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        bluetoothEventCallback = callback;
    }

    GB_EventDispatcher& GetEventDispatcher()
    {
        return publicDispatcher;
    }

private:
    static GB_SystemBluetoothWatcherOptions NormalizeOptions(const GB_SystemBluetoothWatcherOptions& inputOptions)
    {
        GB_SystemBluetoothWatcherOptions normalizedOptions = inputOptions;
        if (normalizedOptions.maxDispatchQueueSize == 0)
        {
            normalizedOptions.maxDispatchQueueSize = 64;
        }

        return normalizedOptions;
    }

    static bool HasBluetoothPnPIdentity(const std::string& text)
    {
        if (text.empty())
        {
            return false;
        }

        if (StartsWithAsciiNoCase(text, "BTH\\") || StartsWithAsciiNoCase(text, "BTHLE\\") || StartsWithAsciiNoCase(text, "BTHENUM\\") || StartsWithAsciiNoCase(text, "BTHLEDEVICE\\") || StartsWithAsciiNoCase(text, "BTHLEENUM\\") || StartsWithAsciiNoCase(text, "BTHHFENUM\\") || StartsWithAsciiNoCase(text, "BTHMODEM\\") || StartsWithAsciiNoCase(text, "Bluetooth\\") || StartsWithAsciiNoCase(text, "SWD\\RADIO\\Bluetooth"))
        {
            return true;
        }
        if (ContainsAsciiNoCase(text, "\\BTH") || ContainsAsciiNoCase(text, "#BTH") || ContainsAsciiNoCase(text, "Bluetooth"))
        {
            return true;
        }

        return false;
    }

    static bool HasBluetoothRadioPnPIdentity(const std::string& text)
    {
        if (text.empty())
        {
            return false;
        }

        return StartsWithAsciiNoCase(text, "SWD\\RADIO\\Bluetooth") || ContainsAsciiNoCase(text, "SWD\\RADIO\\Bluetooth") || ContainsAsciiNoCase(text, "BTH\\MS_BTH") || ContainsAsciiNoCase(text, "#BTH#MS_BTH");
    }

    static bool IsBluetoothRelatedDeviceEvent(const GB_SystemDeviceEvent& deviceEvent)
    {
        if (HasBluetoothPnPIdentity(deviceEvent.deviceInstanceId) || HasBluetoothPnPIdentity(deviceEvent.deviceInterfacePath))
        {
            return true;
        }

        return false;
    }

    static bool IsBluetoothRadioDeviceEvent(const GB_SystemDeviceEvent& deviceEvent)
    {
        return HasBluetoothRadioPnPIdentity(deviceEvent.deviceInstanceId) || HasBluetoothRadioPnPIdentity(deviceEvent.deviceInterfacePath);
    }

    static GB_BluetoothEventType MapDeviceEventType(const GB_SystemDeviceEvent& deviceEvent)
    {
        if (IsBluetoothRadioDeviceEvent(deviceEvent))
        {
            return GB_BluetoothEventType::RadioChanged;
        }

        switch (deviceEvent.eventType)
        {
        case GB_SystemDeviceEventType::DeviceInstanceEnumerated:
        case GB_SystemDeviceEventType::DeviceInstanceStarted:
        case GB_SystemDeviceEventType::DeviceInterfaceArrived:
            return GB_BluetoothEventType::DeviceAdded;
        case GB_SystemDeviceEventType::DeviceInstanceRemoved:
        case GB_SystemDeviceEventType::DeviceInterfaceRemoved:
        case GB_SystemDeviceEventType::DeviceRemoveComplete:
            return GB_BluetoothEventType::DeviceRemoved;
        case GB_SystemDeviceEventType::DeviceNodesChanged:
        case GB_SystemDeviceEventType::DeviceQueryRemove:
        case GB_SystemDeviceEventType::DeviceQueryRemoveFailed:
        case GB_SystemDeviceEventType::DeviceRemovePending:
        case GB_SystemDeviceEventType::DeviceCustomEvent:
            return GB_BluetoothEventType::DeviceUpdated;
        default:
            break;
        }

        return GB_BluetoothEventType::Unknown;
    }

    static GB_BluetoothEvent BuildBluetoothEvent(const GB_SystemDeviceEvent& deviceEvent)
    {
        GB_BluetoothEvent bluetoothEvent;
        bluetoothEvent.eventType = MapDeviceEventType(deviceEvent);
        bluetoothEvent.eventName = GetBluetoothEventName(bluetoothEvent.eventType);
        bluetoothEvent.sourceName = deviceEvent.sourceName.empty() ? u8"GB_SystemDeviceWatcher" : deviceEvent.sourceName;
        bluetoothEvent.timestampMilliseconds = deviceEvent.timestampMilliseconds;
        bluetoothEvent.deviceInstanceId = deviceEvent.deviceInstanceId;
        bluetoothEvent.deviceInterfacePath = deviceEvent.deviceInterfacePath;
        bluetoothEvent.interfaceClassGuid = deviceEvent.interfaceClassGuid;
        bluetoothEvent.nativeAction = deviceEvent.nativeAction;
        return bluetoothEvent;
    }

    void DispatchTypedCallback(const GB_Event& event)
    {
        const GB_BluetoothEvent* bluetoothEvent = event.payload.AnyCast<GB_BluetoothEvent>();
        if (bluetoothEvent == nullptr)
        {
            return;
        }

        GB_SystemBluetoothWatcher::BluetoothEventCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callback = bluetoothEventCallback;
        }

        if (callback)
        {
            callback(*bluetoothEvent);
        }
    }

    void HandleSystemDeviceEvent(const GB_SystemDeviceEvent& deviceEvent) noexcept
    {
        if (!acceptingDeviceEvents.load(std::memory_order_acquire))
        {
            return;
        }
        if (!IsBluetoothRelatedDeviceEvent(deviceEvent))
        {
            return;
        }

        try
        {
            GB_BluetoothEvent bluetoothEvent = BuildBluetoothEvent(deviceEvent);
            if (bluetoothEvent.eventType == GB_BluetoothEventType::Unknown)
            {
                return;
            }

            GB_Event typedEvent(bluetoothEvent.eventName, GB_Variant(bluetoothEvent), u8"GB_SystemBluetoothWatcher");
            typedEvent.timestampMilliseconds = bluetoothEvent.timestampMilliseconds;
            (void)typedDispatcher.Post(typedEvent);

            GB_Event publicEvent(bluetoothEvent.eventName, GB_SystemBluetooth::GetEventTypeName(bluetoothEvent.eventType), u8"GB_SystemBluetoothWatcher");
            publicEvent.timestampMilliseconds = bluetoothEvent.timestampMilliseconds;
            publicEvent.SetAttribute("eventType", GB_Variant(static_cast<unsigned int>(bluetoothEvent.eventType)));
            publicEvent.SetAttribute("eventTypeName", GB_Variant(GB_SystemBluetooth::GetEventTypeName(bluetoothEvent.eventType)));
            publicEvent.SetAttribute("sourceName", GB_Variant(bluetoothEvent.sourceName));
            publicEvent.SetAttribute("deviceInstanceId", GB_Variant(bluetoothEvent.deviceInstanceId));
            publicEvent.SetAttribute("deviceInterfacePath", GB_Variant(bluetoothEvent.deviceInterfacePath));
            publicEvent.SetAttribute("interfaceClassGuid", GB_Variant(bluetoothEvent.interfaceClassGuid));
            publicEvent.SetAttribute("nativeAction", GB_Variant(bluetoothEvent.nativeAction));
            (void)publicDispatcher.Post(publicEvent);
        }
        catch (...)
        {
        }
    }

private:
    GB_SystemBluetoothWatcherOptions options;
    mutable std::mutex operationMutex;
    mutable std::mutex stateMutex;
    mutable std::mutex callbackMutex;
    bool running = false;
    std::atomic<bool> acceptingDeviceEvents;
    GB_SystemDeviceWatcher deviceWatcher;
    GB_EventDispatcher typedDispatcher;
    GB_EventDispatcher publicDispatcher;
    GB_EventSubscriptionToken typedSubscriptionToken;
    GB_SystemBluetoothWatcher::BluetoothEventCallback bluetoothEventCallback;
};

GB_SystemBluetoothWatcher::GB_SystemBluetoothWatcher() : impl(new Impl(GB_SystemBluetoothWatcherOptions()))
{
}

GB_SystemBluetoothWatcher::GB_SystemBluetoothWatcher(const GB_SystemBluetoothWatcherOptions& options) : impl(new Impl(options))
{
}

GB_SystemBluetoothWatcher::~GB_SystemBluetoothWatcher() noexcept
{
    if (impl)
    {
        (void)impl->Stop();
    }
}

GB_SystemResult GB_SystemBluetoothWatcher::Start()
{
    return impl->Start();
}

GB_SystemResult GB_SystemBluetoothWatcher::Stop()
{
    return impl->Stop();
}

bool GB_SystemBluetoothWatcher::IsRunning() const
{
    return impl->IsRunning();
}

void GB_SystemBluetoothWatcher::SetBluetoothEventCallback(const BluetoothEventCallback& callback)
{
    impl->SetBluetoothEventCallback(callback);
}

GB_EventDispatcher& GB_SystemBluetoothWatcher::GetEventDispatcher()
{
    return impl->GetEventDispatcher();
}
