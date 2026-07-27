#include "GB_SystemDevice.h"
#include "../GB_Utf8String.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <initguid.h>
#include <devpkey.h>
#include <setupapi.h>
#ifdef _MSC_VER
#  pragma comment(lib, "Setupapi.lib")
#  pragma comment(lib, "Cfgmgr32.lib")
#  pragma comment(lib, "User32.lib")
#endif
#endif

namespace
{
    static std::string ToLowerAscii(std::string text)
    {
        for (size_t index = 0; index < text.size(); index++)
        {
            if (text[index] >= 'A' && text[index] <= 'Z')
            {
                text[index] = static_cast<char>(text[index] - 'A' + 'a');
            }
        }

        return text;
    }

    static bool StartsWithAsciiNoCase(const std::string& text, const std::string& prefix)
    {
        if (prefix.size() > text.size())
        {
            return false;
        }

        for (size_t index = 0; index < prefix.size(); index++)
        {
            char leftChar = text[index];
            char rightChar = prefix[index];
            if (leftChar >= 'A' && leftChar <= 'Z')
            {
                leftChar = static_cast<char>(leftChar - 'A' + 'a');
            }
            if (rightChar >= 'A' && rightChar <= 'Z')
            {
                rightChar = static_cast<char>(rightChar - 'A' + 'a');
            }
            if (leftChar != rightChar)
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

    static char ToLowerAsciiCharacter(const char character)
    {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }

    static bool ContainsAsciiNoCase(const std::string& text, const std::string& needle)
    {
        if (needle.empty())
        {
            return true;
        }

        if (needle.size() > text.size())
        {
            return false;
        }

        const std::string::const_iterator foundIter = std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](const char leftCharacter, const char rightCharacter)
            {
                return ToLowerAsciiCharacter(leftCharacter) == ToLowerAsciiCharacter(rightCharacter);
            });
        return foundIter != text.end();
    }

    static bool EqualsAnyAsciiNoCase(const std::string& text, const char* const* candidates, const size_t candidateCount)
    {
        for (size_t index = 0; index < candidateCount; index++)
        {
            if (EqualsAsciiNoCase(text, candidates[index]))
            {
                return true;
            }
        }

        return false;
    }

    static bool StartsWithAnyAsciiNoCase(const std::string& text, const char* const* prefixes, const size_t prefixCount)
    {
        for (size_t index = 0; index < prefixCount; index++)
        {
            if (StartsWithAsciiNoCase(text, prefixes[index]))
            {
                return true;
            }
        }

        return false;
    }

    static bool ContainsAnyAsciiNoCase(const std::string& text, const char* const* needles, const size_t needleCount)
    {
        for (size_t index = 0; index < needleCount; index++)
        {
            if (ContainsAsciiNoCase(text, needles[index]))
            {
                return true;
            }
        }

        return false;
    }

    static bool ContainsNullCharacter(const std::string& text)
    {
        return text.find('\0') != std::string::npos;
    }

    static bool StringListContainsAnyAsciiNoCase(const std::vector<std::string>& values, const char* const* needles, const size_t needleCount)
    {
        for (size_t valueIndex = 0; valueIndex < values.size(); valueIndex++)
        {
            if (ContainsAnyAsciiNoCase(values[valueIndex], needles, needleCount))
            {
                return true;
            }
        }

        return false;
    }

    static std::string NormalizeGuidString(const std::string& guid)
    {
        return ToLowerAscii(guid);
    }

    static bool IsValidSystemDeviceKind(const GB_SystemDeviceKind deviceKind)
    {
        switch (deviceKind)
        {
        case GB_SystemDeviceKind::Unknown:
        case GB_SystemDeviceKind::Usb:
        case GB_SystemDeviceKind::Display:
        case GB_SystemDeviceKind::Battery:
        case GB_SystemDeviceKind::Storage:
        case GB_SystemDeviceKind::Network:
        case GB_SystemDeviceKind::HumanInterface:
        case GB_SystemDeviceKind::Audio:
        case GB_SystemDeviceKind::Bluetooth:
        case GB_SystemDeviceKind::Camera:
        case GB_SystemDeviceKind::Keyboard:
        case GB_SystemDeviceKind::Mouse:
        case GB_SystemDeviceKind::Printer:
            return true;
        default:
            break;
        }

        return false;
    }

#if defined(_WIN32)
#if defined(CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES) && defined(CM_NOTIFY_FILTER_FLAG_ALL_DEVICE_INSTANCES)
#define GB_SYSTEMDEVICE_HAS_CM_NOTIFICATION 1
#else
#define GB_SYSTEMDEVICE_HAS_CM_NOTIFICATION 0
#endif

    class DeviceInfoSetScope
    {
    public:
        explicit DeviceInfoSetScope(HDEVINFO deviceInfoSet) : deviceInfoSet(deviceInfoSet)
        {
        }

        ~DeviceInfoSetScope()
        {
            if (deviceInfoSet != INVALID_HANDLE_VALUE)
            {
                (void)::SetupDiDestroyDeviceInfoList(deviceInfoSet);
                deviceInfoSet = INVALID_HANDLE_VALUE;
            }
        }

        DeviceInfoSetScope(const DeviceInfoSetScope&) = delete;
        DeviceInfoSetScope& operator=(const DeviceInfoSetScope&) = delete;

        HDEVINFO Get() const
        {
            return deviceInfoSet;
        }

        bool IsValid() const
        {
            return deviceInfoSet != INVALID_HANDLE_VALUE;
        }

    private:
        HDEVINFO deviceInfoSet = INVALID_HANDLE_VALUE;
    };

    class RegKeyScope
    {
    public:
        explicit RegKeyScope(HKEY keyHandle) : keyHandle(keyHandle)
        {
        }

        ~RegKeyScope()
        {
            if (keyHandle != nullptr && keyHandle != INVALID_HANDLE_VALUE)
            {
                (void)::RegCloseKey(keyHandle);
                keyHandle = nullptr;
            }
        }

        RegKeyScope(const RegKeyScope&) = delete;
        RegKeyScope& operator=(const RegKeyScope&) = delete;

        HKEY Get() const
        {
            return keyHandle;
        }

        bool IsValid() const
        {
            return keyHandle != nullptr && keyHandle != INVALID_HANDLE_VALUE;
        }

    private:
        HKEY keyHandle = nullptr;
    };

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

