#include "GB_SystemBluetooth.h"
#include "GB_SystemDevice.h"
#include "../GB_Utf8String.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
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
#include <cfgmgr32.h>
#include <devpkey.h>
#ifdef _MSC_VER
#  pragma comment(lib, "Bthprops.lib")
#  pragma comment(lib, "BluetoothApis.lib")
#  pragma comment(lib, "Cfgmgr32.lib")
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

    static const char* const GB_BluetoothClassicDeviceInterfaceGuid = "{00F40965-E89D-4487-9890-87C3ABB211F4}";
    static const char* const GB_BluetoothLeDeviceInterfaceGuid = "{781AEE18-7733-4CE4-ADD0-91F41C67B592}";
    static const char* const GB_BluetoothGattServiceDeviceInterfaceGuid = "{6E3BB679-4372-40C8-9EAA-4509DF260CD8}";
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

    static bool IsValidGattSessionAccessMode(const GB_BluetoothGattSessionAccessMode accessMode)
    {
        switch (accessMode)
        {
        case GB_BluetoothGattSessionAccessMode::ReadOnly:
        case GB_BluetoothGattSessionAccessMode::ReadWrite:
            return true;
        default:
            break;
        }

        return false;
    }

    struct AsciiNoCaseHash
    {
        size_t operator()(const std::string& text) const noexcept
        {
            size_t hashValue = sizeof(size_t) >= 8 ? static_cast<size_t>(14695981039346656037ull) : static_cast<size_t>(2166136261u);
            const size_t primeValue = sizeof(size_t) >= 8 ? static_cast<size_t>(1099511628211ull) : static_cast<size_t>(16777619u);
            for (size_t index = 0; index < text.size(); index++)
            {
                hashValue ^= static_cast<unsigned char>(ToLowerAsciiChar(text[index]));
                hashValue *= primeValue;
            }

            return hashValue;
        }
    };

    struct AsciiNoCaseEqual
    {
        bool operator()(const std::string& leftText, const std::string& rightText) const noexcept
        {
            return EqualsAsciiNoCase(leftText, rightText);
        }
    };

    static std::string GetBluetoothEventName(const GB_BluetoothEventType eventType)
    {
        return std::string("SystemBluetooth.") + GB_SystemBluetooth::GetEventTypeName(eventType);
    }

    static GB_SystemResult MakeBluetoothWatcherInitializationFailedResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"蓝牙监听器内部状态初始化失败，通常是内存或线程同步资源分配失败。");
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

    template<size_t BufferSize>
    static std::string WideFixedBufferToUtf8(const wchar_t(&text)[BufferSize])
    {
        size_t textLength = 0;
        while (textLength < BufferSize && text[textLength] != L'\0')
        {
            textLength++;
        }
        if (textLength == 0)
        {
            return std::string();
        }

        return WideStringToUtf8Safe(std::wstring(text, textLength));
    }

    static std::string GuidToString(const GUID& guid)
    {
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", static_cast<unsigned long>(guid.Data1), static_cast<unsigned int>(guid.Data2), static_cast<unsigned int>(guid.Data3), static_cast<unsigned int>(guid.Data4[0]), static_cast<unsigned int>(guid.Data4[1]), static_cast<unsigned int>(guid.Data4[2]), static_cast<unsigned int>(guid.Data4[3]), static_cast<unsigned int>(guid.Data4[4]), static_cast<unsigned int>(guid.Data4[5]), static_cast<unsigned int>(guid.Data4[6]), static_cast<unsigned int>(guid.Data4[7]));
        return std::string(buffer);
    }

    static std::string SystemTimeToString(const SYSTEMTIME& systemTime)
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

    static GB_SystemResult MakeBluetoothBooleanFailureResult(const DWORD win32Error, const std::string& operationName, const std::string& message)
    {
        if (win32Error == ERROR_SUCCESS)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NativeApiFailed, operationName, message + u8" 原生 API 返回失败，但 GetLastError 未提供错误码。");
        }

        return GB_SystemResult::FromWin32Error(static_cast<uint32_t>(win32Error), operationName, message);
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

        GenericHandleScope(GenericHandleScope&& other) noexcept : handle(other.Detach())
        {
        }

        GenericHandleScope& operator=(GenericHandleScope&& other) noexcept
        {
            if (this != &other)
            {
                Close();
                handle = other.Detach();
            }

            return *this;
        }

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

        HANDLE Detach() noexcept
        {
            HANDLE detachedHandle = handle;
            handle = INVALID_HANDLE_VALUE;
            return detachedHandle;
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

    template<size_t BufferSize>
    class SensitiveWideBufferScope final
    {
    public:
        explicit SensitiveWideBufferScope(std::array<wchar_t, BufferSize>& buffer) noexcept : buffer(buffer)
        {
        }

        ~SensitiveWideBufferScope() noexcept
        {
            (void)::SecureZeroMemory(buffer.data(), buffer.size() * sizeof(wchar_t));
        }

        SensitiveWideBufferScope(const SensitiveWideBufferScope&) = delete;
        SensitiveWideBufferScope& operator=(const SensitiveWideBufferScope&) = delete;

    private:
        std::array<wchar_t, BufferSize>& buffer;
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
        return ::IsBthLEUuidMatch(left, right) != FALSE;
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

    enum class BluetoothLeInterfaceHandleAccess
    {
        ReadOnly,
        ReadWrite
    };

    static GB_SystemResult OpenBluetoothLeInterfaceHandle(const std::string& interfacePath, const BluetoothLeInterfaceHandleAccess accessMode, const bool allowReadOnlyFallback, GenericHandleScope& handleScope, const std::string& operationName, bool* openedWithWriteAccess = nullptr)
    {
        handleScope.Reset(INVALID_HANDLE_VALUE);
        if (openedWithWriteAccess != nullptr)
        {
            *openedWithWriteAccess = false;
        }
        if (interfacePath.empty() || ContainsNullCharacter(interfacePath))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE 设备接口或 GATT 服务接口路径不能为空且不能包含空字符。");
        }

        std::wstring interfacePathWide;
        try
        {
            interfacePathWide = GB_Utf8ToWString(interfacePath);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"转换 BLE 接口路径时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, operationName, u8"BLE 接口路径 UTF-8 转 UTF-16 失败。");
        }

        const DWORD desiredAccess = accessMode == BluetoothLeInterfaceHandleAccess::ReadWrite ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
        HANDLE deviceHandle = ::CreateFileW(interfacePathWide.c_str(), desiredAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        const DWORD primaryOpenError = deviceHandle == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
        bool hasWriteAccess = deviceHandle != INVALID_HANDLE_VALUE && accessMode == BluetoothLeInterfaceHandleAccess::ReadWrite;
        bool attemptedSecondaryOpen = false;

        if (deviceHandle == INVALID_HANDLE_VALUE && accessMode == BluetoothLeInterfaceHandleAccess::ReadWrite && allowReadOnlyFallback)
        {
            attemptedSecondaryOpen = true;
            deviceHandle = ::CreateFileW(interfacePathWide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            hasWriteAccess = false;
        }
        else if (deviceHandle == INVALID_HANDLE_VALUE && accessMode == BluetoothLeInterfaceHandleAccess::ReadOnly)
        {
            attemptedSecondaryOpen = true;
            deviceHandle = ::CreateFileW(interfacePathWide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            hasWriteAccess = deviceHandle != INVALID_HANDLE_VALUE;
        }
        if (deviceHandle == INVALID_HANDLE_VALUE)
        {
            const DWORD finalOpenError = ::GetLastError();
            const char* const failureMessage = accessMode == BluetoothLeInterfaceHandleAccess::ReadWrite ? (attemptedSecondaryOpen ? u8"CreateFileW 无法以读写或只读权限打开 BLE 设备接口或 GATT 服务接口。" : u8"CreateFileW 无法以读写权限打开 BLE 设备接口或 GATT 服务接口。") : (attemptedSecondaryOpen ? u8"CreateFileW 无法以只读或读写权限打开 BLE 设备接口或 GATT 服务接口。" : u8"CreateFileW 无法以只读权限打开 BLE 设备接口或 GATT 服务接口。");
            GB_SystemResult result = MakeBluetoothBooleanFailureResult(finalOpenError, operationName, failureMessage);
            if (primaryOpenError != ERROR_SUCCESS && primaryOpenError != finalOpenError)
            {
                try
                {
                    result.message += u8" 首次打开的 Win32 错误码=";
                    result.message += std::to_string(static_cast<uint32_t>(primaryOpenError));
                    result.message += ".";
                }
                catch (...)
                {
                }
            }
            return result;
        }

        handleScope.Reset(deviceHandle);
        if (openedWithWriteAccess != nullptr)
        {
            *openedWithWriteAccess = hasWriteAccess;
        }
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

    static size_t GetMaxGattCharacteristicValueBufferSize()
    {
        const size_t maxDataSize = GetMaxGattCharacteristicValueDataSize();
        if (maxDataSize > (std::numeric_limits<size_t>::max)() - sizeof(BTH_LE_GATT_CHARACTERISTIC_VALUE))
        {
            return (std::numeric_limits<size_t>::max)();
        }

        // BTH_LE_GATT_CHARACTERISTIC_VALUE 末尾包含 Data[1]，不同驱动返回的所需缓冲区大小可能按
        // sizeof(结构体)+数据长度或 offsetof(Data)+数据长度计算。这里为系统返回值保留结构尾部对齐余量，
        // 真正的数据长度仍由 DataSize 和 ATT 最大值共同约束。
        return sizeof(BTH_LE_GATT_CHARACTERISTIC_VALUE) + maxDataSize;
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

    static GB_BluetoothGattServiceInfo ConvertGattServiceInfo(const BTH_LE_GATT_SERVICE& nativeService, const std::string& deviceInterfacePath, const GB_SystemDeviceInterfaceInfo& serviceInterface)
    {
        GB_BluetoothGattServiceInfo service;
        service.deviceInterfacePath = deviceInterfacePath;
        service.deviceId = deviceInterfacePath;
        service.serviceDeviceInstanceId = serviceInterface.deviceInstanceId;
        service.serviceInterfacePath = serviceInterface.interfacePath;
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
        characteristic.serviceDeviceInstanceId = service.serviceDeviceInstanceId;
        characteristic.serviceInterfacePath = service.serviceInterfacePath;
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
        if (IsHResultFromWin32Error(hresult, ERROR_INVALID_FUNCTION))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"Windows GATT 缓存中没有可用主服务，无法完成服务枚举；请确认设备已配对、处于可访问状态，并在设备重连后重试。");
        }
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
            if (IsHResultFromWin32Error(hresult, ERROR_INVALID_FUNCTION))
            {
                services.clear();
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"读取 BLE GATT 主服务时发现 Windows 缓存中已经没有可用服务，原有会话缓存保持不变；请确认设备仍处于可访问状态并重试。");
            }

            if (hresult == S_OK || IsHResultFromWin32Error(hresult, ERROR_MORE_DATA) || IsHResultFromWin32Error(hresult, ERROR_INVALID_USER_BUFFER))
            {
                USHORT refreshedServiceCount = 0;
                const HRESULT countHResult = ::BluetoothGATTGetServices(deviceHandle, 0, nullptr, &refreshedServiceCount, BLUETOOTH_GATT_FLAG_NONE);
                if (IsHResultFromWin32Error(countHResult, ERROR_INVALID_FUNCTION))
                {
                    services.clear();
                    return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"重新查询 BLE GATT 主服务时发现 Windows 缓存中没有可用服务。");
                }
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

    static GB_SystemResult ReadNativeGattCharacteristics(HANDLE deviceHandle, BTH_LE_GATT_SERVICE& service, std::vector<BTH_LE_GATT_CHARACTERISTIC>& characteristics, const std::string& operationName)
    {
        characteristics.clear();
        if (deviceHandle == nullptr || deviceHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE 设备句柄无效。");
        }

        USHORT characteristicCount = 0;
        HRESULT hresult = ::BluetoothGATTGetCharacteristics(deviceHandle, &service, 0, nullptr, &characteristicCount, BLUETOOTH_GATT_FLAG_NONE);
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
            hresult = ::BluetoothGATTGetCharacteristics(deviceHandle, &service, characteristicCount, characteristics.data(), &returnedCharacteristicCount, BLUETOOTH_GATT_FLAG_NONE);
            if (hresult == S_OK && returnedCharacteristicCount <= characteristicCount)
            {
                characteristics.resize(static_cast<size_t>(returnedCharacteristicCount));
                return GB_SystemResult::Succeeded(operationName);
            }

            if (hresult == S_OK || IsHResultFromWin32Error(hresult, ERROR_MORE_DATA) || IsHResultFromWin32Error(hresult, ERROR_INVALID_USER_BUFFER))
            {
                USHORT refreshedCharacteristicCount = 0;
                const HRESULT countHResult = ::BluetoothGATTGetCharacteristics(deviceHandle, &service, 0, nullptr, &refreshedCharacteristicCount, BLUETOOTH_GATT_FLAG_NONE);
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

    static GB_SystemResult ReadGattCharacteristicValue(HANDLE deviceHandle, BTH_LE_GATT_CHARACTERISTIC& nativeCharacteristic, std::vector<uint8_t>& value, const GB_BluetoothGattReadOptions& options, const std::string& operationName)
    {
        value.clear();
        GB_SystemResult validateResult = ValidateGattReadOptions(options, operationName);
        if (validateResult.IsFailed())
        {
            return validateResult;
        }
        if (deviceHandle == nullptr || deviceHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"BLE GATT 会话没有有效设备句柄。");
        }
        if (nativeCharacteristic.IsReadable == FALSE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"当前系统枚举到的 BLE GATT 特征未声明可读属性。");
        }

        const ULONG flags = BuildGattReadFlags(options);
        const size_t headerSize = GetGattCharacteristicValueHeaderSize();
        for (int retryIndex = 0; retryIndex < 3; retryIndex++)
        {
            USHORT requiredValueSize = 0;
            HRESULT hresult = ::BluetoothGATTGetCharacteristicValue(deviceHandle, &nativeCharacteristic, 0, nullptr, &requiredValueSize, flags);
            if (hresult != S_OK && !IsHResultFromWin32Error(hresult, ERROR_MORE_DATA) && !IsHResultFromWin32Error(hresult, ERROR_INVALID_USER_BUFFER))
            {
                return MapBluetoothGattHResult(hresult, operationName, u8"BluetoothGATTGetCharacteristicValue 查询 BLE GATT 特征值缓冲区大小失败。");
            }
            if (requiredValueSize == 0)
            {
                return GB_SystemResult::Succeeded(operationName);
            }
            if (static_cast<size_t>(requiredValueSize) < headerSize)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, u8"系统返回的 BLE GATT 特征值缓冲区大小小于结构头部。");
            }

            if (static_cast<size_t>(requiredValueSize) > GetMaxGattCharacteristicValueBufferSize())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, u8"系统返回的 BLE GATT 特征值缓冲区大小超过 ATT 属性值及结构开销允许范围。");
            }

            const size_t requiredDataSize = static_cast<size_t>(requiredValueSize) - headerSize;
            std::vector<ULONG> valueBuffer;
            GB_SystemResult allocationResult = AllocateGattCharacteristicValueBuffer(requiredDataSize, valueBuffer, operationName, u8"分配 BLE GATT 特征值缓冲区时内存不足。");
            if (allocationResult.IsFailed())
            {
                return allocationResult;
            }

            const size_t bufferByteCapacity = GetGattCharacteristicValueBufferByteCapacity(valueBuffer);
            if (bufferByteCapacity < static_cast<size_t>(requiredValueSize))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, operationName, u8"BLE GATT 特征值缓冲区容量计算错误。");
            }

            PBTH_LE_GATT_CHARACTERISTIC_VALUE nativeValue = reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(valueBuffer.data());
            hresult = ::BluetoothGATTGetCharacteristicValue(deviceHandle, &nativeCharacteristic, static_cast<ULONG>(bufferByteCapacity), nativeValue, nullptr, flags);
            if (hresult == S_OK)
            {
                const size_t dataSize = static_cast<size_t>(nativeValue->DataSize);
                if (dataSize > GetMaxGattCharacteristicValueDataSize() || headerSize > bufferByteCapacity || dataSize > bufferByteCapacity - headerSize)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, u8"系统返回的 BLE GATT 特征值长度超过实际缓冲区容量或 ATT 属性值上限。");
                }

                try
                {
                    value.assign(nativeValue->Data, nativeValue->Data + dataSize);
                }
                catch (...)
                {
                    value.clear();
                    return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"保存 BLE GATT 特征值时内存不足。");
                }

                return GB_SystemResult::Succeeded(operationName);
            }

            if (IsHResultFromWin32Error(hresult, ERROR_MORE_DATA) || IsHResultFromWin32Error(hresult, ERROR_INVALID_USER_BUFFER))
            {
                continue;
            }

            return MapBluetoothGattHResult(hresult, operationName, u8"BluetoothGATTGetCharacteristicValue 读取 BLE GATT 特征值失败。");
        }

        return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, u8"BluetoothGATTGetCharacteristicValue 多次报告缓冲区不足，特征值长度持续变化。");
    }

    static GB_SystemResult WriteGattCharacteristicValue(HANDLE deviceHandle, BTH_LE_GATT_CHARACTERISTIC& nativeCharacteristic, const std::vector<uint8_t>& value, const GB_BluetoothGattWriteOptions& options, const std::string& operationName)
    {
        GB_SystemResult validateResult = ValidateGattWriteOptions(options, operationName);
        if (validateResult.IsFailed())
        {
            return validateResult;
        }
        if (deviceHandle == nullptr || deviceHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"BLE GATT 会话没有有效设备句柄。");
        }
        if (value.size() > GetMaxGattCharacteristicValueDataSize())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE GATT 特征写入数据超过 ATT 属性值上限。");
        }

        if (options.signedWrite)
        {
            if (nativeCharacteristic.IsSignedWritable == FALSE)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"当前系统枚举到的 BLE GATT 特征未声明 SignedWrite 属性。");
            }
        }
        else if (options.writeWithoutResponse)
        {
            if (nativeCharacteristic.IsWritableWithoutResponse == FALSE)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"当前系统枚举到的 BLE GATT 特征未声明 WriteWithoutResponse 属性。");
            }
        }
        else if (nativeCharacteristic.IsWritable == FALSE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"当前系统枚举到的 BLE GATT 特征未声明可写属性。");
        }

        std::vector<ULONG> valueBuffer;
        GB_SystemResult allocationResult = AllocateGattCharacteristicValueBuffer(value.size(), valueBuffer, operationName, u8"分配 BLE GATT 特征写入缓冲区时内存不足。");
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
        const HRESULT hresult = ::BluetoothGATTSetCharacteristicValue(deviceHandle, &nativeCharacteristic, nativeValue, static_cast<BTH_LE_GATT_RELIABLE_WRITE_CONTEXT>(0), flags);
        if (hresult != S_OK)
        {
            return MapBluetoothGattHResult(hresult, operationName, u8"BluetoothGATTSetCharacteristicValue 写入 BLE GATT 特征值失败。", true);
        }

        return GB_SystemResult::Succeeded(operationName);
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
        device.pairStatus = GB_BluetoothPairStatus::Paired;
        device.connectionStatus = GB_BluetoothConnectionStatus::Unknown;
        device.isRemembered = true;
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
            radio.name = WideFixedBufferToUtf8(nativeRadioInfo.szName);
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
            if (lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_SERVICE_DOES_NOT_EXIST || lastError == ERROR_SERVICE_NOT_ACTIVE)
            {
                return GB_SystemResult::Succeeded(operationName, u8"当前系统未发现蓝牙无线电。");
            }

            return MakeBluetoothBooleanFailureResult(lastError, operationName, u8"BluetoothFindFirstRadio 查找第一块蓝牙无线电失败。");
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
            if (lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_SERVICE_DOES_NOT_EXIST || lastError == ERROR_SERVICE_NOT_ACTIVE)
            {
                return GB_SystemResult::Succeeded(operationName, u8"当前系统未发现蓝牙无线电。");
            }

            return MakeBluetoothBooleanFailureResult(lastError, operationName, u8"BluetoothFindFirstRadio 检查蓝牙无线电可用性失败。");
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

    static std::mutex& GetClassicBluetoothDeviceMutationMutex(const std::string& deviceAddress)
    {
        static std::array<std::mutex, 32> deviceMutationMutexes;
        const size_t mutexIndex = AsciiNoCaseHash()(deviceAddress) % deviceMutationMutexes.size();
        return deviceMutationMutexes[mutexIndex];
    }

    static AUTHENTICATION_REQUIREMENTS ToNativeAuthenticationRequirement(const GB_BluetoothAuthenticationRequirement authenticationRequirement)
    {
        switch (authenticationRequirement)
        {
        case GB_BluetoothAuthenticationRequirement::MitmProtectionRequired:
            return MITMProtectionRequired;
        case GB_BluetoothAuthenticationRequirement::MitmProtectionNotRequiredBonding:
            return MITMProtectionNotRequiredBonding;
        case GB_BluetoothAuthenticationRequirement::MitmProtectionRequiredBonding:
            return MITMProtectionRequiredBonding;
        case GB_BluetoothAuthenticationRequirement::MitmProtectionNotRequiredGeneralBonding:
            return MITMProtectionNotRequiredGeneralBonding;
        case GB_BluetoothAuthenticationRequirement::MitmProtectionRequiredGeneralBonding:
            return MITMProtectionRequiredGeneralBonding;
        case GB_BluetoothAuthenticationRequirement::MitmProtectionNotRequired:
        default:
            break;
        }

        return MITMProtectionNotRequired;
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
            if (lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_SERVICE_DOES_NOT_EXIST || lastError == ERROR_SERVICE_NOT_ACTIVE)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetRadios", u8"当前系统未发现蓝牙无线电。");
            }

            return MakeBluetoothBooleanFailureResult(lastError, u8"GB_SystemBluetooth::GetRadios", u8"BluetoothFindFirstRadio 枚举蓝牙无线电失败。");
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
                if (nextError == ERROR_NO_MORE_ITEMS)
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
                return MakeBluetoothBooleanFailureResult(nextError, u8"GB_SystemBluetooth::GetRadios", u8"BluetoothFindNextRadio 枚举下一个蓝牙无线电失败。");
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

    static bool ClassicDeviceMatchesQueryOptions(const BLUETOOTH_DEVICE_INFO& nativeDeviceInfo, const GB_BluetoothClassicDeviceQueryOptions& options)
    {
        if (nativeDeviceInfo.fAuthenticated != FALSE && options.includeAuthenticated)
        {
            return true;
        }
        if (nativeDeviceInfo.fRemembered != FALSE && options.includeRemembered)
        {
            return true;
        }
        if (nativeDeviceInfo.fConnected != FALSE && options.includeConnected)
        {
            return true;
        }

        const bool isUnknownDevice = nativeDeviceInfo.fAuthenticated == FALSE && nativeDeviceInfo.fRemembered == FALSE && nativeDeviceInfo.fConnected == FALSE;
        return isUnknownDevice && options.includeUnknown;
    }

    static GB_BluetoothDeviceInfo ConvertDeviceInfo(const BLUETOOTH_DEVICE_INFO& nativeDeviceInfo, const GB_BluetoothRadioInfo& radioInfo)
    {
        GB_BluetoothDeviceInfo device;
        device.address = FormatBluetoothAddress(nativeDeviceInfo.Address);
        device.deviceId = device.address;
        device.nativeDeviceId = device.address;
        device.radioId = radioInfo.radioId;
        device.radioAddress = radioInfo.address;
        device.name = WideFixedBufferToUtf8(nativeDeviceInfo.szName);
        device.deviceKind = GB_BluetoothDeviceKind::Classic;
        device.isRemembered = nativeDeviceInfo.fRemembered != FALSE;
        device.isAuthenticated = nativeDeviceInfo.fAuthenticated != FALSE;
        device.isConnected = nativeDeviceInfo.fConnected != FALSE;
        device.pairStatus = device.isAuthenticated ? GB_BluetoothPairStatus::Paired : GB_BluetoothPairStatus::Unpaired;
        device.connectionStatus = device.isConnected ? GB_BluetoothConnectionStatus::Connected : GB_BluetoothConnectionStatus::Disconnected;
        device.isClassicSupported = true;
        device.isLowEnergySupported = false;
        device.classOfDevice = nativeDeviceInfo.ulClassofDevice;
        device.lastSeenTime = SystemTimeToString(nativeDeviceInfo.stLastSeen);
        device.lastUsedTime = SystemTimeToString(nativeDeviceInfo.stLastUsed);
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
                    std::sort(serviceGuids.begin(), serviceGuids.end());
                    serviceGuids.erase(std::unique(serviceGuids.begin(), serviceGuids.end()), serviceGuids.end());

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
                    if (countResult == ERROR_NOT_FOUND || countResult == ERROR_SERVICE_DOES_NOT_EXIST)
                    {
                        serviceGuids.clear();
                        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::ReadInstalledServices");
                    }

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

    static GB_SystemResult TryGetNativeClassicDeviceInfoByAddress(HANDLE radioHandle, const GB_BluetoothRadioInfo& radioInfo, const std::string& targetAddress, const GB_BluetoothClassicDeviceQueryOptions& options, BLUETOOTH_DEVICE_INFO& nativeDeviceInfo, GB_BluetoothDeviceInfo* publicDevice, bool& found)
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
        if (!ClassicDeviceMatchesQueryOptions(nativeDeviceInfo, options))
        {
            nativeDeviceInfo = {};
            nativeDeviceInfo.dwSize = sizeof(nativeDeviceInfo);
            return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::TryGetNativeClassicDeviceInfoByAddress", u8"设备存在，但不符合当前经典蓝牙查询返回类别。");
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

            if (options.includeInstalledServices)
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
            GB_SystemResult fastLookupResult = TryGetNativeClassicDeviceInfoByAddress(radioHandle, radioInfo, targetAddress, options, nativeDeviceInfo, publicDevice, found);
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
            if (lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_NOT_FOUND)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio");
            }

            return MakeBluetoothBooleanFailureResult(lastError, u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio", u8"BluetoothFindFirstDevice 查找经典蓝牙设备失败。");
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
                if (nextError == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }

                return MakeBluetoothBooleanFailureResult(nextError, u8"GB_SystemBluetooth::FindNativeClassicDeviceForRadio", u8"BluetoothFindNextDevice 查找下一个经典蓝牙设备失败。");
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
            if (lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_NOT_FOUND)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::FindNativeClassicDeviceAcrossRadios");
            }

            return MakeBluetoothBooleanFailureResult(lastError, u8"GB_SystemBluetooth::FindNativeClassicDeviceAcrossRadios", u8"BluetoothFindFirstDevice 跨全部本机蓝牙无线电查找经典蓝牙设备失败。");
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
                if (nextError == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }

                return MakeBluetoothBooleanFailureResult(nextError, u8"GB_SystemBluetooth::FindNativeClassicDeviceAcrossRadios", u8"BluetoothFindNextDevice 跨全部本机蓝牙无线电查找下一个经典蓝牙设备失败。");
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
            if (lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_NOT_FOUND)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetClassicDevices");
            }

            return MakeBluetoothBooleanFailureResult(lastError, u8"GB_SystemBluetooth::GetClassicDevices", u8"BluetoothFindFirstDevice 枚举经典蓝牙设备失败。");
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
                if (nextError == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }

                return MakeBluetoothBooleanFailureResult(nextError, u8"GB_SystemBluetooth::GetClassicDevices", u8"BluetoothFindNextDevice 枚举下一个经典蓝牙设备失败。");
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

    struct BluetoothInterfaceIdentity
    {
        std::string interfacePath;
        std::string deviceInstanceId;
        std::string parentInstanceId;
        std::string containerId;
        std::string address;
    };

    static GB_SystemResult BuildBluetoothInterfaceIdentity(const GB_SystemDeviceInterfaceInfo& deviceInterface, BluetoothInterfaceIdentity& identity, const std::string& operationName)
    {
        identity = BluetoothInterfaceIdentity();
        try
        {
            identity.interfacePath = deviceInterface.interfacePath;
            identity.deviceInstanceId = deviceInterface.deviceInstanceId;
            if (!TryExtractBluetoothAddressFromText(identity.deviceInstanceId, identity.address))
            {
                (void)TryExtractBluetoothAddressFromText(identity.interfacePath, identity.address);
            }
        }
        catch (...)
        {
            identity = BluetoothInterfaceIdentity();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"保存蓝牙设备接口身份信息时内存不足。");
        }

        return GB_SystemResult::Succeeded(operationName);
    }

    static bool IsConfigRetDeviceNotFound(const CONFIGRET configResult)
    {
        if (configResult == CR_NO_SUCH_DEVNODE)
        {
            return true;
        }
#if defined(CR_NO_SUCH_DEVINST)
        if (configResult == CR_NO_SUCH_DEVINST)
        {
            return true;
        }
#endif
#if defined(CR_INVALID_DEVNODE)
        if (configResult == CR_INVALID_DEVNODE)
        {
            return true;
        }
#endif
#if defined(CR_INVALID_DEVINST)
        if (configResult == CR_INVALID_DEVINST)
        {
            return true;
        }
#endif
#if defined(CR_INVALID_DEVICE_ID)
        if (configResult == CR_INVALID_DEVICE_ID)
        {
            return true;
        }
#endif

        return false;
    }

    static GB_SystemResult MakeConfigRetFailureResult(const CONFIGRET configResult, const std::string& operationName, const std::string& message)
    {
        const DWORD win32Error = ::CM_MapCrToWin32Err(configResult, ERROR_GEN_FAILURE);
        return GB_SystemResult::FromWin32Error(static_cast<uint32_t>(win32Error), operationName, message);
    }

    static GB_SystemResult EnrichBluetoothInterfaceIdentity(BluetoothInterfaceIdentity& identity, const std::string& operationName)
    {
        if (identity.deviceInstanceId.empty() || (!identity.parentInstanceId.empty() && !identity.containerId.empty()))
        {
            return GB_SystemResult::Succeeded(operationName);
        }

        std::wstring deviceInstanceIdWide;
        try
        {
            deviceInstanceIdWide = GB_Utf8ToWString(identity.deviceInstanceId);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"转换蓝牙 PnP 设备实例 ID 时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, operationName, u8"蓝牙 PnP 设备实例 ID 不是合法 UTF-8 文本。");
        }

        DEVINST deviceInstance = 0;
        const CONFIGRET locateResult = ::CM_Locate_DevNodeW(&deviceInstance, const_cast<DEVINSTID_W>(deviceInstanceIdWide.c_str()), CM_LOCATE_DEVNODE_NORMAL);
        if (IsConfigRetDeviceNotFound(locateResult))
        {
            return GB_SystemResult::Succeeded(operationName);
        }
        if (locateResult != CR_SUCCESS)
        {
            return MakeConfigRetFailureResult(locateResult, operationName, u8"CM_Locate_DevNodeW 定位蓝牙 PnP 设备实例失败。");
        }

        try
        {
            if (identity.parentInstanceId.empty())
            {
                DEVINST parentDeviceInstance = 0;
                const CONFIGRET parentResult = ::CM_Get_Parent(&parentDeviceInstance, deviceInstance, 0);
                if (parentResult == CR_SUCCESS)
                {
                    ULONG parentIdLength = 0;
                    const CONFIGRET parentSizeResult = ::CM_Get_Device_ID_Size(&parentIdLength, parentDeviceInstance, 0);
                    if (parentSizeResult == CR_SUCCESS)
                    {
                        std::vector<wchar_t> parentIdBuffer(static_cast<size_t>(parentIdLength) + 1u, L'\0');
                        const CONFIGRET parentIdResult = ::CM_Get_Device_IDW(parentDeviceInstance, parentIdBuffer.data(), static_cast<ULONG>(parentIdBuffer.size()), 0);
                        if (parentIdResult == CR_SUCCESS)
                        {
                            identity.parentInstanceId = WideStringToUtf8Safe(std::wstring(parentIdBuffer.data(), static_cast<size_t>(parentIdLength)));
                        }
                    }
                }
            }

            if (identity.containerId.empty())
            {
                GUID containerId = {};
                DEVPROPTYPE propertyType = 0;
                ULONG propertySize = static_cast<ULONG>(sizeof(containerId));
                const CONFIGRET propertyResult = ::CM_Get_DevNode_PropertyW(deviceInstance, &DEVPKEY_Device_ContainerId, &propertyType, reinterpret_cast<PBYTE>(&containerId), &propertySize, 0);
                if (propertyResult == CR_SUCCESS && propertyType == DEVPROP_TYPE_GUID && propertySize >= sizeof(containerId))
                {
                    identity.containerId = GuidToString(containerId);
                }
            }

            if (identity.address.empty() && !TryExtractBluetoothAddressFromText(identity.deviceInstanceId, identity.address))
            {
                (void)TryExtractBluetoothAddressFromText(identity.parentInstanceId, identity.address);
            }
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"保存蓝牙 PnP 设备身份信息时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, operationName, u8"读取蓝牙 PnP 设备父节点或 ContainerId 时发生内部错误。");
        }

        return GB_SystemResult::Succeeded(operationName);
    }

    static bool AreBluetoothInterfaceIdentitiesStronglyRelated(const BluetoothInterfaceIdentity& deviceIdentity, const BluetoothInterfaceIdentity& serviceIdentity)
    {
        if (!deviceIdentity.address.empty() && !serviceIdentity.address.empty())
        {
            return deviceIdentity.address == serviceIdentity.address;
        }
        if (!deviceIdentity.containerId.empty() && !serviceIdentity.containerId.empty() && EqualsAsciiNoCase(deviceIdentity.containerId, serviceIdentity.containerId))
        {
            return true;
        }
        if (!deviceIdentity.deviceInstanceId.empty() && !serviceIdentity.deviceInstanceId.empty() && EqualsAsciiNoCase(deviceIdentity.deviceInstanceId, serviceIdentity.deviceInstanceId))
        {
            return true;
        }
        if (!deviceIdentity.deviceInstanceId.empty() && !serviceIdentity.parentInstanceId.empty() && EqualsAsciiNoCase(deviceIdentity.deviceInstanceId, serviceIdentity.parentInstanceId))
        {
            return true;
        }

        return false;
    }

    static bool DoBluetoothInterfaceAddressesConflict(const BluetoothInterfaceIdentity& deviceIdentity, const BluetoothInterfaceIdentity& serviceIdentity)
    {
        return !deviceIdentity.address.empty() && !serviceIdentity.address.empty() && deviceIdentity.address != serviceIdentity.address;
    }

    static GB_SystemResult GetBluetoothInterfaceIdentityByPath(const std::string& interfaceClassGuid, const std::string& interfacePath, BluetoothInterfaceIdentity& identity, const std::string& operationName)
    {
        identity = BluetoothInterfaceIdentity();
        if (interfacePath.empty() || ContainsNullCharacter(interfacePath))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"蓝牙设备接口路径不能为空且不能包含空字符。");
        }

        std::vector<GB_SystemDeviceInterfaceInfo> deviceInterfaces;
        GB_SystemResult interfaceResult = GB_SystemDevice::GetDeviceInterfacesByClassGuid(interfaceClassGuid, deviceInterfaces, true);
        if (interfaceResult.IsFailed())
        {
            return interfaceResult.WithOperationName(operationName);
        }

        for (size_t index = 0; index < deviceInterfaces.size(); index++)
        {
            if (AreDeviceInterfacePathsEqual(deviceInterfaces[index].interfacePath, interfacePath))
            {
                return BuildBluetoothInterfaceIdentity(deviceInterfaces[index], identity, operationName);
            }
        }

        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"未在当前存在的 Windows 设备接口中找到指定蓝牙接口路径。");
    }

    static GB_SystemResult GetGattServiceInterfacesForDevice(const BluetoothInterfaceIdentity& inputDeviceIdentity, std::vector<GB_SystemDeviceInterfaceInfo>& serviceInterfaces, const std::string& operationName)
    {
        serviceInterfaces.clear();

        BluetoothInterfaceIdentity deviceIdentity;
        try
        {
            deviceIdentity = inputDeviceIdentity;
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"复制 BLE 设备接口身份信息时内存不足。");
        }
        if (deviceIdentity.interfacePath.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"BLE GATT 会话缺少经过设备接口枚举验证的目标设备身份信息。");
        }

        std::vector<GB_SystemDeviceInterfaceInfo> allServiceInterfaces;
        GB_SystemResult interfaceResult = GB_SystemDevice::GetDeviceInterfacesByClassGuid(GB_BluetoothGattServiceDeviceInterfaceGuid, allServiceInterfaces, true);
        if (interfaceResult.IsFailed())
        {
            return interfaceResult.WithOperationName(operationName);
        }

        std::vector<GB_SystemDeviceInterfaceInfo> strongMatches;
        std::vector<GB_SystemDeviceInterfaceInfo> fallbackCandidates;
        try
        {
            strongMatches.reserve(allServiceInterfaces.size());
            fallbackCandidates.reserve(allServiceInterfaces.size());
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"预分配 BLE GATT 服务接口候选缓冲区时内存不足。");
        }

        bool deviceIdentityEnriched = false;
        GB_SystemResult firstIdentityFailure = GB_SystemResult::Succeeded(operationName);
        for (size_t index = 0; index < allServiceInterfaces.size(); index++)
        {
            BluetoothInterfaceIdentity serviceIdentity;
            GB_SystemResult serviceIdentityResult = BuildBluetoothInterfaceIdentity(allServiceInterfaces[index], serviceIdentity, operationName);
            if (serviceIdentityResult.IsFailed())
            {
                if (firstIdentityFailure.IsSucceeded())
                {
                    firstIdentityFailure = std::move(serviceIdentityResult);
                }
                continue;
            }

            bool strongMatch = AreBluetoothInterfaceIdentitiesStronglyRelated(deviceIdentity, serviceIdentity);
            bool addressesConflict = DoBluetoothInterfaceAddressesConflict(deviceIdentity, serviceIdentity);
            if (!strongMatch && !addressesConflict)
            {
                if (!deviceIdentityEnriched)
                {
                    GB_SystemResult enrichDeviceResult = EnrichBluetoothInterfaceIdentity(deviceIdentity, operationName);
                    deviceIdentityEnriched = true;
                    if (enrichDeviceResult.IsFailed() && firstIdentityFailure.IsSucceeded())
                    {
                        firstIdentityFailure = std::move(enrichDeviceResult);
                    }
                }

                GB_SystemResult enrichServiceResult = EnrichBluetoothInterfaceIdentity(serviceIdentity, operationName);
                if (enrichServiceResult.IsFailed())
                {
                    if (firstIdentityFailure.IsSucceeded())
                    {
                        firstIdentityFailure = std::move(enrichServiceResult);
                    }
                    continue;
                }

                strongMatch = AreBluetoothInterfaceIdentitiesStronglyRelated(deviceIdentity, serviceIdentity);
                addressesConflict = DoBluetoothInterfaceAddressesConflict(deviceIdentity, serviceIdentity);
            }
            if (addressesConflict)
            {
                continue;
            }

            try
            {
                if (strongMatch)
                {
                    strongMatches.push_back(allServiceInterfaces[index]);
                }
                else
                {
                    // PnP 元数据可能不完整。不能仅凭缺失的地址、ContainerId 或父节点关系否定归属，
                    // 后续会通过 BluetoothGATTGetCharacteristics 的原生服务层级校验排除其它设备的服务接口。
                    fallbackCandidates.push_back(allServiceInterfaces[index]);
                }
            }
            catch (...)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"保存 BLE GATT 服务接口候选信息时内存不足。");
            }
        }

        const auto sortByInterfacePath = [](const GB_SystemDeviceInterfaceInfo& leftInterface, const GB_SystemDeviceInterfaceInfo& rightInterface)
            {
                return leftInterface.interfacePath < rightInterface.interfacePath;
            };
        std::sort(strongMatches.begin(), strongMatches.end(), sortByInterfacePath);
        std::sort(fallbackCandidates.begin(), fallbackCandidates.end(), sortByInterfacePath);

        try
        {
            serviceInterfaces.reserve(strongMatches.size() + fallbackCandidates.size());
            serviceInterfaces.insert(serviceInterfaces.end(), strongMatches.begin(), strongMatches.end());
            serviceInterfaces.insert(serviceInterfaces.end(), fallbackCandidates.begin(), fallbackCandidates.end());
        }
        catch (...)
        {
            serviceInterfaces.clear();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"构建 BLE GATT 服务接口候选列表时内存不足。");
        }

        if (serviceInterfaces.empty() && firstIdentityFailure.IsFailed())
        {
            firstIdentityFailure.WithMessage(u8"读取 BLE GATT 服务接口的 PnP 身份信息失败，无法建立可验证的服务接口候选列表。");
            return firstIdentityFailure;
        }

        if (!fallbackCandidates.empty())
        {
            try
            {
                std::string message = u8"已优先排列 PnP 身份明确匹配的 GATT 服务接口，并保留 ";
                message += std::to_string(fallbackCandidates.size());
                message += u8" 个身份信息不完整的候选接口供原生 GATT 层级校验。";
                return GB_SystemResult::Succeeded(operationName, message);
            }
            catch (...)
            {
                return GB_SystemResult::Succeeded(operationName, u8"已保留身份信息不完整的 GATT 服务接口候选项，并将在后续执行原生层级校验。");
            }
        }

        return GB_SystemResult::Succeeded(operationName);
    }

