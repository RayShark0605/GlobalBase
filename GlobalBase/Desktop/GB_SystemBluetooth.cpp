#include "GB_SystemBluetooth.h"
#include "GB_SystemDevice.h"
#include "../GB_Utf8String.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
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
#include <bluetoothleapis.h>
#include <bthledef.h>
#ifdef _MSC_VER
#  pragma comment(lib, "Bthprops.lib")
#  pragma comment(lib, "BluetoothApis.lib")
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

    static bool EqualsAsciiNoCase(const std::string& leftText, const std::string& rightText)
    {
        return leftText.size() == rightText.size() && StartsWithAsciiNoCase(leftText, rightText);
    }

    static const char* const GB_BluetoothLeDeviceInterfaceGuid = "{781AEE18-7733-4CE4-ADD0-91F41C67B592}";
    static const char* const GB_BluetoothRadioInterfaceGuid = "{0850302A-B344-4FDA-9BE9-90576B8D46F0}";

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

        size_t beginIndex = 0;
        while (beginIndex < address.size() && IsAsciiWhitespace(address[beginIndex]))
        {
            beginIndex++;
        }

        size_t endIndex = address.size();
        while (endIndex > beginIndex && IsAsciiWhitespace(address[endIndex - 1]))
        {
            endIndex--;
        }

        const size_t addressLength = endIndex - beginIndex;
        char digits[12] = {};
        if (addressLength == 12)
        {
            for (size_t index = 0; index < 12; index++)
            {
                const char character = address[beginIndex + index];
                if (HexValue(character) < 0)
                {
                    return false;
                }

                digits[index] = character;
            }
        }
        else if (addressLength == 17)
        {
            const char separator = address[beginIndex + 2];
            if (separator != ':' && separator != '-')
            {
                return false;
            }

            size_t digitIndex = 0;
            for (size_t groupIndex = 0; groupIndex < 6; groupIndex++)
            {
                const size_t textIndex = beginIndex + groupIndex * 3;
                if (!IsHexPairAt(address, textIndex))
                {
                    return false;
                }
                if (groupIndex < 5 && address[textIndex + 2] != separator)
                {
                    return false;
                }

                digits[digitIndex] = address[textIndex];
                digitIndex++;
                digits[digitIndex] = address[textIndex + 1];
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

    static bool IsValidGattCacheMode(const GB_BluetoothGattCacheMode cacheMode)
    {
        switch (cacheMode)
        {
        case GB_BluetoothGattCacheMode::Default:
        case GB_BluetoothGattCacheMode::ForceReadFromDevice:
        case GB_BluetoothGattCacheMode::ForceReadFromCache:
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

#if defined(_WIN32)
    static const size_t GB_MaxNativeBluetoothArrayBytes = 16u * 1024u * 1024u;

    static bool TryMultiplyAndAddSize(const size_t value, const size_t multiplier, const size_t addition, size_t& result)
    {
        result = 0;
        if (multiplier == 0)
        {
            result = addition;
            return true;
        }
        if (value > ((std::numeric_limits<size_t>::max)() - addition) / multiplier)
        {
            return false;
        }

        result = value * multiplier + addition;
        return true;
    }

    template<typename T>
    static bool IsReasonableNativeBluetoothArrayCount(const size_t elementCount)
    {
        return elementCount <= GB_MaxNativeBluetoothArrayBytes / sizeof(T);
    }

    static std::string ToLowerAscii(std::string text)
    {
        for (size_t index = 0; index < text.size(); index++)
        {
            text[index] = ToLowerAsciiChar(text[index]);
        }

        return text;
    }

    static bool AreDeviceInterfacePathsEqual(const std::string& leftPath, const std::string& rightPath)
    {
        return EqualsAsciiNoCase(leftPath, rightPath);
    }

    static GB_SystemResult MakeUnsupportedBleResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, u8"当前操作只支持经典蓝牙设备；BLE 配对、实时连接状态和广播扫描需要 Windows Runtime 等对应能力，不能用经典蓝牙 API 伪装结果。");
    }

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

        BluetoothRadioHandleEntry(HANDLE radioHandle, GB_BluetoothRadioInfo&& radioInfo) : radioHandle(radioHandle), radioInfo(std::move(radioInfo))
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

        GB_BluetoothRadioInfo TakeInfo()
        {
            return std::move(radioInfo);
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
            radioInfo = std::move(other.radioInfo);
            other.radioHandle = nullptr;
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

        return GB_SystemResult::FromWin32Error(static_cast<uint32_t>(win32Error), operationName, message).WithMessage(message);
    }

    class GenericHandleScope
    {
    public:
        explicit GenericHandleScope(HANDLE handle = INVALID_HANDLE_VALUE) : handle(handle)
        {
        }

        ~GenericHandleScope()
        {
            Close();
        }

        GenericHandleScope(const GenericHandleScope&) = delete;
        GenericHandleScope& operator=(const GenericHandleScope&) = delete;

        HANDLE Get() const
        {
            return handle;
        }

        HANDLE* Put()
        {
            Close();
            handle = INVALID_HANDLE_VALUE;
            return &handle;
        }

        bool IsValid() const
        {
            return handle != nullptr && handle != INVALID_HANDLE_VALUE;
        }

        void Reset(HANDLE newHandle)
        {
            if (handle != newHandle)
            {
                Close();
                handle = newHandle;
            }
        }

    private:
        void Close() noexcept
        {
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            {
                (void)::CloseHandle(handle);
                handle = INVALID_HANDLE_VALUE;
            }
        }

    private:
        HANDLE handle = INVALID_HANDLE_VALUE;
    };

    class SensitiveWideStringScope
    {
    public:
        explicit SensitiveWideStringScope(std::wstring& text) noexcept : text(text)
        {
        }

        ~SensitiveWideStringScope() noexcept
        {
            if (!text.empty())
            {
                (void)::SecureZeroMemory(&text[0], text.size() * sizeof(wchar_t));
            }
        }

        SensitiveWideStringScope(const SensitiveWideStringScope&) = delete;
        SensitiveWideStringScope& operator=(const SensitiveWideStringScope&) = delete;

    private:
        std::wstring& text;
    };

    static bool IsHResultFromWin32Error(const HRESULT hresult, const DWORD win32Error)
    {
        return hresult == HRESULT_FROM_WIN32(win32Error) || (hresult >= 0 && static_cast<DWORD>(hresult) == win32Error);
    }

    static HRESULT NormalizeBluetoothGattHResult(const HRESULT hresult)
    {
        if (hresult > 0 && hresult <= 0xffff)
        {
            return HRESULT_FROM_WIN32(static_cast<DWORD>(hresult));
        }

        return hresult;
    }

    static GB_SystemErrorCode GetBluetoothGattErrorCode(const HRESULT normalizedHResult)
    {
        if (IsHResultFromWin32Error(normalizedHResult, ERROR_NOT_FOUND) || IsHResultFromWin32Error(normalizedHResult, ERROR_FILE_NOT_FOUND))
        {
            return GB_SystemErrorCode::NotFound;
        }
        if (IsHResultFromWin32Error(normalizedHResult, ERROR_BAD_COMMAND) || IsHResultFromWin32Error(normalizedHResult, ERROR_INVALID_ACCESS) || IsHResultFromWin32Error(normalizedHResult, ERROR_INVALID_HANDLE) || IsHResultFromWin32Error(normalizedHResult, ERROR_NOT_READY) || IsHResultFromWin32Error(normalizedHResult, ERROR_DEVICE_NOT_CONNECTED) || IsHResultFromWin32Error(normalizedHResult, ERROR_ACCESS_DENIED))
        {
            return GB_SystemErrorCode::InvalidState;
        }
        if (IsHResultFromWin32Error(normalizedHResult, ERROR_INVALID_FUNCTION))
        {
            return GB_SystemErrorCode::InvalidState;
        }
        if (IsHResultFromWin32Error(normalizedHResult, ERROR_BAD_NET_RESP))
        {
            return GB_SystemErrorCode::OperationFailed;
        }
        if (IsHResultFromWin32Error(normalizedHResult, ERROR_INVALID_PARAMETER) || IsHResultFromWin32Error(normalizedHResult, ERROR_INVALID_USER_BUFFER))
        {
            return GB_SystemErrorCode::InvalidArgument;
        }
        if (IsHResultFromWin32Error(normalizedHResult, ERROR_PRIVILEGE_NOT_HELD))
        {
            return GB_SystemErrorCode::PermissionDenied;
        }
        if (IsHResultFromWin32Error(normalizedHResult, ERROR_BUSY) || IsHResultFromWin32Error(normalizedHResult, ERROR_RETRY))
        {
            return GB_SystemErrorCode::ResourceBusy;
        }
        if (IsHResultFromWin32Error(normalizedHResult, ERROR_OPERATION_ABORTED) || IsHResultFromWin32Error(normalizedHResult, ERROR_CANCELLED))
        {
            return GB_SystemErrorCode::Cancelled;
        }
        if (IsHResultFromWin32Error(normalizedHResult, ERROR_SEM_TIMEOUT) || IsHResultFromWin32Error(normalizedHResult, ERROR_TIMEOUT))
        {
            return GB_SystemErrorCode::Timeout;
        }
        if (IsHResultFromWin32Error(normalizedHResult, ERROR_NO_SYSTEM_RESOURCES) || IsHResultFromWin32Error(normalizedHResult, ERROR_NOT_ENOUGH_MEMORY) || normalizedHResult == E_OUTOFMEMORY)
        {
            return GB_SystemErrorCode::ResourceAllocationFailed;
        }
        if (IsHResultFromWin32Error(normalizedHResult, ERROR_NOT_SUPPORTED) || IsHResultFromWin32Error(normalizedHResult, ERROR_CALL_NOT_IMPLEMENTED) || normalizedHResult == E_NOTIMPL)
        {
            return GB_SystemErrorCode::UnsupportedPlatform;
        }

#if defined(E_BLUETOOTH_ATT_INVALID_HANDLE)
        if (normalizedHResult == E_BLUETOOTH_ATT_INVALID_HANDLE || normalizedHResult == E_BLUETOOTH_ATT_ATTRIBUTE_NOT_FOUND)
        {
            return GB_SystemErrorCode::NotFound;
        }
        if (normalizedHResult == E_BLUETOOTH_ATT_READ_NOT_PERMITTED || normalizedHResult == E_BLUETOOTH_ATT_WRITE_NOT_PERMITTED || normalizedHResult == E_BLUETOOTH_ATT_INSUFFICIENT_AUTHENTICATION || normalizedHResult == E_BLUETOOTH_ATT_INSUFFICIENT_AUTHORIZATION || normalizedHResult == E_BLUETOOTH_ATT_INSUFFICIENT_ENCRYPTION_KEY_SIZE || normalizedHResult == E_BLUETOOTH_ATT_INSUFFICIENT_ENCRYPTION)
        {
            return GB_SystemErrorCode::PermissionDenied;
        }
        if (normalizedHResult == E_BLUETOOTH_ATT_INVALID_PDU || normalizedHResult == E_BLUETOOTH_ATT_INVALID_OFFSET || normalizedHResult == E_BLUETOOTH_ATT_INVALID_ATTRIBUTE_VALUE_LENGTH)
        {
            return GB_SystemErrorCode::InvalidArgument;
        }
        if (normalizedHResult == E_BLUETOOTH_ATT_PREPARE_QUEUE_FULL)
        {
            return GB_SystemErrorCode::ResourceBusy;
        }
        if (normalizedHResult == E_BLUETOOTH_ATT_INSUFFICIENT_RESOURCES)
        {
            return GB_SystemErrorCode::ResourceAllocationFailed;
        }
        if (normalizedHResult == E_BLUETOOTH_ATT_REQUEST_NOT_SUPPORTED || normalizedHResult == E_BLUETOOTH_ATT_UNSUPPORTED_GROUP_TYPE)
        {
            return GB_SystemErrorCode::OperationFailed;
        }
#if defined(E_BLUETOOTH_ATT_ATTRIBUTE_NOT_LONG)
        if (normalizedHResult == E_BLUETOOTH_ATT_ATTRIBUTE_NOT_LONG)
        {
            return GB_SystemErrorCode::InvalidState;
        }
#endif
#if defined(E_BLUETOOTH_ATT_UNLIKELY)
        if (normalizedHResult == E_BLUETOOTH_ATT_UNLIKELY)
        {
            return GB_SystemErrorCode::OperationFailed;
        }
#endif
#if defined(E_BLUETOOTH_ATT_UNKNOWN_ERROR)
        if (normalizedHResult == E_BLUETOOTH_ATT_UNKNOWN_ERROR)
        {
            return GB_SystemErrorCode::OperationFailed;
        }
#endif
#endif

        const GB_SystemResult mappedResult = GB_SystemResult::FromHResult(static_cast<int32_t>(normalizedHResult));
        return mappedResult.IsSucceeded() ? GB_SystemErrorCode::OperationFailed : mappedResult.errorCode;
    }

    static GB_SystemResult MapBluetoothGattHResult(const HRESULT hresult, const std::string& operationName, const std::string& message, const bool invalidFunctionMeansResourceBusy = false)
    {
        const HRESULT normalizedHResult = NormalizeBluetoothGattHResult(hresult);
        if (normalizedHResult == S_OK)
        {
            return GB_SystemResult::Succeeded(operationName, message);
        }

        GB_SystemResult result = GB_SystemResult::FromHResult(static_cast<int32_t>(normalizedHResult), operationName, message).WithMessage(message);
        result.errorCode = invalidFunctionMeansResourceBusy && IsHResultFromWin32Error(normalizedHResult, ERROR_INVALID_FUNCTION) ? GB_SystemErrorCode::ResourceBusy : GetBluetoothGattErrorCode(normalizedHResult);
        return result;
    }

    static bool TryParseHexUInt32Fixed(const std::string& text, const size_t beginIndex, const size_t digitCount, uint32_t& value)
    {
        value = 0;
        if (beginIndex + digitCount > text.size())
        {
            return false;
        }

        uint32_t parsedValue = 0;
        for (size_t index = 0; index < digitCount; index++)
        {
            const int hexValue = HexValue(text[beginIndex + index]);
            if (hexValue < 0)
            {
                return false;
            }

            parsedValue = (parsedValue << 4) | static_cast<uint32_t>(hexValue);
        }

        value = parsedValue;
        return true;
    }

    static bool TryParseGuidString(const std::string& guidText, GUID& guid)
    {
        guid = GUID();
        if (guidText.empty() || ContainsNullCharacter(guidText))
        {
            return false;
        }

        size_t beginIndex = 0;
        while (beginIndex < guidText.size() && IsAsciiWhitespace(guidText[beginIndex]))
        {
            beginIndex++;
        }

        size_t endIndex = guidText.size();
        while (endIndex > beginIndex && IsAsciiWhitespace(guidText[endIndex - 1]))
        {
            endIndex--;
        }

        size_t bodyBeginIndex = beginIndex;
        const size_t trimmedLength = endIndex - beginIndex;
        if (trimmedLength == 38 && guidText[beginIndex] == '{' && guidText[endIndex - 1] == '}')
        {
            bodyBeginIndex++;
            endIndex--;
        }
        else if (trimmedLength != 36)
        {
            return false;
        }

        if (endIndex - bodyBeginIndex != 36 || guidText[bodyBeginIndex + 8] != '-' || guidText[bodyBeginIndex + 13] != '-' || guidText[bodyBeginIndex + 18] != '-' || guidText[bodyBeginIndex + 23] != '-')
        {
            return false;
        }

        uint32_t data1 = 0;
        uint32_t data2 = 0;
        uint32_t data3 = 0;
        uint32_t data4Part0 = 0;
        uint32_t data4Part1 = 0;
        uint32_t data4Part2 = 0;
        uint32_t data4Part3 = 0;
        uint32_t data4Part4 = 0;
        uint32_t data4Part5 = 0;
        uint32_t data4Part6 = 0;
        uint32_t data4Part7 = 0;
        if (!TryParseHexUInt32Fixed(guidText, bodyBeginIndex, 8, data1) || !TryParseHexUInt32Fixed(guidText, bodyBeginIndex + 9, 4, data2) || !TryParseHexUInt32Fixed(guidText, bodyBeginIndex + 14, 4, data3) || !TryParseHexUInt32Fixed(guidText, bodyBeginIndex + 19, 2, data4Part0) || !TryParseHexUInt32Fixed(guidText, bodyBeginIndex + 21, 2, data4Part1) || !TryParseHexUInt32Fixed(guidText, bodyBeginIndex + 24, 2, data4Part2) || !TryParseHexUInt32Fixed(guidText, bodyBeginIndex + 26, 2, data4Part3) || !TryParseHexUInt32Fixed(guidText, bodyBeginIndex + 28, 2, data4Part4) || !TryParseHexUInt32Fixed(guidText, bodyBeginIndex + 30, 2, data4Part5) || !TryParseHexUInt32Fixed(guidText, bodyBeginIndex + 32, 2, data4Part6) || !TryParseHexUInt32Fixed(guidText, bodyBeginIndex + 34, 2, data4Part7))
        {
            return false;
        }

        guid.Data1 = data1;
        guid.Data2 = static_cast<unsigned short>(data2);
        guid.Data3 = static_cast<unsigned short>(data3);
        guid.Data4[0] = static_cast<unsigned char>(data4Part0);
        guid.Data4[1] = static_cast<unsigned char>(data4Part1);
        guid.Data4[2] = static_cast<unsigned char>(data4Part2);
        guid.Data4[3] = static_cast<unsigned char>(data4Part3);
        guid.Data4[4] = static_cast<unsigned char>(data4Part4);
        guid.Data4[5] = static_cast<unsigned char>(data4Part5);
        guid.Data4[6] = static_cast<unsigned char>(data4Part6);
        guid.Data4[7] = static_cast<unsigned char>(data4Part7);
        return true;
    }

    static std::string FormatShortUuid(const uint16_t shortUuid)
    {
        char buffer[8] = {};
        std::snprintf(buffer, sizeof(buffer), "0x%04X", static_cast<unsigned int>(shortUuid));
        return std::string(buffer);
    }

    static std::string FormatBthLeUuid(const BTH_LE_UUID& uuid)
    {
        if (uuid.IsShortUuid != FALSE)
        {
            return FormatShortUuid(static_cast<uint16_t>(uuid.Value.ShortUuid));
        }

        return GuidToString(uuid.Value.LongUuid);
    }

    static bool AreBthLeUuidsEqual(const BTH_LE_UUID& left, const BTH_LE_UUID& right)
    {
        if ((left.IsShortUuid != FALSE) != (right.IsShortUuid != FALSE))
        {
            return false;
        }
        if (left.IsShortUuid != FALSE)
        {
            return left.Value.ShortUuid == right.Value.ShortUuid;
        }

        return ::IsEqualGUID(left.Value.LongUuid, right.Value.LongUuid) != FALSE;
    }

    static GB_SystemResult BuildNativeBthLeUuid(const std::string& uuidText, const bool isShortUuid, const uint16_t shortUuid, BTH_LE_UUID& uuid, const std::string& operationName)
    {
        uuid = BTH_LE_UUID();
        if (isShortUuid)
        {
            uuid.IsShortUuid = TRUE;
            uuid.Value.ShortUuid = shortUuid;
            return GB_SystemResult::Succeeded(operationName);
        }

        GUID longUuid = GUID();
        if (!TryParseGuidString(uuidText, longUuid))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE GATT UUID 不是有效 128 位 GUID。短 UUID 请使用 isShortUuid=true 和 shortUuid 字段。");
        }

        uuid.IsShortUuid = FALSE;
        uuid.Value.LongUuid = longUuid;
        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult ValidateGattReadOptions(const GB_BluetoothGattReadOptions& options, const std::string& operationName)
    {
        if (!IsValidGattCacheMode(options.cacheMode))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"cacheMode 不是有效的 GB_BluetoothGattCacheMode 值。");
        }

        return GB_SystemResult::Succeeded(operationName);
    }

    static ULONG BuildGattReadFlags(const GB_BluetoothGattReadOptions& options)
    {
        ULONG flags = BLUETOOTH_GATT_FLAG_NONE;
        if (options.cacheMode == GB_BluetoothGattCacheMode::ForceReadFromDevice)
        {
            flags |= BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE;
        }
        else if (options.cacheMode == GB_BluetoothGattCacheMode::ForceReadFromCache)
        {
            flags |= BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_CACHE;
        }

        if (options.requireEncryptedConnection)
        {
            flags |= BLUETOOTH_GATT_FLAG_CONNECTION_ENCRYPTED;
        }
        if (options.requireAuthenticatedConnection)
        {
            flags |= BLUETOOTH_GATT_FLAG_CONNECTION_AUTHENTICATED;
        }

        return flags;
    }

    static GB_SystemResult ValidateGattWriteOptions(const GB_BluetoothGattWriteOptions& options, const std::string& operationName)
    {
        if (options.signedWrite && !options.writeWithoutResponse)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"Signed Write 必须与 WriteWithoutResponse 一起使用。");
        }
        if (options.signedWrite && (options.requireEncryptedConnection || options.requireAuthenticatedConnection))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"Signed Write 不能同时要求加密或认证链路。");
        }

        return GB_SystemResult::Succeeded(operationName);
    }

    static ULONG BuildGattWriteFlags(const GB_BluetoothGattWriteOptions& options)
    {
        ULONG flags = BLUETOOTH_GATT_FLAG_NONE;
        if (options.writeWithoutResponse)
        {
            flags |= BLUETOOTH_GATT_FLAG_WRITE_WITHOUT_RESPONSE;
        }
        if (options.signedWrite)
        {
            flags |= BLUETOOTH_GATT_FLAG_SIGNED_WRITE;
        }
        if (options.requireEncryptedConnection)
        {
            flags |= BLUETOOTH_GATT_FLAG_CONNECTION_ENCRYPTED;
        }
        if (options.requireAuthenticatedConnection)
        {
            flags |= BLUETOOTH_GATT_FLAG_CONNECTION_AUTHENTICATED;
        }

        return flags;
    }

    enum class BluetoothLeDeviceHandleAccess
    {
        ReadOnly,
        ReadWrite
    };

    static GB_SystemResult OpenBluetoothLeDeviceHandle(const std::string& deviceInterfacePath, const BluetoothLeDeviceHandleAccess accessMode, GenericHandleScope& handleScope, const std::string& operationName)
    {
        handleScope.Reset(INVALID_HANDLE_VALUE);
        if (deviceInterfacePath.empty() || ContainsNullCharacter(deviceInterfacePath))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE 设备接口路径不能为空且不能包含空字符。");
        }

        std::wstring deviceInterfacePathWide;
        try
        {
            deviceInterfacePathWide = GB_Utf8ToWString(deviceInterfacePath);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"转换 BLE 设备接口路径时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, operationName, u8"BLE 设备接口路径 UTF-8 转 UTF-16 失败。");
        }

        HANDLE deviceHandle = ::CreateFileW(deviceInterfacePathWide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD readWriteError = deviceHandle == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
        if (deviceHandle == INVALID_HANDLE_VALUE && accessMode == BluetoothLeDeviceHandleAccess::ReadOnly)
        {
            deviceHandle = ::CreateFileW(deviceInterfacePathWide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (deviceHandle == INVALID_HANDLE_VALUE)
        {
            const DWORD lastError = ::GetLastError();
            GB_SystemResult result = GB_SystemResult::FromWin32Error(static_cast<uint32_t>(lastError), operationName, accessMode == BluetoothLeDeviceHandleAccess::ReadWrite ? u8"CreateFileW 以读写方式打开 BLE 设备接口失败。" : u8"CreateFileW 打开 BLE 设备接口失败；读写方式和只读降级方式均不可用。");
            if (accessMode == BluetoothLeDeviceHandleAccess::ReadOnly && readWriteError != ERROR_SUCCESS && readWriteError != lastError)
            {
                try
                {
                    result.message += u8" 首次读写打开的 Win32 错误码=";
                    result.message += std::to_string(static_cast<uint32_t>(readWriteError));
                    result.message += ".";
                }
                catch (...)
                {
                }
            }
            return result;
        }

        handleScope.Reset(deviceHandle);
        return GB_SystemResult::Succeeded(operationName);
    }

    static size_t GetGattCharacteristicValueHeaderSize()
    {
        return offsetof(BTH_LE_GATT_CHARACTERISTIC_VALUE, Data);
    }

    static size_t GetMaxGattCharacteristicValueDataSize()
    {
#if defined(BTH_LE_ATT_MAX_VALUE_SIZE)
        return static_cast<size_t>(BTH_LE_ATT_MAX_VALUE_SIZE);
#else
        return 512;
#endif
    }

    static size_t GetGattCharacteristicValueBufferByteCapacity(const std::vector<ULONG>& valueBuffer)
    {
        if (valueBuffer.size() > (std::numeric_limits<size_t>::max)() / sizeof(ULONG))
        {
            return 0;
        }

        return valueBuffer.size() * sizeof(ULONG);
    }

    static GB_SystemResult AllocateGattCharacteristicValueBuffer(const size_t dataSize, std::vector<ULONG>& valueBuffer, const std::string& operationName, const std::string& failureMessage)
    {
        valueBuffer.clear();
        const size_t headerSize = GetGattCharacteristicValueHeaderSize();
        if (dataSize > static_cast<size_t>((std::numeric_limits<ULONG>::max)()) || headerSize > (std::numeric_limits<size_t>::max)() - dataSize)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE GATT 特征值长度过大。");
        }

        size_t bufferSize = headerSize + dataSize;
        if (bufferSize < sizeof(BTH_LE_GATT_CHARACTERISTIC_VALUE))
        {
            bufferSize = sizeof(BTH_LE_GATT_CHARACTERISTIC_VALUE);
        }

        const size_t wordCount = bufferSize / sizeof(ULONG) + (bufferSize % sizeof(ULONG) == 0 ? 0 : 1);
        try
        {
            valueBuffer.assign(wordCount, 0);
        }
        catch (...)
        {
            valueBuffer.clear();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, failureMessage);
        }

        return GB_SystemResult::Succeeded(operationName);
    }

    static uint32_t BuildGattCharacteristicPropertyFlags(const BTH_LE_GATT_CHARACTERISTIC& characteristic)
    {
        uint32_t propertyFlags = 0;
        if (characteristic.IsBroadcastable != FALSE)
        {
            propertyFlags |= static_cast<uint32_t>(GB_BluetoothGattCharacteristicProperty::Broadcast);
        }
        if (characteristic.IsReadable != FALSE)
        {
            propertyFlags |= static_cast<uint32_t>(GB_BluetoothGattCharacteristicProperty::Read);
        }
        if (characteristic.IsWritable != FALSE)
        {
            propertyFlags |= static_cast<uint32_t>(GB_BluetoothGattCharacteristicProperty::Write);
        }
        if (characteristic.IsWritableWithoutResponse != FALSE)
        {
            propertyFlags |= static_cast<uint32_t>(GB_BluetoothGattCharacteristicProperty::WriteWithoutResponse);
        }
        if (characteristic.IsSignedWritable != FALSE)
        {
            propertyFlags |= static_cast<uint32_t>(GB_BluetoothGattCharacteristicProperty::SignedWrite);
        }
        if (characteristic.IsNotifiable != FALSE)
        {
            propertyFlags |= static_cast<uint32_t>(GB_BluetoothGattCharacteristicProperty::Notify);
        }
        if (characteristic.IsIndicatable != FALSE)
        {
            propertyFlags |= static_cast<uint32_t>(GB_BluetoothGattCharacteristicProperty::Indicate);
        }
        if (characteristic.HasExtendedProperties != FALSE)
        {
            propertyFlags |= static_cast<uint32_t>(GB_BluetoothGattCharacteristicProperty::ExtendedProperties);
        }

        return propertyFlags;
    }

    static GB_BluetoothGattServiceInfo ConvertGattServiceInfo(const BTH_LE_GATT_SERVICE& nativeService, const std::string& deviceInterfacePath)
    {
        GB_BluetoothGattServiceInfo service;
        service.deviceInterfacePath = deviceInterfacePath;
        service.deviceId = deviceInterfacePath;
        service.uuid = FormatBthLeUuid(nativeService.ServiceUuid);
        service.isShortUuid = nativeService.ServiceUuid.IsShortUuid != FALSE;
        service.shortUuid = service.isShortUuid ? static_cast<uint16_t>(nativeService.ServiceUuid.Value.ShortUuid) : 0;
        service.attributeHandle = static_cast<uint16_t>(nativeService.AttributeHandle);
        return service;
    }

    static GB_BluetoothGattCharacteristicInfo ConvertGattCharacteristicInfo(const BTH_LE_GATT_CHARACTERISTIC& nativeCharacteristic, const GB_BluetoothGattServiceInfo& service, const std::string& deviceInterfacePath)
    {
        GB_BluetoothGattCharacteristicInfo characteristic;
        characteristic.deviceInterfacePath = deviceInterfacePath;
        characteristic.deviceId = deviceInterfacePath;
        characteristic.serviceUuid = service.uuid;
        characteristic.serviceShortUuid = service.shortUuid;
        characteristic.isServiceShortUuid = service.isShortUuid;
        characteristic.serviceAttributeHandle = static_cast<uint16_t>(nativeCharacteristic.ServiceHandle);
        characteristic.characteristicUuid = FormatBthLeUuid(nativeCharacteristic.CharacteristicUuid);
        characteristic.isCharacteristicShortUuid = nativeCharacteristic.CharacteristicUuid.IsShortUuid != FALSE;
        characteristic.characteristicShortUuid = characteristic.isCharacteristicShortUuid ? static_cast<uint16_t>(nativeCharacteristic.CharacteristicUuid.Value.ShortUuid) : 0;
        characteristic.attributeHandle = static_cast<uint16_t>(nativeCharacteristic.AttributeHandle);
        characteristic.characteristicValueHandle = static_cast<uint16_t>(nativeCharacteristic.CharacteristicValueHandle);
        characteristic.propertyFlags = BuildGattCharacteristicPropertyFlags(nativeCharacteristic);
        characteristic.isBroadcastable = nativeCharacteristic.IsBroadcastable != FALSE;
        characteristic.isReadable = nativeCharacteristic.IsReadable != FALSE;
        characteristic.isWritable = nativeCharacteristic.IsWritable != FALSE;
        characteristic.isWritableWithoutResponse = nativeCharacteristic.IsWritableWithoutResponse != FALSE;
        characteristic.isSignedWritable = nativeCharacteristic.IsSignedWritable != FALSE;
        characteristic.isNotifiable = nativeCharacteristic.IsNotifiable != FALSE;
        characteristic.isIndicatable = nativeCharacteristic.IsIndicatable != FALSE;
        characteristic.hasExtendedProperties = nativeCharacteristic.HasExtendedProperties != FALSE;
        return characteristic;
    }

    static bool TryGetLargerUShortCount(const USHORT currentCount, const USHORT returnedCount, const USHORT refreshedCount, USHORT& nextCount)
    {
        const size_t maxCount = static_cast<size_t>((std::numeric_limits<USHORT>::max)());
        size_t candidateCount = (std::max)(static_cast<size_t>(returnedCount), static_cast<size_t>(refreshedCount));
        if (candidateCount <= static_cast<size_t>(currentCount))
        {
            const size_t growth = (std::max)(static_cast<size_t>(1), static_cast<size_t>(currentCount) / 2u);
            candidateCount = (std::min)(maxCount, static_cast<size_t>(currentCount) + growth);
        }
        if (candidateCount <= static_cast<size_t>(currentCount) || candidateCount > maxCount)
        {
            nextCount = currentCount;
            return false;
        }

        nextCount = static_cast<USHORT>(candidateCount);
        return true;
    }

    static bool TryGetLargerGuidCount(const DWORD currentCount, const DWORD returnedCount, const DWORD refreshedCount, DWORD& nextCount)
    {
        const size_t maxCount = (std::min)(static_cast<size_t>((std::numeric_limits<DWORD>::max)()), GB_MaxNativeBluetoothArrayBytes / sizeof(GUID));
        size_t candidateCount = (std::max)(static_cast<size_t>(returnedCount), static_cast<size_t>(refreshedCount));
        if (candidateCount <= static_cast<size_t>(currentCount))
        {
            const size_t growth = (std::max)(static_cast<size_t>(1), static_cast<size_t>(currentCount) / 2u);
            candidateCount = (std::min)(maxCount, static_cast<size_t>(currentCount) + growth);
        }
        if (candidateCount <= static_cast<size_t>(currentCount) || candidateCount > maxCount)
        {
            nextCount = currentCount;
            return false;
        }

        nextCount = static_cast<DWORD>(candidateCount);
        return true;
    }

    static GB_SystemResult ReadNativeGattServices(HANDLE deviceHandle, std::vector<BTH_LE_GATT_SERVICE>& services, const std::string& operationName)
    {
        services.clear();
        if (deviceHandle == nullptr || deviceHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE 设备句柄无效。");
        }

        USHORT serviceCount = 0;
        HRESULT hresult = ::BluetoothGATTGetServices(deviceHandle, 0, nullptr, &serviceCount, BLUETOOTH_GATT_FLAG_NONE);
        if (hresult != S_OK && !IsHResultFromWin32Error(hresult, ERROR_MORE_DATA) && !IsHResultFromWin32Error(hresult, ERROR_INVALID_USER_BUFFER))
        {
            return MapBluetoothGattHResult(hresult, operationName, u8"BluetoothGATTGetServices 查询 BLE GATT 服务数量失败。");
        }
        if (serviceCount == 0)
        {
            return GB_SystemResult::Succeeded(operationName);
        }

        for (int retryIndex = 0; retryIndex < 5; retryIndex++)
        {
            if (!IsReasonableNativeBluetoothArrayCount<BTH_LE_GATT_SERVICE>(static_cast<size_t>(serviceCount)))
            {
                services.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"系统返回的 BLE GATT 服务数量对应缓冲区过大，已拒绝分配。");
            }

            try
            {
                services.resize(static_cast<size_t>(serviceCount));
            }
            catch (...)
            {
                services.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"分配 BLE GATT 服务缓冲区时内存不足。");
            }

            USHORT returnedServiceCount = serviceCount;
            hresult = ::BluetoothGATTGetServices(deviceHandle, serviceCount, services.data(), &returnedServiceCount, BLUETOOTH_GATT_FLAG_NONE);
            if (hresult == S_OK && returnedServiceCount <= serviceCount)
            {
                services.resize(static_cast<size_t>(returnedServiceCount));
                return GB_SystemResult::Succeeded(operationName);
            }

            if (hresult == S_OK || IsHResultFromWin32Error(hresult, ERROR_MORE_DATA) || IsHResultFromWin32Error(hresult, ERROR_INVALID_USER_BUFFER))
            {
                USHORT refreshedServiceCount = 0;
                const HRESULT countHResult = ::BluetoothGATTGetServices(deviceHandle, 0, nullptr, &refreshedServiceCount, BLUETOOTH_GATT_FLAG_NONE);
                const bool countQuerySucceeded = countHResult == S_OK || IsHResultFromWin32Error(countHResult, ERROR_MORE_DATA) || IsHResultFromWin32Error(countHResult, ERROR_INVALID_USER_BUFFER);
                USHORT nextServiceCount = serviceCount;
                if (!TryGetLargerUShortCount(serviceCount, returnedServiceCount, countQuerySucceeded ? refreshedServiceCount : 0, nextServiceCount))
                {
                    services.clear();
                    return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, u8"BluetoothGATTGetServices 持续报告缓冲区不足，但无法得到更大的有效服务数量。");
                }
                if (!countQuerySucceeded && returnedServiceCount <= serviceCount)
                {
                    services.clear();
                    return MapBluetoothGattHResult(countHResult, operationName, u8"BluetoothGATTGetServices 重新查询 BLE GATT 服务数量失败。");
                }

                serviceCount = nextServiceCount;
                continue;
            }

            services.clear();
            return MapBluetoothGattHResult(hresult, operationName, u8"BluetoothGATTGetServices 读取 BLE GATT 服务失败。");
        }

        services.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, u8"BluetoothGATTGetServices 多次返回更多数据，服务数量持续变化。");
    }

    static GB_SystemResult ReadNativeGattCharacteristics(HANDLE deviceHandle, const BTH_LE_GATT_SERVICE& service, std::vector<BTH_LE_GATT_CHARACTERISTIC>& characteristics, const std::string& operationName)
    {
        characteristics.clear();
        if (deviceHandle == nullptr || deviceHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE 设备句柄无效。");
        }

        USHORT characteristicCount = 0;
        HRESULT hresult = ::BluetoothGATTGetCharacteristics(deviceHandle, const_cast<BTH_LE_GATT_SERVICE*>(&service), 0, nullptr, &characteristicCount, BLUETOOTH_GATT_FLAG_NONE);
        if (hresult != S_OK && !IsHResultFromWin32Error(hresult, ERROR_MORE_DATA) && !IsHResultFromWin32Error(hresult, ERROR_INVALID_USER_BUFFER))
        {
            return MapBluetoothGattHResult(hresult, operationName, u8"BluetoothGATTGetCharacteristics 查询 BLE GATT 特征数量失败。");
        }
        if (characteristicCount == 0)
        {
            return GB_SystemResult::Succeeded(operationName);
        }

        for (int retryIndex = 0; retryIndex < 5; retryIndex++)
        {
            if (!IsReasonableNativeBluetoothArrayCount<BTH_LE_GATT_CHARACTERISTIC>(static_cast<size_t>(characteristicCount)))
            {
                characteristics.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"系统返回的 BLE GATT 特征数量对应缓冲区过大，已拒绝分配。");
            }

            try
            {
                characteristics.resize(static_cast<size_t>(characteristicCount));
            }
            catch (...)
            {
                characteristics.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"分配 BLE GATT 特征缓冲区时内存不足。");
            }

            USHORT returnedCharacteristicCount = characteristicCount;
            hresult = ::BluetoothGATTGetCharacteristics(deviceHandle, const_cast<BTH_LE_GATT_SERVICE*>(&service), characteristicCount, characteristics.data(), &returnedCharacteristicCount, BLUETOOTH_GATT_FLAG_NONE);
            if (hresult == S_OK && returnedCharacteristicCount <= characteristicCount)
            {
                characteristics.resize(static_cast<size_t>(returnedCharacteristicCount));
                return GB_SystemResult::Succeeded(operationName);
            }

            if (hresult == S_OK || IsHResultFromWin32Error(hresult, ERROR_MORE_DATA) || IsHResultFromWin32Error(hresult, ERROR_INVALID_USER_BUFFER))
            {
                USHORT refreshedCharacteristicCount = 0;
                const HRESULT countHResult = ::BluetoothGATTGetCharacteristics(deviceHandle, const_cast<BTH_LE_GATT_SERVICE*>(&service), 0, nullptr, &refreshedCharacteristicCount, BLUETOOTH_GATT_FLAG_NONE);
                const bool countQuerySucceeded = countHResult == S_OK || IsHResultFromWin32Error(countHResult, ERROR_MORE_DATA) || IsHResultFromWin32Error(countHResult, ERROR_INVALID_USER_BUFFER);
                USHORT nextCharacteristicCount = characteristicCount;
                if (!TryGetLargerUShortCount(characteristicCount, returnedCharacteristicCount, countQuerySucceeded ? refreshedCharacteristicCount : 0, nextCharacteristicCount))
                {
                    characteristics.clear();
                    return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, u8"BluetoothGATTGetCharacteristics 持续报告缓冲区不足，但无法得到更大的有效特征数量。");
                }
                if (!countQuerySucceeded && returnedCharacteristicCount <= characteristicCount)
                {
                    characteristics.clear();
                    return MapBluetoothGattHResult(countHResult, operationName, u8"BluetoothGATTGetCharacteristics 重新查询 BLE GATT 特征数量失败。");
                }

                characteristicCount = nextCharacteristicCount;
                continue;
            }

            characteristics.clear();
            return MapBluetoothGattHResult(hresult, operationName, u8"BluetoothGATTGetCharacteristics 读取 BLE GATT 特征失败。");
        }

        characteristics.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, u8"BluetoothGATTGetCharacteristics 多次返回更多数据，特征数量持续变化。");
    }

    static GB_SystemResult FindNativeGattService(HANDLE deviceHandle, const GB_BluetoothGattServiceInfo& service, BTH_LE_GATT_SERVICE& nativeService, const std::string& operationName)
    {
        nativeService = BTH_LE_GATT_SERVICE();
        if (service.attributeHandle == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE GATT 服务 attributeHandle 不能为 0。");
        }

        BTH_LE_UUID expectedUuid = BTH_LE_UUID();
        GB_SystemResult uuidResult = BuildNativeBthLeUuid(service.uuid, service.isShortUuid, service.shortUuid, expectedUuid, operationName);
        if (uuidResult.IsFailed())
        {
            return uuidResult;
        }

        std::vector<BTH_LE_GATT_SERVICE> nativeServices;
        GB_SystemResult serviceResult = ReadNativeGattServices(deviceHandle, nativeServices, operationName);
        if (serviceResult.IsFailed())
        {
            return serviceResult;
        }

        for (size_t index = 0; index < nativeServices.size(); index++)
        {
            if (nativeServices[index].AttributeHandle == service.attributeHandle && AreBthLeUuidsEqual(nativeServices[index].ServiceUuid, expectedUuid))
            {
                nativeService = nativeServices[index];
                return GB_SystemResult::Succeeded(operationName);
            }
        }

        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"未在当前 BLE 设备中找到指定 GATT 服务，设备缓存可能已变化。");
    }

    static GB_SystemResult FindNativeGattCharacteristic(HANDLE deviceHandle, const GB_BluetoothGattCharacteristicInfo& characteristic, BTH_LE_GATT_CHARACTERISTIC& nativeCharacteristic, const std::string& operationName)
    {
        nativeCharacteristic = BTH_LE_GATT_CHARACTERISTIC();
        if (characteristic.serviceAttributeHandle == 0 || characteristic.attributeHandle == 0 || characteristic.characteristicValueHandle == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE GATT 特征句柄信息不完整。");
        }

        GB_BluetoothGattServiceInfo service;
        try
        {
            service.deviceId = characteristic.deviceId;
            service.deviceInterfacePath = characteristic.deviceInterfacePath;
            service.uuid = characteristic.serviceUuid;
            service.shortUuid = characteristic.serviceShortUuid;
            service.isShortUuid = characteristic.isServiceShortUuid;
            service.attributeHandle = characteristic.serviceAttributeHandle;
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"保存 BLE GATT 服务定位信息时内存不足。");
        }

        BTH_LE_GATT_SERVICE nativeService = BTH_LE_GATT_SERVICE();
        GB_SystemResult serviceResult = FindNativeGattService(deviceHandle, service, nativeService, operationName);
        if (serviceResult.IsFailed())
        {
            return serviceResult;
        }

        BTH_LE_UUID expectedUuid = BTH_LE_UUID();
        GB_SystemResult uuidResult = BuildNativeBthLeUuid(characteristic.characteristicUuid, characteristic.isCharacteristicShortUuid, characteristic.characteristicShortUuid, expectedUuid, operationName);
        if (uuidResult.IsFailed())
        {
            return uuidResult;
        }

        std::vector<BTH_LE_GATT_CHARACTERISTIC> nativeCharacteristics;
        GB_SystemResult characteristicResult = ReadNativeGattCharacteristics(deviceHandle, nativeService, nativeCharacteristics, operationName);
        if (characteristicResult.IsFailed())
        {
            return characteristicResult;
        }

        for (size_t index = 0; index < nativeCharacteristics.size(); index++)
        {
            if (nativeCharacteristics[index].ServiceHandle == characteristic.serviceAttributeHandle && nativeCharacteristics[index].AttributeHandle == characteristic.attributeHandle && nativeCharacteristics[index].CharacteristicValueHandle == characteristic.characteristicValueHandle && AreBthLeUuidsEqual(nativeCharacteristics[index].CharacteristicUuid, expectedUuid))
            {
                nativeCharacteristic = nativeCharacteristics[index];
                return GB_SystemResult::Succeeded(operationName);
            }
        }

        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"未在当前 BLE GATT 服务中找到指定特征，设备缓存可能已变化。");
    }

    static bool TryExtractBluetoothAddressFromText(const std::string& text, std::string& address)
    {
        address.clear();
        if (text.size() < 16)
        {
            return false;
        }

        for (size_t index = 0; index + 16 <= text.size(); index++)
        {
            if (ToLowerAsciiChar(text[index]) != 'd' || ToLowerAsciiChar(text[index + 1]) != 'e' || ToLowerAsciiChar(text[index + 2]) != 'v' || (text[index + 3] != '_' && text[index + 3] != '-'))
            {
                continue;
            }

            const size_t addressBegin = index + 4;
            bool allHex = true;
            for (size_t digitIndex = 0; digitIndex < 12; digitIndex++)
            {
                if (HexValue(text[addressBegin + digitIndex]) < 0)
                {
                    allHex = false;
                    break;
                }
            }
            if (!allHex)
            {
                continue;
            }
            if (addressBegin + 12 < text.size() && HexValue(text[addressBegin + 12]) >= 0)
            {
                continue;
            }

            uint64_t addressValue = 0;
            for (size_t digitIndex = 0; digitIndex < 12; digitIndex++)
            {
                addressValue = (addressValue << 4) | static_cast<uint64_t>(HexValue(text[addressBegin + digitIndex]));
            }
            if (addressValue == 0)
            {
                continue;
            }

            address = FormatBluetoothAddressValue(addressValue);
            return true;
        }

        return false;
    }

    static GB_BluetoothDeviceInfo ConvertLowEnergyDeviceInterfaceInfo(const GB_SystemDeviceInterfaceInfo& deviceInterface)
    {
        GB_BluetoothDeviceInfo device;
        device.deviceInstanceId = deviceInterface.deviceInstanceId;
        device.deviceInterfacePath = deviceInterface.interfacePath;
        device.nativeDeviceId = !device.deviceInstanceId.empty() ? device.deviceInstanceId : device.deviceInterfacePath;
        device.deviceId = device.nativeDeviceId;
        device.name = deviceInterface.associatedDeviceName;

        std::string address;
        if (TryExtractBluetoothAddressFromText(device.deviceInstanceId, address) || TryExtractBluetoothAddressFromText(device.deviceInterfacePath, address))
        {
            device.address = address;
        }

        device.deviceKind = GB_BluetoothDeviceKind::LowEnergy;
        device.pairStatus = GB_BluetoothPairStatus::Unknown;
        device.connectionStatus = GB_BluetoothConnectionStatus::Unknown;
        device.isRemembered = false;
        device.isAuthenticated = false;
        device.isConnected = false;
        device.isClassicSupported = false;
        device.isLowEnergySupported = true;
        device.sourceName = u8"Win32BluetoothLEDeviceInterface";
        return device;
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

        try
        {
            radio.address = FormatBluetoothAddress(nativeRadioInfo.address);
            radio.radioId = radio.address;
            radio.nativeDeviceId.clear();
            radio.name = WideNullTerminatedStringToUtf8(nativeRadioInfo.szName);
            radio.classOfDevice = nativeRadioInfo.ulClassofDevice;
            radio.manufacturer = nativeRadioInfo.manufacturer;
            radio.isClassicSupported = true;
            radio.isLowEnergySupported = false;
            radio.isConnectable = ::BluetoothIsConnectable(radioHandle) != FALSE;
            radio.isDiscoverable = ::BluetoothIsDiscoverable(radioHandle) != FALSE;
        }
        catch (...)
        {
            radio = GB_BluetoothRadioInfo();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetRadios", u8"保存蓝牙无线电信息时内存不足。");
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetRadios");
    }

    static GB_SystemResult GetFirstRadioInfo(GB_BluetoothRadioInfo& radio, bool& found, const std::string& operationName)
    {
        radio = GB_BluetoothRadioInfo();
        found = false;

        BLUETOOTH_FIND_RADIO_PARAMS radioParams = {};
        radioParams.dwSize = sizeof(radioParams);

        HANDLE firstRadioHandle = nullptr;
        ::SetLastError(ERROR_SUCCESS);
        HBLUETOOTH_RADIO_FIND findHandle = ::BluetoothFindFirstRadio(&radioParams, &firstRadioHandle);
        if (findHandle == nullptr)
        {
            const DWORD lastError = ::GetLastError();
            if (lastError == ERROR_SUCCESS || lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_SERVICE_DOES_NOT_EXIST || lastError == ERROR_SERVICE_NOT_ACTIVE)
            {
                return GB_SystemResult::Succeeded(operationName, u8"当前系统未发现蓝牙无线电。");
            }

            return GB_SystemResult::FromWin32Error(lastError, operationName, u8"BluetoothFindFirstRadio 查找第一块蓝牙无线电失败。");
        }

        BluetoothRadioFindScope findScope(findHandle);
        GenericHandleScope radioHandle(firstRadioHandle);
        GB_SystemResult infoResult = BuildRadioInfo(radioHandle.Get(), radio);
        if (infoResult.IsFailed())
        {
            return infoResult.WithOperationName(operationName);
        }

        found = true;
        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult CheckBluetoothRadioAvailable(bool& available, const std::string& operationName)
    {
        available = false;

        BLUETOOTH_FIND_RADIO_PARAMS radioParams = {};
        radioParams.dwSize = sizeof(radioParams);

        HANDLE firstRadioHandle = nullptr;
        ::SetLastError(ERROR_SUCCESS);
        HBLUETOOTH_RADIO_FIND findHandle = ::BluetoothFindFirstRadio(&radioParams, &firstRadioHandle);
        if (findHandle == nullptr)
        {
            const DWORD lastError = ::GetLastError();
            if (lastError == ERROR_SUCCESS || lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_SERVICE_DOES_NOT_EXIST || lastError == ERROR_SERVICE_NOT_ACTIVE)
            {
                return GB_SystemResult::Succeeded(operationName, u8"当前系统未发现蓝牙无线电。");
            }

            return GB_SystemResult::FromWin32Error(lastError, operationName, u8"BluetoothFindFirstRadio 检查蓝牙无线电可用性失败。");
        }

        BluetoothRadioFindScope findScope(findHandle);
        GenericHandleScope radioHandle(firstRadioHandle);
        if (!radioHandle.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"BluetoothFindFirstRadio 返回了无效蓝牙无线电句柄。");
        }

        available = true;
        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult BuildBluetoothBooleanApiFailureResult(const DWORD lastError, const std::string& operationName, const std::string& message)
    {
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
            const DWORD lastError = ::GetLastError();
            if ((::BluetoothIsConnectable(radioHandle) != FALSE) == enabled)
            {
                return GB_SystemResult::Succeeded(operationName);
            }

            return BuildBluetoothBooleanApiFailureResult(lastError, operationName, enabled ? u8"BluetoothEnableIncomingConnections 打开入站连接失败。" : u8"BluetoothEnableIncomingConnections 关闭入站连接失败。");
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
            const DWORD lastError = ::GetLastError();
            if ((::BluetoothIsDiscoverable(radioHandle) != FALSE) == enabled)
            {
                return GB_SystemResult::Succeeded(operationName);
            }

            return BuildBluetoothBooleanApiFailureResult(lastError, operationName, enabled ? u8"BluetoothEnableDiscovery 打开可发现性失败。" : u8"BluetoothEnableDiscovery 关闭可发现性失败。");
        }

        return GB_SystemResult::Succeeded(operationName);
    }

    static std::mutex& GetRadioStateChangeMutex()
    {
        static std::mutex radioStateChangeMutex;
        return radioStateChangeMutex;
    }

    struct BluetoothRadioStateSnapshot
    {
        HANDLE radioHandle = nullptr;
        bool isConnectable = false;
        bool isDiscoverable = false;
    };

    static GB_SystemResult CaptureRadioStateSnapshots(const std::vector<BluetoothRadioHandleEntry*>& targetRadioHandles, std::vector<BluetoothRadioStateSnapshot>& snapshots, const std::string& operationName)
    {
        snapshots.clear();
        try
        {
            snapshots.reserve(targetRadioHandles.size());
            for (size_t index = 0; index < targetRadioHandles.size(); index++)
            {
                HANDLE radioHandle = targetRadioHandles[index] != nullptr ? targetRadioHandles[index]->GetHandle() : nullptr;
                if (radioHandle == nullptr || radioHandle == INVALID_HANDLE_VALUE)
                {
                    snapshots.clear();
                    return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"目标蓝牙无线电句柄无效，无法创建状态快照。");
                }

                BluetoothRadioStateSnapshot snapshot;
                snapshot.radioHandle = radioHandle;
                snapshot.isConnectable = ::BluetoothIsConnectable(radioHandle) != FALSE;
                snapshot.isDiscoverable = ::BluetoothIsDiscoverable(radioHandle) != FALSE;
                snapshots.push_back(snapshot);
            }
        }
        catch (...)
        {
            snapshots.clear();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"保存蓝牙无线电原始状态时内存不足。");
        }

        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult RestoreRadioState(const BluetoothRadioStateSnapshot& snapshot, const std::string& operationName)
    {
        if (snapshot.isDiscoverable)
        {
            GB_SystemResult connectableResult = SetRadioIncomingConnections(snapshot.radioHandle, true, operationName);
            if (connectableResult.IsFailed())
            {
                return connectableResult;
            }

            return SetRadioDiscovery(snapshot.radioHandle, true, operationName);
        }

        GB_SystemResult discoveryResult = SetRadioDiscovery(snapshot.radioHandle, false, operationName);
        if (discoveryResult.IsFailed())
        {
            return discoveryResult;
        }

        return SetRadioIncomingConnections(snapshot.radioHandle, snapshot.isConnectable, operationName);
    }

    static GB_SystemResult RestoreRadioStateSnapshots(const std::vector<BluetoothRadioStateSnapshot>& snapshots, const size_t snapshotCount, const std::string& operationName)
    {
        const size_t restoreCount = (std::min)(snapshotCount, snapshots.size());
        GB_SystemResult firstFailure = GB_SystemResult::Succeeded(operationName);
        for (size_t reverseIndex = restoreCount; reverseIndex > 0; reverseIndex--)
        {
            GB_SystemResult restoreResult = RestoreRadioState(snapshots[reverseIndex - 1], operationName);
            if (restoreResult.IsFailed() && firstFailure.IsSucceeded())
            {
                firstFailure = std::move(restoreResult);
            }
        }

        return firstFailure;
    }

    static GB_SystemResult RollbackRadioStatesAfterFailure(GB_SystemResult failureResult, const std::vector<BluetoothRadioStateSnapshot>& snapshots, const size_t snapshotCount, const std::string& operationName)
    {
        const GB_SystemResult rollbackResult = RestoreRadioStateSnapshots(snapshots, snapshotCount, operationName);
        if (rollbackResult.IsFailed())
        {
            try
            {
                failureResult.message += u8" 状态修改失败后尝试回滚，但至少一个蓝牙无线电未能恢复到原始状态：";
                failureResult.message += rollbackResult.GetDisplayMessage();
            }
            catch (...)
            {
            }
        }

        return failureResult;
    }

    static GB_SystemResult EnumerateRadioHandles(std::vector<BluetoothRadioHandleEntry>& radioHandles)
    {
        radioHandles.clear();

        BLUETOOTH_FIND_RADIO_PARAMS radioParams = {};
        radioParams.dwSize = sizeof(radioParams);

        HANDLE firstRadioHandle = nullptr;
        ::SetLastError(ERROR_SUCCESS);
        HBLUETOOTH_RADIO_FIND findHandle = ::BluetoothFindFirstRadio(&radioParams, &firstRadioHandle);
        if (findHandle == nullptr)
        {
            const DWORD lastError = ::GetLastError();
            if (lastError == ERROR_SUCCESS || lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_SERVICE_DOES_NOT_EXIST || lastError == ERROR_SERVICE_NOT_ACTIVE)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetRadios", u8"当前系统未发现蓝牙无线电。");
            }

            return GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemBluetooth::GetRadios", u8"BluetoothFindFirstRadio 枚举蓝牙无线电失败。");
        }

        BluetoothRadioFindScope findScope(findHandle);
        HANDLE currentRadioHandle = firstRadioHandle;
        if (currentRadioHandle == nullptr || currentRadioHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemBluetooth::GetRadios", u8"BluetoothFindFirstRadio 返回了无效蓝牙无线电句柄。");
        }

        while (true)
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
                BluetoothRadioHandleEntry radioHandleEntry(currentRadioHandle, std::move(radioInfo));
                currentRadioHandle = nullptr;
                radioHandles.push_back(std::move(radioHandleEntry));
            }
            catch (...)
            {
                if (currentRadioHandle != nullptr && currentRadioHandle != INVALID_HANDLE_VALUE)
                {
                    (void)::CloseHandle(currentRadioHandle);
                    currentRadioHandle = nullptr;
                }
                radioHandles.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetRadios", u8"保存蓝牙无线电句柄时内存不足。");
            }
            ::SetLastError(ERROR_SUCCESS);
            if (::BluetoothFindNextRadio(findScope.Get(), &currentRadioHandle) == FALSE)
            {
                const DWORD nextError = ::GetLastError();
                if (nextError == ERROR_SUCCESS || nextError == ERROR_NO_MORE_ITEMS)
                {
                    if (currentRadioHandle != nullptr && currentRadioHandle != INVALID_HANDLE_VALUE)
                    {
                        (void)::CloseHandle(currentRadioHandle);
                        currentRadioHandle = nullptr;
                    }
                    break;
                }

                if (currentRadioHandle != nullptr && currentRadioHandle != INVALID_HANDLE_VALUE)
                {
                    (void)::CloseHandle(currentRadioHandle);
                    currentRadioHandle = nullptr;
                }
                radioHandles.clear();
                return GB_SystemResult::FromWin32Error(nextError, u8"GB_SystemBluetooth::GetRadios", u8"BluetoothFindNextRadio 枚举下一个蓝牙无线电失败。");
            }
            if (currentRadioHandle == nullptr || currentRadioHandle == INVALID_HANDLE_VALUE)
            {
                radioHandles.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemBluetooth::GetRadios", u8"BluetoothFindNextRadio 返回了无效蓝牙无线电句柄。");
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
        if (options.requestFreshInquiry && options.inquiryTimeoutMultiplier > 48)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::GetClassicDevices", u8"inquiryTimeoutMultiplier 不能大于 48；Windows Bluetooth API 对更大的值会立即返回 E_INVALIDARG。");
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetClassicDevices");
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
        searchParams.cTimeoutMultiplier = options.requestFreshInquiry ? static_cast<UCHAR>(options.inquiryTimeoutMultiplier) : 0;
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

        for (int retryIndex = 0; retryIndex < 5; retryIndex++)
        {
            if (!IsReasonableNativeBluetoothArrayCount<GUID>(static_cast<size_t>(serviceCount)))
            {
                serviceGuids.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::ReadInstalledServices", u8"系统返回的经典蓝牙服务数量对应缓冲区过大，已拒绝分配。");
            }

            try
            {
                std::vector<GUID> nativeServiceGuids(static_cast<size_t>(serviceCount));
                DWORD returnedServiceCount = serviceCount;
                serviceResult = ::BluetoothEnumerateInstalledServices(radioHandle, &nativeDeviceInfo, &returnedServiceCount, nativeServiceGuids.data());
                if (serviceResult == ERROR_SUCCESS && returnedServiceCount <= serviceCount)
                {
                    serviceGuids.reserve(static_cast<size_t>(returnedServiceCount));
                    for (DWORD index = 0; index < returnedServiceCount; index++)
                    {
                        serviceGuids.push_back(GuidToString(nativeServiceGuids[index]));
                    }

                    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::ReadInstalledServices");
                }

                if (serviceResult == ERROR_NOT_FOUND || serviceResult == ERROR_SERVICE_DOES_NOT_EXIST)
                {
                    serviceGuids.clear();
                    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::ReadInstalledServices");
                }

                if (serviceResult == ERROR_SUCCESS || serviceResult == ERROR_MORE_DATA)
                {
                    DWORD refreshedServiceCount = 0;
                    const DWORD countResult = ::BluetoothEnumerateInstalledServices(radioHandle, &nativeDeviceInfo, &refreshedServiceCount, nullptr);
                    const bool countQuerySucceeded = countResult == ERROR_SUCCESS || countResult == ERROR_MORE_DATA;
                    DWORD nextServiceCount = serviceCount;
                    if (!TryGetLargerGuidCount(serviceCount, returnedServiceCount, countQuerySucceeded ? refreshedServiceCount : 0, nextServiceCount))
                    {
                        serviceGuids.clear();
                        return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, u8"GB_SystemBluetooth::ReadInstalledServices", u8"BluetoothEnumerateInstalledServices 持续报告缓冲区不足，但无法得到更大的有效服务数量。");
                    }
                    if (!countQuerySucceeded && returnedServiceCount <= serviceCount)
                    {
                        serviceGuids.clear();
                        return GB_SystemResult::FromWin32Error(countResult, u8"GB_SystemBluetooth::ReadInstalledServices", u8"BluetoothEnumerateInstalledServices 重新查询经典蓝牙服务数量失败。");
                    }

                    serviceCount = nextServiceCount;
                    continue;
                }

                serviceGuids.clear();
                return GB_SystemResult::FromWin32Error(serviceResult, u8"GB_SystemBluetooth::ReadInstalledServices", u8"BluetoothEnumerateInstalledServices 读取经典蓝牙设备已安装服务失败。");
            }
            catch (const std::bad_alloc&)
            {
                serviceGuids.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::ReadInstalledServices", u8"保存经典蓝牙服务 GUID 时内存不足。");
            }
            catch (...)
            {
                serviceGuids.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemBluetooth::ReadInstalledServices", u8"保存经典蓝牙服务 GUID 时发生内部错误。");
            }
        }

        serviceGuids.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, u8"GB_SystemBluetooth::ReadInstalledServices", u8"BluetoothEnumerateInstalledServices 多次返回更多数据，服务 GUID 数量持续变化。");
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
            try
            {
                *publicDevice = ConvertDeviceInfo(nativeDeviceInfo, radioInfo);
            }
            catch (...)
            {
                *publicDevice = GB_BluetoothDeviceInfo();
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::TryGetNativeClassicDeviceInfoByAddress", u8"保存经典蓝牙设备信息时内存不足。");
            }

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
        ::SetLastError(ERROR_SUCCESS);
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
                    try
                    {
                        *publicDevice = ConvertDeviceInfo(nativeDeviceInfo, radioInfo);
                    }
                    catch (...)
                    {
                        *publicDevice = GB_BluetoothDeviceInfo();
                        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio", u8"保存经典蓝牙设备信息时内存不足。");
                    }

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
            ::SetLastError(ERROR_SUCCESS);
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

    static GB_SystemResult FindNativeClassicDeviceAcrossRadios(const GB_BluetoothClassicDeviceQueryOptions& options, const std::string& targetAddress, BLUETOOTH_DEVICE_INFO& nativeDeviceInfo, bool& found)
    {
        found = false;
        nativeDeviceInfo = {};
        nativeDeviceInfo.dwSize = sizeof(nativeDeviceInfo);

        BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams = BuildSearchParams(nullptr, options);
        ::SetLastError(ERROR_SUCCESS);
        HBLUETOOTH_DEVICE_FIND findHandle = ::BluetoothFindFirstDevice(&searchParams, &nativeDeviceInfo);
        if (findHandle == nullptr)
        {
            const DWORD lastError = ::GetLastError();
            if (lastError == ERROR_SUCCESS || lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_NOT_FOUND)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::FindNativeClassicDeviceAcrossRadios");
            }

            return GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemBluetooth::FindNativeClassicDeviceAcrossRadios", u8"BluetoothFindFirstDevice 跨全部本机蓝牙无线电查找经典蓝牙设备失败。");
        }

        BluetoothDeviceFindScope findScope(findHandle);
        while (true)
        {
            if (FormatBluetoothAddress(nativeDeviceInfo.Address) == targetAddress)
            {
                found = true;
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::FindNativeClassicDeviceAcrossRadios");
            }

            BLUETOOTH_DEVICE_INFO nextDeviceInfo = {};
            nextDeviceInfo.dwSize = sizeof(nextDeviceInfo);
            ::SetLastError(ERROR_SUCCESS);
            if (::BluetoothFindNextDevice(findScope.Get(), &nextDeviceInfo) == FALSE)
            {
                const DWORD nextError = ::GetLastError();
                if (nextError == ERROR_SUCCESS || nextError == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }

                return GB_SystemResult::FromWin32Error(nextError, u8"GB_SystemBluetooth::FindNativeClassicDeviceAcrossRadios", u8"BluetoothFindNextDevice 跨全部本机蓝牙无线电查找下一个经典蓝牙设备失败。");
            }
            nativeDeviceInfo = nextDeviceInfo;
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::FindNativeClassicDeviceAcrossRadios");
    }

    static GB_SystemResult EnumerateClassicDevicesForRadio(HANDLE radioHandle, const GB_BluetoothRadioInfo& radioInfo, const GB_BluetoothClassicDeviceQueryOptions& options, std::vector<GB_BluetoothDeviceInfo>& devices, std::unordered_set<std::string>& seenAddresses)
    {
        BLUETOOTH_DEVICE_INFO nativeDeviceInfo = {};
        nativeDeviceInfo.dwSize = sizeof(nativeDeviceInfo);

        BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams = BuildSearchParams(radioHandle, options);
        ::SetLastError(ERROR_SUCCESS);
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
            GB_BluetoothDeviceInfo device;
            std::string seenKey;
            bool isNewDevice = false;
            try
            {
                device = ConvertDeviceInfo(nativeDeviceInfo, radioInfo);
                seenKey = device.radioAddress + "|" + device.address;
                isNewDevice = seenAddresses.insert(seenKey).second;
            }
            catch (...)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetClassicDevices", u8"保存经典蓝牙设备索引时内存不足。");
            }

            if (isNewDevice)
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
                    devices.push_back(std::move(device));
                }
                catch (...)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetClassicDevices", u8"保存经典蓝牙设备信息时内存不足。");
                }
            }

            BLUETOOTH_DEVICE_INFO nextDeviceInfo = {};
            nextDeviceInfo.dwSize = sizeof(nextDeviceInfo);
            ::SetLastError(ERROR_SUCCESS);
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
            radios.push_back(radioHandles[index].TakeInfo());
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
#if !defined(_WIN32)
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::GetDefaultRadio", u8"当前平台不支持 Windows 蓝牙无线电枚举。");
#else
    return GetFirstRadioInfo(radio, found, u8"GB_SystemBluetooth::GetDefaultRadio");