    static std::wstring Utf8ToWideSafe(const std::string& text)
    {
        try
        {
            return GB_Utf8ToWString(text);
        }
        catch (...)
        {
            return std::wstring();
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

    static int HexValue(const wchar_t character)
    {
        if (character >= L'0' && character <= L'9')
        {
            return static_cast<int>(character - L'0');
        }
        if (character >= L'a' && character <= L'f')
        {
            return static_cast<int>(character - L'a') + 10;
        }
        if (character >= L'A' && character <= L'F')
        {
            return static_cast<int>(character - L'A') + 10;
        }

        return -1;
    }

    static bool ParseHexValue(const std::wstring& text, const size_t startIndex, const size_t length, uint64_t& value)
    {
        if (startIndex + length > text.size())
        {
            return false;
        }

        value = 0;
        for (size_t index = 0; index < length; index++)
        {
            const int digit = HexValue(text[startIndex + index]);
            if (digit < 0)
            {
                return false;
            }

            value = (value << 4) | static_cast<uint64_t>(digit);
        }

        return true;
    }

    static bool TryParseGuidString(const std::string& guidText, GUID& guid)
    {
        guid = GUID();
        std::wstring text = Utf8ToWideSafe(guidText);
        if (text.empty())
        {
            return false;
        }

        if (text.size() == 38 && text[0] == L'{' && text[37] == L'}')
        {
            text = text.substr(1, 36);
        }
        if (text.size() != 36 || text[8] != L'-' || text[13] != L'-' || text[18] != L'-' || text[23] != L'-')
        {
            return false;
        }

        uint64_t value = 0;
        if (!ParseHexValue(text, 0, 8, value))
        {
            return false;
        }
        guid.Data1 = static_cast<unsigned long>(value);
        if (!ParseHexValue(text, 9, 4, value))
        {
            return false;
        }
        guid.Data2 = static_cast<unsigned short>(value);
        if (!ParseHexValue(text, 14, 4, value))
        {
            return false;
        }
        guid.Data3 = static_cast<unsigned short>(value);
        if (!ParseHexValue(text, 19, 2, value))
        {
            return false;
        }
        guid.Data4[0] = static_cast<unsigned char>(value);
        if (!ParseHexValue(text, 21, 2, value))
        {
            return false;
        }
        guid.Data4[1] = static_cast<unsigned char>(value);

        size_t textIndex = 24;
        for (size_t byteIndex = 2; byteIndex < 8; byteIndex++)
        {
            if (!ParseHexValue(text, textIndex, 2, value))
            {
                return false;
            }
            guid.Data4[byteIndex] = static_cast<unsigned char>(value);
            textIndex += 2;
        }

        return true;
    }

    static std::string CanonicalizeGuidString(const std::string& guidText)
    {
        GUID guid = {};
        if (TryParseGuidString(guidText, guid))
        {
            return NormalizeGuidString(GuidToString(guid));
        }

        return NormalizeGuidString(guidText);
    }

    static bool EqualsGuidString(const std::string& leftGuidText, const std::string& rightGuidText)
    {
        const std::string leftGuid = CanonicalizeGuidString(leftGuidText);
        const std::string rightGuid = CanonicalizeGuidString(rightGuidText);
        return !leftGuid.empty() && !rightGuid.empty() && leftGuid == rightGuid;
    }

    static bool IsOneOfClassGuid(const std::string& classGuid, const char* const* classGuids, const size_t classGuidCount)
    {
        if (classGuid.empty() || classGuids == nullptr)
        {
            return false;
        }

        const std::string normalizedClassGuid = CanonicalizeGuidString(classGuid);
        for (size_t index = 0; index < classGuidCount; index++)
        {
            if (normalizedClassGuid == NormalizeGuidString(classGuids[index]))
            {
                return true;
            }
        }

        return false;
    }

    static GB_SystemResult ConfigRetToResult(const CONFIGRET configResult, const std::string& operationName, const std::string& message)
    {
        const DWORD win32Error = ::CM_MapCrToWin32Err(configResult, ERROR_GEN_FAILURE);
        return GB_SystemResult::FromWin32Error(win32Error, operationName, message);
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

    static bool TryCheckDevicePresentByConfigManager(const std::string& instanceId, bool& isPresent)
    {
        isPresent = false;
        if (instanceId.empty() || ContainsNullCharacter(instanceId))
        {
            return false;
        }

        const std::wstring instanceIdWide = Utf8ToWideSafe(instanceId);
        if (instanceIdWide.empty())
        {
            return false;
        }

        DEVINST deviceInstance = 0;
        const CONFIGRET configResult = ::CM_Locate_DevNodeW(&deviceInstance, const_cast<DEVINSTID_W>(instanceIdWide.c_str()), CM_LOCATE_DEVNODE_NORMAL);
        if (configResult == CR_SUCCESS)
        {
            isPresent = true;
            return true;
        }
        if (IsConfigRetDeviceNotFound(configResult))
        {
            isPresent = false;
            return true;
        }

        return false;
    }

    static bool IsReasonableDevicePropertyBufferSize(const DWORD requiredSize)
    {
        static const DWORD maxPropertyBufferSize = 16u * 1024u * 1024u;
        return requiredSize > 0 && requiredSize <= maxPropertyBufferSize;
    }

    static bool IsReasonableDeviceInterfaceDetailBufferSize(const DWORD requiredSize)
    {
        static const DWORD maxInterfaceDetailBufferSize = 16u * 1024u * 1024u;
        const DWORD minInterfaceDetailBufferSize = static_cast<DWORD>(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) + sizeof(wchar_t));
        return requiredSize >= minInterfaceDetailBufferSize && requiredSize <= maxInterfaceDetailBufferSize;
    }

    template<typename TContainer>
    static bool TryReserve(TContainer& container, const size_t capacity)
    {
        try
        {
            container.reserve(capacity);
        }
        catch (...)
        {
            return false;
        }

        return true;
    }

    static bool TryAssignDeviceInterfaceDetailBuffer(const DWORD requiredSize, std::vector<BYTE>& detailBuffer)
    {
        if (!IsReasonableDeviceInterfaceDetailBufferSize(requiredSize))
        {
            detailBuffer.clear();
            return false;
        }

        try
        {
            detailBuffer.assign(static_cast<size_t>(requiredSize), 0);
        }
        catch (...)
        {
            detailBuffer.clear();
            return false;
        }

        return true;
    }

    static bool TryAssignByteBuffer(const DWORD requiredSize, std::vector<BYTE>& propertyBuffer)
    {
        if (!IsReasonableDevicePropertyBufferSize(requiredSize))
        {
            propertyBuffer.clear();
            return false;
        }

        if (static_cast<size_t>(requiredSize) > (std::numeric_limits<size_t>::max)() - sizeof(wchar_t) * 2)
        {
            propertyBuffer.clear();
            return false;
        }

        try
        {
            propertyBuffer.assign(static_cast<size_t>(requiredSize) + sizeof(wchar_t) * 2, 0);
        }
        catch (...)
        {
            propertyBuffer.clear();
            return false;
        }

        return true;
    }


    static bool TryCopyByteBufferToWideBuffer(const std::vector<BYTE>& byteBuffer, std::vector<wchar_t>& wideBuffer)
    {
        wideBuffer.clear();
        if (byteBuffer.empty())
        {
            return false;
        }

        const size_t byteCount = byteBuffer.size();
        const size_t charCount = (byteCount + sizeof(wchar_t) - 1) / sizeof(wchar_t);
        if (charCount > (std::numeric_limits<size_t>::max)() - 2)
        {
            return false;
        }

        try
        {
            wideBuffer.assign(charCount + 2, L'\0');
            std::memcpy(wideBuffer.data(), byteBuffer.data(), byteCount);
        }
        catch (...)
        {
            wideBuffer.clear();
            return false;
        }

        return true;
    }

    static bool TryConvertWideStringBufferToUtf8(const std::vector<BYTE>& byteBuffer, std::string& value)
    {
        value.clear();

        std::vector<wchar_t> wideBuffer;
        if (!TryCopyByteBufferToWideBuffer(byteBuffer, wideBuffer))
        {
            return false;
        }

        size_t length = 0;
        while (length < wideBuffer.size() && wideBuffer[length] != L'\0')
        {
            length++;
        }

        try
        {
            value = WideStringToUtf8Safe(std::wstring(wideBuffer.data(), length));
        }
        catch (...)
        {
            value.clear();
            return false;
        }

        return true;
    }

    static bool TryConvertWideStringListBufferToUtf8(const std::vector<BYTE>& byteBuffer, std::vector<std::string>& values)
    {
        values.clear();

        std::vector<wchar_t> wideBuffer;
        if (!TryCopyByteBufferToWideBuffer(byteBuffer, wideBuffer))
        {
            return false;
        }

        size_t offset = 0;
        try
        {
            while (offset < wideBuffer.size() && wideBuffer[offset] != L'\0')
            {
                size_t length = 0;
                while (offset + length < wideBuffer.size() && wideBuffer[offset + length] != L'\0')
                {
                    length++;
                }
                if (offset + length >= wideBuffer.size())
                {
                    values.clear();
                    return false;
                }

                values.push_back(WideStringToUtf8Safe(std::wstring(wideBuffer.data() + offset, length)));
                offset += length + 1;
            }
        }
        catch (...)
        {
            values.clear();
            return false;
        }

        return true;
    }

    static bool TryReadDevicePropertyBuffer(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, const DEVPROPKEY& propertyKey, DEVPROPTYPE& propertyType, std::vector<BYTE>& propertyBuffer)
    {
        propertyType = 0;
        propertyBuffer.clear();

        DWORD requiredSize = 0;
        if (!::SetupDiGetDevicePropertyW(deviceInfoSet, &deviceInfoData, &propertyKey, &propertyType, nullptr, 0, &requiredSize, 0))
        {
            const DWORD lastError = ::GetLastError();
            if (lastError != ERROR_INSUFFICIENT_BUFFER || !IsReasonableDevicePropertyBufferSize(requiredSize))
            {
                return false;
            }
        }

        for (int retryIndex = 0; retryIndex < 3; retryIndex++)
        {
            if (!TryAssignByteBuffer(requiredSize, propertyBuffer))
            {
                return false;
            }

            DWORD actualSize = requiredSize;
            if (::SetupDiGetDevicePropertyW(deviceInfoSet, &deviceInfoData, &propertyKey, &propertyType, propertyBuffer.data(), static_cast<DWORD>(propertyBuffer.size()), &actualSize, 0))
            {
                const size_t preservedSize = static_cast<size_t>(actualSize) + sizeof(wchar_t) * 2;
                if (preservedSize < propertyBuffer.size())
                {
                    propertyBuffer.resize(preservedSize);
                }
                return true;
            }

            const DWORD lastError = ::GetLastError();
            if (lastError != ERROR_INSUFFICIENT_BUFFER || !IsReasonableDevicePropertyBufferSize(actualSize))
            {
                propertyBuffer.clear();
                return false;
            }

            requiredSize = actualSize;
        }

        propertyBuffer.clear();
        return false;
    }

    static bool TryReadDeviceStringProperty(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, const DEVPROPKEY& propertyKey, std::string& value)
    {
        value.clear();

        DEVPROPTYPE propertyType = 0;
        std::vector<BYTE> propertyBuffer;
        if (!TryReadDevicePropertyBuffer(deviceInfoSet, deviceInfoData, propertyKey, propertyType, propertyBuffer))
        {
            return false;
        }

        if (propertyType != DEVPROP_TYPE_STRING || propertyBuffer.empty())
        {
            return false;
        }

        return TryConvertWideStringBufferToUtf8(propertyBuffer, value);
    }

    static bool TryReadDeviceGuidProperty(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, const DEVPROPKEY& propertyKey, std::string& value)
    {
        value.clear();

        DEVPROPTYPE propertyType = 0;
        std::vector<BYTE> propertyBuffer;
        if (!TryReadDevicePropertyBuffer(deviceInfoSet, deviceInfoData, propertyKey, propertyType, propertyBuffer))
        {
            return false;
        }

        if (propertyType != DEVPROP_TYPE_GUID || propertyBuffer.size() < sizeof(GUID))
        {
            return false;
        }

        GUID guid = {};
        std::memcpy(&guid, propertyBuffer.data(), sizeof(guid));
        value = GuidToString(guid);
        return true;
    }

    static bool TryReadDeviceBooleanProperty(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, const DEVPROPKEY& propertyKey, bool& value)
    {
        value = false;

        DEVPROPTYPE propertyType = 0;
        std::vector<BYTE> propertyBuffer;
        if (!TryReadDevicePropertyBuffer(deviceInfoSet, deviceInfoData, propertyKey, propertyType, propertyBuffer))
        {
            return false;
        }

        if (propertyType != DEVPROP_TYPE_BOOLEAN || propertyBuffer.size() < sizeof(DEVPROP_BOOLEAN))
        {
            return false;
        }

        DEVPROP_BOOLEAN booleanValue = DEVPROP_FALSE;
        std::memcpy(&booleanValue, propertyBuffer.data(), sizeof(booleanValue));
        value = booleanValue != DEVPROP_FALSE;
        return true;
    }

    static bool TryReadDeviceUInt32Property(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, const DEVPROPKEY& propertyKey, uint32_t& value)
    {
        value = 0;

        DEVPROPTYPE propertyType = 0;
        std::vector<BYTE> propertyBuffer;
        if (!TryReadDevicePropertyBuffer(deviceInfoSet, deviceInfoData, propertyKey, propertyType, propertyBuffer))
        {
            return false;
        }

        if ((propertyType != DEVPROP_TYPE_UINT32 && propertyType != DEVPROP_TYPE_INT32) || propertyBuffer.size() < sizeof(uint32_t))
        {
            return false;
        }

        std::memcpy(&value, propertyBuffer.data(), sizeof(value));
        return true;
    }

    static bool TryReadDeviceFileTimeProperty(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, const DEVPROPKEY& propertyKey, std::string& value)
    {
        value.clear();

        DEVPROPTYPE propertyType = 0;
        std::vector<BYTE> propertyBuffer;
        if (!TryReadDevicePropertyBuffer(deviceInfoSet, deviceInfoData, propertyKey, propertyType, propertyBuffer))
        {
            return false;
        }

        if (propertyType != DEVPROP_TYPE_FILETIME || propertyBuffer.size() < sizeof(FILETIME))
        {
            return false;
        }

        FILETIME fileTime = {};
        std::memcpy(&fileTime, propertyBuffer.data(), sizeof(fileTime));
        SYSTEMTIME systemTime = {};
        if (!::FileTimeToSystemTime(&fileTime, &systemTime))
        {
            return false;
        }

        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u", static_cast<unsigned int>(systemTime.wYear), static_cast<unsigned int>(systemTime.wMonth), static_cast<unsigned int>(systemTime.wDay));
        value = buffer;
        return true;
    }

    static bool TryReadDeviceStringListProperty(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, const DEVPROPKEY& propertyKey, std::vector<std::string>& values)
    {
        values.clear();

        DEVPROPTYPE propertyType = 0;
        std::vector<BYTE> propertyBuffer;
        if (!TryReadDevicePropertyBuffer(deviceInfoSet, deviceInfoData, propertyKey, propertyType, propertyBuffer))
        {
            return false;
        }

        if (propertyType != DEVPROP_TYPE_STRING_LIST || propertyBuffer.empty())
        {
            return false;
        }

        return TryConvertWideStringListBufferToUtf8(propertyBuffer, values);
    }

    static bool TryReadDeviceRegistryPropertyBuffer(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, DWORD registryProperty, DWORD& registryType, std::vector<BYTE>& propertyBuffer)
    {
        registryType = 0;
        propertyBuffer.clear();

        DWORD requiredSize = 0;
        if (!::SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &deviceInfoData, registryProperty, &registryType, nullptr, 0, &requiredSize))
        {
            const DWORD lastError = ::GetLastError();
            if (lastError != ERROR_INSUFFICIENT_BUFFER || !IsReasonableDevicePropertyBufferSize(requiredSize))
            {
                return false;
            }
        }

        for (int retryIndex = 0; retryIndex < 3; retryIndex++)
        {
            if (!TryAssignByteBuffer(requiredSize, propertyBuffer))
            {
                return false;
            }

            DWORD actualSize = requiredSize;
            if (::SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &deviceInfoData, registryProperty, &registryType, propertyBuffer.data(), static_cast<DWORD>(propertyBuffer.size()), &actualSize))
            {
                const size_t preservedSize = static_cast<size_t>(actualSize) + sizeof(wchar_t) * 2;
                if (preservedSize < propertyBuffer.size())
                {
                    propertyBuffer.resize(preservedSize);
                }
                return true;
            }

            const DWORD lastError = ::GetLastError();
            if (lastError != ERROR_INSUFFICIENT_BUFFER || !IsReasonableDevicePropertyBufferSize(actualSize))
            {
                propertyBuffer.clear();
                return false;
            }

            requiredSize = actualSize;
        }

        propertyBuffer.clear();
        return false;
    }

    static bool TryReadDeviceRegistryString(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, DWORD registryProperty, std::string& value)
    {
        value.clear();

        DWORD registryType = 0;
        std::vector<BYTE> propertyBuffer;
        if (!TryReadDeviceRegistryPropertyBuffer(deviceInfoSet, deviceInfoData, registryProperty, registryType, propertyBuffer))
        {
            return false;
        }

        if ((registryType != REG_SZ && registryType != REG_EXPAND_SZ) || propertyBuffer.empty())
        {
            return false;
        }

        return TryConvertWideStringBufferToUtf8(propertyBuffer, value);
    }

    static bool TryReadDeviceRegistryUInt32(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, DWORD registryProperty, uint32_t& value)
    {
        value = 0;

        DWORD registryType = 0;
        std::vector<BYTE> propertyBuffer;
        if (!TryReadDeviceRegistryPropertyBuffer(deviceInfoSet, deviceInfoData, registryProperty, registryType, propertyBuffer))
        {
            return false;
        }

        if (registryType != REG_DWORD || propertyBuffer.size() < sizeof(DWORD))
        {
            return false;
        }

        DWORD nativeValue = 0;
        std::memcpy(&nativeValue, propertyBuffer.data(), sizeof(nativeValue));
        value = static_cast<uint32_t>(nativeValue);
        return true;
    }