#endif
}

class GB_BluetoothGattSession::Impl
{
public:
    GB_SystemResult Open(const std::string& inputDeviceInterfacePath, const GB_BluetoothGattSessionAccessMode inputAccessMode)
    {
        if (!IsValidGattSessionAccessMode(inputAccessMode))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_BluetoothGattSession::Open", u8"accessMode 不是有效的 GB_BluetoothGattSessionAccessMode 值。");
        }

#if !defined(_WIN32)
        (void)inputDeviceInterfacePath;
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_BluetoothGattSession::Open", u8"当前平台不支持 Windows BLE GATT 会话。");
#else
        std::lock_guard<std::mutex> lock(stateMutex);

        BluetoothInterfaceIdentity newDeviceIdentity;
        GB_SystemResult identityResult = GetBluetoothInterfaceIdentityByPath(GB_BluetoothLeDeviceInterfaceGuid, inputDeviceInterfacePath, newDeviceIdentity, u8"GB_BluetoothGattSession::Open");
        if (identityResult.IsFailed())
        {
            return identityResult.WithMessage(u8"deviceInterfacePath 必须是当前存在的 GUID_BLUETOOTHLE_DEVICE_INTERFACE 设备接口路径。");
        }

        GenericHandleScope newDeviceHandle;
        GB_SystemResult openResult = OpenBluetoothLeInterfaceHandle(inputDeviceInterfacePath, BluetoothLeInterfaceHandleAccess::ReadOnly, false, newDeviceHandle, u8"GB_BluetoothGattSession::Open");
        if (openResult.IsFailed())
        {
            return openResult;
        }