#endif
}

GB_SystemResult GB_SystemBluetooth::IsBluetoothAvailable(bool& available)
{
    available = false;
#if !defined(_WIN32)
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::IsBluetoothAvailable", u8"当前平台不支持 Windows 蓝牙无线电枚举。");
#else
    return CheckBluetoothRadioAvailable(available, u8"GB_SystemBluetooth::IsBluetoothAvailable");
#endif
}

GB_SystemResult GB_SystemBluetooth::SetRadioConnectable(const std::string& radioAddress, const bool enabled)
{
#if !defined(_WIN32)
    (void)radioAddress;
    (void)enabled;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::SetRadioConnectable", u8"当前平台不支持 Windows 蓝牙入站连接状态设置。");
#else
    const std::lock_guard<std::mutex> changeLock(GetRadioStateChangeMutex());

    std::vector<BluetoothRadioHandleEntry> radioHandles;
    std::vector<BluetoothRadioHandleEntry*> targetRadioHandles;
    GB_SystemResult targetResult = FindTargetRadioHandles(radioAddress, radioHandles, targetRadioHandles, u8"GB_SystemBluetooth::SetRadioConnectable");
    if (targetResult.IsFailed())
    {
        return targetResult;
    }

    std::vector<BluetoothRadioStateSnapshot> snapshots;
    GB_SystemResult snapshotResult = CaptureRadioStateSnapshots(targetRadioHandles, snapshots, u8"GB_SystemBluetooth::SetRadioConnectable");
    if (snapshotResult.IsFailed())
    {
        return snapshotResult;
    }

    for (size_t index = 0; index < targetRadioHandles.size(); index++)
    {
        HANDLE radioHandle = targetRadioHandles[index]->GetHandle();
        if (!enabled && ::BluetoothIsDiscoverable(radioHandle) != FALSE)
        {
            GB_SystemResult discoveryResult = SetRadioDiscovery(radioHandle, false, u8"GB_SystemBluetooth::SetRadioConnectable");
            if (discoveryResult.IsFailed())
            {
                return RollbackRadioStatesAfterFailure(std::move(discoveryResult), snapshots, index + 1, u8"GB_SystemBluetooth::SetRadioConnectable");
            }
        }

        GB_SystemResult connectableResult = SetRadioIncomingConnections(radioHandle, enabled, u8"GB_SystemBluetooth::SetRadioConnectable");
        if (connectableResult.IsFailed())
        {
            return RollbackRadioStatesAfterFailure(std::move(connectableResult), snapshots, index + 1, u8"GB_SystemBluetooth::SetRadioConnectable");
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
    const std::lock_guard<std::mutex> changeLock(GetRadioStateChangeMutex());

    std::vector<BluetoothRadioHandleEntry> radioHandles;
    std::vector<BluetoothRadioHandleEntry*> targetRadioHandles;
    GB_SystemResult targetResult = FindTargetRadioHandles(radioAddress, radioHandles, targetRadioHandles, u8"GB_SystemBluetooth::SetRadioDiscoverable");
    if (targetResult.IsFailed())
    {
        return targetResult;
    }

    std::vector<BluetoothRadioStateSnapshot> snapshots;
    GB_SystemResult snapshotResult = CaptureRadioStateSnapshots(targetRadioHandles, snapshots, u8"GB_SystemBluetooth::SetRadioDiscoverable");
    if (snapshotResult.IsFailed())
    {
        return snapshotResult;
    }

    for (size_t index = 0; index < targetRadioHandles.size(); index++)
    {
        HANDLE radioHandle = targetRadioHandles[index]->GetHandle();
        if (enabled && ::BluetoothIsConnectable(radioHandle) == FALSE)
        {
            GB_SystemResult connectableResult = SetRadioIncomingConnections(radioHandle, true, u8"GB_SystemBluetooth::SetRadioDiscoverable");
            if (connectableResult.IsFailed())
            {
                return RollbackRadioStatesAfterFailure(std::move(connectableResult), snapshots, index + 1, u8"GB_SystemBluetooth::SetRadioDiscoverable");
            }
        }

        GB_SystemResult discoveryResult = SetRadioDiscovery(radioHandle, enabled, u8"GB_SystemBluetooth::SetRadioDiscoverable");
        if (discoveryResult.IsFailed())
        {
            return RollbackRadioStatesAfterFailure(std::move(discoveryResult), snapshots, index + 1, u8"GB_SystemBluetooth::SetRadioDiscoverable");
        }
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::SetRadioDiscoverable");
#endif
}

GB_SystemResult GB_SystemBluetooth::GetClassicDevices(std::vector<GB_BluetoothDeviceInfo>& devices, const GB_BluetoothClassicDeviceQueryOptions& options)
{
    devices.clear();
#if !defined(_WIN32)
    (void)options;
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
    size_t seenAddressCapacity = 0;
    size_t deviceCapacity = 0;
    if (!TryMultiplyAndAddSize(radioHandles.size(), 8u, 8u, seenAddressCapacity) || !TryMultiplyAndAddSize(radioHandles.size(), 8u, 0u, deviceCapacity))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetClassicDevices", u8"经典蓝牙设备结果容量计算溢出。");
    }
    try
    {
        seenAddresses.reserve(seenAddressCapacity);
        devices.reserve(deviceCapacity);
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
#if !defined(_WIN32)
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::GetLowEnergyDevices", u8"当前平台不支持 Windows BLE 设备接口枚举。");
#else
    std::vector<GB_SystemDeviceInterfaceInfo> deviceInterfaces;
    GB_SystemResult interfaceResult = GB_SystemDevice::GetDeviceInterfacesByClassGuid(GB_BluetoothLeDeviceInterfaceGuid, deviceInterfaces, true);
    if (interfaceResult.IsFailed())
    {
        return interfaceResult.WithOperationName(u8"GB_SystemBluetooth::GetLowEnergyDevices");
    }

    std::unordered_set<std::string> seenInterfacePaths;
    size_t seenInterfaceCapacity = 0;
    if (!TryMultiplyAndAddSize(deviceInterfaces.size(), 1u, 8u, seenInterfaceCapacity))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetLowEnergyDevices", u8"BLE 设备接口结果容量计算溢出。");
    }
    try
    {
        seenInterfacePaths.reserve(seenInterfaceCapacity);
        devices.reserve(deviceInterfaces.size());
    }
    catch (...)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetLowEnergyDevices", u8"预分配 BLE 设备结果缓冲区时内存不足。");
    }

    for (size_t index = 0; index < deviceInterfaces.size(); index++)
    {
        if (deviceInterfaces[index].interfacePath.empty())
        {
            continue;
        }

        bool isNewInterface = false;
        try
        {
            isNewInterface = seenInterfacePaths.insert(ToLowerAscii(deviceInterfaces[index].interfacePath)).second;
        }
        catch (...)
        {
            devices.clear();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetLowEnergyDevices", u8"保存 BLE 设备接口索引时内存不足。");
        }
        if (!isNewInterface)
        {
            continue;
        }

        try
        {
            devices.push_back(ConvertLowEnergyDeviceInterfaceInfo(deviceInterfaces[index]));
        }
        catch (...)
        {
            devices.clear();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetLowEnergyDevices", u8"保存 BLE 设备信息时内存不足。");
        }
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetLowEnergyDevices");
#endif
}

GB_SystemResult GB_SystemBluetooth::GetGattServices(const std::string& deviceInterfacePath, std::vector<GB_BluetoothGattServiceInfo>& services)
{
    services.clear();
#if !defined(_WIN32)
    (void)deviceInterfacePath;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::GetGattServices", u8"当前平台不支持 Windows BLE GATT 服务枚举。");
#else
    GenericHandleScope deviceHandle;
    GB_SystemResult openResult = OpenBluetoothLeDeviceHandle(deviceInterfacePath, BluetoothLeDeviceHandleAccess::ReadOnly, deviceHandle, u8"GB_SystemBluetooth::GetGattServices");
    if (openResult.IsFailed())
    {
        return openResult;
    }

    std::vector<BTH_LE_GATT_SERVICE> nativeServices;
    GB_SystemResult serviceResult = ReadNativeGattServices(deviceHandle.Get(), nativeServices, u8"GB_SystemBluetooth::GetGattServices");
    if (serviceResult.IsFailed())
    {
        return serviceResult;
    }

    try
    {
        services.reserve(nativeServices.size());
        for (size_t index = 0; index < nativeServices.size(); index++)
        {
            services.push_back(ConvertGattServiceInfo(nativeServices[index], deviceInterfacePath));
        }
    }
    catch (...)
    {
        services.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetGattServices", u8"保存 BLE GATT 服务信息时内存不足。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetGattServices");
#endif
}

GB_SystemResult GB_SystemBluetooth::GetGattCharacteristics(const std::string& deviceInterfacePath, const GB_BluetoothGattServiceInfo& service, std::vector<GB_BluetoothGattCharacteristicInfo>& characteristics)
{
    characteristics.clear();
#if !defined(_WIN32)
    (void)deviceInterfacePath;
    (void)service;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::GetGattCharacteristics", u8"当前平台不支持 Windows BLE GATT 特征枚举。");
#else
    const std::string targetDeviceInterfacePath = !deviceInterfacePath.empty() ? deviceInterfacePath : service.deviceInterfacePath;
    if (!deviceInterfacePath.empty() && !service.deviceInterfacePath.empty() && !AreDeviceInterfacePathsEqual(deviceInterfacePath, service.deviceInterfacePath))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::GetGattCharacteristics", u8"deviceInterfacePath 与 service.deviceInterfacePath 不一致。");
    }

    GenericHandleScope deviceHandle;
    GB_SystemResult openResult = OpenBluetoothLeDeviceHandle(targetDeviceInterfacePath, BluetoothLeDeviceHandleAccess::ReadOnly, deviceHandle, u8"GB_SystemBluetooth::GetGattCharacteristics");
    if (openResult.IsFailed())
    {
        return openResult;
    }

    BTH_LE_GATT_SERVICE nativeService = BTH_LE_GATT_SERVICE();
    GB_SystemResult nativeServiceResult = FindNativeGattService(deviceHandle.Get(), service, nativeService, u8"GB_SystemBluetooth::GetGattCharacteristics");
    if (nativeServiceResult.IsFailed())
    {
        return nativeServiceResult;
    }

    std::vector<BTH_LE_GATT_CHARACTERISTIC> nativeCharacteristics;
    GB_SystemResult characteristicResult = ReadNativeGattCharacteristics(deviceHandle.Get(), nativeService, nativeCharacteristics, u8"GB_SystemBluetooth::GetGattCharacteristics");
    if (characteristicResult.IsFailed())
    {
        return characteristicResult;
    }

    try
    {
        const GB_BluetoothGattServiceInfo normalizedService = ConvertGattServiceInfo(nativeService, targetDeviceInterfacePath);
        characteristics.reserve(nativeCharacteristics.size());
        for (size_t index = 0; index < nativeCharacteristics.size(); index++)
        {
            if (nativeCharacteristics[index].ServiceHandle != nativeService.AttributeHandle)
            {
                characteristics.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, u8"GB_SystemBluetooth::GetGattCharacteristics", u8"BluetoothGATTGetCharacteristics 返回了不属于目标服务的特征句柄。");
            }

            characteristics.push_back(ConvertGattCharacteristicInfo(nativeCharacteristics[index], normalizedService, targetDeviceInterfacePath));
        }
    }
    catch (...)
    {
        characteristics.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::GetGattCharacteristics", u8"保存 BLE GATT 特征信息时内存不足。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetGattCharacteristics");
#endif
}

GB_SystemResult GB_SystemBluetooth::ReadGattCharacteristic(const std::string& deviceInterfacePath, const GB_BluetoothGattCharacteristicInfo& characteristic, std::vector<uint8_t>& value, const GB_BluetoothGattReadOptions& options)
{
    value.clear();
#if !defined(_WIN32)
    (void)deviceInterfacePath;
    (void)characteristic;
    (void)options;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"当前平台不支持 Windows BLE GATT 特征读取。");
#else
    GB_SystemResult validateResult = ValidateGattReadOptions(options, u8"GB_SystemBluetooth::ReadGattCharacteristic");
    if (validateResult.IsFailed())
    {
        return validateResult;
    }

    const std::string targetDeviceInterfacePath = !deviceInterfacePath.empty() ? deviceInterfacePath : characteristic.deviceInterfacePath;
    if (!deviceInterfacePath.empty() && !characteristic.deviceInterfacePath.empty() && !AreDeviceInterfacePathsEqual(deviceInterfacePath, characteristic.deviceInterfacePath))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"deviceInterfacePath 与 characteristic.deviceInterfacePath 不一致。");
    }

    GenericHandleScope deviceHandle;
    GB_SystemResult openResult = OpenBluetoothLeDeviceHandle(targetDeviceInterfacePath, BluetoothLeDeviceHandleAccess::ReadOnly, deviceHandle, u8"GB_SystemBluetooth::ReadGattCharacteristic");
    if (openResult.IsFailed())
    {
        return openResult;
    }

    BTH_LE_GATT_CHARACTERISTIC nativeCharacteristic = BTH_LE_GATT_CHARACTERISTIC();
    GB_SystemResult characteristicResult = FindNativeGattCharacteristic(deviceHandle.Get(), characteristic, nativeCharacteristic, u8"GB_SystemBluetooth::ReadGattCharacteristic");
    if (characteristicResult.IsFailed())
    {
        return characteristicResult;
    }
    if (nativeCharacteristic.IsReadable == FALSE)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"当前系统枚举到的 BLE GATT 特征未声明可读属性。");
    }

    const ULONG flags = BuildGattReadFlags(options);
    for (int retryIndex = 0; retryIndex < 3; retryIndex++)
    {
        USHORT requiredValueSize = 0;
        HRESULT hresult = ::BluetoothGATTGetCharacteristicValue(deviceHandle.Get(), &nativeCharacteristic, 0, nullptr, &requiredValueSize, flags);
        if (hresult != S_OK && !IsHResultFromWin32Error(hresult, ERROR_MORE_DATA))
        {
            return MapBluetoothGattHResult(hresult, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"BluetoothGATTGetCharacteristicValue 查询 BLE GATT 特征值缓冲区大小失败。");
        }
        if (requiredValueSize == 0)
        {
            return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::ReadGattCharacteristic");
        }

        const size_t headerSize = GetGattCharacteristicValueHeaderSize();
        if (static_cast<size_t>(requiredValueSize) < headerSize)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"系统返回的 BLE GATT 特征值缓冲区大小小于结构头部。");
        }

        const size_t requiredDataSize = static_cast<size_t>(requiredValueSize) - headerSize;
        if (requiredDataSize > GetMaxGattCharacteristicValueDataSize())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"系统返回的 BLE GATT 特征值长度超过 ATT 属性值上限。");
        }

        std::vector<ULONG> valueBuffer;
        GB_SystemResult allocationResult = AllocateGattCharacteristicValueBuffer(requiredDataSize, valueBuffer, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"分配 BLE GATT 特征值缓冲区时内存不足。");
        if (allocationResult.IsFailed())
        {
            return allocationResult;
        }

        const size_t bufferByteCapacity = GetGattCharacteristicValueBufferByteCapacity(valueBuffer);
        if (bufferByteCapacity < static_cast<size_t>(requiredValueSize))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"BLE GATT 特征值缓冲区容量计算错误。");
        }

        PBTH_LE_GATT_CHARACTERISTIC_VALUE nativeValue = reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(valueBuffer.data());
        USHORT actualValueSize = requiredValueSize;
        hresult = ::BluetoothGATTGetCharacteristicValue(deviceHandle.Get(), &nativeCharacteristic, requiredValueSize, nativeValue, &actualValueSize, flags);
        if (hresult == S_OK)
        {
            const size_t actualSize = static_cast<size_t>(actualValueSize);
            const size_t dataSize = static_cast<size_t>(nativeValue->DataSize);
            if (actualSize < headerSize || actualSize > static_cast<size_t>(requiredValueSize) || actualSize > bufferByteCapacity || dataSize > actualSize - headerSize || dataSize > GetMaxGattCharacteristicValueDataSize())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"系统返回的 BLE GATT 特征值长度与实际写入缓冲区长度不一致，或超过 ATT 属性值上限。");
            }

            try
            {
                value.assign(nativeValue->Data, nativeValue->Data + dataSize);
            }
            catch (...)
            {
                value.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"保存 BLE GATT 特征值时内存不足。");
            }

            return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::ReadGattCharacteristic");
        }

        if (IsHResultFromWin32Error(hresult, ERROR_MORE_DATA) || IsHResultFromWin32Error(hresult, ERROR_INVALID_USER_BUFFER))
        {
            continue;
        }

        return MapBluetoothGattHResult(hresult, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"BluetoothGATTGetCharacteristicValue 读取 BLE GATT 特征值失败。");
    }

    return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"BluetoothGATTGetCharacteristicValue 多次返回更多数据，特征值长度不稳定。");