    static bool TryReadDeviceRegistryStringList(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, DWORD registryProperty, std::vector<std::string>& values)
    {
        values.clear();

        DWORD registryType = 0;
        std::vector<BYTE> propertyBuffer;
        if (!TryReadDeviceRegistryPropertyBuffer(deviceInfoSet, deviceInfoData, registryProperty, registryType, propertyBuffer))
        {
            return false;
        }

        if (registryType != REG_MULTI_SZ || propertyBuffer.empty())
        {
            return false;
        }

        return TryConvertWideStringListBufferToUtf8(propertyBuffer, values);
    }

    static bool TryGetDeviceInstanceId(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, std::string& instanceId)
    {
        instanceId.clear();

        DWORD requiredLength = 0;
        if (!::SetupDiGetDeviceInstanceIdW(deviceInfoSet, &deviceInfoData, nullptr, 0, &requiredLength))
        {
            const DWORD lastError = ::GetLastError();
            if (lastError != ERROR_INSUFFICIENT_BUFFER || requiredLength == 0)
            {
                return false;
            }
        }

        std::vector<wchar_t> buffer;
        try
        {
            buffer.assign(static_cast<size_t>(requiredLength) + 1, L'\0');
        }
        catch (...)
        {
            return false;
        }

        if (!::SetupDiGetDeviceInstanceIdW(deviceInfoSet, &deviceInfoData, buffer.data(), requiredLength, nullptr))
        {
            return false;
        }

        instanceId = WideNullTerminatedStringToUtf8(buffer.data());
        return !instanceId.empty();
    }

    static void FillStringPropertyWithFallback(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, const DEVPROPKEY& propertyKey, DWORD registryProperty, std::string& value)
    {
        if (TryReadDeviceStringProperty(deviceInfoSet, deviceInfoData, propertyKey, value))
        {
            return;
        }

        (void)TryReadDeviceRegistryString(deviceInfoSet, deviceInfoData, registryProperty, value);
    }

    static void FillStringListPropertyWithFallback(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, const DEVPROPKEY& propertyKey, DWORD registryProperty, std::vector<std::string>& values)
    {
        if (TryReadDeviceStringListProperty(deviceInfoSet, deviceInfoData, propertyKey, values))
        {
            return;
        }

        (void)TryReadDeviceRegistryStringList(deviceInfoSet, deviceInfoData, registryProperty, values);
    }

    static std::string ExtractEnumeratorNameFromInstanceId(const std::string& instanceId)
    {
        const size_t backslashIndex = instanceId.find('\\');
        if (backslashIndex == std::string::npos)
        {
            return std::string();
        }

        return instanceId.substr(0, backslashIndex);
    }

    static bool IsUsbConnectedDevice(const GB_SystemDeviceInfo& deviceInfo)
    {
        static const char* const usbEnumerators[] = { "USB", "USBSTOR", "USBPRINT", "USBCCGP", "USB4" };
        static const char* const usbInstancePrefixes[] = { "USB\\", "USBSTOR\\", "USBPRINT\\", "USBCCGP\\", "USB4\\" };
        static const char* const usbIdNeedles[] = { "USB\\VID_", "USBSTOR\\", "USBPRINT\\", "USBCCGP\\", "USB4\\" };

        if (EqualsAnyAsciiNoCase(deviceInfo.enumeratorName, usbEnumerators, sizeof(usbEnumerators) / sizeof(usbEnumerators[0])))
        {
            return true;
        }

        if (StartsWithAnyAsciiNoCase(deviceInfo.instanceId, usbInstancePrefixes, sizeof(usbInstancePrefixes) / sizeof(usbInstancePrefixes[0])))
        {
            return true;
        }

        return StringListContainsAnyAsciiNoCase(deviceInfo.hardwareIds, usbIdNeedles, sizeof(usbIdNeedles) / sizeof(usbIdNeedles[0])) || StringListContainsAnyAsciiNoCase(deviceInfo.compatibleIds, usbIdNeedles, sizeof(usbIdNeedles) / sizeof(usbIdNeedles[0]));
    }

    static bool IsBluetoothConnectedDevice(const GB_SystemDeviceInfo& deviceInfo)
    {
        static const char* const bluetoothEnumerators[] = { "BTH", "BTHLE", "BTHENUM", "BTHLEENUM", "BTHHFENUM", "BTHMODEM", "Bluetooth" };
        static const char* const bluetoothInstancePrefixes[] = { "BTH\\", "BTHLE\\", "BTHENUM\\", "BTHLEENUM\\", "BTHHFENUM\\", "BTHMODEM\\", "Bluetooth\\", "SWD\\RADIO\\Bluetooth" };
        static const char* const bluetoothIdNeedles[] = { "BTHENUM\\", "BTHLE\\", "BTH\\MS_BTH", "BTH\\MS_RFCOMM", "SWD\\RADIO\\Bluetooth" };

        if (EqualsAnyAsciiNoCase(deviceInfo.enumeratorName, bluetoothEnumerators, sizeof(bluetoothEnumerators) / sizeof(bluetoothEnumerators[0])))
        {
            return true;
        }

        if (StartsWithAnyAsciiNoCase(deviceInfo.instanceId, bluetoothInstancePrefixes, sizeof(bluetoothInstancePrefixes) / sizeof(bluetoothInstancePrefixes[0])))
        {
            return true;
        }

        return StringListContainsAnyAsciiNoCase(deviceInfo.hardwareIds, bluetoothIdNeedles, sizeof(bluetoothIdNeedles) / sizeof(bluetoothIdNeedles[0])) || StringListContainsAnyAsciiNoCase(deviceInfo.compatibleIds, bluetoothIdNeedles, sizeof(bluetoothIdNeedles) / sizeof(bluetoothIdNeedles[0]));
    }

    static GB_SystemDeviceKind DeduceDeviceKind(const GB_SystemDeviceInfo& deviceInfo)
    {
        const std::string className = ToLowerAscii(deviceInfo.className);
        const std::string enumeratorName = ToLowerAscii(deviceInfo.enumeratorName);
        const std::string instanceId = ToLowerAscii(deviceInfo.instanceId);
        const std::string friendlyName = ToLowerAscii(deviceInfo.friendlyName);
        const std::string description = ToLowerAscii(deviceInfo.description);

        static const char* const displayClassGuids[] = { "{4D36E968-E325-11CE-BFC1-08002BE10318}", "{4D36E96E-E325-11CE-BFC1-08002BE10318}" };
        static const char* const batteryClassGuids[] = { "{72631E54-78A4-11D0-BCF7-00AA00B7B32A}" };
        static const char* const storageClassGuids[] = { "{4D36E967-E325-11CE-BFC1-08002BE10318}", "{71A27CDD-812A-11D0-BEC7-08002BE2092F}", "{4D36E96A-E325-11CE-BFC1-08002BE10318}", "{4D36E97B-E325-11CE-BFC1-08002BE10318}" };
        static const char* const networkClassGuids[] = { "{4D36E972-E325-11CE-BFC1-08002BE10318}" };
        static const char* const keyboardClassGuids[] = { "{4D36E96B-E325-11CE-BFC1-08002BE10318}" };
        static const char* const mouseClassGuids[] = { "{4D36E96F-E325-11CE-BFC1-08002BE10318}" };
        static const char* const hidClassGuids[] = { "{745A17A0-74D3-11D0-B6FE-00A0C90F57DA}" };
        static const char* const audioClassGuids[] = { "{4D36E96C-E325-11CE-BFC1-08002BE10318}", "{C166523C-FE0C-4A94-A586-F1A80CFBBF3E}" };
        static const char* const bluetoothClassGuids[] = { "{E0CBF06C-CD8B-4647-BB8A-263B43F0F974}" };
        static const char* const cameraClassGuids[] = { "{CA3E7AB9-B4C3-4AE6-8251-579EF933890F}", "{6BDD1FC6-810F-11D0-BEC7-08002BE2092F}" };
        static const char* const printerClassGuids[] = { "{4D36E979-E325-11CE-BFC1-08002BE10318}", "{1ED2BBF9-11F0-4084-B21F-AD83A8E6DCDC}" };
        static const char* const usbClassGuids[] = { "{36FC9E60-C465-11CF-8056-444553540000}" };

        if (className == "keyboard" || IsOneOfClassGuid(deviceInfo.classGuid, keyboardClassGuids, sizeof(keyboardClassGuids) / sizeof(keyboardClassGuids[0])))
        {
            return GB_SystemDeviceKind::Keyboard;
        }
        if (className == "mouse" || IsOneOfClassGuid(deviceInfo.classGuid, mouseClassGuids, sizeof(mouseClassGuids) / sizeof(mouseClassGuids[0])))
        {
            return GB_SystemDeviceKind::Mouse;
        }
        if (className == "monitor" || className == "display" || StartsWithAsciiNoCase(instanceId, "DISPLAY\\") || IsOneOfClassGuid(deviceInfo.classGuid, displayClassGuids, sizeof(displayClassGuids) / sizeof(displayClassGuids[0])))
        {
            return GB_SystemDeviceKind::Display;
        }
        if (className == "battery" || IsOneOfClassGuid(deviceInfo.classGuid, batteryClassGuids, sizeof(batteryClassGuids) / sizeof(batteryClassGuids[0])))
        {
            return GB_SystemDeviceKind::Battery;
        }
        if (className == "diskdrive" || className == "volume" || className == "scsiadapter" || className == "storage" || className == "wdc" || IsOneOfClassGuid(deviceInfo.classGuid, storageClassGuids, sizeof(storageClassGuids) / sizeof(storageClassGuids[0])))
        {
            return GB_SystemDeviceKind::Storage;
        }
        if (className == "net" || IsOneOfClassGuid(deviceInfo.classGuid, networkClassGuids, sizeof(networkClassGuids) / sizeof(networkClassGuids[0])))
        {
            return GB_SystemDeviceKind::Network;
        }
        if (className == "media" || className == "audioendpoint" || friendlyName.find("audio") != std::string::npos || description.find("audio") != std::string::npos || IsOneOfClassGuid(deviceInfo.classGuid, audioClassGuids, sizeof(audioClassGuids) / sizeof(audioClassGuids[0])))
        {
            return GB_SystemDeviceKind::Audio;
        }
        if (className == "camera" || className == "image" || friendlyName.find("camera") != std::string::npos || description.find("camera") != std::string::npos || IsOneOfClassGuid(deviceInfo.classGuid, cameraClassGuids, sizeof(cameraClassGuids) / sizeof(cameraClassGuids[0])))
        {
            return GB_SystemDeviceKind::Camera;
        }
        if (className == "printer" || className == "printqueue" || IsOneOfClassGuid(deviceInfo.classGuid, printerClassGuids, sizeof(printerClassGuids) / sizeof(printerClassGuids[0])))
        {
            return GB_SystemDeviceKind::Printer;
        }
        if (className == "hidclass" || IsOneOfClassGuid(deviceInfo.classGuid, hidClassGuids, sizeof(hidClassGuids) / sizeof(hidClassGuids[0])))
        {
            return GB_SystemDeviceKind::HumanInterface;
        }
        if (className == "bluetooth" || enumeratorName == "bth" || enumeratorName == "bthle" || IsBluetoothConnectedDevice(deviceInfo) || IsOneOfClassGuid(deviceInfo.classGuid, bluetoothClassGuids, sizeof(bluetoothClassGuids) / sizeof(bluetoothClassGuids[0])))
        {
            return GB_SystemDeviceKind::Bluetooth;
        }
        if (IsUsbConnectedDevice(deviceInfo) || IsOneOfClassGuid(deviceInfo.classGuid, usbClassGuids, sizeof(usbClassGuids) / sizeof(usbClassGuids[0])))
        {
            return GB_SystemDeviceKind::Usb;
        }

        return GB_SystemDeviceKind::Unknown;
    }

    static bool FillDevNodeStatus(SP_DEVINFO_DATA& deviceInfoData, GB_SystemDeviceInfo& deviceInfo)
    {
        ULONG status = 0;
        ULONG problemCode = 0;
        const CONFIGRET configResult = ::CM_Get_DevNode_Status(&status, &problemCode, deviceInfoData.DevInst, 0);
        if (configResult != CR_SUCCESS)
        {
            return false;
        }

        deviceInfo.devNodeStatus = static_cast<uint32_t>(status);
        deviceInfo.problemCode = static_cast<uint32_t>(problemCode);
        deviceInfo.isStarted = (status & DN_STARTED) != 0;
        deviceInfo.hasProblem = (status & DN_HAS_PROBLEM) != 0;
        deviceInfo.isDisabled = deviceInfo.hasProblem && problemCode == CM_PROB_DISABLED;
        return true;
    }