        std::string newDeviceInterfacePath;
        try
        {
            newDeviceInterfacePath = inputDeviceInterfacePath;
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_BluetoothGattSession::Open", u8"保存 BLE 设备接口路径时内存不足。");
        }

        ClearLocked();
        deviceHandle = std::move(newDeviceHandle);
        deviceInterfacePath.swap(newDeviceInterfacePath);
        deviceIdentity = std::move(newDeviceIdentity);
        accessMode = inputAccessMode;
        isOpen = true;
        writeEnabled = inputAccessMode == GB_BluetoothGattSessionAccessMode::ReadWrite;
        return GB_SystemResult::Succeeded(u8"GB_BluetoothGattSession::Open");
#endif
    }

    GB_SystemResult Close()
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        ClearLocked();
        return GB_SystemResult::Succeeded(u8"GB_BluetoothGattSession::Close");
    }

    bool IsOpen() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return isOpen;
    }

    bool IsWriteEnabled() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return isOpen && writeEnabled;
    }

    std::string GetDeviceInterfacePath() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return deviceInterfacePath;
    }

    GB_SystemResult RefreshCache()
    {
#if !defined(_WIN32)
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_BluetoothGattSession::RefreshCache", u8"当前平台不支持 Windows BLE GATT 会话。");
#else
        std::lock_guard<std::mutex> lock(stateMutex);
        return RefreshServicesLocked(u8"GB_BluetoothGattSession::RefreshCache");
#endif
    }

    GB_SystemResult GetServices(std::vector<GB_BluetoothGattServiceInfo>& services)
    {
        services.clear();
#if !defined(_WIN32)
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_BluetoothGattSession::GetServices", u8"当前平台不支持 Windows BLE GATT 会话。");
#else
        std::lock_guard<std::mutex> lock(stateMutex);
        GB_SystemResult ensureResult = EnsureServicesLoadedLocked(u8"GB_BluetoothGattSession::GetServices");
        if (ensureResult.IsFailed())
        {
            return ensureResult;
        }

        std::vector<GB_BluetoothGattServiceInfo> outputServices;
        try
        {
            outputServices.reserve(cachedServices.size());
            for (size_t index = 0; index < cachedServices.size(); index++)
            {
                outputServices.push_back(cachedServices[index].serviceInfo);
            }
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_BluetoothGattSession::GetServices", u8"复制 BLE GATT 服务信息时内存不足。");
        }

        services.swap(outputServices);
        return GB_SystemResult::Succeeded(u8"GB_BluetoothGattSession::GetServices", ensureResult.message);
#endif
    }

    GB_SystemResult GetCharacteristics(const GB_BluetoothGattServiceInfo& service, std::vector<GB_BluetoothGattCharacteristicInfo>& characteristics)
    {
        characteristics.clear();
#if !defined(_WIN32)
        (void)service;
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_BluetoothGattSession::GetCharacteristics", u8"当前平台不支持 Windows BLE GATT 会话。");
#else
        std::lock_guard<std::mutex> lock(stateMutex);
        size_t serviceIndex = 0;
        GB_SystemResult findResult = FindServiceLocked(service, serviceIndex, u8"GB_BluetoothGattSession::GetCharacteristics");
        if (findResult.IsFailed())
        {
            return findResult;
        }

        CachedService& cachedService = cachedServices[serviceIndex];
        GB_SystemResult ensureResult = EnsureCharacteristicsLoadedLocked(cachedService, u8"GB_BluetoothGattSession::GetCharacteristics");
        if (ensureResult.IsFailed())
        {
            return ensureResult;
        }

        std::vector<GB_BluetoothGattCharacteristicInfo> outputCharacteristics;
        try
        {
            outputCharacteristics = cachedService.characteristicInfos;
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_BluetoothGattSession::GetCharacteristics", u8"复制 BLE GATT 特征信息时内存不足。");
        }

        characteristics.swap(outputCharacteristics);
        return GB_SystemResult::Succeeded(u8"GB_BluetoothGattSession::GetCharacteristics");
#endif
    }

    GB_SystemResult ReadCharacteristic(const GB_BluetoothGattCharacteristicInfo& characteristic, std::vector<uint8_t>& value, const GB_BluetoothGattReadOptions& options)
    {
        value.clear();
#if !defined(_WIN32)
        (void)characteristic;
        (void)options;
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_BluetoothGattSession::ReadCharacteristic", u8"当前平台不支持 Windows BLE GATT 会话。");
#else
        const GB_SystemResult validateResult = ValidateGattReadOptions(options, u8"GB_BluetoothGattSession::ReadCharacteristic");
        if (validateResult.IsFailed())
        {
            return validateResult;
        }

        std::lock_guard<std::mutex> lock(stateMutex);
        size_t serviceIndex = 0;
        size_t characteristicIndex = 0;
        GB_SystemResult findResult = FindCharacteristicLocked(characteristic, serviceIndex, characteristicIndex, u8"GB_BluetoothGattSession::ReadCharacteristic");
        if (findResult.IsFailed())
        {
            return findResult;
        }

        CachedService& cachedService = cachedServices[serviceIndex];
        if (!cachedService.serviceHandle.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_BluetoothGattSession::ReadCharacteristic", u8"BLE GATT 服务接口句柄无效，请调用 RefreshCache() 重新建立服务缓存。");
        }

        return ReadGattCharacteristicValue(cachedService.serviceHandle.Get(), cachedService.nativeCharacteristics[characteristicIndex], value, options, u8"GB_BluetoothGattSession::ReadCharacteristic");
#endif
    }

    GB_SystemResult WriteCharacteristic(const GB_BluetoothGattCharacteristicInfo& characteristic, const std::vector<uint8_t>& value, const GB_BluetoothGattWriteOptions& options)
    {
#if !defined(_WIN32)
        (void)characteristic;
        (void)value;
        (void)options;
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_BluetoothGattSession::WriteCharacteristic", u8"当前平台不支持 Windows BLE GATT 会话。");
#else
        const GB_SystemResult validateResult = ValidateGattWriteOptions(options, u8"GB_BluetoothGattSession::WriteCharacteristic");
        if (validateResult.IsFailed())
        {
            return validateResult;
        }
        if (value.size() > GetMaxGattCharacteristicValueDataSize())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_BluetoothGattSession::WriteCharacteristic", u8"BLE GATT 特征写入数据超过 ATT 属性值上限。");
        }

        std::lock_guard<std::mutex> lock(stateMutex);
        if (!isOpen)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotInitialized, u8"GB_BluetoothGattSession::WriteCharacteristic", u8"BLE GATT 会话尚未打开。");
        }
        if (!writeEnabled)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::PermissionDenied, u8"GB_BluetoothGattSession::WriteCharacteristic", u8"BLE GATT 会话未以 ReadWrite 模式打开。");
        }

        size_t serviceIndex = 0;
        size_t characteristicIndex = 0;
        GB_SystemResult findResult = FindCharacteristicLocked(characteristic, serviceIndex, characteristicIndex, u8"GB_BluetoothGattSession::WriteCharacteristic");
        if (findResult.IsFailed())
        {
            return findResult;
        }

        CachedService& cachedService = cachedServices[serviceIndex];
        if (!cachedService.serviceHandle.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_BluetoothGattSession::WriteCharacteristic", u8"BLE GATT 服务接口句柄无效，请调用 RefreshCache() 重新建立服务缓存。");
        }
        const GB_SystemResult writeAccessResult = EnsureServiceWriteAccessLocked(cachedService, u8"GB_BluetoothGattSession::WriteCharacteristic");
        if (writeAccessResult.IsFailed())
        {
            return writeAccessResult;
        }

        return WriteGattCharacteristicValue(cachedService.serviceHandle.Get(), cachedService.nativeCharacteristics[characteristicIndex], value, options, u8"GB_BluetoothGattSession::WriteCharacteristic");