#endif
}

GB_SystemResult GB_SystemBluetooth::WriteGattCharacteristic(const std::string& deviceInterfacePath, const GB_BluetoothGattCharacteristicInfo& characteristic, const std::vector<uint8_t>& value, const GB_BluetoothGattWriteOptions& options)
{
#if !defined(_WIN32)
    (void)deviceInterfacePath;
    (void)characteristic;
    (void)value;
    (void)options;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemBluetooth::WriteGattCharacteristic", u8"当前平台不支持 Windows BLE GATT 特征写入。");
#else
    GB_SystemResult validateResult = ValidateGattWriteOptions(options, u8"GB_SystemBluetooth::WriteGattCharacteristic");
    if (validateResult.IsFailed())
    {
        return validateResult;
    }

    const std::string targetDeviceInterfacePath = !deviceInterfacePath.empty() ? deviceInterfacePath : characteristic.deviceInterfacePath;
    if (!deviceInterfacePath.empty() && !characteristic.deviceInterfacePath.empty() && !AreDeviceInterfacePathsEqual(deviceInterfacePath, characteristic.deviceInterfacePath))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::WriteGattCharacteristic", u8"deviceInterfacePath 与 characteristic.deviceInterfacePath 不一致。");
    }
    if (value.size() > GetMaxGattCharacteristicValueDataSize())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::WriteGattCharacteristic", u8"BLE GATT 特征写入数据超过 ATT 属性值上限。");
    }

    GenericHandleScope deviceHandle;
    GB_SystemResult openResult = OpenBluetoothLeDeviceHandle(targetDeviceInterfacePath, BluetoothLeDeviceHandleAccess::ReadWrite, deviceHandle, u8"GB_SystemBluetooth::WriteGattCharacteristic");
    if (openResult.IsFailed())
    {
        return openResult;
    }

    BTH_LE_GATT_CHARACTERISTIC nativeCharacteristic = BTH_LE_GATT_CHARACTERISTIC();
    GB_SystemResult characteristicResult = FindNativeGattCharacteristic(deviceHandle.Get(), characteristic, nativeCharacteristic, u8"GB_SystemBluetooth::WriteGattCharacteristic");
    if (characteristicResult.IsFailed())
    {
        return characteristicResult;
    }
    if (options.signedWrite)
    {
        if (nativeCharacteristic.IsSignedWritable == FALSE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemBluetooth::WriteGattCharacteristic", u8"当前系统枚举到的 BLE GATT 特征未声明 SignedWrite 属性。");
        }
    }
    else if (options.writeWithoutResponse)
    {
        if (nativeCharacteristic.IsWritableWithoutResponse == FALSE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemBluetooth::WriteGattCharacteristic", u8"当前系统枚举到的 BLE GATT 特征未声明 WriteWithoutResponse 属性。");
        }
    }
    else if (nativeCharacteristic.IsWritable == FALSE)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemBluetooth::WriteGattCharacteristic", u8"当前系统枚举到的 BLE GATT 特征未声明可写属性。");
    }

    std::vector<ULONG> valueBuffer;
    GB_SystemResult allocationResult = AllocateGattCharacteristicValueBuffer(value.size(), valueBuffer, u8"GB_SystemBluetooth::WriteGattCharacteristic", u8"分配 BLE GATT 特征写入缓冲区时内存不足。");
    if (allocationResult.IsFailed())
    {
        return allocationResult;
    }

    PBTH_LE_GATT_CHARACTERISTIC_VALUE nativeValue = reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(valueBuffer.data());
    nativeValue->DataSize = static_cast<ULONG>(value.size());
    if (!value.empty())
    {
        std::memcpy(nativeValue->Data, value.data(), value.size());
    }

    const ULONG flags = BuildGattWriteFlags(options);
    const HRESULT hresult = ::BluetoothGATTSetCharacteristicValue(deviceHandle.Get(), &nativeCharacteristic, nativeValue, static_cast<BTH_LE_GATT_RELIABLE_WRITE_CONTEXT>(0), flags);
    if (hresult != S_OK)
    {
        return MapBluetoothGattHResult(hresult, u8"GB_SystemBluetooth::WriteGattCharacteristic", u8"BluetoothGATTSetCharacteristicValue 写入 BLE GATT 特征值失败。", true);
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::WriteGattCharacteristic");
#endif
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
    GB_BluetoothClassicDeviceQueryOptions lookupOptions = options;
    lookupOptions.includeAuthenticated = true;
    lookupOptions.includeRemembered = true;
    lookupOptions.includeUnknown = true;
    lookupOptions.includeConnected = true;

    GB_SystemResult validateResult = ValidateClassicDeviceQueryOptions(lookupOptions);
    if (validateResult.IsFailed())
    {
        return validateResult.WithOperationName(u8"GB_SystemBluetooth::GetClassicDeviceByAddress");
    }

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
    (void)deviceId;
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

    std::wstring pinCodeWide;
    SensitiveWideStringScope pinCodeScope(pinCodeWide);
    if (!options.pinCodeUtf8.empty())
    {
        try
        {
            pinCodeWide = GB_Utf8ToWString(options.pinCodeUtf8);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::PairDevice", u8"转换经典蓝牙 PIN 时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, u8"GB_SystemBluetooth::PairDevice", u8"pinCodeUtf8 转换为 UTF-16 失败。");
        }
        if (pinCodeWide.empty() || pinCodeWide.size() > BLUETOOTH_MAX_PASSKEY_SIZE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::PairDevice", u8"PIN 长度必须大于 0 且不能超过 BLUETOOTH_MAX_PASSKEY_SIZE。");
        }
    }
    if (!options.allowSystemPairingUi && pinCodeWide.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::PairDevice", u8"不允许系统配对 UI 时必须提供 pinCodeUtf8。");
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
    bool matchedDevice = false;

    const GB_BluetoothClassicDeviceQueryOptions cachedLookupOptions = MakeLookupOptions(false);
    for (size_t index = 0; index < radioHandles.size(); index++)
    {
        bool currentFound = false;
        GB_SystemResult findResult = FindNativeClassicDeviceForRadio(radioHandles[index].GetHandle(), radioHandles[index].GetInfo(), cachedLookupOptions, address, nativeDeviceInfo, nullptr, currentFound);
        if (findResult.IsFailed())
        {
            return findResult.WithOperationName(u8"GB_SystemBluetooth::PairDevice");
        }
        if (currentFound)
        {
            matchedRadioHandle = radioHandles[index].GetHandle();
            matchedDevice = true;
            break;
        }
    }

    if (!matchedDevice)
    {
        const GB_BluetoothClassicDeviceQueryOptions freshLookupOptions = MakeLookupOptions(true);
        GB_SystemResult findResult = FindNativeClassicDeviceAcrossRadios(freshLookupOptions, address, nativeDeviceInfo, matchedDevice);
        if (findResult.IsFailed())
        {
            return findResult.WithOperationName(u8"GB_SystemBluetooth::PairDevice");
        }
    }

    if (!matchedDevice)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_SystemBluetooth::PairDevice", u8"未找到指定经典蓝牙设备；请确认设备处于可发现或已记住状态。");
    }
    if (nativeDeviceInfo.fAuthenticated != FALSE)
    {
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::PairDevice", u8"指定经典蓝牙设备已经完成配对。");
    }

    DWORD pairResult = ERROR_SUCCESS;
    if (!pinCodeWide.empty())
    {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4995)