    static void FillDeviceInfo(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData, const bool presentOnly, const bool readDriverInfo, GB_SystemDeviceInfo& deviceInfo)
    {
        (void)TryGetDeviceInstanceId(deviceInfoSet, deviceInfoData, deviceInfo.instanceId);
        FillStringPropertyWithFallback(deviceInfoSet, deviceInfoData, DEVPKEY_Device_FriendlyName, SPDRP_FRIENDLYNAME, deviceInfo.friendlyName);
        FillStringPropertyWithFallback(deviceInfoSet, deviceInfoData, DEVPKEY_Device_DeviceDesc, SPDRP_DEVICEDESC, deviceInfo.description);
        FillStringPropertyWithFallback(deviceInfoSet, deviceInfoData, DEVPKEY_Device_Class, SPDRP_CLASS, deviceInfo.className);
        FillStringPropertyWithFallback(deviceInfoSet, deviceInfoData, DEVPKEY_Device_EnumeratorName, SPDRP_ENUMERATOR_NAME, deviceInfo.enumeratorName);
        FillStringPropertyWithFallback(deviceInfoSet, deviceInfoData, DEVPKEY_Device_Manufacturer, SPDRP_MFG, deviceInfo.manufacturer);
        FillStringPropertyWithFallback(deviceInfoSet, deviceInfoData, DEVPKEY_Device_Service, SPDRP_SERVICE, deviceInfo.serviceName);
        FillStringPropertyWithFallback(deviceInfoSet, deviceInfoData, DEVPKEY_Device_LocationInfo, SPDRP_LOCATION_INFORMATION, deviceInfo.location);
        FillStringListPropertyWithFallback(deviceInfoSet, deviceInfoData, DEVPKEY_Device_HardwareIds, SPDRP_HARDWAREID, deviceInfo.hardwareIds);
        FillStringListPropertyWithFallback(deviceInfoSet, deviceInfoData, DEVPKEY_Device_CompatibleIds, SPDRP_COMPATIBLEIDS, deviceInfo.compatibleIds);
        (void)TryReadDeviceStringProperty(deviceInfoSet, deviceInfoData, DEVPKEY_Device_Parent, deviceInfo.parentInstanceId);
        (void)TryReadDeviceGuidProperty(deviceInfoSet, deviceInfoData, DEVPKEY_Device_ContainerId, deviceInfo.containerId);

        if (!TryReadDeviceGuidProperty(deviceInfoSet, deviceInfoData, DEVPKEY_Device_ClassGuid, deviceInfo.classGuid))
        {
            (void)TryReadDeviceRegistryString(deviceInfoSet, deviceInfoData, SPDRP_CLASSGUID, deviceInfo.classGuid);
        }

        if (!TryReadDeviceUInt32Property(deviceInfoSet, deviceInfoData, DEVPKEY_Device_Capabilities, deviceInfo.capabilities))
        {
            (void)TryReadDeviceRegistryUInt32(deviceInfoSet, deviceInfoData, SPDRP_CAPABILITIES, deviceInfo.capabilities);
        }
        if (!TryReadDeviceUInt32Property(deviceInfoSet, deviceInfoData, DEVPKEY_Device_RemovalPolicy, deviceInfo.removalPolicy))
        {
            (void)TryReadDeviceRegistryUInt32(deviceInfoSet, deviceInfoData, SPDRP_REMOVAL_POLICY, deviceInfo.removalPolicy);
        }

        if (readDriverInfo)
        {
            (void)TryReadDeviceStringProperty(deviceInfoSet, deviceInfoData, DEVPKEY_Device_DriverProvider, deviceInfo.driverProvider);
            (void)TryReadDeviceStringProperty(deviceInfoSet, deviceInfoData, DEVPKEY_Device_DriverVersion, deviceInfo.driverVersion);
            (void)TryReadDeviceFileTimeProperty(deviceInfoSet, deviceInfoData, DEVPKEY_Device_DriverDate, deviceInfo.driverDate);
        }

        if (deviceInfo.enumeratorName.empty())
        {
            deviceInfo.enumeratorName = ExtractEnumeratorNameFromInstanceId(deviceInfo.instanceId);
        }

        bool isPresentProperty = false;
        const bool hasPresentProperty = TryReadDeviceBooleanProperty(deviceInfoSet, deviceInfoData, DEVPKEY_Device_IsPresent, isPresentProperty);
        (void)FillDevNodeStatus(deviceInfoData, deviceInfo);
        if (hasPresentProperty)
        {
            deviceInfo.isPresent = isPresentProperty;
        }
        else
        {
            bool configManagerPresent = false;
            if (TryCheckDevicePresentByConfigManager(deviceInfo.instanceId, configManagerPresent))
            {
                deviceInfo.isPresent = configManagerPresent;
            }
            else
            {
                deviceInfo.isPresent = presentOnly;
            }
        }

        deviceInfo.isUsbConnected = IsUsbConnectedDevice(deviceInfo);
        deviceInfo.isBluetoothConnected = IsBluetoothConnectedDevice(deviceInfo);
        deviceInfo.isRemovable = (deviceInfo.capabilities & CM_DEVCAP_REMOVABLE) != 0 || deviceInfo.removalPolicy == CM_REMOVAL_POLICY_EXPECT_ORDERLY_REMOVAL || deviceInfo.removalPolicy == CM_REMOVAL_POLICY_EXPECT_SURPRISE_REMOVAL;
        deviceInfo.deviceKind = DeduceDeviceKind(deviceInfo);
    }

    static bool DeviceMatchesOptions(const GB_SystemDeviceInfo& deviceInfo, const GB_SystemDeviceQueryOptions& options)
    {
        if (!options.classGuid.empty() && !EqualsGuidString(deviceInfo.classGuid, options.classGuid))
        {
            return false;
        }
        if (!options.enumeratorName.empty() && !EqualsAsciiNoCase(deviceInfo.enumeratorName, options.enumeratorName))
        {
            return false;
        }
        if (options.deviceKind != GB_SystemDeviceKind::Unknown && deviceInfo.deviceKind != options.deviceKind)
        {
            return false;
        }
        if (options.usbConnectedOnly && !deviceInfo.isUsbConnected)
        {
            return false;
        }
        if (options.bluetoothConnectedOnly && !deviceInfo.isBluetoothConnected)
        {
            return false;
        }

        return true;
    }

    static std::string GetBestDeviceName(const GB_SystemDeviceInfo& deviceInfo)
    {
        if (!deviceInfo.friendlyName.empty())
        {
            return deviceInfo.friendlyName;
        }
        if (!deviceInfo.description.empty())
        {
            return deviceInfo.description;
        }

        return deviceInfo.instanceId;
    }

    static GB_SystemResult AppendUniqueGuid(const GUID& classGuid, std::unordered_set<std::string>& seenGuids, std::vector<GUID>& classGuids, const std::string& operationName, const std::string& message)
    {
        const std::string normalizedGuid = NormalizeGuidString(GuidToString(classGuid));
        try
        {
            if (!seenGuids.insert(normalizedGuid).second)
            {
                return GB_SystemResult::Succeeded(operationName);
            }
            classGuids.push_back(classGuid);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, message);
        }

        return GB_SystemResult::Succeeded(operationName);
    }

#if defined(CM_ENUMERATE_CLASSES_INTERFACE)
    static GB_SystemResult BuildInstalledInterfaceClassGuidsByConfigManager(std::vector<GUID>& classGuids)
    {
        classGuids.clear();

        std::unordered_set<std::string> seenGuids;
        if (!TryReserve(seenGuids, 128) || !TryReserve(classGuids, 128))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDevice::GetDeviceInterfaces", u8"初始化设备接口类 GUID 缓存时内存不足。");
        }

        for (ULONG classIndex = 0;; classIndex++)
        {
            GUID classGuid = {};
            const CONFIGRET configResult = ::CM_Enumerate_Classes(classIndex, &classGuid, CM_ENUMERATE_CLASSES_INTERFACE);
            if (configResult == CR_NO_SUCH_VALUE)
            {
                break;
            }
            if (configResult == CR_INVALID_DATA)
            {
                continue;
            }
            if (configResult != CR_SUCCESS)
            {
                return ConfigRetToResult(configResult, u8"GB_SystemDevice::GetDeviceInterfaces", u8"CM_Enumerate_Classes 枚举设备接口类失败。");
            }

            const GB_SystemResult appendResult = AppendUniqueGuid(classGuid, seenGuids, classGuids, u8"GB_SystemDevice::GetDeviceInterfaces", u8"保存设备接口类 GUID 时内存不足。");
            if (appendResult.IsFailed())
            {
                return appendResult;
            }
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemDevice::GetDeviceInterfaces", u8"已通过 ConfigMgr 读取系统已安装设备接口类。");
    }