#endif
    }

private:
#if defined(_WIN32)
    struct CachedService
    {
        BTH_LE_GATT_SERVICE nativeService = BTH_LE_GATT_SERVICE();
        GB_BluetoothGattServiceInfo serviceInfo;
        GenericHandleScope serviceHandle;
        bool serviceHandleHasWriteAccess = false;
        bool characteristicsLoaded = false;
        std::vector<BTH_LE_GATT_CHARACTERISTIC> nativeCharacteristics;
        std::vector<GB_BluetoothGattCharacteristicInfo> characteristicInfos;
    };

    static GB_SystemResult ProbeGattServiceHierarchy(HANDLE serviceHandle, BTH_LE_GATT_SERVICE& service, bool& compatible, const std::string& operationName)
    {
        compatible = false;
        if (serviceHandle == nullptr || serviceHandle == INVALID_HANDLE_VALUE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE GATT 服务接口句柄无效。");
        }

        USHORT characteristicCount = 0;
        const HRESULT hresult = ::BluetoothGATTGetCharacteristics(serviceHandle, &service, 0, nullptr, &characteristicCount, BLUETOOTH_GATT_FLAG_NONE);
        if (hresult == S_OK || IsHResultFromWin32Error(hresult, ERROR_MORE_DATA) || IsHResultFromWin32Error(hresult, ERROR_INVALID_USER_BUFFER))
        {
            compatible = true;
            return GB_SystemResult::Succeeded(operationName);
        }
        if (IsHResultFromWin32Error(hresult, ERROR_ACCESS_DENIED) || IsHResultFromWin32Error(hresult, ERROR_NOT_FOUND) || IsHResultFromWin32Error(hresult, ERROR_FILE_NOT_FOUND))
        {
            return GB_SystemResult::Succeeded(operationName);
        }

        return MapBluetoothGattHResult(hresult, operationName, u8"验证 BLE GATT 服务接口与主服务层级关系失败。");
    }

    static bool ContainsCachedService(const std::vector<CachedService>& services, const BTH_LE_GATT_SERVICE& nativeService)
    {
        for (size_t index = 0; index < services.size(); index++)
        {
            if (services[index].nativeService.AttributeHandle == nativeService.AttributeHandle && AreBthLeUuidsEqual(services[index].nativeService.ServiceUuid, nativeService.ServiceUuid))
            {
                return true;
            }
        }

        return false;
    }

    GB_SystemResult EnsureOpenLocked(const std::string& operationName) const
    {
        if (!isOpen || !deviceHandle.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotInitialized, operationName, u8"BLE GATT 会话尚未打开。");
        }

        return GB_SystemResult::Succeeded(operationName);
    }

    GB_SystemResult RefreshServicesLocked(const std::string& operationName)
    {
        GB_SystemResult openResult = EnsureOpenLocked(operationName);
        if (openResult.IsFailed())
        {
            return openResult;
        }

        std::vector<BTH_LE_GATT_SERVICE> nativeDeviceServices;
        GB_SystemResult serviceResult = ReadNativeGattServices(deviceHandle.Get(), nativeDeviceServices, operationName);
        if (serviceResult.IsFailed())
        {
            return serviceResult;
        }
        if (nativeDeviceServices.empty())
        {
            cachedServices.clear();
            servicesLoaded = true;
            return GB_SystemResult::Succeeded(operationName);
        }

        std::vector<GB_SystemDeviceInterfaceInfo> serviceInterfaces;
        GB_SystemResult interfaceResult = GetGattServiceInterfacesForDevice(deviceIdentity, serviceInterfaces, operationName);
        if (interfaceResult.IsFailed())
        {
            return interfaceResult;
        }
        if (serviceInterfaces.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"Windows 已返回 BLE 主服务，但未找到属于该设备的 GATT 服务设备接口，无法安全执行特征枚举和读写。");
        }

        std::vector<CachedService> newCachedServices;
        try
        {
            newCachedServices.reserve(nativeDeviceServices.size());
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"预分配 BLE GATT 服务会话缓存时内存不足。");
        }

        GB_SystemResult firstInterfaceFailure = GB_SystemResult::Succeeded(operationName);
        for (size_t interfaceIndex = 0; interfaceIndex < serviceInterfaces.size(); interfaceIndex++)
        {
            if (newCachedServices.size() == nativeDeviceServices.size())
            {
                break;
            }

            GenericHandleScope serviceHandle;
            bool serviceHandleHasWriteAccess = false;
            const BluetoothLeInterfaceHandleAccess preferredAccess = accessMode == GB_BluetoothGattSessionAccessMode::ReadWrite ? BluetoothLeInterfaceHandleAccess::ReadWrite : BluetoothLeInterfaceHandleAccess::ReadOnly;
            GB_SystemResult handleResult = OpenBluetoothLeInterfaceHandle(serviceInterfaces[interfaceIndex].interfacePath, preferredAccess, accessMode == GB_BluetoothGattSessionAccessMode::ReadWrite, serviceHandle, operationName, &serviceHandleHasWriteAccess);
            if (handleResult.IsFailed())
            {
                if (firstInterfaceFailure.IsSucceeded())
                {
                    firstInterfaceFailure = std::move(handleResult);
                    firstInterfaceFailure.WithMessage(u8"打开属于目标 BLE 设备的 GATT 服务接口失败。");
                }
                continue;
            }
            for (size_t serviceIndex = 0; serviceIndex < nativeDeviceServices.size(); serviceIndex++)
            {
                if (ContainsCachedService(newCachedServices, nativeDeviceServices[serviceIndex]))
                {
                    continue;
                }

                bool compatible = false;
                GB_SystemResult probeResult = ProbeGattServiceHierarchy(serviceHandle.Get(), nativeDeviceServices[serviceIndex], compatible, operationName);
                if (probeResult.IsFailed())
                {
                    if (firstInterfaceFailure.IsSucceeded())
                    {
                        firstInterfaceFailure = std::move(probeResult);
                    }
                    continue;
                }
                if (!compatible)
                {
                    continue;
                }

                try
                {
                    CachedService cachedService;
                    cachedService.nativeService = nativeDeviceServices[serviceIndex];
                    cachedService.serviceInfo = ConvertGattServiceInfo(nativeDeviceServices[serviceIndex], deviceInterfacePath, serviceInterfaces[interfaceIndex]);
                    cachedService.serviceHandle = std::move(serviceHandle);
                    cachedService.serviceHandleHasWriteAccess = serviceHandleHasWriteAccess;
                    newCachedServices.push_back(std::move(cachedService));
                }
                catch (...)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"构建 BLE GATT 服务会话缓存时内存不足。");
                }

                break;
            }
        }

        if (newCachedServices.empty())
        {
            if (firstInterfaceFailure.IsFailed())
            {
                try
                {
                    firstInterfaceFailure.message += u8" Windows 已返回 BLE 主服务，但没有任何主服务能够通过独立 GATT 服务接口完成层级验证，原有会话缓存保持不变。";
                }
                catch (...)
                {
                }
                return firstInterfaceFailure;
            }

            return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, u8"Windows 已返回 BLE 主服务，但没有任何主服务能够匹配并打开对应的 GATT 服务设备接口，原有会话缓存保持不变。");
        }

        const size_t skippedServiceCount = nativeDeviceServices.size() - newCachedServices.size();
        std::sort(newCachedServices.begin(), newCachedServices.end(), [](const CachedService& leftService, const CachedService& rightService)
            {
                if (leftService.nativeService.AttributeHandle != rightService.nativeService.AttributeHandle)
                {
                    return leftService.nativeService.AttributeHandle < rightService.nativeService.AttributeHandle;
                }
                return leftService.serviceInfo.uuid < rightService.serviceInfo.uuid;
            });
        cachedServices.swap(newCachedServices);
        servicesLoaded = true;
        if (skippedServiceCount == 0)
        {
            return GB_SystemResult::Succeeded(operationName);
        }

        try
        {
            std::string message = u8"BLE GATT 服务缓存已刷新；可用服务数量=";
            message += std::to_string(cachedServices.size());
            message += u8"，因服务接口不可打开、无法关联或层级验证失败而跳过的主服务数量=";
            message += std::to_string(skippedServiceCount);
            message += u8"。已保留全部经过服务句柄层级验证的可用服务。";
            if (firstInterfaceFailure.IsFailed())
            {
                message += u8" 首个跳过原因：";
                message += firstInterfaceFailure.GetDisplayMessage();
            }
            return GB_SystemResult::Succeeded(operationName, message);
        }
        catch (...)
        {
            return GB_SystemResult::Succeeded(operationName, u8"BLE GATT 服务缓存已刷新，并跳过了无法安全建立服务接口缓存的主服务。");
        }
    }

    GB_SystemResult EnsureServicesLoadedLocked(const std::string& operationName)
    {
        if (servicesLoaded)
        {
            return EnsureOpenLocked(operationName);
        }

        return RefreshServicesLocked(operationName);
    }

    GB_SystemResult FindServiceLocked(const GB_BluetoothGattServiceInfo& service, size_t& serviceIndex, const std::string& operationName)
    {
        serviceIndex = 0;
        GB_SystemResult openResult = EnsureOpenLocked(operationName);
        if (openResult.IsFailed())
        {
            return openResult;
        }
        if (!service.deviceInterfacePath.empty() && !AreDeviceInterfacePathsEqual(service.deviceInterfacePath, deviceInterfacePath))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"service.deviceInterfacePath 与当前 GATT 会话设备接口路径不一致。");
        }
        if (service.serviceInterfacePath.empty() || ContainsNullCharacter(service.serviceInterfacePath))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"service.serviceInterfacePath 不能为空且不能包含空字符；请使用当前模块枚举得到的服务信息。");
        }
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

        GB_SystemResult ensureResult = EnsureServicesLoadedLocked(operationName);
        if (ensureResult.IsFailed())
        {
            return ensureResult;
        }

        for (size_t index = 0; index < cachedServices.size(); index++)
        {
            if (AreDeviceInterfacePathsEqual(cachedServices[index].serviceInfo.serviceInterfacePath, service.serviceInterfacePath) && cachedServices[index].nativeService.AttributeHandle == service.attributeHandle && AreBthLeUuidsEqual(cachedServices[index].nativeService.ServiceUuid, expectedUuid))
            {
                serviceIndex = index;
                return GB_SystemResult::Succeeded(operationName);
            }
        }

        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"未在当前 GATT 会话缓存中找到指定服务；设备服务层次或服务接口路径可能已经变化，请调用 RefreshCache() 后重新枚举。");
    }

    GB_SystemResult EnsureServiceWriteAccessLocked(CachedService& cachedService, const std::string& operationName)
    {
        if (cachedService.serviceHandleHasWriteAccess && cachedService.serviceHandle.IsValid())
        {
            return GB_SystemResult::Succeeded(operationName);
        }
        if (cachedService.serviceInfo.serviceInterfacePath.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"目标 GATT 服务缺少设备接口路径，无法重新申请写权限。");
        }

        GenericHandleScope writeHandle;
        bool openedWithWriteAccess = false;
        GB_SystemResult openResult = OpenBluetoothLeInterfaceHandle(cachedService.serviceInfo.serviceInterfacePath, BluetoothLeInterfaceHandleAccess::ReadWrite, false, writeHandle, operationName, &openedWithWriteAccess);
        if (openResult.IsFailed())
        {
            return openResult.WithMessage(u8"目标 GATT 服务接口无法以读写权限打开，无法写入特征值。");
        }
        if (!openedWithWriteAccess || !writeHandle.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::PermissionDenied, operationName, u8"目标 GATT 服务接口未获得有效写访问权限。");
        }

        bool compatible = false;
        GB_SystemResult probeResult = ProbeGattServiceHierarchy(writeHandle.Get(), cachedService.nativeService, compatible, operationName);
        if (probeResult.IsFailed())
        {
            return probeResult.WithMessage(u8"重新以读写权限打开 GATT 服务接口后，验证服务层级关系失败。");
        }
        if (!compatible)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"重新以读写权限打开的 GATT 服务接口与当前缓存服务不匹配，请调用 RefreshCache() 后重新枚举。");
        }

        cachedService.serviceHandle = std::move(writeHandle);
        cachedService.serviceHandleHasWriteAccess = true;
        return GB_SystemResult::Succeeded(operationName);
    }

    GB_SystemResult EnsureCharacteristicsLoadedLocked(CachedService& cachedService, const std::string& operationName)
    {
        if (cachedService.characteristicsLoaded)
        {
            return GB_SystemResult::Succeeded(operationName);
        }
        if (!cachedService.serviceHandle.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"BLE GATT 服务接口句柄无效。");
        }

        std::vector<BTH_LE_GATT_CHARACTERISTIC> nativeCharacteristics;
        GB_SystemResult characteristicResult = ReadNativeGattCharacteristics(cachedService.serviceHandle.Get(), cachedService.nativeService, nativeCharacteristics, operationName);
        if (characteristicResult.IsFailed())
        {
            return characteristicResult;
        }

        std::vector<GB_BluetoothGattCharacteristicInfo> characteristicInfos;
        try
        {
            characteristicInfos.reserve(nativeCharacteristics.size());
            for (size_t index = 0; index < nativeCharacteristics.size(); index++)
            {
                if (nativeCharacteristics[index].ServiceHandle != cachedService.nativeService.AttributeHandle)
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::OperationFailed, operationName, u8"BluetoothGATTGetCharacteristics 返回了不属于目标服务的特征句柄。");
                }

                characteristicInfos.push_back(ConvertGattCharacteristicInfo(nativeCharacteristics[index], cachedService.serviceInfo, deviceInterfacePath));
            }
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"构建 BLE GATT 特征会话缓存时内存不足。");
        }

        cachedService.nativeCharacteristics.swap(nativeCharacteristics);
        cachedService.characteristicInfos.swap(characteristicInfos);
        cachedService.characteristicsLoaded = true;
        return GB_SystemResult::Succeeded(operationName);
    }

    GB_SystemResult FindCharacteristicLocked(const GB_BluetoothGattCharacteristicInfo& characteristic, size_t& serviceIndex, size_t& characteristicIndex, const std::string& operationName)
    {
        serviceIndex = 0;
        characteristicIndex = 0;
        GB_SystemResult openResult = EnsureOpenLocked(operationName);
        if (openResult.IsFailed())
        {
            return openResult;
        }
        if (!characteristic.deviceInterfacePath.empty() && !AreDeviceInterfacePathsEqual(characteristic.deviceInterfacePath, deviceInterfacePath))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"characteristic.deviceInterfacePath 与当前 GATT 会话设备接口路径不一致。");
        }
        if (characteristic.serviceInterfacePath.empty() || ContainsNullCharacter(characteristic.serviceInterfacePath))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"characteristic.serviceInterfacePath 不能为空且不能包含空字符；请使用当前模块枚举得到的特征信息。");
        }
        if (characteristic.serviceAttributeHandle == 0 || characteristic.attributeHandle == 0 || characteristic.characteristicValueHandle == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"BLE GATT 特征句柄信息不完整。");
        }

        BTH_LE_UUID expectedServiceUuid = BTH_LE_UUID();
        GB_SystemResult serviceUuidResult = BuildNativeBthLeUuid(characteristic.serviceUuid, characteristic.isServiceShortUuid, characteristic.serviceShortUuid, expectedServiceUuid, operationName);
        if (serviceUuidResult.IsFailed())
        {
            return serviceUuidResult;
        }

        BTH_LE_UUID expectedCharacteristicUuid = BTH_LE_UUID();
        GB_SystemResult characteristicUuidResult = BuildNativeBthLeUuid(characteristic.characteristicUuid, characteristic.isCharacteristicShortUuid, characteristic.characteristicShortUuid, expectedCharacteristicUuid, operationName);
        if (characteristicUuidResult.IsFailed())
        {
            return characteristicUuidResult;
        }

        GB_SystemResult ensureResult = EnsureServicesLoadedLocked(operationName);
        if (ensureResult.IsFailed())
        {
            return ensureResult;
        }

        bool serviceFound = false;
        for (size_t index = 0; index < cachedServices.size(); index++)
        {
            if (AreDeviceInterfacePathsEqual(cachedServices[index].serviceInfo.serviceInterfacePath, characteristic.serviceInterfacePath) && cachedServices[index].nativeService.AttributeHandle == characteristic.serviceAttributeHandle && AreBthLeUuidsEqual(cachedServices[index].nativeService.ServiceUuid, expectedServiceUuid))
            {
                serviceIndex = index;
                serviceFound = true;
                break;
            }
        }
        if (!serviceFound)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"未在当前 GATT 会话缓存中找到特征所属服务；请调用 RefreshCache() 后重新枚举。");
        }

        CachedService& cachedService = cachedServices[serviceIndex];
        GB_SystemResult characteristicResult = EnsureCharacteristicsLoadedLocked(cachedService, operationName);
        if (characteristicResult.IsFailed())
        {
            return characteristicResult;
        }

        for (size_t index = 0; index < cachedService.nativeCharacteristics.size(); index++)
        {
            const BTH_LE_GATT_CHARACTERISTIC& nativeCharacteristic = cachedService.nativeCharacteristics[index];
            if (nativeCharacteristic.ServiceHandle == characteristic.serviceAttributeHandle && nativeCharacteristic.AttributeHandle == characteristic.attributeHandle && nativeCharacteristic.CharacteristicValueHandle == characteristic.characteristicValueHandle && AreBthLeUuidsEqual(nativeCharacteristic.CharacteristicUuid, expectedCharacteristicUuid))
            {
                characteristicIndex = index;
                return GB_SystemResult::Succeeded(operationName);
            }
        }

        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"未在当前 GATT 会话缓存中找到指定特征；请调用 RefreshCache() 后重新枚举。");
    }