#endif
        pairResult = ::BluetoothAuthenticateDevice(nullptr, matchedRadioHandle, &nativeDeviceInfo, const_cast<PWSTR>(pinCodeWide.c_str()), static_cast<ULONG>(pinCodeWide.size()));
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        if (pairResult == ERROR_SUCCESS)
        {
            return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::PairDevice", u8"经典蓝牙 PIN 配对请求已成功完成。");
        }
        if (pairResult == ERROR_NO_MORE_ITEMS)
        {
            return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::PairDevice", u8"系统返回设备已经完成认证。");
        }

        return MapBluetoothWin32Result(pairResult, u8"GB_SystemBluetooth::PairDevice", u8"BluetoothAuthenticateDevice 使用 PIN 配对经典蓝牙设备失败。");
    }

    pairResult = ::BluetoothAuthenticateDeviceEx(nullptr, matchedRadioHandle, &nativeDeviceInfo, nullptr, MITMProtectionNotRequiredBonding);
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

bool GB_SystemBluetooth::IsValidGattCacheModeValue(const uint64_t cacheModeValue)
{
    switch (cacheModeValue)
    {
    case static_cast<uint64_t>(GB_BluetoothGattCacheMode::Default):
    case static_cast<uint64_t>(GB_BluetoothGattCacheMode::ForceReadFromDevice):
    case static_cast<uint64_t>(GB_BluetoothGattCacheMode::ForceReadFromCache):
        return true;
    default:
        break;
    }

    return false;
}