#endif

    static GB_SystemResult BuildInstalledInterfaceClassGuidsByRegistry(std::vector<GUID>& classGuids)
    {
        classGuids.clear();

        RegKeyScope interfaceRootKey(::SetupDiOpenClassRegKeyExW(nullptr, KEY_READ, DIOCR_INTERFACE, nullptr, nullptr));
        if (!interfaceRootKey.IsValid())
        {
            return GB_SystemResult::FromLastWin32Error(u8"GB_SystemDevice::GetDeviceInterfaces", u8"SetupDiOpenClassRegKeyExW 打开设备接口类注册表根失败。");
        }

        std::unordered_set<std::string> seenGuids;
        if (!TryReserve(seenGuids, 128) || !TryReserve(classGuids, 128))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDevice::GetDeviceInterfaces", u8"初始化设备接口类 GUID 缓存时内存不足。");
        }

        for (DWORD keyIndex = 0;; keyIndex++)
        {
            wchar_t keyName[128] = {};
            DWORD keyNameLength = static_cast<DWORD>((sizeof(keyName) / sizeof(keyName[0])) - 1);
            const LONG enumResult = ::RegEnumKeyExW(interfaceRootKey.Get(), keyIndex, keyName, &keyNameLength, nullptr, nullptr, nullptr, nullptr);
            if (enumResult == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (enumResult != ERROR_SUCCESS)
            {
                return GB_SystemResult::FromWin32Error(static_cast<uint32_t>(enumResult), u8"GB_SystemDevice::GetDeviceInterfaces", u8"枚举设备接口类注册表子项失败。");
            }

            const std::string guidText = WideNullTerminatedStringToUtf8(keyName);
            GUID classGuid = {};
            if (!TryParseGuidString(guidText, classGuid))
            {
                continue;
            }

            const GB_SystemResult appendResult = AppendUniqueGuid(classGuid, seenGuids, classGuids, u8"GB_SystemDevice::GetDeviceInterfaces", u8"保存设备接口类 GUID 时内存不足。");
            if (appendResult.IsFailed())
            {
                return appendResult;
            }
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemDevice::GetDeviceInterfaces", u8"已通过注册表读取系统已安装设备接口类。");
    }

    static GB_SystemResult BuildInstalledInterfaceClassGuids(std::vector<GUID>& classGuids)
    {
        classGuids.clear();

#if defined(CM_ENUMERATE_CLASSES_INTERFACE)
        std::vector<GUID> configManagerClassGuids;
        const GB_SystemResult configManagerResult = BuildInstalledInterfaceClassGuidsByConfigManager(configManagerClassGuids);
        if (configManagerResult.IsSucceeded())
        {
            classGuids = std::move(configManagerClassGuids);
            return configManagerResult;
        }
#endif

        return BuildInstalledInterfaceClassGuidsByRegistry(classGuids);
    }

    static GB_SystemResult EnumerateInterfacesForClassGuid(const GUID& interfaceClassGuid, const GB_SystemDeviceInterfaceQueryOptions& options, std::unordered_set<std::string>& seenInterfacePaths, std::vector<GB_SystemDeviceInterfaceInfo>& outputInterfaces)
    {
        std::wstring deviceInstanceIdWide;
        const wchar_t* enumeratorText = nullptr;
        if (!options.deviceInstanceId.empty())
        {
            deviceInstanceIdWide = Utf8ToWideSafe(options.deviceInstanceId);
            if (deviceInstanceIdWide.empty())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, u8"GB_SystemDevice::GetDeviceInterfaces", u8"设备实例 ID 从 UTF-8 转 UTF-16 失败。");
            }
            enumeratorText = deviceInstanceIdWide.c_str();
        }

        const DWORD flags = DIGCF_DEVICEINTERFACE | (options.presentOnly ? DIGCF_PRESENT : 0);
        DeviceInfoSetScope deviceInfoSet(::SetupDiGetClassDevsW(&interfaceClassGuid, enumeratorText, nullptr, flags));
        if (!deviceInfoSet.IsValid())
        {
            const DWORD lastError = ::GetLastError();
            if (lastError == ERROR_INVALID_DATA || lastError == ERROR_NO_MORE_ITEMS || lastError == ERROR_FILE_NOT_FOUND)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemDevice::GetDeviceInterfaces", u8"该设备接口类当前没有可枚举接口。");
            }

            return GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemDevice::GetDeviceInterfaces", u8"SetupDiGetClassDevsW 获取设备接口集合失败。");
        }

        for (DWORD interfaceIndex = 0;; interfaceIndex++)
        {
            SP_DEVICE_INTERFACE_DATA interfaceData = {};
            interfaceData.cbSize = sizeof(interfaceData);
            if (!::SetupDiEnumDeviceInterfaces(deviceInfoSet.Get(), nullptr, &interfaceClassGuid, interfaceIndex, &interfaceData))
            {
                const DWORD lastError = ::GetLastError();
                if (lastError == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }

                return GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemDevice::GetDeviceInterfaces", u8"SetupDiEnumDeviceInterfaces 枚举设备接口失败。");
            }

            DWORD requiredSize = 0;
            SP_DEVINFO_DATA deviceInfoData = {};
            deviceInfoData.cbSize = sizeof(deviceInfoData);
            (void)::SetupDiGetDeviceInterfaceDetailW(deviceInfoSet.Get(), &interfaceData, nullptr, 0, &requiredSize, &deviceInfoData);
            const DWORD sizeError = ::GetLastError();
            if (requiredSize == 0 || sizeError != ERROR_INSUFFICIENT_BUFFER)
            {
                if (sizeError == ERROR_NO_SUCH_DEVINST || sizeError == ERROR_FILE_NOT_FOUND || sizeError == ERROR_INVALID_DATA)
                {
                    continue;
                }

                return GB_SystemResult::FromWin32Error(sizeError, u8"GB_SystemDevice::GetDeviceInterfaces", u8"SetupDiGetDeviceInterfaceDetailW 获取接口详情长度失败。");
            }

            std::vector<BYTE> detailBuffer;
            if (!TryAssignDeviceInterfaceDetailBuffer(requiredSize, detailBuffer))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDevice::GetDeviceInterfaces", u8"分配设备接口详情缓冲区失败，可能是系统返回的详情长度异常或内存不足。");
            }

            SP_DEVICE_INTERFACE_DETAIL_DATA_W* detailData = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
            detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            deviceInfoData = SP_DEVINFO_DATA();
            deviceInfoData.cbSize = sizeof(deviceInfoData);
            if (!::SetupDiGetDeviceInterfaceDetailW(deviceInfoSet.Get(), &interfaceData, detailData, requiredSize, nullptr, &deviceInfoData))
            {
                const DWORD lastError = ::GetLastError();
                if (lastError == ERROR_NO_SUCH_DEVINST || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_INVALID_DATA)
                {
                    continue;
                }

                return GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemDevice::GetDeviceInterfaces", u8"SetupDiGetDeviceInterfaceDetailW 获取接口详情失败。");
            }

            GB_SystemDeviceInterfaceInfo interfaceInfo;
            interfaceInfo.interfacePath = WideNullTerminatedStringToUtf8(detailData->DevicePath);
            if (interfaceInfo.interfacePath.empty())
            {
                continue;
            }

            interfaceInfo.interfaceClassGuid = GuidToString(interfaceClassGuid);
            interfaceInfo.isEnabled = (interfaceData.Flags & SPINT_ACTIVE) != 0;
            if (options.presentOnly && !interfaceInfo.isEnabled)
            {
                continue;
            }

            GB_SystemDeviceInfo associatedDevice;
            FillDeviceInfo(deviceInfoSet.Get(), deviceInfoData, options.presentOnly, false, associatedDevice);
            if (!options.deviceInstanceId.empty() && !EqualsAsciiNoCase(associatedDevice.instanceId, options.deviceInstanceId))
            {
                continue;
            }
            if (options.presentOnly && !associatedDevice.isPresent)
            {
                continue;
            }

            bool shouldAppendInterface = false;
            try
            {
                const std::string normalizedPath = ToLowerAscii(interfaceInfo.interfacePath);
                shouldAppendInterface = seenInterfacePaths.insert(normalizedPath).second;
            }
            catch (...)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDevice::GetDeviceInterfaces", u8"保存设备接口路径去重信息时内存不足。");
            }

            if (!shouldAppendInterface)
            {
                continue;
            }

            interfaceInfo.deviceInstanceId = associatedDevice.instanceId;
            interfaceInfo.associatedDeviceName = GetBestDeviceName(associatedDevice);
            interfaceInfo.isPresent = associatedDevice.isPresent;

            try
            {
                outputInterfaces.push_back(interfaceInfo);
            }
            catch (...)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDevice::GetDeviceInterfaces", u8"保存设备接口信息时内存不足。");
            }
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemDevice::GetDeviceInterfaces", u8"设备接口枚举完成。");
    }

    static std::string BuildWindowClassName(const void* ownerPointer)
    {
        char buffer[128] = {};
        std::snprintf(buffer, sizeof(buffer), "GB_SystemDeviceWatcher_%lu_%p", static_cast<unsigned long>(::GetCurrentProcessId()), ownerPointer);
        return std::string(buffer);
    }

    static std::string GetDeviceEventName(const GB_SystemDeviceEventType eventType)
    {
        std::string eventName = "SystemDevice.";
        eventName += GB_SystemDevice::GetDeviceEventTypeName(eventType);
        return eventName;
    }

    static GB_SystemDeviceEventType MapDeviceBroadcastEventType(const WPARAM wParam)
    {
        switch (wParam)
        {
        case DBT_DEVNODES_CHANGED:
            return GB_SystemDeviceEventType::DeviceNodesChanged;
        case DBT_DEVICEARRIVAL:
            return GB_SystemDeviceEventType::DeviceInterfaceArrived;
        case DBT_DEVICEREMOVECOMPLETE:
            return GB_SystemDeviceEventType::DeviceInterfaceRemoved;
        case DBT_DEVICEQUERYREMOVE:
            return GB_SystemDeviceEventType::DeviceQueryRemove;
        case DBT_DEVICEQUERYREMOVEFAILED:
            return GB_SystemDeviceEventType::DeviceQueryRemoveFailed;
        case DBT_DEVICEREMOVEPENDING:
            return GB_SystemDeviceEventType::DeviceRemovePending;
        default:
            break;
        }

        return GB_SystemDeviceEventType::Unknown;
    }

#if GB_SYSTEMDEVICE_HAS_CM_NOTIFICATION
    typedef CONFIGRET(WINAPI* CmRegisterNotificationFunc)(PCM_NOTIFY_FILTER filter, PVOID context, PCM_NOTIFY_CALLBACK callback, PHCMNOTIFICATION notifyContext);
    typedef CONFIGRET(WINAPI* CmUnregisterNotificationFunc)(HCMNOTIFICATION notifyContext);

    static CmRegisterNotificationFunc GetCmRegisterNotificationFunc()
    {
        HMODULE moduleHandle = ::GetModuleHandleW(L"CfgMgr32.dll");
        if (moduleHandle == nullptr)
        {
            moduleHandle = ::LoadLibraryW(L"CfgMgr32.dll");
        }
        if (moduleHandle == nullptr)
        {
            return nullptr;
        }

        return reinterpret_cast<CmRegisterNotificationFunc>(::GetProcAddress(moduleHandle, "CM_Register_Notification"));
    }

    static CmUnregisterNotificationFunc GetCmUnregisterNotificationFunc()
    {
        HMODULE moduleHandle = ::GetModuleHandleW(L"CfgMgr32.dll");
        if (moduleHandle == nullptr)
        {
            moduleHandle = ::LoadLibraryW(L"CfgMgr32.dll");
        }
        if (moduleHandle == nullptr)
        {
            return nullptr;
        }

        return reinterpret_cast<CmUnregisterNotificationFunc>(::GetProcAddress(moduleHandle, "CM_Unregister_Notification"));
    }

    static GB_SystemDeviceEventType MapCmNotifyAction(const CM_NOTIFY_ACTION action)
    {
        switch (action)
        {
        case CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL:
            return GB_SystemDeviceEventType::DeviceInterfaceArrived;
        case CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL:
            return GB_SystemDeviceEventType::DeviceInterfaceRemoved;
        case CM_NOTIFY_ACTION_DEVICEQUERYREMOVE:
            return GB_SystemDeviceEventType::DeviceQueryRemove;
        case CM_NOTIFY_ACTION_DEVICEQUERYREMOVEFAILED:
            return GB_SystemDeviceEventType::DeviceQueryRemoveFailed;
        case CM_NOTIFY_ACTION_DEVICEREMOVEPENDING:
            return GB_SystemDeviceEventType::DeviceRemovePending;
        case CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE:
            return GB_SystemDeviceEventType::DeviceRemoveComplete;
        case CM_NOTIFY_ACTION_DEVICECUSTOMEVENT:
            return GB_SystemDeviceEventType::DeviceCustomEvent;
        case CM_NOTIFY_ACTION_DEVICEINSTANCEENUMERATED:
            return GB_SystemDeviceEventType::DeviceInstanceEnumerated;
        case CM_NOTIFY_ACTION_DEVICEINSTANCESTARTED:
            return GB_SystemDeviceEventType::DeviceInstanceStarted;
        case CM_NOTIFY_ACTION_DEVICEINSTANCEREMOVED:
            return GB_SystemDeviceEventType::DeviceInstanceRemoved;
        default:
            break;
        }

        return GB_SystemDeviceEventType::Unknown;
    }
#endif
#endif
}

std::string GB_SystemDevice::GetDeviceKindName(const GB_SystemDeviceKind deviceKind)
{
    switch (deviceKind)
    {
    case GB_SystemDeviceKind::Usb:
        return "Usb";
    case GB_SystemDeviceKind::Display:
        return "Display";
    case GB_SystemDeviceKind::Battery:
        return "Battery";
    case GB_SystemDeviceKind::Storage:
        return "Storage";
    case GB_SystemDeviceKind::Network:
        return "Network";
    case GB_SystemDeviceKind::HumanInterface:
        return "HumanInterface";
    case GB_SystemDeviceKind::Audio:
        return "Audio";
    case GB_SystemDeviceKind::Bluetooth:
        return "Bluetooth";
    case GB_SystemDeviceKind::Camera:
        return "Camera";
    case GB_SystemDeviceKind::Keyboard:
        return "Keyboard";
    case GB_SystemDeviceKind::Mouse:
        return "Mouse";
    case GB_SystemDeviceKind::Printer:
        return "Printer";
    case GB_SystemDeviceKind::Unknown:
    default:
        break;
    }

    return "Unknown";
}

std::string GB_SystemDevice::GetDeviceEventTypeName(const GB_SystemDeviceEventType eventType)
{
    switch (eventType)
    {
    case GB_SystemDeviceEventType::DeviceNodesChanged:
        return "DeviceNodesChanged";
    case GB_SystemDeviceEventType::DeviceInstanceEnumerated:
        return "DeviceInstanceEnumerated";
    case GB_SystemDeviceEventType::DeviceInstanceStarted:
        return "DeviceInstanceStarted";
    case GB_SystemDeviceEventType::DeviceInstanceRemoved:
        return "DeviceInstanceRemoved";
    case GB_SystemDeviceEventType::DeviceInterfaceArrived:
        return "DeviceInterfaceArrived";
    case GB_SystemDeviceEventType::DeviceInterfaceRemoved:
        return "DeviceInterfaceRemoved";
    case GB_SystemDeviceEventType::DeviceQueryRemove:
        return "DeviceQueryRemove";
    case GB_SystemDeviceEventType::DeviceQueryRemoveFailed:
        return "DeviceQueryRemoveFailed";
    case GB_SystemDeviceEventType::DeviceRemovePending:
        return "DeviceRemovePending";
    case GB_SystemDeviceEventType::DeviceRemoveComplete:
        return "DeviceRemoveComplete";
    case GB_SystemDeviceEventType::DeviceCustomEvent:
        return "DeviceCustomEvent";
    case GB_SystemDeviceEventType::Unknown:
    default:
        break;
    }

    return "Unknown";
}