#endif

    void ClearLocked() noexcept
    {
#if defined(_WIN32)
        cachedServices.clear();
        deviceHandle.Reset(INVALID_HANDLE_VALUE);
        deviceIdentity.interfacePath.clear();
        deviceIdentity.deviceInstanceId.clear();
        deviceIdentity.parentInstanceId.clear();
        deviceIdentity.containerId.clear();
        deviceIdentity.address.clear();
        servicesLoaded = false;
#endif
        deviceInterfacePath.clear();
        accessMode = GB_BluetoothGattSessionAccessMode::ReadOnly;
        isOpen = false;
        writeEnabled = false;
    }

private:
    mutable std::mutex stateMutex;
    std::string deviceInterfacePath;
    GB_BluetoothGattSessionAccessMode accessMode = GB_BluetoothGattSessionAccessMode::ReadOnly;
    bool isOpen = false;
    bool writeEnabled = false;
#if defined(_WIN32)
    GenericHandleScope deviceHandle;
    BluetoothInterfaceIdentity deviceIdentity;
    bool servicesLoaded = false;
    std::vector<CachedService> cachedServices;
#endif
};

GB_BluetoothGattSession::GB_BluetoothGattSession() : impl(new (std::nothrow) Impl())
{
}

GB_BluetoothGattSession::~GB_BluetoothGattSession() noexcept = default;