std::string GB_SystemBluetooth::GetGattCacheModeName(const GB_BluetoothGattCacheMode cacheMode)
{
    if (!IsValidGattCacheMode(cacheMode))
    {
        return "Invalid";
    }

    switch (cacheMode)
    {
    case GB_BluetoothGattCacheMode::Default:
        return "Default";
    case GB_BluetoothGattCacheMode::ForceReadFromDevice:
        return "ForceReadFromDevice";
    case GB_BluetoothGattCacheMode::ForceReadFromCache:
        return "ForceReadFromCache";
    default:
        break;
    }

    return "Invalid";
}

class GB_SystemBluetoothWatcher::Impl
{
private:
    enum class LifecycleState
    {
        Stopped,
        Starting,
        Running,
        Stopping,
        StopFailed
    };

public:
    explicit Impl(const GB_SystemBluetoothWatcherOptions& inputOptions) : options(NormalizeOptions(inputOptions)), acceptingDeviceEvents(false), eventDispatcher(GB_EventDispatcher::MakeQueuedOptions(options.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropOldest, u8"GB_SystemBluetoothWatcher"))
    {
    }

    ~Impl() noexcept
    {
        (void)Stop();
    }

    GB_SystemResult Start()
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (lifecycleState == LifecycleState::Running)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::Start", u8"蓝牙监听器已经启动。");
            }
            if (lifecycleState == LifecycleState::Starting || lifecycleState == LifecycleState::Stopping)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceBusy, u8"GB_SystemBluetoothWatcher::Start", u8"蓝牙监听器正在启动或停止，不能并发启动。");
            }
            if (lifecycleState == LifecycleState::StopFailed)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemBluetoothWatcher::Start", u8"上一次停止没有完整成功；请先再次调用 Stop() 完成清理，再重新启动。");
            }

            lifecycleState = LifecycleState::Starting;
        }

        GB_SystemResult result = EnsureTypedSubscription();
        if (result.IsFailed())
        {
            SetLifecycleState(LifecycleState::Stopped);
            return result.WithOperationName(u8"GB_SystemBluetoothWatcher::Start");
        }

        bool dispatcherStarted = false;
        bool deviceCallbackInstalled = false;
        try
        {
            result = eventDispatcher.Start();
            if (result.IsFailed())
            {
                SetLifecycleState(LifecycleState::Stopped);
                return result.WithOperationName(u8"GB_SystemBluetoothWatcher::Start");
            }
            dispatcherStarted = true;

            acceptingDeviceEvents.store(true, std::memory_order_release);
            deviceWatcher.SetDeviceEventCallback([this](const GB_SystemDeviceEvent& deviceEvent)
                {
                    HandleSystemDeviceEvent(deviceEvent);
                });
            deviceCallbackInstalled = true;

            result = deviceWatcher.Start();
            if (result.IsFailed())
            {
                acceptingDeviceEvents.store(false, std::memory_order_release);
                ClearDeviceWatcherCallbackSilently();
                deviceCallbackInstalled = false;
                (void)deviceWatcher.Stop();
                (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
                SetLifecycleState(LifecycleState::Stopped);
                return result.WithOperationName(u8"GB_SystemBluetoothWatcher::Start");
            }
        }
        catch (const std::bad_alloc&)
        {
            acceptingDeviceEvents.store(false, std::memory_order_release);
            if (deviceCallbackInstalled)
            {
                ClearDeviceWatcherCallbackSilently();
            }
            (void)deviceWatcher.Stop();
            if (dispatcherStarted)
            {
                (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            }
            SetLifecycleState(LifecycleState::Stopped);
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetoothWatcher::Start", u8"启动蓝牙监听器时内存不足。");
        }
        catch (...)
        {
            acceptingDeviceEvents.store(false, std::memory_order_release);
            if (deviceCallbackInstalled)
            {
                ClearDeviceWatcherCallbackSilently();
            }
            (void)deviceWatcher.Stop();
            if (dispatcherStarted)
            {
                (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            }
            SetLifecycleState(LifecycleState::Stopped);
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemBluetoothWatcher::Start", u8"启动蓝牙监听器时发生内部错误。");
        }

        SetLifecycleState(LifecycleState::Running);
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::Start");
    }

    GB_SystemResult Stop()
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (lifecycleState == LifecycleState::Stopped)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::Stop", u8"蓝牙监听器未启动。");
            }
            if (lifecycleState == LifecycleState::Starting)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceBusy, u8"GB_SystemBluetoothWatcher::Stop", u8"蓝牙监听器正在启动，请在 Start() 返回后重试停止。");
            }
            if (lifecycleState == LifecycleState::Stopping)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceBusy, u8"GB_SystemBluetoothWatcher::Stop", u8"蓝牙监听器已经在停止；当前调用不重复等待，以避免回调重入与外部停止互相等待。");
            }

            lifecycleState = LifecycleState::Stopping;
        }

        acceptingDeviceEvents.store(false, std::memory_order_release);
        ClearDeviceWatcherCallbackSilently();
        GB_SystemResult deviceStopResult = deviceWatcher.Stop();
        GB_SystemResult dispatcherStopResult = eventDispatcher.Stop(GB_EventDispatcherStopMode::Drain);
        SetLifecycleState(deviceStopResult.IsFailed() || dispatcherStopResult.IsFailed() ? LifecycleState::StopFailed : LifecycleState::Stopped);

        if (deviceStopResult.IsFailed())
        {
            if (dispatcherStopResult.IsFailed())
            {
                try
                {
                    deviceStopResult.message += u8" 事件分发器停止也失败：";
                    deviceStopResult.message += dispatcherStopResult.GetDisplayMessage();
                }
                catch (...)
                {
                }
            }
            return deviceStopResult.WithOperationName(u8"GB_SystemBluetoothWatcher::Stop");
        }
        if (dispatcherStopResult.IsFailed())
        {
            return dispatcherStopResult.WithOperationName(u8"GB_SystemBluetoothWatcher::Stop");
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::Stop");
    }

    bool IsRunning() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return lifecycleState == LifecycleState::Running;
    }

    void SetBluetoothEventCallback(const GB_SystemBluetoothWatcher::BluetoothEventCallback& callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        bluetoothEventCallback = callback;
    }

    GB_EventDispatcher& GetEventDispatcher()
    {
        return eventDispatcher;
    }