GB_SystemResult GB_SystemDevice::GetDevices(std::vector<GB_SystemDeviceInfo>& devices, const GB_SystemDeviceQueryOptions& options)
{
    devices.clear();
#if !defined(_WIN32)
    (void)options;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemDevice::GetDevices", u8"当前平台不支持 Windows 设备枚举。");
#else
    GUID classGuid = {};
    const bool hasClassGuid = !options.classGuid.empty();
    GB_SystemDeviceQueryOptions normalizedOptions = options;
    if (ContainsNullCharacter(normalizedOptions.classGuid) || ContainsNullCharacter(normalizedOptions.enumeratorName))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemDevice::GetDevices", u8"classGuid 或 enumeratorName 不能包含内嵌空字符。");
    }
    if (!IsValidSystemDeviceKind(normalizedOptions.deviceKind))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemDevice::GetDevices", u8"deviceKind 不是有效的 GB_SystemDeviceKind 枚举值。");
    }

    if (hasClassGuid)
    {
        if (!TryParseGuidString(options.classGuid, classGuid))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemDevice::GetDevices", u8"classGuid 不是有效的 GUID 字符串。");
        }
        normalizedOptions.classGuid = GuidToString(classGuid);
    }

    std::wstring enumeratorNameWide;
    const wchar_t* enumeratorName = nullptr;
    if (!normalizedOptions.enumeratorName.empty())
    {
        enumeratorNameWide = Utf8ToWideSafe(normalizedOptions.enumeratorName);
        if (enumeratorNameWide.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, u8"GB_SystemDevice::GetDevices", u8"enumeratorName 从 UTF-8 转 UTF-16 失败。");
        }
        enumeratorName = enumeratorNameWide.c_str();
    }

    const DWORD flags = (hasClassGuid ? 0 : DIGCF_ALLCLASSES) | (normalizedOptions.presentOnly ? DIGCF_PRESENT : 0);
    DeviceInfoSetScope deviceInfoSet(::SetupDiGetClassDevsW(hasClassGuid ? &classGuid : nullptr, enumeratorName, nullptr, flags));
    if (!deviceInfoSet.IsValid())
    {
        return GB_SystemResult::FromLastWin32Error(u8"GB_SystemDevice::GetDevices", u8"SetupDiGetClassDevsW 获取设备集合失败。");
    }

    std::vector<GB_SystemDeviceInfo> collectedDevices;
    if (!TryReserve(collectedDevices, 256))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDevice::GetDevices", u8"初始化系统设备缓存时内存不足。");
    }
    for (DWORD deviceIndex = 0;; deviceIndex++)
    {
        SP_DEVINFO_DATA deviceInfoData = {};
        deviceInfoData.cbSize = sizeof(deviceInfoData);
        if (!::SetupDiEnumDeviceInfo(deviceInfoSet.Get(), deviceIndex, &deviceInfoData))
        {
            const DWORD lastError = ::GetLastError();
            if (lastError == ERROR_NO_MORE_ITEMS)
            {
                break;
            }

            return GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemDevice::GetDevices", u8"SetupDiEnumDeviceInfo 枚举设备失败。");
        }

        GB_SystemDeviceInfo deviceInfo;
        FillDeviceInfo(deviceInfoSet.Get(), deviceInfoData, normalizedOptions.presentOnly, normalizedOptions.readDriverInfo, deviceInfo);
        if (DeviceMatchesOptions(deviceInfo, normalizedOptions))
        {
            try
            {
                collectedDevices.push_back(deviceInfo);
            }
            catch (...)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDevice::GetDevices", u8"保存系统设备信息时内存不足。");
            }
        }
    }

    devices = std::move(collectedDevices);
    return GB_SystemResult::Succeeded(u8"GB_SystemDevice::GetDevices", u8"系统设备枚举完成。");
#endif
}

GB_SystemResult GB_SystemDevice::GetDevicesByKind(const GB_SystemDeviceKind deviceKind, std::vector<GB_SystemDeviceInfo>& devices, const bool presentOnly)
{
    if (!IsValidSystemDeviceKind(deviceKind))
    {
        devices.clear();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemDevice::GetDevicesByKind", u8"deviceKind 不是有效的 GB_SystemDeviceKind 枚举值。");
    }

    GB_SystemDeviceQueryOptions options;
    options.presentOnly = presentOnly;
    options.deviceKind = deviceKind;
    return GetDevices(devices, options).WithOperationName(u8"GB_SystemDevice::GetDevicesByKind");
}

GB_SystemResult GB_SystemDevice::GetDeviceByInstanceId(const std::string& instanceId, GB_SystemDeviceInfo& deviceInfo, bool& found)
{
    deviceInfo = GB_SystemDeviceInfo();
    found = false;
#if !defined(_WIN32)
    (void)instanceId;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemDevice::GetDeviceByInstanceId", u8"当前平台不支持 Windows 设备查询。");
#else
    if (instanceId.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemDevice::GetDeviceByInstanceId", u8"设备实例 ID 不能为空。");
    }
    if (ContainsNullCharacter(instanceId))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemDevice::GetDeviceByInstanceId", u8"设备实例 ID 不能包含内嵌空字符。");
    }

    const std::wstring instanceIdWide = Utf8ToWideSafe(instanceId);
    if (instanceIdWide.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, u8"GB_SystemDevice::GetDeviceByInstanceId", u8"设备实例 ID 从 UTF-8 转 UTF-16 失败。");
    }

    DeviceInfoSetScope deviceInfoSet(::SetupDiCreateDeviceInfoList(nullptr, nullptr));
    if (!deviceInfoSet.IsValid())
    {
        return GB_SystemResult::FromLastWin32Error(u8"GB_SystemDevice::GetDeviceByInstanceId", u8"SetupDiCreateDeviceInfoList 创建设备集合失败。");
    }

    SP_DEVINFO_DATA deviceInfoData = {};
    deviceInfoData.cbSize = sizeof(deviceInfoData);
    if (!::SetupDiOpenDeviceInfoW(deviceInfoSet.Get(), instanceIdWide.c_str(), nullptr, 0, &deviceInfoData))
    {
        const DWORD lastError = ::GetLastError();
        if (lastError == ERROR_NO_SUCH_DEVINST || lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_INVALID_DATA)
        {
            return GB_SystemResult::Succeeded(u8"GB_SystemDevice::GetDeviceByInstanceId", u8"未找到指定设备实例。");
        }

        return GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemDevice::GetDeviceByInstanceId", u8"SetupDiOpenDeviceInfoW 打开设备实例失败。");
    }

    FillDeviceInfo(deviceInfoSet.Get(), deviceInfoData, false, true, deviceInfo);
    found = true;
    return GB_SystemResult::Succeeded(u8"GB_SystemDevice::GetDeviceByInstanceId", u8"已获取指定设备实例信息。");
#endif
}

GB_SystemResult GB_SystemDevice::DeviceExists(const std::string& instanceId, bool& exists)
{
    exists = false;
#if !defined(_WIN32)
    (void)instanceId;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemDevice::DeviceExists", u8"当前平台不支持 Windows 设备查询。");
#else
    if (instanceId.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemDevice::DeviceExists", u8"设备实例 ID 不能为空。");
    }
    if (ContainsNullCharacter(instanceId))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemDevice::DeviceExists", u8"设备实例 ID 不能包含内嵌空字符。");
    }

    const std::wstring instanceIdWide = Utf8ToWideSafe(instanceId);
    if (instanceIdWide.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, u8"GB_SystemDevice::DeviceExists", u8"设备实例 ID 从 UTF-8 转 UTF-16 失败。");
    }

    DEVINST deviceInstance = 0;
    const CONFIGRET configResult = ::CM_Locate_DevNodeW(&deviceInstance, const_cast<DEVINSTID_W>(instanceIdWide.c_str()), CM_LOCATE_DEVNODE_NORMAL);
    if (configResult == CR_SUCCESS)
    {
        exists = true;
        return GB_SystemResult::Succeeded(u8"GB_SystemDevice::DeviceExists", u8"指定设备实例当前存在。");
    }
    if (IsConfigRetDeviceNotFound(configResult))
    {
        exists = false;
        return GB_SystemResult::Succeeded(u8"GB_SystemDevice::DeviceExists", u8"指定设备实例当前不存在。");
    }

    return ConfigRetToResult(configResult, u8"GB_SystemDevice::DeviceExists", u8"CM_Locate_DevNodeW 检查设备实例失败。");
#endif
}

GB_SystemResult GB_SystemDevice::GetDeviceInterfaces(std::vector<GB_SystemDeviceInterfaceInfo>& deviceInterfaces, const GB_SystemDeviceInterfaceQueryOptions& options)
{
    deviceInterfaces.clear();
#if !defined(_WIN32)
    (void)options;
    return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemDevice::GetDeviceInterfaces", u8"当前平台不支持 Windows 设备接口枚举。");
#else
    if (ContainsNullCharacter(options.interfaceClassGuid) || ContainsNullCharacter(options.deviceInstanceId))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemDevice::GetDeviceInterfaces", u8"interfaceClassGuid 或 deviceInstanceId 不能包含内嵌空字符。");
    }

    std::vector<GUID> interfaceClassGuids;
    if (!options.interfaceClassGuid.empty())
    {
        GUID interfaceClassGuid = {};
        if (!TryParseGuidString(options.interfaceClassGuid, interfaceClassGuid))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemDevice::GetDeviceInterfaces", u8"interfaceClassGuid 不是有效的 GUID 字符串。");
        }

        try
        {
            interfaceClassGuids.push_back(interfaceClassGuid);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDevice::GetDeviceInterfaces", u8"保存设备接口类 GUID 时内存不足。");
        }
    }
    else if (options.enumerateAllInstalledInterfaceClasses)
    {
        GB_SystemResult classResult = BuildInstalledInterfaceClassGuids(interfaceClassGuids);
        if (classResult.IsFailed())
        {
            return classResult.WithOperationName(u8"GB_SystemDevice::GetDeviceInterfaces");
        }
    }
    else
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemDevice::GetDeviceInterfaces", u8"interfaceClassGuid 为空时必须启用 enumerateAllInstalledInterfaceClasses。");
    }

    std::unordered_set<std::string> seenInterfacePaths;
    std::vector<GB_SystemDeviceInterfaceInfo> collectedInterfaces;
    if (!TryReserve(seenInterfacePaths, 1024) || !TryReserve(collectedInterfaces, 256))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDevice::GetDeviceInterfaces", u8"初始化设备接口缓存时内存不足。");
    }
    for (size_t index = 0; index < interfaceClassGuids.size(); index++)
    {
        GB_SystemResult result = EnumerateInterfacesForClassGuid(interfaceClassGuids[index], options, seenInterfacePaths, collectedInterfaces);
        if (result.IsFailed())
        {
            return result.WithOperationName(u8"GB_SystemDevice::GetDeviceInterfaces");
        }
    }

    deviceInterfaces = std::move(collectedInterfaces);
    return GB_SystemResult::Succeeded(u8"GB_SystemDevice::GetDeviceInterfaces", u8"系统设备接口枚举完成。");
#endif
}

GB_SystemResult GB_SystemDevice::GetDeviceInterfacesByClassGuid(const std::string& interfaceClassGuid, std::vector<GB_SystemDeviceInterfaceInfo>& deviceInterfaces, const bool presentOnly)
{
    GB_SystemDeviceInterfaceQueryOptions options;
    options.presentOnly = presentOnly;
    options.interfaceClassGuid = interfaceClassGuid;
    options.enumerateAllInstalledInterfaceClasses = false;
    return GetDeviceInterfaces(deviceInterfaces, options).WithOperationName(u8"GB_SystemDevice::GetDeviceInterfacesByClassGuid");
}