GB_BluetoothGattSession::GB_BluetoothGattSession(GB_BluetoothGattSession&& other) noexcept : impl(std::move(other.impl))
{
}

GB_BluetoothGattSession& GB_BluetoothGattSession::operator=(GB_BluetoothGattSession&& other) noexcept
{
    if (this != &other)
    {
        impl = std::move(other.impl);
    }

    return *this;
}

GB_SystemResult GB_BluetoothGattSession::Open(const std::string& deviceInterfacePath, const GB_BluetoothGattSessionAccessMode accessMode)
{
    if (!impl)
    {
        impl.reset(new (std::nothrow) Impl());
        if (!impl)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_BluetoothGattSession::Open", u8"分配 BLE GATT 会话内部状态失败。");
        }
    }

    return impl->Open(deviceInterfacePath, accessMode);
}

GB_SystemResult GB_BluetoothGattSession::Close()
{
    return impl ? impl->Close() : GB_SystemResult::Succeeded(u8"GB_BluetoothGattSession::Close");
}

bool GB_BluetoothGattSession::IsOpen() const
{
    return impl && impl->IsOpen();
}

bool GB_BluetoothGattSession::IsWriteEnabled() const
{
    return impl && impl->IsWriteEnabled();
}

std::string GB_BluetoothGattSession::GetDeviceInterfacePath() const
{
    return impl ? impl->GetDeviceInterfacePath() : std::string();
}

GB_SystemResult GB_BluetoothGattSession::RefreshCache()
{
    return impl ? impl->RefreshCache() : GB_SystemResult::Failed(GB_SystemErrorCode::NotInitialized, u8"GB_BluetoothGattSession::RefreshCache", u8"BLE GATT 会话内部状态为空。");
}

GB_SystemResult GB_BluetoothGattSession::GetServices(std::vector<GB_BluetoothGattServiceInfo>& services)
{
    services.clear();
    return impl ? impl->GetServices(services) : GB_SystemResult::Failed(GB_SystemErrorCode::NotInitialized, u8"GB_BluetoothGattSession::GetServices", u8"BLE GATT 会话内部状态为空。");
}

GB_SystemResult GB_BluetoothGattSession::GetCharacteristics(const GB_BluetoothGattServiceInfo& service, std::vector<GB_BluetoothGattCharacteristicInfo>& characteristics)
{
    characteristics.clear();
    return impl ? impl->GetCharacteristics(service, characteristics) : GB_SystemResult::Failed(GB_SystemErrorCode::NotInitialized, u8"GB_BluetoothGattSession::GetCharacteristics", u8"BLE GATT 会话内部状态为空。");
}

GB_SystemResult GB_BluetoothGattSession::ReadCharacteristic(const GB_BluetoothGattCharacteristicInfo& characteristic, std::vector<uint8_t>& value, const GB_BluetoothGattReadOptions& options)
{
    value.clear();
    return impl ? impl->ReadCharacteristic(characteristic, value, options) : GB_SystemResult::Failed(GB_SystemErrorCode::NotInitialized, u8"GB_BluetoothGattSession::ReadCharacteristic", u8"BLE GATT 会话内部状态为空。");
}

GB_SystemResult GB_BluetoothGattSession::WriteCharacteristic(const GB_BluetoothGattCharacteristicInfo& characteristic, const std::vector<uint8_t>& value, const GB_BluetoothGattWriteOptions& options)
{
    return impl ? impl->WriteCharacteristic(characteristic, value, options) : GB_SystemResult::Failed(GB_SystemErrorCode::NotInitialized, u8"GB_BluetoothGattSession::WriteCharacteristic", u8"BLE GATT 会话内部状态为空。");
}

bool GB_BluetoothGattSession::IsValidAccessModeValue(const uint64_t accessModeValue)
{
    switch (accessModeValue)
    {
    case static_cast<uint64_t>(GB_BluetoothGattSessionAccessMode::ReadOnly):
    case static_cast<uint64_t>(GB_BluetoothGattSessionAccessMode::ReadWrite):
        return true;
    default:
        break;
    }

    return false;
}

std::string GB_BluetoothGattSession::GetAccessModeName(const GB_BluetoothGattSessionAccessMode accessMode)
{
    switch (accessMode)
    {
    case GB_BluetoothGattSessionAccessMode::ReadOnly:
        return "ReadOnly";
    case GB_BluetoothGattSessionAccessMode::ReadWrite:
        return "ReadWrite";
    default:
        break;
    }

    return "Invalid";
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

    std::sort(radios.begin(), radios.end(), [](const GB_BluetoothRadioInfo& leftRadio, const GB_BluetoothRadioInfo& rightRadio)
        {
            if (leftRadio.address != rightRadio.address)
            {
                return leftRadio.address < rightRadio.address;
            }
            return leftRadio.name < rightRadio.name;
        });
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

    std::sort(devices.begin(), devices.end(), [](const GB_BluetoothDeviceInfo& leftDevice, const GB_BluetoothDeviceInfo& rightDevice)
        {
            if (leftDevice.radioAddress != rightDevice.radioAddress)
            {
                return leftDevice.radioAddress < rightDevice.radioAddress;
            }
            if (leftDevice.address != rightDevice.address)
            {
                return leftDevice.address < rightDevice.address;
            }
            return leftDevice.name < rightDevice.name;
        });
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

    std::unordered_set<std::string, AsciiNoCaseHash, AsciiNoCaseEqual> seenInterfacePaths;
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
            isNewInterface = seenInterfacePaths.insert(deviceInterfaces[index].interfacePath).second;
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

    std::sort(devices.begin(), devices.end(), [](const GB_BluetoothDeviceInfo& leftDevice, const GB_BluetoothDeviceInfo& rightDevice)
        {
            if (leftDevice.deviceId != rightDevice.deviceId)
            {
                return leftDevice.deviceId < rightDevice.deviceId;
            }
            return leftDevice.deviceInterfacePath < rightDevice.deviceInterfacePath;
        });
    return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::GetLowEnergyDevices");
#endif
}

GB_SystemResult GB_SystemBluetooth::GetGattServices(const std::string& deviceInterfacePath, std::vector<GB_BluetoothGattServiceInfo>& services)
{
    services.clear();
    GB_BluetoothGattSession session;
    GB_SystemResult openResult = session.Open(deviceInterfacePath, GB_BluetoothGattSessionAccessMode::ReadOnly);
    if (openResult.IsFailed())
    {
        return openResult.WithOperationName(u8"GB_SystemBluetooth::GetGattServices");
    }

    return session.GetServices(services).WithOperationName(u8"GB_SystemBluetooth::GetGattServices");
}

GB_SystemResult GB_SystemBluetooth::GetGattCharacteristics(const std::string& deviceInterfacePath, const GB_BluetoothGattServiceInfo& service, std::vector<GB_BluetoothGattCharacteristicInfo>& characteristics)
{
    characteristics.clear();
    const std::string& targetDeviceInterfacePath = !deviceInterfacePath.empty() ? deviceInterfacePath : service.deviceInterfacePath;
    if (!deviceInterfacePath.empty() && !service.deviceInterfacePath.empty() && !EqualsAsciiNoCase(deviceInterfacePath, service.deviceInterfacePath))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::GetGattCharacteristics", u8"deviceInterfacePath 与 service.deviceInterfacePath 不一致。");
    }

    GB_BluetoothGattSession session;
    GB_SystemResult openResult = session.Open(targetDeviceInterfacePath, GB_BluetoothGattSessionAccessMode::ReadOnly);
    if (openResult.IsFailed())
    {
        return openResult.WithOperationName(u8"GB_SystemBluetooth::GetGattCharacteristics");
    }

    return session.GetCharacteristics(service, characteristics).WithOperationName(u8"GB_SystemBluetooth::GetGattCharacteristics");
}

GB_SystemResult GB_SystemBluetooth::ReadGattCharacteristic(const std::string& deviceInterfacePath, const GB_BluetoothGattCharacteristicInfo& characteristic, std::vector<uint8_t>& value, const GB_BluetoothGattReadOptions& options)
{
    value.clear();
    const std::string& targetDeviceInterfacePath = !deviceInterfacePath.empty() ? deviceInterfacePath : characteristic.deviceInterfacePath;
    if (!deviceInterfacePath.empty() && !characteristic.deviceInterfacePath.empty() && !EqualsAsciiNoCase(deviceInterfacePath, characteristic.deviceInterfacePath))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::ReadGattCharacteristic", u8"deviceInterfacePath 与 characteristic.deviceInterfacePath 不一致。");
    }

    GB_BluetoothGattSession session;
    GB_SystemResult openResult = session.Open(targetDeviceInterfacePath, GB_BluetoothGattSessionAccessMode::ReadOnly);
    if (openResult.IsFailed())
    {
        return openResult.WithOperationName(u8"GB_SystemBluetooth::ReadGattCharacteristic");
    }

    return session.ReadCharacteristic(characteristic, value, options).WithOperationName(u8"GB_SystemBluetooth::ReadGattCharacteristic");
}