private:
    void SetLifecycleState(const LifecycleState state)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        lifecycleState = state;
    }

    void ClearDeviceWatcherCallbackSilently() noexcept
    {
        try
        {
            deviceWatcher.SetDeviceEventCallback(GB_SystemDeviceWatcher::DeviceEventCallback());
        }
        catch (...)
        {
        }
    }

    GB_SystemResult EnsureTypedSubscription()
    {
        std::lock_guard<std::mutex> lock(subscriptionMutex);
        if (typedSubscriptionToken.IsValid() && eventDispatcher.HasSubscription(typedSubscriptionToken))
        {
            return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::EnsureTypedSubscription");
        }

        typedSubscriptionToken.Reset();
        GB_SystemResult result = eventDispatcher.SubscribeAll([this](const GB_Event& event)
            {
                DispatchTypedCallback(event);
            }, typedSubscriptionToken);
        return result.WithOperationName(u8"GB_SystemBluetoothWatcher::EnsureTypedSubscription");
    }

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
        if (EqualsAsciiNoCase(deviceEvent.interfaceClassGuid, GB_BluetoothLeDeviceInterfaceGuid) || EqualsAsciiNoCase(deviceEvent.interfaceClassGuid, GB_BluetoothRadioInterfaceGuid))
        {
            return true;
        }
        if (HasBluetoothPnPIdentity(deviceEvent.deviceInstanceId) || HasBluetoothPnPIdentity(deviceEvent.deviceInterfacePath))
        {
            return true;
        }

        return false;
    }

    static bool IsBluetoothRadioDeviceEvent(const GB_SystemDeviceEvent& deviceEvent)
    {
        if (EqualsAsciiNoCase(deviceEvent.interfaceClassGuid, GB_BluetoothRadioInterfaceGuid))
        {
            return true;
        }

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
        bluetoothEvent.timestampMilliseconds = deviceEvent.timestampMilliseconds != 0 ? deviceEvent.timestampMilliseconds : GB_EventDispatcher::GetCurrentTimestampMilliseconds();
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
            if (EnsureTypedSubscription().IsFailed())
            {
                return;
            }

            GB_BluetoothEvent bluetoothEvent = BuildBluetoothEvent(deviceEvent);
            if (bluetoothEvent.eventType == GB_BluetoothEventType::Unknown)
            {
                return;
            }

            GB_Event event(bluetoothEvent.eventName, GB_Variant(bluetoothEvent), u8"GB_SystemBluetoothWatcher");
            event.timestampMilliseconds = bluetoothEvent.timestampMilliseconds;
            event.SetAttribute("eventType", GB_Variant(static_cast<unsigned int>(bluetoothEvent.eventType)));
            event.SetAttribute("eventTypeName", GB_Variant(GB_SystemBluetooth::GetEventTypeName(bluetoothEvent.eventType)));
            event.SetAttribute("sourceName", GB_Variant(bluetoothEvent.sourceName));
            event.SetAttribute("deviceInstanceId", GB_Variant(bluetoothEvent.deviceInstanceId));
            event.SetAttribute("deviceInterfacePath", GB_Variant(bluetoothEvent.deviceInterfacePath));
            event.SetAttribute("interfaceClassGuid", GB_Variant(bluetoothEvent.interfaceClassGuid));
            event.SetAttribute("nativeAction", GB_Variant(bluetoothEvent.nativeAction));
            (void)eventDispatcher.Post(event);
        }
        catch (...)
        {
        }
    }

private:
    GB_SystemBluetoothWatcherOptions options;
    mutable std::mutex stateMutex;
    mutable std::mutex callbackMutex;
    mutable std::mutex subscriptionMutex;
    LifecycleState lifecycleState = LifecycleState::Stopped;
    std::atomic<bool> acceptingDeviceEvents;
    GB_SystemDeviceWatcher deviceWatcher;
    GB_EventDispatcher eventDispatcher;
    GB_EventSubscriptionToken typedSubscriptionToken;
    GB_SystemBluetoothWatcher::BluetoothEventCallback bluetoothEventCallback;
};

GB_SystemBluetoothWatcher::GB_SystemBluetoothWatcher() : impl(new Impl(GB_SystemBluetoothWatcherOptions()))
{
}

GB_SystemBluetoothWatcher::GB_SystemBluetoothWatcher(const GB_SystemBluetoothWatcherOptions& options) : impl(new Impl(options))
{
}

GB_SystemBluetoothWatcher::~GB_SystemBluetoothWatcher() noexcept = default;

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