class GB_SystemDeviceWatcher::Impl
{
public:
    Impl() : acceptingNativeNotifications(false), activeNativeNotificationCount(0), typedDispatcher(GB_EventDispatcher::MakeQueuedOptions(1024, GB_EventQueueOverflowPolicy::DropOldest, u8"GB_SystemDeviceWatcher.Typed")), publicDispatcher(GB_EventDispatcher::MakeQueuedOptions(1024, GB_EventQueueOverflowPolicy::DropOldest, u8"GB_SystemDeviceWatcher.Public"))
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
#if !defined(_WIN32)
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemDeviceWatcher::Start", u8"当前平台不支持 Windows 设备变化监听。");
#else
        std::lock_guard<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (running)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemDeviceWatcher::Start", u8"系统设备监听器已经启动。");
            }
        }

        GB_SystemResult result = typedDispatcher.Start();
        if (result.IsFailed())
        {
            return result.WithOperationName(u8"GB_SystemDeviceWatcher::Start");
        }
        result = publicDispatcher.Start();
        if (result.IsFailed())
        {
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return result.WithOperationName(u8"GB_SystemDeviceWatcher::Start");
        }

        EnableNativeNotifications();

#if GB_SYSTEMDEVICE_HAS_CM_NOTIFICATION
        result = TryStartCmNotifications();
        if (result.IsSucceeded())
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            running = true;
            useCmNotification = true;
            return GB_SystemResult::Succeeded(u8"GB_SystemDeviceWatcher::Start", u8"已通过 CM_Register_Notification 启动系统设备监听。");
        }
#endif

        result = StartFallbackWindow();
        if (result.IsFailed())
        {
            DisableNativeNotifications();
            WaitNativeNotificationsFinished();
            (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return result.WithOperationName(u8"GB_SystemDeviceWatcher::Start");
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            running = true;
            useCmNotification = false;
        }
        return GB_SystemResult::Succeeded(u8"GB_SystemDeviceWatcher::Start", u8"已通过隐藏消息窗口启动系统设备监听。");
#endif
    }

    GB_SystemResult Stop()
    {
#if !defined(_WIN32)
        return GB_SystemResult::Succeeded(u8"GB_SystemDeviceWatcher::Stop", u8"当前平台没有需要停止的 Windows 设备监听。");
#else
        std::lock_guard<std::mutex> operationLock(operationMutex);

#if GB_SYSTEMDEVICE_HAS_CM_NOTIFICATION
        std::vector<HCMNOTIFICATION> localCmNotificationHandles;
#endif
        std::thread localThread;
        HWND localWindowHandle = nullptr;
        DWORD localThreadId = 0;

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!running && !windowThread.joinable()
#if GB_SYSTEMDEVICE_HAS_CM_NOTIFICATION
                && cmNotificationHandles.empty()
#endif
                )
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemDeviceWatcher::Stop", u8"系统设备监听器未启动。");
            }

            DisableNativeNotifications();
            running = false;
            localWindowHandle = windowHandle;
            localThreadId = windowThreadId;
            if (windowThread.joinable())
            {
                localThread = std::move(windowThread);
            }
#if GB_SYSTEMDEVICE_HAS_CM_NOTIFICATION
            localCmNotificationHandles.swap(cmNotificationHandles);
            useCmNotification = false;
#endif
        }

#if GB_SYSTEMDEVICE_HAS_CM_NOTIFICATION
        UnregisterCmNotifications(localCmNotificationHandles);
#endif
        WaitNativeNotificationsFinished();

        bool stopMessagePosted = false;
        if (localWindowHandle != nullptr && ::IsWindow(localWindowHandle))
        {
            stopMessagePosted = ::PostMessageW(localWindowHandle, WM_CLOSE, 0, 0) != FALSE;
        }
        if (!stopMessagePosted && localThreadId != 0)
        {
            (void)::PostThreadMessageW(localThreadId, WM_QUIT, 0, 0);
        }

        if (localThread.joinable())
        {
            localThread.join();
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            windowHandle = nullptr;
            windowThreadId = 0;
            startCompleted = false;
        }

        (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Drain);
        (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Drain);
        return GB_SystemResult::Succeeded(u8"GB_SystemDeviceWatcher::Stop", u8"系统设备监听器已停止。");
#endif
    }

    bool IsRunning() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return running;
    }

    void SetDeviceEventCallback(const GB_SystemDeviceWatcher::DeviceEventCallback& callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        deviceEventCallback = callback;
    }

    GB_EventDispatcher& GetEventDispatcher()
    {
        return publicDispatcher;
    }

private:
    void EnableNativeNotifications() noexcept
    {
        acceptingNativeNotifications.store(true, std::memory_order_release);
    }

    void DisableNativeNotifications() noexcept
    {
        acceptingNativeNotifications.store(false, std::memory_order_release);
    }

    bool TryEnterNativeNotification() noexcept
    {
        if (!acceptingNativeNotifications.load(std::memory_order_acquire))
        {
            return false;
        }

        activeNativeNotificationCount.fetch_add(1, std::memory_order_acq_rel);
        if (!acceptingNativeNotifications.load(std::memory_order_acquire))
        {
            LeaveNativeNotification();
            return false;
        }

        return true;
    }

    void LeaveNativeNotification() noexcept
    {
        if (activeNativeNotificationCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            notificationCondition.notify_all();
        }
    }

    void WaitNativeNotificationsFinished()
    {
        std::unique_lock<std::mutex> lock(notificationMutex);
        notificationCondition.wait(lock, [this]()
            {
                return activeNativeNotificationCount.load(std::memory_order_acquire) == 0;
            });
    }

    void DispatchTypedCallback(const GB_Event& event)
    {
        const GB_SystemDeviceEvent* deviceEvent = event.payload.AnyCast<GB_SystemDeviceEvent>();
        if (deviceEvent == nullptr)
        {
            return;
        }

        GB_SystemDeviceWatcher::DeviceEventCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callback = deviceEventCallback;
        }

        if (callback)
        {
            callback(*deviceEvent);
        }
    }

#if defined(_WIN32)
    void FillCommonEventFields(const GB_SystemDeviceEventType eventType, const std::string& sourceName, GB_SystemDeviceEvent& event)
    {
        event.eventType = eventType;
        event.eventName = GetDeviceEventName(eventType);
        event.sourceName = sourceName;
        event.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
    }

#if GB_SYSTEMDEVICE_HAS_CM_NOTIFICATION
    GB_SystemResult TryStartCmNotifications()
    {
        CmRegisterNotificationFunc registerNotificationFunc = GetCmRegisterNotificationFunc();
        if (registerNotificationFunc == nullptr)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, u8"GB_SystemDeviceWatcher::Start", u8"当前运行环境没有 CM_Register_Notification 入口。");
        }

        std::vector<HCMNOTIFICATION> registeredHandles;
        try
        {
            registeredHandles.reserve(2);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDeviceWatcher::Start", u8"分配设备通知句柄缓存失败。");
        }

        CM_NOTIFY_FILTER deviceInstanceFilter = {};
        deviceInstanceFilter.cbSize = sizeof(deviceInstanceFilter);
        deviceInstanceFilter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE;
        deviceInstanceFilter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_DEVICE_INSTANCES;
        deviceInstanceFilter.u.DeviceInstance.InstanceId[0] = L'\0';

        HCMNOTIFICATION deviceInstanceNotification = nullptr;
        CONFIGRET configResult = registerNotificationFunc(&deviceInstanceFilter, this, &Impl::CmNotificationCallback, &deviceInstanceNotification);
        if (configResult != CR_SUCCESS)
        {
            return ConfigRetToResult(configResult, u8"GB_SystemDeviceWatcher::Start", u8"CM_Register_Notification 注册全部设备实例事件失败。");
        }
        try
        {
            registeredHandles.push_back(deviceInstanceNotification);
        }
        catch (...)
        {
            CmUnregisterNotificationFunc unregisterNotificationFunc = GetCmUnregisterNotificationFunc();
            if (unregisterNotificationFunc != nullptr)
            {
                (void)unregisterNotificationFunc(deviceInstanceNotification);
            }
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDeviceWatcher::Start", u8"保存设备实例通知句柄时内存不足。");
        }

        CM_NOTIFY_FILTER deviceInterfaceFilter = {};
        deviceInterfaceFilter.cbSize = sizeof(deviceInterfaceFilter);
        deviceInterfaceFilter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        deviceInterfaceFilter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;

        HCMNOTIFICATION deviceInterfaceNotification = nullptr;
        configResult = registerNotificationFunc(&deviceInterfaceFilter, this, &Impl::CmNotificationCallback, &deviceInterfaceNotification);
        if (configResult != CR_SUCCESS)
        {
            UnregisterCmNotifications(registeredHandles);
            return ConfigRetToResult(configResult, u8"GB_SystemDeviceWatcher::Start", u8"CM_Register_Notification 注册全部设备接口事件失败。");
        }
        try
        {
            registeredHandles.push_back(deviceInterfaceNotification);
        }
        catch (...)
        {
            CmUnregisterNotificationFunc unregisterNotificationFunc = GetCmUnregisterNotificationFunc();
            if (unregisterNotificationFunc != nullptr)
            {
                (void)unregisterNotificationFunc(deviceInterfaceNotification);
            }
            UnregisterCmNotifications(registeredHandles);
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDeviceWatcher::Start", u8"保存设备接口通知句柄时内存不足。");
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            cmNotificationHandles.swap(registeredHandles);
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemDeviceWatcher::Start", u8"CM_Register_Notification 注册完成。");
    }

    static void UnregisterCmNotifications(std::vector<HCMNOTIFICATION>& notificationHandles)
    {
        CmUnregisterNotificationFunc unregisterNotificationFunc = GetCmUnregisterNotificationFunc();
        if (unregisterNotificationFunc == nullptr)
        {
            notificationHandles.clear();
            return;
        }

        for (size_t index = 0; index < notificationHandles.size(); index++)
        {
            if (notificationHandles[index] != nullptr)
            {
                (void)unregisterNotificationFunc(notificationHandles[index]);
            }
        }
        notificationHandles.clear();
    }

    static DWORD CALLBACK CmNotificationCallback(HCMNOTIFICATION notifyHandle, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA eventData, DWORD eventDataSize)
    {
        (void)notifyHandle;
        (void)eventDataSize;

        Impl* impl = static_cast<Impl*>(context);
        if (impl == nullptr)
        {
            return ERROR_SUCCESS;
        }

        if (!impl->TryEnterNativeNotification())
        {
            return ERROR_SUCCESS;
        }

        try
        {
            impl->HandleCmNotification(action, eventData);
        }
        catch (...)
        {
        }
        impl->LeaveNativeNotification();
        return ERROR_SUCCESS;
    }

    void HandleCmNotification(const CM_NOTIFY_ACTION action, const PCM_NOTIFY_EVENT_DATA eventData)
    {
        const GB_SystemDeviceEventType eventType = MapCmNotifyAction(action);
        if (eventType == GB_SystemDeviceEventType::Unknown)
        {
            return;
        }

        GB_SystemDeviceEvent event;
        FillCommonEventFields(eventType, u8"CM_Register_Notification", event);
        event.nativeAction = static_cast<uint32_t>(action);

        if (eventData != nullptr)
        {
            if (eventData->FilterType == CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE)
            {
                event.interfaceClassGuid = GuidToString(eventData->u.DeviceInterface.ClassGuid);
                event.deviceInterfacePath = WideNullTerminatedStringToUtf8(eventData->u.DeviceInterface.SymbolicLink);
            }
            else if (eventData->FilterType == CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE)
            {
                event.deviceInstanceId = WideNullTerminatedStringToUtf8(eventData->u.DeviceInstance.InstanceId);
            }
        }

        PublishEvent(event);
    }