GB_SystemResult GB_SystemBluetooth::WriteGattCharacteristic(const std::string& deviceInterfacePath, const GB_BluetoothGattCharacteristicInfo& characteristic, const std::vector<uint8_t>& value, const GB_BluetoothGattWriteOptions& options)
{
    const std::string& targetDeviceInterfacePath = !deviceInterfacePath.empty() ? deviceInterfacePath : characteristic.deviceInterfacePath;
    if (!deviceInterfacePath.empty() && !characteristic.deviceInterfacePath.empty() && !EqualsAsciiNoCase(deviceInterfacePath, characteristic.deviceInterfacePath))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::WriteGattCharacteristic", u8"deviceInterfacePath 与 characteristic.deviceInterfacePath 不一致。");
    }

    GB_BluetoothGattSession session;
    GB_SystemResult openResult = session.Open(targetDeviceInterfacePath, GB_BluetoothGattSessionAccessMode::ReadWrite);
    if (openResult.IsFailed())
    {
        return openResult.WithOperationName(u8"GB_SystemBluetooth::WriteGattCharacteristic");
    }

    return session.WriteCharacteristic(characteristic, value, options).WithOperationName(u8"GB_SystemBluetooth::WriteGattCharacteristic");
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
    const GB_BluetoothClassicDeviceQueryOptions lookupOptions = options;

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

    GB_BluetoothClassicDeviceQueryOptions lookupOptions = MakeLookupOptions(false);
    lookupOptions.radioAddress = deviceId.radioAddress;

    GB_BluetoothDeviceInfo device;
    bool found = false;
    GB_SystemResult lookupResult = GetClassicDeviceByAddress(address, device, found, lookupOptions);
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
    if (!IsValidAuthenticationRequirementValue(static_cast<uint64_t>(options.authenticationRequirement)))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::PairDevice", u8"authenticationRequirement 不是有效的 GB_BluetoothAuthenticationRequirement 值。");
    }

    std::string targetRadioAddress;
    if (!deviceId.radioAddress.empty())
    {
        targetRadioAddress = NormalizeBluetoothAddress(deviceId.radioAddress);
        if (targetRadioAddress.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::PairDevice", u8"deviceId.radioAddress 不是有效蓝牙地址。");
        }
    }

    if (!GB_IsUtf8(options.pinCodeUtf8) || ContainsNullCharacter(options.pinCodeUtf8))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::PairDevice", u8"pinCodeUtf8 必须是合法 UTF-8 且不能包含空字符。");
    }

    std::array<wchar_t, BLUETOOTH_MAX_PASSKEY_SIZE + 1> pinCodeWide = {};
    SensitiveWideBufferScope<BLUETOOTH_MAX_PASSKEY_SIZE + 1> pinCodeScope(pinCodeWide);
    ULONG pinCodeLength = 0;
    if (!options.pinCodeUtf8.empty())
    {
        if (options.pinCodeUtf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::PairDevice", u8"pinCodeUtf8 过长。");
        }

        const int inputLength = static_cast<int>(options.pinCodeUtf8.size());
        const int requiredLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, options.pinCodeUtf8.data(), inputLength, nullptr, 0);
        if (requiredLength <= 0)
        {
            return GB_SystemResult::FromLastWin32Error(u8"GB_SystemBluetooth::PairDevice", u8"pinCodeUtf8 转换为 UTF-16 失败。");
        }
        if (requiredLength > BLUETOOTH_MAX_PASSKEY_SIZE)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::PairDevice", u8"PIN 长度不能超过 BLUETOOTH_MAX_PASSKEY_SIZE 个 UTF-16 字符。");
        }

        const int convertedLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, options.pinCodeUtf8.data(), inputLength, pinCodeWide.data(), requiredLength);
        if (convertedLength != requiredLength)
        {
            return GB_SystemResult::FromLastWin32Error(u8"GB_SystemBluetooth::PairDevice", u8"pinCodeUtf8 转换为 UTF-16 失败。");
        }
        pinCodeLength = static_cast<ULONG>(convertedLength);
    }
    if (!options.allowSystemPairingUi && pinCodeLength == 0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::PairDevice", u8"不允许系统配对 UI 时必须提供 pinCodeUtf8。");
    }
    if (options.parentWindowHandle != nullptr && ::IsWindow(static_cast<HWND>(options.parentWindowHandle)) == FALSE)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetooth::PairDevice", u8"parentWindowHandle 不是有效 HWND。");
    }

    const std::lock_guard<std::mutex> deviceMutationLock(GetClassicBluetoothDeviceMutationMutex(address));

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

    std::vector<BluetoothRadioHandleEntry*> targetRadioHandles;
    try
    {
        targetRadioHandles.reserve(radioHandles.size());
        for (size_t index = 0; index < radioHandles.size(); index++)
        {
            if (targetRadioAddress.empty() || radioHandles[index].GetInfo().address == targetRadioAddress)
            {
                targetRadioHandles.push_back(&radioHandles[index]);
            }
        }
    }
    catch (...)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetooth::PairDevice", u8"保存配对候选蓝牙无线电时内存不足。");
    }
    if (targetRadioHandles.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_SystemBluetooth::PairDevice", u8"未找到 deviceId.radioAddress 对应的本机蓝牙无线电。");
    }

    BLUETOOTH_DEVICE_INFO nativeDeviceInfo = {};
    HANDLE matchedRadioHandle = nullptr;
    bool matchedDevice = false;

    const GB_BluetoothClassicDeviceQueryOptions cachedLookupOptions = MakeLookupOptions(false);
    for (size_t index = 0; index < targetRadioHandles.size(); index++)
    {
        bool currentFound = false;
        GB_SystemResult findResult = FindNativeClassicDeviceForRadio(targetRadioHandles[index]->GetHandle(), targetRadioHandles[index]->GetInfo(), cachedLookupOptions, address, nativeDeviceInfo, nullptr, currentFound);
        if (findResult.IsFailed())
        {
            return findResult.WithOperationName(u8"GB_SystemBluetooth::PairDevice");
        }
        if (currentFound)
        {
            matchedRadioHandle = targetRadioHandles[index]->GetHandle();
            matchedDevice = true;
            break;
        }
    }

    if (!matchedDevice)
    {
        const GB_BluetoothClassicDeviceQueryOptions freshLookupOptions = MakeLookupOptions(true);
        if (!targetRadioAddress.empty())
        {
            bool currentFound = false;
            GB_SystemResult findResult = FindNativeClassicDeviceForRadio(targetRadioHandles[0]->GetHandle(), targetRadioHandles[0]->GetInfo(), freshLookupOptions, address, nativeDeviceInfo, nullptr, currentFound);
            if (findResult.IsFailed())
            {
                return findResult.WithOperationName(u8"GB_SystemBluetooth::PairDevice");
            }
            if (currentFound)
            {
                matchedRadioHandle = targetRadioHandles[0]->GetHandle();
                matchedDevice = true;
            }
        }
        else
        {
            GB_SystemResult findResult = FindNativeClassicDeviceAcrossRadios(freshLookupOptions, address, nativeDeviceInfo, matchedDevice);
            if (findResult.IsFailed())
            {
                return findResult.WithOperationName(u8"GB_SystemBluetooth::PairDevice");
            }
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

    const HWND parentWindowHandle = static_cast<HWND>(options.parentWindowHandle);
    DWORD pairResult = ERROR_SUCCESS;
    if (pinCodeLength > 0)
    {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4995)
#endif
        pairResult = ::BluetoothAuthenticateDevice(parentWindowHandle, matchedRadioHandle, &nativeDeviceInfo, pinCodeWide.data(), pinCodeLength);
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

    pairResult = ::BluetoothAuthenticateDeviceEx(parentWindowHandle, matchedRadioHandle, &nativeDeviceInfo, nullptr, ToNativeAuthenticationRequirement(options.authenticationRequirement));
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

    const std::lock_guard<std::mutex> deviceMutationLock(GetClassicBluetoothDeviceMutationMutex(address));

    BLUETOOTH_ADDRESS nativeAddress = MakeBluetoothAddress(addressValue);
    const DWORD removeResult = ::BluetoothRemoveDevice(&nativeAddress);
    if (removeResult == ERROR_SUCCESS)
    {
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::RemoveDevice", u8"经典蓝牙设备配对记录及缓存服务信息已移除。");
    }
    if (removeResult == ERROR_NOT_FOUND)
    {
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetooth::RemoveDevice", u8"经典蓝牙设备已不在系统记忆列表中，无需重复移除。");
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

bool GB_SystemBluetooth::IsValidAuthenticationRequirementValue(const uint64_t authenticationRequirementValue)
{
    switch (authenticationRequirementValue)
    {
    case static_cast<uint64_t>(GB_BluetoothAuthenticationRequirement::MitmProtectionNotRequired):
    case static_cast<uint64_t>(GB_BluetoothAuthenticationRequirement::MitmProtectionRequired):
    case static_cast<uint64_t>(GB_BluetoothAuthenticationRequirement::MitmProtectionNotRequiredBonding):
    case static_cast<uint64_t>(GB_BluetoothAuthenticationRequirement::MitmProtectionRequiredBonding):
    case static_cast<uint64_t>(GB_BluetoothAuthenticationRequirement::MitmProtectionNotRequiredGeneralBonding):
    case static_cast<uint64_t>(GB_BluetoothAuthenticationRequirement::MitmProtectionRequiredGeneralBonding):
        return true;
    default:
        break;
    }

    return false;
}

std::string GB_SystemBluetooth::GetAuthenticationRequirementName(const GB_BluetoothAuthenticationRequirement authenticationRequirement)
{
    switch (authenticationRequirement)
    {
    case GB_BluetoothAuthenticationRequirement::MitmProtectionNotRequired:
        return "MitmProtectionNotRequired";
    case GB_BluetoothAuthenticationRequirement::MitmProtectionRequired:
        return "MitmProtectionRequired";
    case GB_BluetoothAuthenticationRequirement::MitmProtectionNotRequiredBonding:
        return "MitmProtectionNotRequiredBonding";
    case GB_BluetoothAuthenticationRequirement::MitmProtectionRequiredBonding:
        return "MitmProtectionRequiredBonding";
    case GB_BluetoothAuthenticationRequirement::MitmProtectionNotRequiredGeneralBonding:
        return "MitmProtectionNotRequiredGeneralBonding";
    case GB_BluetoothAuthenticationRequirement::MitmProtectionRequiredGeneralBonding:
        return "MitmProtectionRequiredGeneralBonding";
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
    explicit Impl(const GB_SystemBluetoothWatcherOptions& inputOptions) : options(NormalizeOptions(inputOptions)), acceptingDeviceEvents(false), internalDroppedEventCount(0), eventDispatcher(GB_EventDispatcher::MakeQueuedOptions(options.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropOldest, u8"GB_SystemBluetoothWatcher"))
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

        try
        {
            result = eventDispatcher.Start();
            if (result.IsFailed())
            {
                SetLifecycleState(LifecycleState::Stopped);
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
                return RollbackStartFailure(std::move(result));
            }
        }
        catch (const std::bad_alloc&)
        {
            return RollbackStartFailure(GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetoothWatcher::Start", u8"启动蓝牙监听器时内存不足。"));
        }
        catch (...)
        {
            return RollbackStartFailure(GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemBluetoothWatcher::Start", u8"启动蓝牙监听器时发生内部错误。"));
        }

        SetLifecycleState(LifecycleState::Running);
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::Start");
    }

    GB_SystemResult Stop()
    {
        if (IsCurrentThreadExecutingBluetoothCallback())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemBluetoothWatcher::Stop", u8"不能在蓝牙事件回调内部停止监听器；请让回调返回后从其它线程或外层控制流调用 Stop()。");
        }

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

    GB_SystemResult Subscribe(const GB_BluetoothEventType eventType, const GB_SystemBluetoothWatcher::BluetoothEventCallback& callback, GB_EventSubscriptionToken& subscriptionToken)
    {
        if (!IsValidBluetoothEventType(eventType) || eventType == GB_BluetoothEventType::Unknown)
        {
            subscriptionToken.Reset();
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetoothWatcher::Subscribe", u8"eventType 必须是有效且非 Unknown 的蓝牙事件类型。");
        }

        try
        {
            const std::string eventName = GetBluetoothEventName(eventType);
            return SubscribeInternal(eventName, callback, subscriptionToken, u8"GB_SystemBluetoothWatcher::Subscribe");
        }
        catch (const std::bad_alloc&)
        {
            subscriptionToken.Reset();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemBluetoothWatcher::Subscribe", u8"构建蓝牙事件订阅名称时内存不足。");
        }
        catch (...)
        {
            subscriptionToken.Reset();
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemBluetoothWatcher::Subscribe", u8"构建蓝牙事件订阅名称时发生内部错误。");
        }
    }

    GB_SystemResult SubscribeAll(const GB_SystemBluetoothWatcher::BluetoothEventCallback& callback, GB_EventSubscriptionToken& subscriptionToken)
    {
        return SubscribeInternal(std::string(), callback, subscriptionToken, u8"GB_SystemBluetoothWatcher::SubscribeAll");
    }

    GB_SystemResult Unsubscribe(const GB_EventSubscriptionToken& subscriptionToken)
    {
        if (!subscriptionToken.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemBluetoothWatcher::Unsubscribe", u8"订阅 token 无效。");
        }

        std::lock_guard<std::mutex> lock(subscriptionMutex);
        size_t tokenIndex = externalSubscriptionTokens.size();
        for (size_t index = 0; index < externalSubscriptionTokens.size(); index++)
        {
            if (externalSubscriptionTokens[index] == subscriptionToken)
            {
                tokenIndex = index;
                break;
            }
        }
        if (tokenIndex == externalSubscriptionTokens.size())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, u8"GB_SystemBluetoothWatcher::Unsubscribe", u8"指定 token 不是当前监听器创建的外部蓝牙事件订阅。");
        }

        GB_SystemResult unsubscribeResult = eventDispatcher.Unsubscribe(subscriptionToken);
        if (unsubscribeResult.IsFailed() && unsubscribeResult.errorCode != GB_SystemErrorCode::NotFound)
        {
            return unsubscribeResult.WithOperationName(u8"GB_SystemBluetoothWatcher::Unsubscribe");
        }

        externalSubscriptionTokens.erase(externalSubscriptionTokens.begin() + static_cast<std::ptrdiff_t>(tokenIndex));
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::Unsubscribe", u8"已取消蓝牙事件订阅。");
    }

    GB_SystemResult ClearSubscriptions()
    {
        std::lock_guard<std::mutex> lock(subscriptionMutex);
        GB_SystemResult firstFailure = GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::ClearSubscriptions");
        size_t retainedTokenCount = 0;
        for (size_t index = 0; index < externalSubscriptionTokens.size(); index++)
        {
            GB_SystemResult unsubscribeResult = eventDispatcher.Unsubscribe(externalSubscriptionTokens[index]);
            if (unsubscribeResult.IsFailed() && unsubscribeResult.errorCode != GB_SystemErrorCode::NotFound)
            {
                if (firstFailure.IsSucceeded())
                {
                    firstFailure = std::move(unsubscribeResult);
                }
                if (retainedTokenCount != index)
                {
                    externalSubscriptionTokens[retainedTokenCount] = externalSubscriptionTokens[index];
                }
                retainedTokenCount++;
            }
        }
        externalSubscriptionTokens.resize(retainedTokenCount);

        if (firstFailure.IsFailed())
        {
            return firstFailure.WithOperationName(u8"GB_SystemBluetoothWatcher::ClearSubscriptions");
        }
        return GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::ClearSubscriptions", u8"已清除全部外部蓝牙事件订阅。");
    }

    size_t GetSubscriptionCount() const
    {
        std::lock_guard<std::mutex> lock(subscriptionMutex);
        return externalSubscriptionTokens.size();
    }

    size_t GetPendingEventCount() const
    {
        return eventDispatcher.GetPendingEventCount();
    }

    uint64_t GetDispatchedEventCount() const
    {
        return eventDispatcher.GetDispatchedEventCount();
    }

    uint64_t GetDroppedEventCount() const
    {
        const uint64_t dispatcherDroppedEventCount = eventDispatcher.GetDroppedEventCount();
        const uint64_t localDroppedEventCount = internalDroppedEventCount.load(std::memory_order_relaxed);
        return AddSaturating(dispatcherDroppedEventCount, localDroppedEventCount);
    }

    uint64_t GetCallbackExceptionCount() const
    {
        return eventDispatcher.GetCallbackExceptionCount();
    }

private:
    GB_SystemResult RollbackStartFailure(GB_SystemResult failureResult)
    {
        acceptingDeviceEvents.store(false, std::memory_order_release);
        ClearDeviceWatcherCallbackSilently();

        GB_SystemResult deviceStopResult = deviceWatcher.Stop();
        GB_SystemResult dispatcherStopResult = eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
        const bool cleanupFailed = deviceStopResult.IsFailed() || dispatcherStopResult.IsFailed();
        SetLifecycleState(cleanupFailed ? LifecycleState::StopFailed : LifecycleState::Stopped);

        try
        {
            if (deviceStopResult.IsFailed())
            {
                failureResult.message += u8" 启动回滚期间停止底层设备监听器失败：";
                failureResult.message += deviceStopResult.GetDisplayMessage();
            }
            if (dispatcherStopResult.IsFailed())
            {
                failureResult.message += u8" 启动回滚期间停止事件分发器失败：";
                failureResult.message += dispatcherStopResult.GetDisplayMessage();
            }
        }
        catch (...)
        {
        }

        return failureResult.WithOperationName(u8"GB_SystemBluetoothWatcher::Start");
    }

    GB_SystemResult SubscribeInternal(const std::string& eventName, const GB_SystemBluetoothWatcher::BluetoothEventCallback& callback, GB_EventSubscriptionToken& subscriptionToken, const std::string& operationName)
    {
        subscriptionToken.Reset();
        if (!callback)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"蓝牙事件订阅回调不能为空。");
        }

        GB_EventDispatcher::Callback eventCallback;
        try
        {
            eventCallback = [this, callback](const GB_Event& event)
                {
                    DispatchSubscribedCallback(event, callback);
                };
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"复制蓝牙事件订阅回调时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, operationName, u8"复制蓝牙事件订阅回调时发生内部错误。");
        }

        std::lock_guard<std::mutex> lock(subscriptionMutex);
        try
        {
            if (externalSubscriptionTokens.size() == (std::numeric_limits<size_t>::max)())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"蓝牙事件订阅 token 数量已经达到容器上限。");
            }
            externalSubscriptionTokens.reserve(externalSubscriptionTokens.size() + 1);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"预留蓝牙事件订阅 token 存储空间时内存不足。");
        }

        GB_SystemResult subscribeResult = eventName.empty() ? eventDispatcher.SubscribeAll(eventCallback, subscriptionToken) : eventDispatcher.Subscribe(eventName, eventCallback, subscriptionToken);
        if (subscribeResult.IsFailed())
        {
            subscriptionToken.Reset();
            return subscribeResult.WithOperationName(operationName);
        }

        externalSubscriptionTokens.push_back(subscriptionToken);
        return GB_SystemResult::Succeeded(operationName, eventName.empty() ? u8"已订阅全部蓝牙事件。" : u8"已订阅指定类型的蓝牙事件。");
    }

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

        static const char* const instancePrefixes[] =
        {
            "BTH\\",
            "BTHLE\\",
            "BTHENUM\\",
            "BTHLEDEVICE\\",
            "BTHLEENUM\\",
            "BTHHFENUM\\",
            "BTHMODEM\\",
            "Bluetooth\\",
            "SWD\\RADIO\\Bluetooth"
        };
        for (size_t index = 0; index < sizeof(instancePrefixes) / sizeof(instancePrefixes[0]); index++)
        {
            if (StartsWithAsciiNoCase(text, instancePrefixes[index]))
            {
                return true;
            }
        }

        static const char* const interfaceTokens[] =
        {
            "\\BTH\\",
            "#BTH#",
            "\\BTHLE\\",
            "#BTHLE#",
            "\\BTHENUM\\",
            "#BTHENUM#",
            "\\BTHLEDEVICE\\",
            "#BTHLEDEVICE#",
            "\\BTHLEENUM\\",
            "#BTHLEENUM#",
            "\\BTHHFENUM\\",
            "#BTHHFENUM#",
            "\\BTHMODEM\\",
            "#BTHMODEM#",
            "SWD\\RADIO\\Bluetooth"
        };
        for (size_t index = 0; index < sizeof(interfaceTokens) / sizeof(interfaceTokens[0]); index++)
        {
            if (ContainsAsciiNoCase(text, interfaceTokens[index]))
            {
                return true;
            }
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

    static bool HasBluetoothGattServicePnPIdentity(const std::string& text)
    {
        if (text.empty())
        {
            return false;
        }

        return StartsWithAsciiNoCase(text, "BTHLEDEVICE\\") || ContainsAsciiNoCase(text, "\\BTHLEDEVICE\\") || ContainsAsciiNoCase(text, "#BTHLEDEVICE#");
    }

    static bool IsBluetoothRelatedDeviceEvent(const GB_SystemDeviceEvent& deviceEvent)
    {
        if (EqualsAsciiNoCase(deviceEvent.interfaceClassGuid, GB_BluetoothClassicDeviceInterfaceGuid) || EqualsAsciiNoCase(deviceEvent.interfaceClassGuid, GB_BluetoothLeDeviceInterfaceGuid) || EqualsAsciiNoCase(deviceEvent.interfaceClassGuid, GB_BluetoothGattServiceDeviceInterfaceGuid) || EqualsAsciiNoCase(deviceEvent.interfaceClassGuid, GB_BluetoothRadioInterfaceGuid))
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
        if (EqualsAsciiNoCase(deviceEvent.interfaceClassGuid, GB_BluetoothGattServiceDeviceInterfaceGuid) || HasBluetoothGattServicePnPIdentity(deviceEvent.deviceInstanceId) || HasBluetoothGattServicePnPIdentity(deviceEvent.deviceInterfacePath))
        {
            return deviceEvent.eventType == GB_SystemDeviceEventType::Unknown ? GB_BluetoothEventType::Unknown : GB_BluetoothEventType::DeviceUpdated;
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
            BluetoothCallbackExecutionScope executionScope(*this);
            callback(*bluetoothEvent);
        }
    }

    void DispatchSubscribedCallback(const GB_Event& event, const GB_SystemBluetoothWatcher::BluetoothEventCallback& callback)
    {
        if (!callback)
        {
            return;
        }

        const GB_BluetoothEvent* bluetoothEvent = event.payload.AnyCast<GB_BluetoothEvent>();
        if (bluetoothEvent == nullptr)
        {
            return;
        }

        BluetoothCallbackExecutionScope executionScope(*this);
        callback(*bluetoothEvent);
    }

    class BluetoothCallbackExecutionScope final
    {
    public:
        explicit BluetoothCallbackExecutionScope(Impl& owner) : previousOwner(GetCurrentThreadBluetoothCallbackOwner())
        {
            GetCurrentThreadBluetoothCallbackOwner() = &owner;
        }

        ~BluetoothCallbackExecutionScope() noexcept
        {
            GetCurrentThreadBluetoothCallbackOwner() = previousOwner;
        }

        BluetoothCallbackExecutionScope(const BluetoothCallbackExecutionScope&) = delete;
        BluetoothCallbackExecutionScope& operator=(const BluetoothCallbackExecutionScope&) = delete;

    private:
        Impl* previousOwner = nullptr;
    };

    static Impl*& GetCurrentThreadBluetoothCallbackOwner() noexcept
    {
        static thread_local Impl* callbackOwner = nullptr;
        return callbackOwner;
    }

    bool IsCurrentThreadExecutingBluetoothCallback() const noexcept
    {
        return GetCurrentThreadBluetoothCallbackOwner() == this;
    }

    static uint64_t AddSaturating(const uint64_t leftValue, const uint64_t rightValue) noexcept
    {
        const uint64_t maxValue = (std::numeric_limits<uint64_t>::max)();
        return leftValue > maxValue - rightValue ? maxValue : leftValue + rightValue;
    }

    static void IncrementSaturatingCounter(std::atomic<uint64_t>& counter) noexcept
    {
        uint64_t currentValue = counter.load(std::memory_order_relaxed);
        const uint64_t maxValue = (std::numeric_limits<uint64_t>::max)();
        while (currentValue != maxValue)
        {
            if (counter.compare_exchange_weak(currentValue, currentValue + 1, std::memory_order_relaxed, std::memory_order_relaxed))
            {
                return;
            }
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

            const std::string eventName = bluetoothEvent.eventName;
            const uint64_t timestampMilliseconds = bluetoothEvent.timestampMilliseconds;
            GB_Event event(eventName, GB_Variant(std::move(bluetoothEvent)), u8"GB_SystemBluetoothWatcher");
            event.timestampMilliseconds = timestampMilliseconds;
            const GB_SystemResult postResult = eventDispatcher.Post(event);
            if (postResult.IsFailed())
            {
                IncrementSaturatingCounter(internalDroppedEventCount);
            }
        }
        catch (...)
        {
            IncrementSaturatingCounter(internalDroppedEventCount);
        }
    }

private:
    GB_SystemBluetoothWatcherOptions options;
    mutable std::mutex stateMutex;
    mutable std::mutex callbackMutex;
    mutable std::mutex subscriptionMutex;
    LifecycleState lifecycleState = LifecycleState::Stopped;
    std::atomic<bool> acceptingDeviceEvents;
    std::atomic<uint64_t> internalDroppedEventCount;
    GB_SystemDeviceWatcher deviceWatcher;
    GB_EventDispatcher eventDispatcher;
    GB_EventSubscriptionToken typedSubscriptionToken;
    std::vector<GB_EventSubscriptionToken> externalSubscriptionTokens;
    GB_SystemBluetoothWatcher::BluetoothEventCallback bluetoothEventCallback;
};