#endif

    GB_SystemResult StartFallbackWindow()
    {
        std::unique_lock<std::mutex> lock(stateMutex);
        startCompleted = false;
        startResult = GB_SystemResult::Succeeded(u8"GB_SystemDeviceWatcher::Start");
        windowHandle = nullptr;
        windowThreadId = 0;

        try
        {
            windowThread = std::thread(&Impl::WindowThreadMain, this);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDeviceWatcher::Start", u8"创建设备监听消息线程时内存不足。");
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemDeviceWatcher::Start", u8"创建设备监听消息线程失败。");
        }

        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        const bool ready = startCondition.wait_until(lock, deadline, [this]()
            {
                return startCompleted;
            });

        if (!ready)
        {
            std::thread localThread;
            HWND localWindowHandle = windowHandle;
            const DWORD localThreadId = windowThreadId;
            if (windowThread.joinable())
            {
                localThread = std::move(windowThread);
            }
            lock.unlock();

            if (localWindowHandle != nullptr)
            {
                (void)::PostMessageW(localWindowHandle, WM_CLOSE, 0, 0);
            }
            else if (localThreadId != 0)
            {
                (void)::PostThreadMessageW(localThreadId, WM_QUIT, 0, 0);
            }
            if (localThread.joinable())
            {
                localThread.join();
            }

            std::lock_guard<std::mutex> cleanupLock(stateMutex);
            windowHandle = nullptr;
            windowThreadId = 0;
            startCompleted = false;
            return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, u8"GB_SystemDeviceWatcher::Start", u8"等待设备监听隐藏窗口创建超时。");
        }

        if (startResult.IsFailed())
        {
            GB_SystemResult failedResult = startResult;
            std::thread localThread;
            if (windowThread.joinable())
            {
                localThread = std::move(windowThread);
            }
            lock.unlock();

            if (localThread.joinable())
            {
                localThread.join();
            }

            std::lock_guard<std::mutex> cleanupLock(stateMutex);
            windowHandle = nullptr;
            windowThreadId = 0;
            startCompleted = false;
            return failedResult;
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemDeviceWatcher::Start", u8"设备监听隐藏窗口已启动。");
    }

    void MarkStartCompleted(const GB_SystemResult& result)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        startResult = result;
        startCompleted = true;
        startCondition.notify_all();
    }

    GB_SystemResult RegisterFallbackNotifications()
    {
        if (windowHandle == nullptr)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemDeviceWatcher::Start", u8"隐藏窗口句柄为空，无法注册设备接口通知。");
        }

        DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

        HDEVNOTIFY notifyHandle = ::RegisterDeviceNotificationW(windowHandle, &filter, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
        if (notifyHandle == nullptr)
        {
            return GB_SystemResult::FromLastWin32Error(u8"GB_SystemDeviceWatcher::Start", u8"RegisterDeviceNotificationW 注册全部设备接口通知失败。");
        }

        try
        {
            deviceNotifyHandles.push_back(notifyHandle);
        }
        catch (...)
        {
            (void)::UnregisterDeviceNotification(notifyHandle);
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemDeviceWatcher::Start", u8"保存设备通知句柄时内存不足。");
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemDeviceWatcher::Start", u8"RegisterDeviceNotificationW 注册完成。");
    }

    void UnregisterFallbackNotifications()
    {
        for (size_t index = 0; index < deviceNotifyHandles.size(); index++)
        {
            if (deviceNotifyHandles[index] != nullptr)
            {
                (void)::UnregisterDeviceNotification(deviceNotifyHandles[index]);
            }
        }
        deviceNotifyHandles.clear();
    }

    void WindowThreadMain() noexcept
    {
        try
        {
            WindowThreadMainImpl();
        }
        catch (...)
        {
            HWND currentWindowHandle = nullptr;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                currentWindowHandle = windowHandle;
                windowHandle = nullptr;
                windowThreadId = 0;
                if (!startCompleted)
                {
                    startResult.errorCode = GB_SystemErrorCode::InternalError;
                    startResult.errorSource = GB_NativeErrorSource::None;
                    startResult.nativeErrorCode = 0;
                    startResult.hresult = GB_SystemResult::ErrorCodeToHResult(GB_SystemErrorCode::InternalError);
                    startResult.nativeMessage.clear();
                    startCompleted = true;
                }
            }

            UnregisterFallbackNotifications();
            if (currentWindowHandle != nullptr && ::IsWindow(currentWindowHandle) != FALSE)
            {
                (void)::DestroyWindow(currentWindowHandle);
            }
            startCondition.notify_all();
        }
    }

    void WindowThreadMainImpl()
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            windowThreadId = ::GetCurrentThreadId();
        }

        const std::string classNameUtf8 = BuildWindowClassName(this);
        const std::wstring className = Utf8ToWideSafe(classNameUtf8);
        if (className.empty())
        {
            MarkStartCompleted(GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, u8"GB_SystemDeviceWatcher::Start", u8"隐藏窗口类名从 UTF-8 转 UTF-16 失败。"));
            return;
        }

        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Impl::WindowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className.c_str();

        if (::RegisterClassExW(&windowClass) == 0)
        {
            const DWORD lastError = ::GetLastError();
            MarkStartCompleted(GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemDeviceWatcher::Start", u8"RegisterClassExW 注册隐藏窗口类失败。"));
            return;
        }

        HWND createdWindowHandle = ::CreateWindowExW(0, className.c_str(), L"GB_SystemDeviceWatcher", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, windowClass.hInstance, this);
        if (createdWindowHandle == nullptr)
        {
            const DWORD lastError = ::GetLastError();
            (void)::UnregisterClassW(className.c_str(), windowClass.hInstance);
            MarkStartCompleted(GB_SystemResult::FromWin32Error(lastError, u8"GB_SystemDeviceWatcher::Start", u8"CreateWindowExW 创建隐藏窗口失败。"));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            windowHandle = createdWindowHandle;
        }

        const GB_SystemResult registerResult = RegisterFallbackNotifications();
        if (registerResult.IsFailed())
        {
            if (::IsWindow(createdWindowHandle))
            {
                ::DestroyWindow(createdWindowHandle);
            }
            (void)::UnregisterClassW(className.c_str(), windowClass.hInstance);
            MarkStartCompleted(registerResult);
            return;
        }

        MarkStartCompleted(GB_SystemResult::Succeeded(u8"GB_SystemDeviceWatcher::Start", u8"设备监听隐藏窗口创建完成。"));

        MSG message = {};
        while (::GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        UnregisterFallbackNotifications();
        if (::IsWindow(createdWindowHandle))
        {
            ::DestroyWindow(createdWindowHandle);
        }
        (void)::UnregisterClassW(className.c_str(), windowClass.hInstance);

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (windowHandle == createdWindowHandle)
            {
                windowHandle = nullptr;
            }
            windowThreadId = 0;
        }
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        try
        {
            Impl* impl = nullptr;
            if (message == WM_NCCREATE)
            {
                CREATESTRUCTW* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
                impl = createStruct == nullptr ? nullptr : static_cast<Impl*>(createStruct->lpCreateParams);
                if (impl != nullptr)
                {
                    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
                }
            }
            else
            {
                impl = reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            }

            if (impl != nullptr)
            {
                return impl->HandleWindowMessage(hwnd, message, wParam, lParam);
            }
        }
        catch (...)
        {
        }

        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_DEVICECHANGE:
            if (!TryEnterNativeNotification())
            {
                return TRUE;
            }
            try
            {
                HandleDeviceChange(message, wParam, lParam);
            }
            catch (...)
            {
            }
            LeaveNativeNotification();
            return TRUE;
        case WM_CLOSE:
            UnregisterFallbackNotifications();
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
        }

        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void HandleDeviceChange(const UINT message, const WPARAM wParam, const LPARAM lParam)
    {
        const GB_SystemDeviceEventType eventType = MapDeviceBroadcastEventType(wParam);
        if (eventType == GB_SystemDeviceEventType::Unknown)
        {
            return;
        }

        GB_SystemDeviceEvent event;
        FillCommonEventFields(eventType, u8"RegisterDeviceNotification", event);
        event.nativeAction = static_cast<uint32_t>(wParam);
        event.nativeMessage = static_cast<uint32_t>(message);
        event.nativeWParam = static_cast<uint64_t>(wParam);

        const DEV_BROADCAST_HDR* broadcastHeader = reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);
        if (broadcastHeader != nullptr)
        {
            if (broadcastHeader->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE)
            {
                if (wParam != DBT_DEVNODES_CHANGED)
                {
                    return;
                }
            }
            else
            {
                const DEV_BROADCAST_DEVICEINTERFACE_W* deviceInterface = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(broadcastHeader);
                event.deviceInterfacePath = WideNullTerminatedStringToUtf8(deviceInterface->dbcc_name);
                event.interfaceClassGuid = GuidToString(deviceInterface->dbcc_classguid);
            }
        }

        PublishEvent(event);
    }

    void AddCommonAttributes(const GB_SystemDeviceEvent& deviceEvent, GB_Event& event)
    {
        event.SetAttribute("eventType", GB_Variant(static_cast<unsigned int>(deviceEvent.eventType)));
        event.SetAttribute("eventTypeName", GB_Variant(GB_SystemDevice::GetDeviceEventTypeName(deviceEvent.eventType)));
        event.SetAttribute("sourceName", GB_Variant(deviceEvent.sourceName));
        event.SetAttribute("deviceInterfacePath", GB_Variant(deviceEvent.deviceInterfacePath));
        event.SetAttribute("deviceInstanceId", GB_Variant(deviceEvent.deviceInstanceId));
        event.SetAttribute("interfaceClassGuid", GB_Variant(deviceEvent.interfaceClassGuid));
        event.SetAttribute("nativeAction", GB_Variant(deviceEvent.nativeAction));
        event.SetAttribute("nativeMessage", GB_Variant(deviceEvent.nativeMessage));
        event.SetAttribute("nativeWParam", GB_Variant(static_cast<unsigned long long>(deviceEvent.nativeWParam)));
    }

    GB_Event BuildPublicEvent(const GB_SystemDeviceEvent& deviceEvent)
    {
        GB_Event event(deviceEvent.eventName, GB_SystemDevice::GetDeviceEventTypeName(deviceEvent.eventType), deviceEvent.sourceName.empty() ? u8"GB_SystemDeviceWatcher" : deviceEvent.sourceName);
        event.timestampMilliseconds = deviceEvent.timestampMilliseconds;
        AddCommonAttributes(deviceEvent, event);
        return event;
    }

    void PublishEvent(const GB_SystemDeviceEvent& deviceEvent) noexcept
    {
        try
        {
            GB_Event typedEvent(deviceEvent.eventName, GB_Variant(deviceEvent), deviceEvent.sourceName.empty() ? u8"GB_SystemDeviceWatcher" : deviceEvent.sourceName);
            typedEvent.timestampMilliseconds = deviceEvent.timestampMilliseconds;
            (void)typedDispatcher.Post(typedEvent);
            (void)publicDispatcher.Post(BuildPublicEvent(deviceEvent));
        }
        catch (...)
        {
        }
    }
#endif

private:
    mutable std::mutex operationMutex;
    mutable std::mutex stateMutex;
    mutable std::mutex callbackMutex;
    mutable std::mutex notificationMutex;
    std::condition_variable startCondition;
    std::condition_variable notificationCondition;
    std::atomic<bool> acceptingNativeNotifications;
    std::atomic<size_t> activeNativeNotificationCount;
    std::thread windowThread;
    bool running = false;
    bool startCompleted = false;
    bool useCmNotification = false;
    GB_SystemResult startResult;
    GB_SystemDeviceWatcher::DeviceEventCallback deviceEventCallback;
    GB_EventDispatcher typedDispatcher;
    GB_EventDispatcher publicDispatcher;
    GB_EventSubscriptionToken typedSubscriptionToken;

#if defined(_WIN32)
    HWND windowHandle = nullptr;
    DWORD windowThreadId = 0;
    std::vector<HDEVNOTIFY> deviceNotifyHandles;
#if GB_SYSTEMDEVICE_HAS_CM_NOTIFICATION
    std::vector<HCMNOTIFICATION> cmNotificationHandles;
#endif
#endif
};

GB_SystemDeviceWatcher::GB_SystemDeviceWatcher() : impl(new Impl())
{
}

GB_SystemDeviceWatcher::~GB_SystemDeviceWatcher() noexcept
{
    if (impl)
    {
        (void)impl->Stop();
    }
}

GB_SystemResult GB_SystemDeviceWatcher::Start()
{
    return impl->Start();
}

GB_SystemResult GB_SystemDeviceWatcher::Stop()
{
    return impl->Stop();
}

bool GB_SystemDeviceWatcher::IsRunning() const
{
    return impl->IsRunning();
}

void GB_SystemDeviceWatcher::SetDeviceEventCallback(const DeviceEventCallback& callback)
{
    impl->SetDeviceEventCallback(callback);
}

GB_EventDispatcher& GB_SystemDeviceWatcher::GetEventDispatcher()
{
    return impl->GetEventDispatcher();
}