std::unique_ptr<GB_SystemBluetoothWatcher::Impl> GB_SystemBluetoothWatcher::CreateImpl(const GB_SystemBluetoothWatcherOptions& options) noexcept
{
    try
    {
        return std::unique_ptr<Impl>(new Impl(options));
    }
    catch (...)
    {
        return std::unique_ptr<Impl>();
    }
}

GB_SystemBluetoothWatcher::GB_SystemBluetoothWatcher() : impl(CreateImpl(GB_SystemBluetoothWatcherOptions()))
{
}

GB_SystemBluetoothWatcher::GB_SystemBluetoothWatcher(const GB_SystemBluetoothWatcherOptions& options) : impl(CreateImpl(options))
{
}

GB_SystemBluetoothWatcher::~GB_SystemBluetoothWatcher() noexcept = default;

bool GB_SystemBluetoothWatcher::IsValid() const
{
    return impl != nullptr;
}

GB_SystemResult GB_SystemBluetoothWatcher::Start()
{
    return impl ? impl->Start() : MakeBluetoothWatcherInitializationFailedResult(u8"GB_SystemBluetoothWatcher::Start");
}

GB_SystemResult GB_SystemBluetoothWatcher::Stop()
{
    return impl ? impl->Stop() : GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::Stop", u8"蓝牙监听器内部状态为空，无需停止。");
}

bool GB_SystemBluetoothWatcher::IsRunning() const
{
    return impl && impl->IsRunning();
}

void GB_SystemBluetoothWatcher::SetBluetoothEventCallback(const BluetoothEventCallback& callback)
{
    if (impl)
    {
        impl->SetBluetoothEventCallback(callback);
    }
}

GB_SystemResult GB_SystemBluetoothWatcher::Subscribe(const GB_BluetoothEventType eventType, const BluetoothEventCallback& callback, GB_EventSubscriptionToken& subscriptionToken)
{
    subscriptionToken.Reset();
    return impl ? impl->Subscribe(eventType, callback, subscriptionToken) : MakeBluetoothWatcherInitializationFailedResult(u8"GB_SystemBluetoothWatcher::Subscribe");
}

GB_SystemResult GB_SystemBluetoothWatcher::SubscribeAll(const BluetoothEventCallback& callback, GB_EventSubscriptionToken& subscriptionToken)
{
    subscriptionToken.Reset();
    return impl ? impl->SubscribeAll(callback, subscriptionToken) : MakeBluetoothWatcherInitializationFailedResult(u8"GB_SystemBluetoothWatcher::SubscribeAll");
}

GB_SystemResult GB_SystemBluetoothWatcher::Unsubscribe(const GB_EventSubscriptionToken& subscriptionToken)
{
    return impl ? impl->Unsubscribe(subscriptionToken) : MakeBluetoothWatcherInitializationFailedResult(u8"GB_SystemBluetoothWatcher::Unsubscribe");
}

GB_SystemResult GB_SystemBluetoothWatcher::ClearSubscriptions()
{
    return impl ? impl->ClearSubscriptions() : GB_SystemResult::Succeeded(u8"GB_SystemBluetoothWatcher::ClearSubscriptions", u8"蓝牙监听器内部状态为空，没有可清理的订阅。");
}

size_t GB_SystemBluetoothWatcher::GetSubscriptionCount() const
{
    return impl ? impl->GetSubscriptionCount() : 0;
}

size_t GB_SystemBluetoothWatcher::GetPendingEventCount() const
{
    return impl ? impl->GetPendingEventCount() : 0;
}

uint64_t GB_SystemBluetoothWatcher::GetDispatchedEventCount() const
{
    return impl ? impl->GetDispatchedEventCount() : 0;
}

uint64_t GB_SystemBluetoothWatcher::GetDroppedEventCount() const
{
    return impl ? impl->GetDroppedEventCount() : 0;
}

uint64_t GB_SystemBluetoothWatcher::GetCallbackExceptionCount() const
{
    return impl ? impl->GetCallbackExceptionCount() : 0;
}
