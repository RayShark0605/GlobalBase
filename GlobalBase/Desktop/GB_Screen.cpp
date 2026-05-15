#include "GB_Screen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <cstring>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <shellscalingapi.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#pragma comment(lib, "Setupapi.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#endif

namespace internal
{
#if defined(_WIN32)
    struct EdidInfo
    {
        std::string manufacturerCode;
        std::string brandName;
        std::string productName;
        std::string serialNumber;
        double physicalWidthMm = 0.0;
        double physicalHeightMm = 0.0;
    };

    struct RuntimeMonitorInfo
    {
        HMONITOR monitorHandle = nullptr;
        RECT monitorRect = { 0, 0, 0, 0 };
        bool isPrimary = false;
        UINT effectiveDpiX = 96;
        UINT effectiveDpiY = 96;
        UINT rawDpiX = 0;
        UINT rawDpiY = 0;
    };

    struct PathScreenInfo
    {
        std::wstring gdiDeviceName;
        std::wstring monitorDevicePath;
        std::wstring adapterDevicePath;
        std::wstring monitorFriendlyName;
        int currentPixelWidth = 0;
        int currentPixelHeight = 0;
        int preferredPixelWidth = 0;
        int preferredPixelHeight = 0;
        double refreshRateHz = 0.0;
        bool isInternal = false;
        LUID adapterId = { 0, 0 };
        UINT32 sourceId = 0;
        UINT32 targetId = 0;
    };

    static const GUID kGuidDevinterfaceMonitor = { 0xe6f07b5f, 0xee97, 0x4a90, { 0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7 } };

    class SetupDiInfoSetScope
    {
    public:
        explicit SetupDiInfoSetScope(HDEVINFO deviceInfoSet) : deviceInfoSet(deviceInfoSet)
        {
        }

        ~SetupDiInfoSetScope()
        {
            if (deviceInfoSet != INVALID_HANDLE_VALUE)
            {
                ::SetupDiDestroyDeviceInfoList(deviceInfoSet);
                deviceInfoSet = INVALID_HANDLE_VALUE;
            }
        }

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
                ::RegCloseKey(keyHandle);
                keyHandle = nullptr;
            }
        }

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

    class ScreenDcScope
    {
    public:
        ScreenDcScope()
        {
            screenDc = ::GetDC(nullptr);
        }

        ~ScreenDcScope()
        {
            if (screenDc != nullptr)
            {
                ::ReleaseDC(nullptr, screenDc);
                screenDc = nullptr;
            }
        }

        HDC Get() const
        {
            return screenDc;
        }

        bool IsValid() const
        {
            return screenDc != nullptr;
        }

    private:
        HDC screenDc = nullptr;
    };

    class CompatibleDcScope
    {
    public:
        explicit CompatibleDcScope(HDC sourceDc)
        {
            memoryDc = ::CreateCompatibleDC(sourceDc);
        }

        ~CompatibleDcScope()
        {
            if (memoryDc != nullptr)
            {
                ::DeleteDC(memoryDc);
                memoryDc = nullptr;
            }
        }

        HDC Get() const
        {
            return memoryDc;
        }

        bool IsValid() const
        {
            return memoryDc != nullptr;
        }

    private:
        HDC memoryDc = nullptr;
    };

    class BitmapScope
    {
    public:
        explicit BitmapScope(HBITMAP bitmapHandle) : bitmapHandle(bitmapHandle)
        {
        }

        ~BitmapScope()
        {
            if (bitmapHandle != nullptr)
            {
                ::DeleteObject(bitmapHandle);
                bitmapHandle = nullptr;
            }
        }

        HBITMAP Get() const
        {
            return bitmapHandle;
        }

        bool IsValid() const
        {
            return bitmapHandle != nullptr;
        }

    private:
        HBITMAP bitmapHandle = nullptr;
    };

    class IconBitmapScope
    {
    public:
        explicit IconBitmapScope(HBITMAP bitmapHandle) : bitmapHandle(bitmapHandle)
        {
        }

        ~IconBitmapScope()
        {
            if (bitmapHandle != nullptr)
            {
                ::DeleteObject(bitmapHandle);
                bitmapHandle = nullptr;
            }
        }

        HBITMAP Get() const
        {
            return bitmapHandle;
        }

    private:
        HBITMAP bitmapHandle = nullptr;
    };


    class SelectObjectScope
    {
    public:
        SelectObjectScope(HDC dcHandle, HGDIOBJ objectHandle) : dcHandle(dcHandle)
        {
            if (dcHandle != nullptr && objectHandle != nullptr)
            {
                const HGDIOBJ oldObject = ::SelectObject(dcHandle, objectHandle);
                if (oldObject != nullptr && oldObject != HGDI_ERROR)
                {
                    oldObjectHandle = oldObject;
                    isValid = true;
                }
            }
        }

        ~SelectObjectScope()
        {
            if (isValid && dcHandle != nullptr)
            {
                (void)::SelectObject(dcHandle, oldObjectHandle);
            }
        }

        bool IsValid() const
        {
            return isValid;
        }

    private:
        HDC dcHandle = nullptr;
        HGDIOBJ oldObjectHandle = nullptr;
        bool isValid = false;
    };

    class DpiAwarenessScope
    {
    public:
        DpiAwarenessScope()
        {
            const HMODULE user32Module = ::GetModuleHandleW(L"user32.dll");
            if (user32Module == nullptr)
            {
                return;
            }

            const auto setThreadDpiAwarenessContext = reinterpret_cast<SetThreadDpiAwarenessContextFunction>(::GetProcAddress(user32Module, "SetThreadDpiAwarenessContext"));
            if (setThreadDpiAwarenessContext == nullptr)
            {
                return;
            }

            setThreadDpiAwarenessContextFunction = setThreadDpiAwarenessContext;
            previousContext = setThreadDpiAwarenessContextFunction(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            isActive = (previousContext != nullptr);
        }

        ~DpiAwarenessScope()
        {
            if (isActive && setThreadDpiAwarenessContextFunction != nullptr)
            {
                (void)setThreadDpiAwarenessContextFunction(previousContext);
            }
        }

    private:
        using SetThreadDpiAwarenessContextFunction = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT dpiContext);

        SetThreadDpiAwarenessContextFunction setThreadDpiAwarenessContextFunction = nullptr;
        DPI_AWARENESS_CONTEXT previousContext = nullptr;
        bool isActive = false;
    };

    static std::wstring TrimWideString(const std::wstring& text)
    {
        if (text.empty())
        {
            return std::wstring();
        }

        size_t beginIndex = 0;
        size_t endIndex = text.size();

        while (beginIndex < text.size() && (text[beginIndex] == L' ' || text[beginIndex] == L'\t' || text[beginIndex] == L'\r' || text[beginIndex] == L'\n' || text[beginIndex] == L'\0'))
        {
            beginIndex++;
        }

        while (endIndex > beginIndex && (text[endIndex - 1] == L' ' || text[endIndex - 1] == L'\t' || text[endIndex - 1] == L'\r' || text[endIndex - 1] == L'\n' || text[endIndex - 1] == L'\0'))
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    static std::string TrimString(const std::string& text)
    {
        if (text.empty())
        {
            return std::string();
        }

        size_t beginIndex = 0;
        size_t endIndex = text.size();

        while (beginIndex < text.size() && (text[beginIndex] == ' ' || text[beginIndex] == '\t' || text[beginIndex] == '\r' || text[beginIndex] == '\n' || text[beginIndex] == '\0'))
        {
            beginIndex++;
        }

        while (endIndex > beginIndex && (text[endIndex - 1] == ' ' || text[endIndex - 1] == '\t' || text[endIndex - 1] == '\r' || text[endIndex - 1] == '\n' || text[endIndex - 1] == '\0'))
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    static std::string ToLowerAsciiString(std::string text)
    {
        for (size_t i = 0; i < text.size(); i++)
        {
            const unsigned char currentChar = static_cast<unsigned char>(text[i]);
            if (currentChar >= 'A' && currentChar <= 'Z')
            {
                text[i] = static_cast<char>(currentChar - 'A' + 'a');
            }
        }

        return text;
    }

    static std::wstring Utf8ToWideString(const std::string& text)
    {
        if (text.empty())
        {
            return std::wstring();
        }

        const int requiredLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (requiredLength <= 0)
        {
            return std::wstring();
        }

        std::wstring result(static_cast<size_t>(requiredLength), L'\0');
        const int convertedLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), &result[0], requiredLength);
        if (convertedLength != requiredLength)
        {
            return std::wstring();
        }

        return result;
    }

    static std::string WideStringToUtf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return std::string();
        }

        const int requiredLength = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (requiredLength <= 0)
        {
            return std::string();
        }

        std::string result(static_cast<size_t>(requiredLength), '\0');
        const int convertedLength = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &result[0], requiredLength, nullptr, nullptr);
        if (convertedLength != requiredLength)
        {
            return std::string();
        }

        return result;
    }

    static std::wstring ToLowerWideString(std::wstring text)
    {
        for (size_t i = 0; i < text.size(); i++)
        {
            text[i] = static_cast<wchar_t>(::towlower(text[i]));
        }
        return text;
    }

    static std::wstring NormalizeDevicePath(std::wstring devicePath)
    {
        devicePath = TrimWideString(devicePath);
        devicePath = ToLowerWideString(devicePath);

        for (size_t i = 0; i < devicePath.size(); i++)
        {
            if (devicePath[i] == L'/')
            {
                devicePath[i] = L'\\';
            }
        }

        return devicePath;
    }

    static double RationalToDouble(const DISPLAYCONFIG_RATIONAL& rational)
    {
        if (rational.Denominator == 0)
        {
            return 0.0;
        }

        return static_cast<double>(rational.Numerator) / static_cast<double>(rational.Denominator);
    }

    static bool IsInternalOutputTechnology(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY outputTechnology)
    {
        switch (outputTechnology)
        {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
            return true;
        default:
            break;
        }

        return false;
    }

    static bool TryGetSourceDeviceName(const LUID& adapterId, UINT32 sourceId, std::wstring& gdiDeviceName)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceDeviceName = {};
        sourceDeviceName.header.size = sizeof(sourceDeviceName);
        sourceDeviceName.header.adapterId = adapterId;
        sourceDeviceName.header.id = sourceId;
        sourceDeviceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;

        if (::DisplayConfigGetDeviceInfo(&sourceDeviceName.header) != ERROR_SUCCESS)
        {
            return false;
        }

        gdiDeviceName = sourceDeviceName.viewGdiDeviceName;
        return !gdiDeviceName.empty();
    }

    static bool TryGetTargetDeviceName(const LUID& adapterId, UINT32 targetId, std::wstring& monitorFriendlyName, std::wstring& monitorDevicePath)
    {
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetDeviceName = {};
        targetDeviceName.header.size = sizeof(targetDeviceName);
        targetDeviceName.header.adapterId = adapterId;
        targetDeviceName.header.id = targetId;
        targetDeviceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;

        if (::DisplayConfigGetDeviceInfo(&targetDeviceName.header) != ERROR_SUCCESS)
        {
            return false;
        }

        monitorFriendlyName = targetDeviceName.monitorFriendlyDeviceName;
        monitorDevicePath = targetDeviceName.monitorDevicePath;
        return true;
    }

    static bool TryGetAdapterDevicePath(const LUID& adapterId, std::wstring& adapterDevicePath)
    {
        DISPLAYCONFIG_ADAPTER_NAME adapterName = {};
        adapterName.header.size = sizeof(adapterName);
        adapterName.header.adapterId = adapterId;
        adapterName.header.id = 0;
        adapterName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;

        if (::DisplayConfigGetDeviceInfo(&adapterName.header) != ERROR_SUCCESS)
        {
            return false;
        }

        adapterDevicePath = adapterName.adapterDevicePath;
        return !adapterDevicePath.empty();
    }

    static bool TryGetTargetPreferredMode(const LUID& adapterId, UINT32 targetId, int& preferredPixelWidth, int& preferredPixelHeight)
    {
        preferredPixelWidth = 0;
        preferredPixelHeight = 0;

        DISPLAYCONFIG_TARGET_PREFERRED_MODE preferredMode = {};
        preferredMode.header.size = sizeof(preferredMode);
        preferredMode.header.adapterId = adapterId;
        preferredMode.header.id = targetId;
        preferredMode.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;

        if (::DisplayConfigGetDeviceInfo(&preferredMode.header) != ERROR_SUCCESS)
        {
            return false;
        }

        preferredPixelWidth = static_cast<int>(preferredMode.width);
        preferredPixelHeight = static_cast<int>(preferredMode.height);
        return preferredPixelWidth > 0 && preferredPixelHeight > 0;
    }

    struct ShcoreApi
    {
        using GetDpiForMonitorFunction = HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);

        HMODULE moduleHandle = nullptr;
        GetDpiForMonitorFunction getDpiForMonitorFunction = nullptr;

        ShcoreApi()
        {
            moduleHandle = ::GetModuleHandleW(L"Shcore.dll");
            if (moduleHandle == nullptr)
            {
                moduleHandle = ::LoadLibraryW(L"Shcore.dll");
            }

            if (moduleHandle != nullptr)
            {
                getDpiForMonitorFunction = reinterpret_cast<GetDpiForMonitorFunction>(::GetProcAddress(moduleHandle, "GetDpiForMonitor"));
            }
        }
    };

    static const ShcoreApi& GetShcoreApi()
    {
        static const ShcoreApi shcoreApi;
        return shcoreApi;
    }

    static bool TryGetMonitorDpi(HMONITOR monitorHandle, MONITOR_DPI_TYPE dpiType, UINT& dpiX, UINT& dpiY)
    {
        dpiX = 0;
        dpiY = 0;

        const ShcoreApi& shcoreApi = GetShcoreApi();
        if (shcoreApi.getDpiForMonitorFunction == nullptr)
        {
            return false;
        }

        const HRESULT result = shcoreApi.getDpiForMonitorFunction(monitorHandle, dpiType, &dpiX, &dpiY);
        return SUCCEEDED(result);
    }

    static void FillFallbackMonitorDpi(HMONITOR monitorHandle, UINT& effectiveDpiX, UINT& effectiveDpiY, UINT& rawDpiX, UINT& rawDpiY)
    {
        effectiveDpiX = 96;
        effectiveDpiY = 96;
        rawDpiX = 0;
        rawDpiY = 0;

        if (TryGetMonitorDpi(monitorHandle, MDT_EFFECTIVE_DPI, effectiveDpiX, effectiveDpiY))
        {
            (void)TryGetMonitorDpi(monitorHandle, MDT_RAW_DPI, rawDpiX, rawDpiY);
            return;
        }

        ScreenDcScope screenDc;
        if (!screenDc.IsValid())
        {
            return;
        }

        const int logPixelsX = ::GetDeviceCaps(screenDc.Get(), LOGPIXELSX);
        const int logPixelsY = ::GetDeviceCaps(screenDc.Get(), LOGPIXELSY);
        if (logPixelsX > 0)
        {
            effectiveDpiX = static_cast<UINT>(logPixelsX);
        }
        if (logPixelsY > 0)
        {
            effectiveDpiY = static_cast<UINT>(logPixelsY);
        }
    }

    static BOOL CALLBACK MonitorEnumProc(HMONITOR monitorHandle, HDC, LPRECT, LPARAM userData)
    {
        if (userData == 0)
        {
            return TRUE;
        }

        MONITORINFOEXW monitorInfo = {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!::GetMonitorInfoW(monitorHandle, &monitorInfo))
        {
            return TRUE;
        }

        UINT effectiveDpiX = 96;
        UINT effectiveDpiY = 96;
        UINT rawDpiX = 0;
        UINT rawDpiY = 0;
        FillFallbackMonitorDpi(monitorHandle, effectiveDpiX, effectiveDpiY, rawDpiX, rawDpiY);

        RuntimeMonitorInfo runtimeMonitorInfo;
        runtimeMonitorInfo.monitorHandle = monitorHandle;
        runtimeMonitorInfo.monitorRect = monitorInfo.rcMonitor;
        runtimeMonitorInfo.isPrimary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
        runtimeMonitorInfo.effectiveDpiX = effectiveDpiX;
        runtimeMonitorInfo.effectiveDpiY = effectiveDpiY;
        runtimeMonitorInfo.rawDpiX = rawDpiX;
        runtimeMonitorInfo.rawDpiY = rawDpiY;

        std::map<std::wstring, RuntimeMonitorInfo>* runtimeMonitorMap = reinterpret_cast<std::map<std::wstring, RuntimeMonitorInfo>*>(userData);
        (*runtimeMonitorMap)[monitorInfo.szDevice] = runtimeMonitorInfo;
        return TRUE;
    }

    static std::map<std::wstring, RuntimeMonitorInfo> CollectRuntimeMonitors()
    {
        std::map<std::wstring, RuntimeMonitorInfo> runtimeMonitorMap;
        (void)::EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&runtimeMonitorMap));
        return runtimeMonitorMap;
    }

    static bool TryReadRegistryBinaryValue(HKEY keyHandle, const wchar_t* valueName, std::vector<uint8_t>& valueData)
    {
        valueData.clear();
        if (keyHandle == nullptr || valueName == nullptr)
        {
            return false;
        }

        DWORD valueType = 0;
        DWORD valueSize = 0;
        LONG queryResult = ::RegQueryValueExW(keyHandle, valueName, nullptr, &valueType, nullptr, &valueSize);
        if (queryResult != ERROR_SUCCESS || valueType != REG_BINARY || valueSize == 0)
        {
            return false;
        }

        valueData.resize(static_cast<size_t>(valueSize));
        queryResult = ::RegQueryValueExW(keyHandle, valueName, nullptr, &valueType, valueData.data(), &valueSize);
        if (queryResult != ERROR_SUCCESS || valueType != REG_BINARY || valueSize == 0)
        {
            valueData.clear();
            return false;
        }

        valueData.resize(static_cast<size_t>(valueSize));
        return true;
    }

    static char DecodeEdidManufacturerCharacter(const uint16_t manufacturerWord, const int bitShift)
    {
        const uint16_t encodedCharacter = static_cast<uint16_t>((manufacturerWord >> bitShift) & 0x1F);
        if (encodedCharacter < 1 || encodedCharacter > 26)
        {
            return '\0';
        }

        return static_cast<char>('A' + encodedCharacter - 1);
    }

    static std::string ParseEdidManufacturerCode(const std::vector<uint8_t>& edidBytes)
    {
        if (edidBytes.size() < 10)
        {
            return std::string();
        }

        const uint16_t manufacturerWord = static_cast<uint16_t>((static_cast<uint16_t>(edidBytes[8]) << 8) | static_cast<uint16_t>(edidBytes[9]));
        const char firstCharacter = DecodeEdidManufacturerCharacter(manufacturerWord, 10);
        const char secondCharacter = DecodeEdidManufacturerCharacter(manufacturerWord, 5);
        const char thirdCharacter = DecodeEdidManufacturerCharacter(manufacturerWord, 0);
        if (firstCharacter == '\0' || secondCharacter == '\0' || thirdCharacter == '\0')
        {
            return std::string();
        }

        const char manufacturerCode[4] = { firstCharacter, secondCharacter, thirdCharacter, 0 };
        return std::string(manufacturerCode);
    }

    static std::string MapManufacturerCodeToBrandName(const std::string& manufacturerCode)
    {
        static const std::unordered_map<std::string, std::string> manufacturerNameMap =
        {
            { "ACR", "Acer" },
            { "AOC", "AOC" },
            { "APP", "Apple" },
            { "AUS", "ASUS" },
            { "AUO", "AU Optronics" },
            { "BNQ", "BenQ" },
            { "BOE", "BOE" },
            { "CMN", "Innolux" },
            { "DEL", "Dell" },
            { "EIZ", "EIZO" },
            { "GSM", "LG" },
            { "HEW", "HP" },
            { "HWP", "HP" },
            { "IVM", "iiyama" },
            { "LEN", "Lenovo" },
            { "MSH", "Microsoft" },
            { "PHL", "Philips" },
            { "SAM", "Samsung" },
            { "SNY", "Sony" },
            { "VSC", "ViewSonic" }
        };

        const auto iterator = manufacturerNameMap.find(manufacturerCode);
        if (iterator != manufacturerNameMap.end())
        {
            return iterator->second;
        }

        return manufacturerCode;
    }

    static std::string ExtractAsciiTextFromEdidDescriptor(const uint8_t* descriptorData, size_t descriptorSize)
    {
        if (descriptorData == nullptr || descriptorSize < 18)
        {
            return std::string();
        }

        std::string text;
        text.reserve(13);
        for (size_t i = 5; i < 18; i++)
        {
            const uint8_t value = descriptorData[i];
            if (value == 0x0A || value == 0x00)
            {
                break;
            }

            if (value >= 32 && value <= 126)
            {
                text.push_back(static_cast<char>(value));
            }
        }

        return TrimString(text);
    }

    static void ParseEdidDetailedTimingPhysicalSize(const uint8_t* descriptorData, size_t descriptorSize, double& physicalWidthMm, double& physicalHeightMm)
    {
        if (descriptorData == nullptr || descriptorSize < 18)
        {
            return;
        }

        const bool isDetailedTiming = !(descriptorData[0] == 0x00 && descriptorData[1] == 0x00);
        if (!isDetailedTiming)
        {
            return;
        }

        const int widthMm = static_cast<int>(descriptorData[12]) | ((static_cast<int>(descriptorData[14]) & 0xF0) << 4);
        const int heightMm = static_cast<int>(descriptorData[13]) | ((static_cast<int>(descriptorData[14]) & 0x0F) << 8);
        if (physicalWidthMm <= 0.0 && widthMm > 0)
        {
            physicalWidthMm = static_cast<double>(widthMm);
        }
        if (physicalHeightMm <= 0.0 && heightMm > 0)
        {
            physicalHeightMm = static_cast<double>(heightMm);
        }
    }

    static bool IsValidEdidBaseBlock(const std::vector<uint8_t>& edidBytes)
    {
        static const uint8_t kEdidHeader[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

        if (edidBytes.size() < 128)
        {
            return false;
        }

        for (size_t i = 0; i < 8; i++)
        {
            if (edidBytes[i] != kEdidHeader[i])
            {
                return false;
            }
        }

        uint32_t checksum = 0;
        for (size_t i = 0; i < 128; i++)
        {
            checksum += static_cast<uint32_t>(edidBytes[i]);
        }

        return (checksum & 0xFFu) == 0u;
    }

    static bool ParseEdid(const std::vector<uint8_t>& edidBytes, EdidInfo& edidInfo)
    {
        edidInfo = EdidInfo();
        if (!IsValidEdidBaseBlock(edidBytes))
        {
            return false;
        }

        edidInfo.manufacturerCode = ParseEdidManufacturerCode(edidBytes);
        edidInfo.brandName = MapManufacturerCodeToBrandName(edidInfo.manufacturerCode);

        const int basicWidthCm = static_cast<int>(edidBytes[21]);
        const int basicHeightCm = static_cast<int>(edidBytes[22]);
        if (basicWidthCm > 0)
        {
            edidInfo.physicalWidthMm = static_cast<double>(basicWidthCm) * 10.0;
        }
        if (basicHeightCm > 0)
        {
            edidInfo.physicalHeightMm = static_cast<double>(basicHeightCm) * 10.0;
        }

        for (size_t i = 54; i + 18 <= edidBytes.size() && i < 126; i += 18)
        {
            const uint8_t* descriptorData = &edidBytes[i];
            ParseEdidDetailedTimingPhysicalSize(descriptorData, 18, edidInfo.physicalWidthMm, edidInfo.physicalHeightMm);

            if (!(descriptorData[0] == 0x00 && descriptorData[1] == 0x00))
            {
                continue;
            }

            const uint8_t descriptorType = descriptorData[3];
            if (descriptorType == 0xFC)
            {
                edidInfo.productName = ExtractAsciiTextFromEdidDescriptor(descriptorData, 18);
            }
            else if (descriptorType == 0xFF)
            {
                edidInfo.serialNumber = ExtractAsciiTextFromEdidDescriptor(descriptorData, 18);
            }
        }

        return true;
    }

    static bool TryReadEdidByDeviceInfoData(HDEVINFO deviceInfoSetHandle, SP_DEVINFO_DATA& deviceInfoData, EdidInfo& edidInfo)
    {
        edidInfo = EdidInfo();
        if (deviceInfoSetHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        RegKeyScope deviceRegistryKey(::SetupDiOpenDevRegKey(deviceInfoSetHandle, &deviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ));
        if (!deviceRegistryKey.IsValid())
        {
            return false;
        }

        std::vector<uint8_t> edidBytes;
        if (!TryReadRegistryBinaryValue(deviceRegistryKey.Get(), L"EDID", edidBytes))
        {
            HKEY deviceParametersKeyHandle = nullptr;
            if (::RegOpenKeyExW(deviceRegistryKey.Get(), L"Device Parameters", 0, KEY_READ, &deviceParametersKeyHandle) != ERROR_SUCCESS)
            {
                return false;
            }

            RegKeyScope deviceParametersKey(deviceParametersKeyHandle);
            if (!TryReadRegistryBinaryValue(deviceParametersKey.Get(), L"EDID", edidBytes))
            {
                return false;
            }
        }

        return ParseEdid(edidBytes, edidInfo);
    }

    static std::unordered_map<std::wstring, EdidInfo> CollectEdidInfoByMonitorDevicePath()
    {
        std::unordered_map<std::wstring, EdidInfo> edidInfoMap;

        SetupDiInfoSetScope deviceInfoSet(::SetupDiGetClassDevsW(&kGuidDevinterfaceMonitor, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT));
        if (!deviceInfoSet.IsValid())
        {
            return edidInfoMap;
        }

        for (DWORD interfaceIndex = 0;; interfaceIndex++)
        {
            SP_DEVICE_INTERFACE_DATA interfaceData = {};
            interfaceData.cbSize = sizeof(interfaceData);
            if (!::SetupDiEnumDeviceInterfaces(deviceInfoSet.Get(), nullptr, &kGuidDevinterfaceMonitor, interfaceIndex, &interfaceData))
            {
                if (::GetLastError() == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }
                continue;
            }

            DWORD requiredSize = 0;
            SP_DEVINFO_DATA deviceInfoData = {};
            deviceInfoData.cbSize = sizeof(deviceInfoData);
            (void)::SetupDiGetDeviceInterfaceDetailW(deviceInfoSet.Get(), &interfaceData, nullptr, 0, &requiredSize, &deviceInfoData);
            if (requiredSize == 0)
            {
                continue;
            }

            std::vector<uint8_t> detailBuffer(requiredSize, 0);
            SP_DEVICE_INTERFACE_DETAIL_DATA_W* detailData = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
            detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!::SetupDiGetDeviceInterfaceDetailW(deviceInfoSet.Get(), &interfaceData, detailData, requiredSize, nullptr, &deviceInfoData))
            {
                continue;
            }

            const std::wstring normalizedDevicePath = NormalizeDevicePath(detailData->DevicePath);
            if (normalizedDevicePath.empty())
            {
                continue;
            }

            EdidInfo edidInfo;
            if (!TryReadEdidByDeviceInfoData(deviceInfoSet.Get(), deviceInfoData, edidInfo))
            {
                continue;
            }

            edidInfoMap[normalizedDevicePath] = edidInfo;
        }

        return edidInfoMap;
    }

    static std::vector<PathScreenInfo> CollectPathScreenInfo()
    {
        std::vector<PathScreenInfo> pathScreenInfos;

        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG status = ::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (status != ERROR_SUCCESS || pathCount == 0)
        {
            return pathScreenInfos;
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> pathInfos(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modeInfos(modeCount);

        bool querySucceeded = false;
        for (int retryIndex = 0; retryIndex < 4; retryIndex++)
        {
            UINT32 currentPathCount = pathCount;
            UINT32 currentModeCount = modeCount;
            status = ::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &currentPathCount, pathInfos.data(), &currentModeCount, modeInfos.data(), nullptr);
            if (status == ERROR_SUCCESS)
            {
                pathInfos.resize(currentPathCount);
                modeInfos.resize(currentModeCount);
                querySucceeded = true;
                break;
            }

            if (status != ERROR_INSUFFICIENT_BUFFER)
            {
                return pathScreenInfos;
            }

            status = ::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
            if (status != ERROR_SUCCESS || pathCount == 0)
            {
                return pathScreenInfos;
            }

            pathInfos.resize(pathCount);
            modeInfos.resize(modeCount);
        }

        if (!querySucceeded || pathInfos.empty())
        {
            return pathScreenInfos;
        }

        struct SourceKey
        {
            LUID adapterId = { 0, 0 };
            UINT32 sourceId = 0;

            bool operator<(const SourceKey& other) const
            {
                if (adapterId.HighPart != other.adapterId.HighPart)
                {
                    return adapterId.HighPart < other.adapterId.HighPart;
                }
                if (adapterId.LowPart != other.adapterId.LowPart)
                {
                    return adapterId.LowPart < other.adapterId.LowPart;
                }
                return sourceId < other.sourceId;
            }
        };

        struct TargetKey
        {
            LUID adapterId = { 0, 0 };
            UINT32 targetId = 0;

            bool operator<(const TargetKey& other) const
            {
                if (adapterId.HighPart != other.adapterId.HighPart)
                {
                    return adapterId.HighPart < other.adapterId.HighPart;
                }
                if (adapterId.LowPart != other.adapterId.LowPart)
                {
                    return adapterId.LowPart < other.adapterId.LowPart;
                }
                return targetId < other.targetId;
            }
        };

        std::map<SourceKey, DISPLAYCONFIG_SOURCE_MODE> sourceModeMap;
        for (size_t i = 0; i < modeInfos.size(); i++)
        {
            const DISPLAYCONFIG_MODE_INFO& modeInfo = modeInfos[i];
            if (modeInfo.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
            {
                SourceKey sourceKey;
                sourceKey.adapterId = modeInfo.adapterId;
                sourceKey.sourceId = modeInfo.id;
                sourceModeMap[sourceKey] = modeInfo.sourceMode;
            }
        }

        std::set<TargetKey> collectedTargets;
        for (size_t i = 0; i < pathInfos.size(); i++)
        {
            const DISPLAYCONFIG_PATH_INFO& pathInfo = pathInfos[i];

            TargetKey targetKey;
            targetKey.adapterId = pathInfo.targetInfo.adapterId;
            targetKey.targetId = pathInfo.targetInfo.id;
            if (collectedTargets.find(targetKey) != collectedTargets.end())
            {
                continue;
            }
            collectedTargets.insert(targetKey);

            PathScreenInfo pathScreenInfo;
            pathScreenInfo.adapterId = pathInfo.targetInfo.adapterId;
            pathScreenInfo.sourceId = pathInfo.sourceInfo.id;
            pathScreenInfo.targetId = pathInfo.targetInfo.id;
            pathScreenInfo.refreshRateHz = RationalToDouble(pathInfo.targetInfo.refreshRate);
            pathScreenInfo.isInternal = IsInternalOutputTechnology(pathInfo.targetInfo.outputTechnology);

            (void)TryGetSourceDeviceName(pathInfo.sourceInfo.adapterId, pathInfo.sourceInfo.id, pathScreenInfo.gdiDeviceName);
            (void)TryGetTargetDeviceName(pathInfo.targetInfo.adapterId, pathInfo.targetInfo.id, pathScreenInfo.monitorFriendlyName, pathScreenInfo.monitorDevicePath);
            (void)TryGetAdapterDevicePath(pathInfo.targetInfo.adapterId, pathScreenInfo.adapterDevicePath);
            (void)TryGetTargetPreferredMode(pathInfo.targetInfo.adapterId, pathInfo.targetInfo.id, pathScreenInfo.preferredPixelWidth, pathScreenInfo.preferredPixelHeight);

            SourceKey sourceKey;
            sourceKey.adapterId = pathInfo.sourceInfo.adapterId;
            sourceKey.sourceId = pathInfo.sourceInfo.id;
            const auto sourceModeIterator = sourceModeMap.find(sourceKey);
            if (sourceModeIterator != sourceModeMap.end())
            {
                pathScreenInfo.currentPixelWidth = static_cast<int>(sourceModeIterator->second.width);
                pathScreenInfo.currentPixelHeight = static_cast<int>(sourceModeIterator->second.height);
            }

            if (!pathScreenInfo.gdiDeviceName.empty())
            {
                DEVMODEW deviceMode = {};
                deviceMode.dmSize = sizeof(deviceMode);
                if (::EnumDisplaySettingsExW(pathScreenInfo.gdiDeviceName.c_str(), ENUM_CURRENT_SETTINGS, &deviceMode, 0))
                {
                    if (deviceMode.dmPelsWidth > 0)
                    {
                        pathScreenInfo.currentPixelWidth = static_cast<int>(deviceMode.dmPelsWidth);
                    }
                    if (deviceMode.dmPelsHeight > 0)
                    {
                        pathScreenInfo.currentPixelHeight = static_cast<int>(deviceMode.dmPelsHeight);
                    }
                    if (pathScreenInfo.refreshRateHz <= 0.0 && deviceMode.dmDisplayFrequency > 1)
                    {
                        pathScreenInfo.refreshRateHz = static_cast<double>(deviceMode.dmDisplayFrequency);
                    }
                }
            }

            pathScreenInfos.push_back(pathScreenInfo);
        }

        return pathScreenInfos;
    }

    static std::string ExtractBrandName(const std::string& friendlyName, const std::string& fallbackBrandName)
    {
        const std::string trimmedFriendlyName = TrimString(friendlyName);
        const std::string trimmedFallbackBrandName = TrimString(fallbackBrandName);
        if (trimmedFriendlyName.empty() || trimmedFriendlyName == "Generic PnP Monitor")
        {
            return trimmedFallbackBrandName;
        }

        if (!trimmedFallbackBrandName.empty())
        {
            const std::string lowerFriendlyName = ToLowerAsciiString(trimmedFriendlyName);
            const std::string lowerFallbackBrandName = ToLowerAsciiString(trimmedFallbackBrandName);
            if (lowerFriendlyName.find(lowerFallbackBrandName) == 0)
            {
                return trimmedFallbackBrandName;
            }
        }

        const size_t spaceIndex = trimmedFriendlyName.find(' ');
        if (spaceIndex == std::string::npos)
        {
            return trimmedFriendlyName;
        }

        const std::string firstToken = trimmedFriendlyName.substr(0, spaceIndex);
        if (firstToken.size() <= 2 && !trimmedFallbackBrandName.empty())
        {
            return trimmedFallbackBrandName;
        }

        return firstToken;
    }

    static bool TryConvertFloorToLong(const double value, long& integerValue)
    {
        if (!std::isfinite(value))
        {
            return false;
        }

        const double flooredValue = std::floor(value);
        if (flooredValue < static_cast<double>(std::numeric_limits<long>::min()) || flooredValue > static_cast<double>(std::numeric_limits<long>::max()))
        {
            return false;
        }

        integerValue = static_cast<long>(flooredValue);
        return true;
    }

    static bool TryConvertCeilToLong(const double value, long& integerValue)
    {
        if (!std::isfinite(value))
        {
            return false;
        }

        const double ceiledValue = std::ceil(value);
        if (ceiledValue < static_cast<double>(std::numeric_limits<long>::min()) || ceiledValue > static_cast<double>(std::numeric_limits<long>::max()))
        {
            return false;
        }

        integerValue = static_cast<long>(ceiledValue);
        return true;
    }

    static void FillScaleFactors(GB_ScreenInfo& screenInfo)
    {
        screenInfo.scaleFactorX = screenInfo.effectiveDpiX > 0.0 ? screenInfo.effectiveDpiX / 96.0 : 1.0;
        screenInfo.scaleFactorY = screenInfo.effectiveDpiY > 0.0 ? screenInfo.effectiveDpiY / 96.0 : 1.0;
    }

    static void FillDiagonalInches(GB_ScreenInfo& screenInfo)
    {
        if (screenInfo.physicalWidthMm > 0.0 && screenInfo.physicalHeightMm > 0.0)
        {
            const double diagonalMm = std::sqrt(screenInfo.physicalWidthMm * screenInfo.physicalWidthMm + screenInfo.physicalHeightMm * screenInfo.physicalHeightMm);
            screenInfo.diagonalInches = diagonalMm / 25.4;
        }
        else
        {
            screenInfo.diagonalInches = 0.0;
        }
    }

    static GB_ScreenInfo BuildRuntimeOnlyScreenInfo(const std::wstring& gdiDeviceName, const RuntimeMonitorInfo& runtimeMonitorInfo)
    {
        GB_ScreenInfo screenInfo;
        screenInfo.gdiDeviceName = WideStringToUtf8(gdiDeviceName);
        screenInfo.virtualScreenRectangle = GB_Rectangle(static_cast<double>(runtimeMonitorInfo.monitorRect.left), static_cast<double>(runtimeMonitorInfo.monitorRect.top), static_cast<double>(runtimeMonitorInfo.monitorRect.right), static_cast<double>(runtimeMonitorInfo.monitorRect.bottom));
        screenInfo.currentPixelWidth = runtimeMonitorInfo.monitorRect.right - runtimeMonitorInfo.monitorRect.left;
        screenInfo.currentPixelHeight = runtimeMonitorInfo.monitorRect.bottom - runtimeMonitorInfo.monitorRect.top;
        screenInfo.preferredPixelWidth = screenInfo.currentPixelWidth;
        screenInfo.preferredPixelHeight = screenInfo.currentPixelHeight;
        screenInfo.effectiveDpiX = static_cast<double>(runtimeMonitorInfo.effectiveDpiX);
        screenInfo.effectiveDpiY = static_cast<double>(runtimeMonitorInfo.effectiveDpiY);
        screenInfo.rawDpiX = static_cast<double>(runtimeMonitorInfo.rawDpiX);
        screenInfo.rawDpiY = static_cast<double>(runtimeMonitorInfo.rawDpiY);
        FillScaleFactors(screenInfo);
        screenInfo.isPrimary = runtimeMonitorInfo.isPrimary;

        DEVMODEW deviceMode = {};
        deviceMode.dmSize = sizeof(deviceMode);
        if (::EnumDisplaySettingsExW(gdiDeviceName.c_str(), ENUM_CURRENT_SETTINGS, &deviceMode, 0))
        {
            if (deviceMode.dmPelsWidth > 0)
            {
                screenInfo.currentPixelWidth = static_cast<int>(deviceMode.dmPelsWidth);
            }
            if (deviceMode.dmPelsHeight > 0)
            {
                screenInfo.currentPixelHeight = static_cast<int>(deviceMode.dmPelsHeight);
            }
            if (deviceMode.dmDisplayFrequency > 1)
            {
                screenInfo.refreshRateHz = static_cast<double>(deviceMode.dmDisplayFrequency);
            }
        }

        screenInfo.preferredPixelWidth = screenInfo.currentPixelWidth;
        screenInfo.preferredPixelHeight = screenInfo.currentPixelHeight;
        return screenInfo;
    }

    static GB_Rectangle GetVirtualScreenRectangleInternal()
    {
        const int virtualLeft = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int virtualTop = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int virtualWidth = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int virtualHeight = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (virtualWidth <= 0 || virtualHeight <= 0)
        {
            return GB_Rectangle::Invalid;
        }

        return GB_Rectangle(static_cast<double>(virtualLeft), static_cast<double>(virtualTop), static_cast<double>(virtualLeft + virtualWidth), static_cast<double>(virtualTop + virtualHeight));
    }

    static bool IntersectCaptureRectangle(const GB_Rectangle& requestedRectangle, RECT& clippedRectangle)
    {
        clippedRectangle.left = 0;
        clippedRectangle.top = 0;
        clippedRectangle.right = 0;
        clippedRectangle.bottom = 0;

        const GB_Rectangle virtualScreenRectangle = GetVirtualScreenRectangleInternal();
        if (!virtualScreenRectangle.IsValid())
        {
            return false;
        }

        GB_Rectangle captureRectangle = requestedRectangle;
        if (!captureRectangle.IsValid())
        {
            captureRectangle = virtualScreenRectangle;
        }
        else
        {
            captureRectangle = captureRectangle.Intersected(virtualScreenRectangle);
        }

        if (!captureRectangle.IsValid())
        {
            return false;
        }

        long left = 0;
        long top = 0;
        long right = 0;
        long bottom = 0;
        if (!TryConvertFloorToLong(captureRectangle.minX, left) ||
            !TryConvertFloorToLong(captureRectangle.minY, top) ||
            !TryConvertCeilToLong(captureRectangle.maxX, right) ||
            !TryConvertCeilToLong(captureRectangle.maxY, bottom))
        {
            return false;
        }

        if (right <= left || bottom <= top)
        {
            return false;
        }

        clippedRectangle.left = left;
        clippedRectangle.top = top;
        clippedRectangle.right = right;
        clippedRectangle.bottom = bottom;
        return true;
    }

    static bool CaptureRectangleToImage(const RECT& captureRectangle, GB_Image& screenImage)
    {
        const int captureWidth = captureRectangle.right - captureRectangle.left;
        const int captureHeight = captureRectangle.bottom - captureRectangle.top;
        if (captureWidth <= 0 || captureHeight <= 0)
        {
            return false;
        }

        ScreenDcScope screenDc;
        if (!screenDc.IsValid())
        {
            return false;
        }

        CompatibleDcScope memoryDc(screenDc.Get());
        if (!memoryDc.IsValid())
        {
            return false;
        }

        BITMAPINFO bitmapInfo = {};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = captureWidth;
        bitmapInfo.bmiHeader.biHeight = -captureHeight;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* bitmapBits = nullptr;
        BitmapScope bitmap(::CreateDIBSection(screenDc.Get(), &bitmapInfo, DIB_RGB_COLORS, &bitmapBits, nullptr, 0));
        if (!bitmap.IsValid() || bitmapBits == nullptr)
        {
            return false;
        }

        SelectObjectScope selectedBitmap(memoryDc.Get(), bitmap.Get());
        if (!selectedBitmap.IsValid())
        {
            return false;
        }

        if (!::BitBlt(memoryDc.Get(), 0, 0, captureWidth, captureHeight, screenDc.Get(), captureRectangle.left, captureRectangle.top, SRCCOPY | CAPTUREBLT))
        {
            return false;
        }

        ::GdiFlush();

        cv::Mat capturedMat(captureHeight, captureWidth, CV_8UC4, bitmapBits, static_cast<size_t>(captureWidth) * 4);
        if (capturedMat.empty())
        {
            return false;
        }

        if (capturedMat.isContinuous())
        {
            const size_t pixelCount = capturedMat.total();
            uint32_t* pixelData = reinterpret_cast<uint32_t*>(capturedMat.data);
            for (size_t pixelIndex = 0; pixelIndex < pixelCount; pixelIndex++)
            {
                pixelData[pixelIndex] |= 0xFF000000u;
            }
        }
        else
        {
            for (int rowIndex = 0; rowIndex < captureHeight; rowIndex++)
            {
                uint32_t* rowPixelData = capturedMat.ptr<uint32_t>(rowIndex);
                for (int colIndex = 0; colIndex < captureWidth; colIndex++)
                {
                    rowPixelData[colIndex] |= 0xFF000000u;
                }
            }
        }

        return screenImage.SetFromCvMat(capturedMat, GB_ImageCopyMode::DeepCopy);
    }


    static bool TryGetCurrentCursorInfo(CURSORINFO& cursorInfo)
    {
        std::memset(&cursorInfo, 0, sizeof(cursorInfo));
        cursorInfo.cbSize = sizeof(cursorInfo);
        return ::GetCursorInfo(&cursorInfo) != FALSE;
    }

    static bool TryGetCurrentVisibleCursorInfo(CURSORINFO& cursorInfo)
    {
        if (!TryGetCurrentCursorInfo(cursorInfo))
        {
            return false;
        }

        if ((cursorInfo.flags & CURSOR_SHOWING) == 0 || cursorInfo.hCursor == nullptr)
        {
            std::memset(&cursorInfo, 0, sizeof(cursorInfo));
            cursorInfo.cbSize = sizeof(cursorInfo);
            return false;
        }

        return true;
    }

    static bool TryGetCursorBitmapSize(const HCURSOR cursorHandle, int& cursorWidth, int& cursorHeight, ICONINFO& iconInfo)
    {
        cursorWidth = 0;
        cursorHeight = 0;
        std::memset(&iconInfo, 0, sizeof(iconInfo));

        if (cursorHandle == nullptr)
        {
            return false;
        }

        if (::GetIconInfo(cursorHandle, &iconInfo) == FALSE)
        {
            return false;
        }

        BITMAP bitmapInfo = {};
        if (iconInfo.hbmColor != nullptr)
        {
            if (::GetObjectW(iconInfo.hbmColor, sizeof(bitmapInfo), &bitmapInfo) <= 0)
            {
                return false;
            }

            cursorWidth = bitmapInfo.bmWidth;
            cursorHeight = std::abs(bitmapInfo.bmHeight);
            return cursorWidth > 0 && cursorHeight > 0;
        }

        if (iconInfo.hbmMask == nullptr)
        {
            return false;
        }

        if (::GetObjectW(iconInfo.hbmMask, sizeof(bitmapInfo), &bitmapInfo) <= 0)
        {
            return false;
        }

        const int maskBitmapHeight = std::abs(bitmapInfo.bmHeight);
        if (bitmapInfo.bmWidth <= 0 || maskBitmapHeight <= 0 || (maskBitmapHeight % 2) != 0)
        {
            return false;
        }

        cursorWidth = bitmapInfo.bmWidth;
        cursorHeight = maskBitmapHeight / 2;
        return cursorWidth > 0 && cursorHeight > 0;
    }

    static bool TryOverlayCursorOnBgraImage(const HCURSOR cursorHandle, const int cursorLeftOnImage, const int cursorTopOnImage, const int cursorWidth, const int cursorHeight, GB_Image& backgroundImage)
    {
        if (cursorHandle == nullptr || cursorWidth <= 0 || cursorHeight <= 0 || backgroundImage.IsEmpty() || backgroundImage.GetDepth() != GB_ImageDepth::UInt8 || backgroundImage.GetChannels() != 4)
        {
            return false;
        }

        if (backgroundImage.GetWidth() > static_cast<size_t>(std::numeric_limits<int>::max()) || backgroundImage.GetHeight() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        const int imageWidth = static_cast<int>(backgroundImage.GetWidth());
        const int imageHeight = static_cast<int>(backgroundImage.GetHeight());
        const size_t sourceRowStrideBytes = backgroundImage.GetRowStrideBytes();
        const size_t rowByteCount = static_cast<size_t>(imageWidth) * 4u;
        if (sourceRowStrideBytes < rowByteCount)
        {
            return false;
        }

        const uint64_t pixelByteCount64 = static_cast<uint64_t>(imageWidth) * static_cast<uint64_t>(imageHeight) * 4ull;
        if (pixelByteCount64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        {
            return false;
        }

        std::vector<uint8_t> bgraBuffer;
        try
        {
            bgraBuffer.resize(static_cast<size_t>(pixelByteCount64));
        }
        catch (...)
        {
            return false;
        }

        for (int rowIndex = 0; rowIndex < imageHeight; rowIndex++)
        {
            const unsigned char* rowData = backgroundImage.GetRowData(static_cast<size_t>(rowIndex));
            if (rowData == nullptr)
            {
                return false;
            }

            std::memcpy(bgraBuffer.data() + static_cast<size_t>(rowIndex) * rowByteCount, rowData, rowByteCount);
        }

        for (size_t pixelOffset = 3; pixelOffset < bgraBuffer.size(); pixelOffset += 4)
        {
            bgraBuffer[pixelOffset] = 255;
        }

        ScreenDcScope screenDc;
        if (!screenDc.IsValid())
        {
            return false;
        }

        CompatibleDcScope memoryDc(screenDc.Get());
        if (!memoryDc.IsValid())
        {
            return false;
        }

        BITMAPINFO bitmapInfo = {};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = imageWidth;
        bitmapInfo.bmiHeader.biHeight = -imageHeight;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* bitmapBits = nullptr;
        BitmapScope bitmap(::CreateDIBSection(screenDc.Get(), &bitmapInfo, DIB_RGB_COLORS, &bitmapBits, nullptr, 0));
        if (!bitmap.IsValid() || bitmapBits == nullptr)
        {
            return false;
        }

        std::memcpy(bitmapBits, bgraBuffer.data(), bgraBuffer.size());

        SelectObjectScope selectedBitmap(memoryDc.Get(), bitmap.Get());
        if (!selectedBitmap.IsValid())
        {
            return false;
        }

        if (::DrawIconEx(memoryDc.Get(), cursorLeftOnImage, cursorTopOnImage, cursorHandle, cursorWidth, cursorHeight, 0, nullptr, DI_NORMAL) == FALSE)
        {
            return false;
        }

        ::GdiFlush();
        std::memcpy(bgraBuffer.data(), bitmapBits, bgraBuffer.size());

        for (size_t pixelOffset = 3; pixelOffset < bgraBuffer.size(); pixelOffset += 4)
        {
            bgraBuffer[pixelOffset] = 255;
        }

        for (int rowIndex = 0; rowIndex < imageHeight; rowIndex++)
        {
            unsigned char* rowData = backgroundImage.GetRowData(static_cast<size_t>(rowIndex));
            if (rowData == nullptr)
            {
                return false;
            }

            std::memcpy(rowData, bgraBuffer.data() + static_cast<size_t>(rowIndex) * rowByteCount, rowByteCount);
        }

        return true;
    }

    static bool TryOverlayCurrentCursorOnImage(const RECT& captureRectangle, GB_Image& screenImage)
    {
        CURSORINFO cursorInfo = {};
        if (!TryGetCurrentVisibleCursorInfo(cursorInfo))
        {
            return true;
        }

        int cursorWidth = 0;
        int cursorHeight = 0;
        ICONINFO iconInfo = {};
        if (!TryGetCursorBitmapSize(cursorInfo.hCursor, cursorWidth, cursorHeight, iconInfo))
        {
            return false;
        }

        IconBitmapScope maskBitmapScope(iconInfo.hbmMask);
        IconBitmapScope colorBitmapScope(iconInfo.hbmColor);

        const long cursorLeft = cursorInfo.ptScreenPos.x - static_cast<long>(iconInfo.xHotspot);
        const long cursorTop = cursorInfo.ptScreenPos.y - static_cast<long>(iconInfo.yHotspot);
        const long cursorRight = cursorLeft + static_cast<long>(cursorWidth);
        const long cursorBottom = cursorTop + static_cast<long>(cursorHeight);

        if (cursorRight <= captureRectangle.left || cursorBottom <= captureRectangle.top || cursorLeft >= captureRectangle.right || cursorTop >= captureRectangle.bottom)
        {
            return true;
        }

        const int cursorLeftOnImage = static_cast<int>(cursorLeft - captureRectangle.left);
        const int cursorTopOnImage = static_cast<int>(cursorTop - captureRectangle.top);
        return TryOverlayCursorOnBgraImage(cursorInfo.hCursor, cursorLeftOnImage, cursorTopOnImage, cursorWidth, cursorHeight, screenImage);
    }

    static bool IsFinitePoint(const GB_Point2d& point)
    {
        return std::isfinite(point.x) && std::isfinite(point.y);
    }

    static bool IsPointInHalfOpenRectangle(const GB_Point2d& point, const GB_Rectangle& rectangle)
    {
        if (!IsFinitePoint(point) || !rectangle.IsValid())
        {
            return false;
        }

        return point.x >= rectangle.minX && point.x < rectangle.maxX && point.y >= rectangle.minY && point.y < rectangle.maxY;
    }

    static bool TryGetPrimaryScreenInfo(const std::vector<GB_ScreenInfo>& screenInfos, GB_ScreenInfo& screenInfo)
    {
        for (size_t i = 0; i < screenInfos.size(); i++)
        {
            if (screenInfos[i].isPrimary)
            {
                screenInfo = screenInfos[i];
                return true;
            }
        }

        if (!screenInfos.empty())
        {
            screenInfo = screenInfos[0];
            return true;
        }

        screenInfo = GB_ScreenInfo();
        return false;
    }

    static bool TryGetScreenInfoByIndex(const std::vector<GB_ScreenInfo>& screenInfos, const int screenIndex, GB_ScreenInfo& screenInfo)
    {
        if (screenIndex < 0 || static_cast<size_t>(screenIndex) >= screenInfos.size())
        {
            screenInfo = GB_ScreenInfo();
            return false;
        }

        screenInfo = screenInfos[static_cast<size_t>(screenIndex)];
        return true;
    }

    static bool TryGetScreenInfoByDeviceName(const std::vector<GB_ScreenInfo>& screenInfos, const std::string& gdiDeviceName, GB_ScreenInfo& screenInfo)
    {
        const std::string normalizedTargetName = ToLowerAsciiString(TrimString(gdiDeviceName));
        if (normalizedTargetName.empty())
        {
            screenInfo = GB_ScreenInfo();
            return false;
        }

        for (size_t i = 0; i < screenInfos.size(); i++)
        {
            if (ToLowerAsciiString(TrimString(screenInfos[i].gdiDeviceName)) == normalizedTargetName)
            {
                screenInfo = screenInfos[i];
                return true;
            }
        }

        screenInfo = GB_ScreenInfo();
        return false;
    }

    static bool TryFindContainingScreenIndex(const std::vector<GB_ScreenInfo>& screenInfos, const GB_Point2d& point, int& screenIndex)
    {
        screenIndex = -1;

        int firstMatchedScreenIndex = -1;
        for (size_t i = 0; i < screenInfos.size(); i++)
        {
            if (!IsPointInHalfOpenRectangle(point, screenInfos[i].virtualScreenRectangle))
            {
                continue;
            }

            if (screenInfos[i].isPrimary)
            {
                screenIndex = static_cast<int>(i);
                return true;
            }

            if (firstMatchedScreenIndex < 0)
            {
                firstMatchedScreenIndex = static_cast<int>(i);
            }
        }

        if (firstMatchedScreenIndex >= 0)
        {
            screenIndex = firstMatchedScreenIndex;
            return true;
        }

        return false;
    }

    static bool TryBuildScreenCaptureRectangle(const GB_ScreenInfo& screenInfo, const GB_Rectangle* screenLocalRectangle, RECT& captureRectangle)
    {
        captureRectangle.left = 0;
        captureRectangle.top = 0;
        captureRectangle.right = 0;
        captureRectangle.bottom = 0;

        if (!screenInfo.virtualScreenRectangle.IsValid())
        {
            return false;
        }

        GB_Rectangle physicalCaptureRectangle = screenInfo.virtualScreenRectangle;
        if (screenLocalRectangle != nullptr)
        {
            if (!screenLocalRectangle->IsValid())
            {
                return false;
            }

            physicalCaptureRectangle = GB_Rectangle(screenInfo.virtualScreenRectangle.minX + screenLocalRectangle->minX, screenInfo.virtualScreenRectangle.minY + screenLocalRectangle->minY, screenInfo.virtualScreenRectangle.minX + screenLocalRectangle->maxX, screenInfo.virtualScreenRectangle.minY + screenLocalRectangle->maxY);
            physicalCaptureRectangle = physicalCaptureRectangle.Intersected(screenInfo.virtualScreenRectangle);
        }

        if (!physicalCaptureRectangle.IsValid())
        {
            return false;
        }

        long left = 0;
        long top = 0;
        long right = 0;
        long bottom = 0;
        if (!TryConvertFloorToLong(physicalCaptureRectangle.minX, left) ||
            !TryConvertFloorToLong(physicalCaptureRectangle.minY, top) ||
            !TryConvertCeilToLong(physicalCaptureRectangle.maxX, right) ||
            !TryConvertCeilToLong(physicalCaptureRectangle.maxY, bottom))
        {
            return false;
        }

        if (right <= left || bottom <= top)
        {
            return false;
        }

        captureRectangle.left = left;
        captureRectangle.top = top;
        captureRectangle.right = right;
        captureRectangle.bottom = bottom;
        return true;
    }

    static bool TryCaptureScreenImage(const GB_ScreenInfo& screenInfo, const GB_Rectangle* screenLocalRectangle, GB_Image& screenImage, const bool withCursor)
    {
        RECT captureRectangle = { 0, 0, 0, 0 };
        if (!TryBuildScreenCaptureRectangle(screenInfo, screenLocalRectangle, captureRectangle))
        {
            screenImage.Clear();
            return false;
        }

        GB_Image capturedImage;
        if (!CaptureRectangleToImage(captureRectangle, capturedImage))
        {
            screenImage.Clear();
            return false;
        }

        if (withCursor && !TryOverlayCurrentCursorOnImage(captureRectangle, capturedImage))
        {
            screenImage.Clear();
            return false;
        }

        screenImage = std::move(capturedImage);
        return !screenImage.IsEmpty();
    }

    static bool TryGetSystemLogicalScale(double& scaleFactorX, double& scaleFactorY)
    {
        scaleFactorX = 1.0;
        scaleFactorY = 1.0;

        const HMODULE user32Module = ::GetModuleHandleW(L"user32.dll");
        if (user32Module != nullptr)
        {
            using GetDpiForSystemFunction = UINT(WINAPI*)();
            const auto getDpiForSystemFunction = reinterpret_cast<GetDpiForSystemFunction>(::GetProcAddress(user32Module, "GetDpiForSystem"));
            if (getDpiForSystemFunction != nullptr)
            {
                const UINT systemDpi = getDpiForSystemFunction();
                if (systemDpi > 0)
                {
                    const double scaleFactor = static_cast<double>(systemDpi) / 96.0;
                    scaleFactorX = scaleFactor;
                    scaleFactorY = scaleFactor;
                    return true;
                }
            }
        }

        const std::vector<GB_ScreenInfo> screenInfos = GB_Screen::GetAllScreens();
        GB_ScreenInfo primaryScreenInfo;
        if (!TryGetPrimaryScreenInfo(screenInfos, primaryScreenInfo))
        {
            return true;
        }

        if (primaryScreenInfo.effectiveDpiX > 0.0)
        {
            scaleFactorX = primaryScreenInfo.effectiveDpiX / 96.0;
        }
        if (primaryScreenInfo.effectiveDpiY > 0.0)
        {
            scaleFactorY = primaryScreenInfo.effectiveDpiY / 96.0;
        }

        if (scaleFactorX <= 0.0)
        {
            scaleFactorX = 1.0;
        }
        if (scaleFactorY <= 0.0)
        {
            scaleFactorY = 1.0;
        }

        return true;
    }


    struct ScreenPainterObjectState
    {
        GB_ScreenPaintObject paintObject;
        std::chrono::steady_clock::time_point expireTime;
    };

    static int GetOpenCvLineType(const bool antialias)
    {
        return antialias ? cv::LINE_AA : cv::LINE_8;
    }

    static bool TryConvertDoubleToIntRound(const double value, int& intValue)
    {
        if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<int>::min()) || value > static_cast<double>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        intValue = static_cast<int>(std::llround(value));
        return true;
    }

    static bool TryConvertDoubleToIntFloor(const double value, int& intValue)
    {
        if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<int>::min()) || value > static_cast<double>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        intValue = static_cast<int>(std::floor(value));
        return true;
    }

    static bool TryConvertDoubleToIntCeil(const double value, int& intValue)
    {
        if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<int>::min()) || value > static_cast<double>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        intValue = static_cast<int>(std::ceil(value));
        return true;
    }

    static cv::Scalar ToBgraScalar(const GB_ColorRGBA& color)
    {
        return cv::Scalar(static_cast<double>(color.b), static_cast<double>(color.g), static_cast<double>(color.r), static_cast<double>(color.a));
    }

    static void BlendStraightBgraOverPremultipliedBgra(cv::Mat& targetPremultipliedBgra, const cv::Mat& sourceStraightBgra)
    {
        if (targetPremultipliedBgra.empty() || sourceStraightBgra.empty() || targetPremultipliedBgra.rows != sourceStraightBgra.rows || targetPremultipliedBgra.cols != sourceStraightBgra.cols || targetPremultipliedBgra.type() != CV_8UC4 || sourceStraightBgra.type() != CV_8UC4)
        {
            return;
        }

        for (int rowIndex = 0; rowIndex < sourceStraightBgra.rows; rowIndex++)
        {
            const cv::Vec4b* sourceRow = sourceStraightBgra.ptr<cv::Vec4b>(rowIndex);
            cv::Vec4b* targetRow = targetPremultipliedBgra.ptr<cv::Vec4b>(rowIndex);

            for (int colIndex = 0; colIndex < sourceStraightBgra.cols; colIndex++)
            {
                const int sourceAlpha = static_cast<int>(sourceRow[colIndex][3]);
                if (sourceAlpha <= 0)
                {
                    continue;
                }

                const int inverseAlpha = 255 - sourceAlpha;
                targetRow[colIndex][0] = static_cast<unsigned char>((static_cast<int>(sourceRow[colIndex][0]) * sourceAlpha + static_cast<int>(targetRow[colIndex][0]) * inverseAlpha + 127) / 255);
                targetRow[colIndex][1] = static_cast<unsigned char>((static_cast<int>(sourceRow[colIndex][1]) * sourceAlpha + static_cast<int>(targetRow[colIndex][1]) * inverseAlpha + 127) / 255);
                targetRow[colIndex][2] = static_cast<unsigned char>((static_cast<int>(sourceRow[colIndex][2]) * sourceAlpha + static_cast<int>(targetRow[colIndex][2]) * inverseAlpha + 127) / 255);
                targetRow[colIndex][3] = static_cast<unsigned char>(sourceAlpha + (static_cast<int>(targetRow[colIndex][3]) * inverseAlpha + 127) / 255);
            }
        }
    }

    static cv::Rect ClipRectToCanvas(const cv::Rect& rectangle, const int canvasWidth, const int canvasHeight)
    {
        const cv::Rect canvasRectangle(0, 0, canvasWidth, canvasHeight);
        return rectangle & canvasRectangle;
    }

    static bool TryGetPolygonPointsInCanvas(const GB_Polygon& polygon, const int virtualScreenLeft, const int virtualScreenTop, std::vector<cv::Point>& points)
    {
        points.clear();

        if (!polygon.IsValid())
        {
            return false;
        }

        const std::vector<GB_Point2d> vertices = polygon.GetVerticesAsDouble();
        if (vertices.size() < 2)
        {
            return false;
        }

        points.reserve(vertices.size());
        for (size_t i = 0; i < vertices.size(); i++)
        {
            int pointX = 0;
            int pointY = 0;
            if (!TryConvertDoubleToIntRound(vertices[i].x - static_cast<double>(virtualScreenLeft), pointX) || !TryConvertDoubleToIntRound(vertices[i].y - static_cast<double>(virtualScreenTop), pointY))
            {
                points.clear();
                return false;
            }

            points.push_back(cv::Point(pointX, pointY));
        }

        return !points.empty();
    }

    static bool RenderPolygonObject(cv::Mat& canvasPremultipliedBgra, const GB_ScreenPaintObject& paintObject, const int virtualScreenLeft, const int virtualScreenTop)
    {
        if (canvasPremultipliedBgra.empty() || canvasPremultipliedBgra.type() != CV_8UC4 || paintObject.objectType != GB_ScreenPaintObjectType::Polygon)
        {
            return false;
        }

        std::vector<cv::Point> points;
        if (!TryGetPolygonPointsInCanvas(paintObject.polygon, virtualScreenLeft, virtualScreenTop, points))
        {
            return false;
        }

        const bool drawFill = paintObject.polygonOptions.fill && paintObject.polygonOptions.fillColor.a > 0 && points.size() >= 3;
        const bool drawBoundary = paintObject.polygonOptions.boundaryThickness > 0 && paintObject.polygonOptions.boundaryColor.a > 0 && points.size() >= 2;
        if (!drawFill && !drawBoundary)
        {
            return false;
        }

        cv::Rect boundingRectangle = cv::boundingRect(points);
        const int boundaryPadding = drawBoundary ? (paintObject.polygonOptions.boundaryThickness + 3) : 2;
        boundingRectangle.x -= boundaryPadding;
        boundingRectangle.y -= boundaryPadding;
        boundingRectangle.width += boundaryPadding * 2;
        boundingRectangle.height += boundaryPadding * 2;
        boundingRectangle = ClipRectToCanvas(boundingRectangle, canvasPremultipliedBgra.cols, canvasPremultipliedBgra.rows);
        if (boundingRectangle.empty())
        {
            return false;
        }

        std::vector<cv::Point> localPoints;
        localPoints.reserve(points.size());
        for (size_t i = 0; i < points.size(); i++)
        {
            localPoints.push_back(cv::Point(points[i].x - boundingRectangle.x, points[i].y - boundingRectangle.y));
        }

        cv::Mat layerStraightBgra(boundingRectangle.height, boundingRectangle.width, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        const int lineType = GetOpenCvLineType(paintObject.polygonOptions.antialias);

        if (drawFill)
        {
            std::vector<std::vector<cv::Point>> fillPoints(1, localPoints);
            cv::fillPoly(layerStraightBgra, fillPoints, ToBgraScalar(paintObject.polygonOptions.fillColor), lineType);
        }

        if (drawBoundary)
        {
            std::vector<std::vector<cv::Point>> boundaryPoints(1, localPoints);
            cv::polylines(layerStraightBgra, boundaryPoints, true, ToBgraScalar(paintObject.polygonOptions.boundaryColor), paintObject.polygonOptions.boundaryThickness, lineType);
        }

        cv::Mat targetRoi = canvasPremultipliedBgra(boundingRectangle);
        BlendStraightBgraOverPremultipliedBgra(targetRoi, layerStraightBgra);
        return true;
    }

    static bool TryConvertGbImageToStraightBgra(const GB_Image& image, cv::Mat& straightBgraImage)
    {
        straightBgraImage.release();
        if (image.IsEmpty())
        {
            return false;
        }

        const GB_Image* sourceImage = &image;
        GB_Image convertedImage;
        if (image.GetDepth() != GB_ImageDepth::UInt8)
        {
            convertedImage = image.ConvertTo(GB_ImageDepth::UInt8);
            if (convertedImage.IsEmpty())
            {
                return false;
            }
            sourceImage = &convertedImage;
        }

        const size_t imageWidth = sourceImage->GetWidth();
        const size_t imageHeight = sourceImage->GetHeight();
        if (imageWidth == 0 || imageHeight == 0 || imageWidth > static_cast<size_t>(std::numeric_limits<int>::max()) || imageHeight > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        straightBgraImage.create(static_cast<int>(imageHeight), static_cast<int>(imageWidth), CV_8UC4);
        for (size_t rowIndex = 0; rowIndex < imageHeight; rowIndex++)
        {
            cv::Vec4b* targetRow = straightBgraImage.ptr<cv::Vec4b>(static_cast<int>(rowIndex));
            for (size_t colIndex = 0; colIndex < imageWidth; colIndex++)
            {
                GB_ColorRGBA pixelColor;
                if (!sourceImage->GetPixelColor(rowIndex, colIndex, pixelColor))
                {
                    straightBgraImage.release();
                    return false;
                }

                targetRow[colIndex][0] = pixelColor.b;
                targetRow[colIndex][1] = pixelColor.g;
                targetRow[colIndex][2] = pixelColor.r;
                targetRow[colIndex][3] = pixelColor.a;
            }
        }

        return !straightBgraImage.empty();
    }

    static bool TryGetImageTargetRectangleInCanvas(const GB_Rectangle& screenRectangle, const int virtualScreenLeft, const int virtualScreenTop, cv::Rect& targetRectangle)
    {
        targetRectangle = cv::Rect();
        if (!screenRectangle.IsValid())
        {
            return false;
        }

        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        if (!TryConvertDoubleToIntFloor(screenRectangle.minX - static_cast<double>(virtualScreenLeft), left) || !TryConvertDoubleToIntFloor(screenRectangle.minY - static_cast<double>(virtualScreenTop), top) || !TryConvertDoubleToIntCeil(screenRectangle.maxX - static_cast<double>(virtualScreenLeft), right) || !TryConvertDoubleToIntCeil(screenRectangle.maxY - static_cast<double>(virtualScreenTop), bottom))
        {
            return false;
        }

        if (right <= left || bottom <= top)
        {
            return false;
        }

        targetRectangle = cv::Rect(left, top, right - left, bottom - top);
        return true;
    }

    static bool RenderImageObject(cv::Mat& canvasPremultipliedBgra, const GB_ScreenPaintObject& paintObject, const int virtualScreenLeft, const int virtualScreenTop)
    {
        if (canvasPremultipliedBgra.empty() || canvasPremultipliedBgra.type() != CV_8UC4 || paintObject.objectType != GB_ScreenPaintObjectType::Image)
        {
            return false;
        }

        cv::Rect targetRectangle;
        if (!TryGetImageTargetRectangleInCanvas(paintObject.imageOptions.screenRectangle, virtualScreenLeft, virtualScreenTop, targetRectangle))
        {
            return false;
        }

        const cv::Rect clippedTargetRectangle = ClipRectToCanvas(targetRectangle, canvasPremultipliedBgra.cols, canvasPremultipliedBgra.rows);
        if (clippedTargetRectangle.empty())
        {
            return false;
        }

        cv::Mat sourceStraightBgra;
        if (!TryConvertGbImageToStraightBgra(paintObject.image, sourceStraightBgra))
        {
            return false;
        }

        cv::Mat resizedStraightBgra;
        const int interpolation = paintObject.imageOptions.smoothResize ? cv::INTER_LINEAR : cv::INTER_NEAREST;
        if (targetRectangle.width == sourceStraightBgra.cols && targetRectangle.height == sourceStraightBgra.rows)
        {
            resizedStraightBgra = sourceStraightBgra;
        }
        else
        {
            cv::resize(sourceStraightBgra, resizedStraightBgra, cv::Size(targetRectangle.width, targetRectangle.height), 0.0, 0.0, interpolation);
        }

        const cv::Rect sourceRoi(clippedTargetRectangle.x - targetRectangle.x, clippedTargetRectangle.y - targetRectangle.y, clippedTargetRectangle.width, clippedTargetRectangle.height);
        cv::Mat sourceClippedStraightBgra = resizedStraightBgra(sourceRoi);
        cv::Mat targetRoi = canvasPremultipliedBgra(clippedTargetRectangle);
        BlendStraightBgraOverPremultipliedBgra(targetRoi, sourceClippedStraightBgra);
        return true;
    }

    static bool TryGetCurrentVirtualScreenRectForPainter(RECT& virtualScreenRect)
    {
        const int left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (width <= 0 || height <= 0)
        {
            virtualScreenRect = { 0, 0, 0, 0 };
            return false;
        }

        virtualScreenRect.left = left;
        virtualScreenRect.top = top;
        virtualScreenRect.right = left + width;
        virtualScreenRect.bottom = top + height;
        return true;
    }

    class ScreenPainterManager
    {
    public:
        ScreenPainterManager()
        {
        }

        ~ScreenPainterManager()
        {
            StopWindowThread();
        }

        uint64_t PaintPolygon(const GB_Polygon& polygon, const GB_ScreenPaintPolygonOptions& paintOptions, const long long displayDurationMilliseconds)
        {
            if (displayDurationMilliseconds <= 0 || !polygon.IsValid())
            {
                return 0;
            }

            const bool drawFill = paintOptions.fill && paintOptions.fillColor.a > 0;
            const bool drawBoundary = paintOptions.boundaryThickness > 0 && paintOptions.boundaryColor.a > 0;
            if (!drawFill && !drawBoundary)
            {
                return 0;
            }

            GB_ScreenPaintObject paintObject;
            paintObject.uid = AllocateUid();
            paintObject.objectType = GB_ScreenPaintObjectType::Polygon;
            paintObject.polygon = polygon;
            paintObject.polygonOptions = paintOptions;
            paintObject.displayDurationMilliseconds = displayDurationMilliseconds;
            paintObject.remainingMilliseconds = displayDurationMilliseconds;

            AddPaintObject(paintObject, displayDurationMilliseconds);
            return paintObject.uid;
        }

        uint64_t PaintImage(const GB_Image& image, const GB_ScreenPaintImageOptions& paintOptions, const long long displayDurationMilliseconds)
        {
            if (displayDurationMilliseconds <= 0 || image.IsEmpty() || !paintOptions.screenRectangle.IsValid() || paintOptions.screenRectangle.Width() <= 0.0 || paintOptions.screenRectangle.Height() <= 0.0)
            {
                return 0;
            }

            GB_ScreenPaintObject paintObject;
            paintObject.uid = AllocateUid();
            paintObject.objectType = GB_ScreenPaintObjectType::Image;
            paintObject.image = GB_Image(image, GB_ImageCopyMode::DeepCopy);
            paintObject.imageOptions = paintOptions;
            paintObject.displayDurationMilliseconds = displayDurationMilliseconds;
            paintObject.remainingMilliseconds = displayDurationMilliseconds;

            AddPaintObject(paintObject, displayDurationMilliseconds);
            return paintObject.uid;
        }

        std::vector<uint64_t> GetPaintedObjectUids()
        {
            bool needRefresh = false;
            std::vector<uint64_t> uids;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                needRefresh = RemoveExpiredObjectsLocked();
                uids.reserve(paintObjects.size());
                for (auto iterator = paintObjects.begin(); iterator != paintObjects.end(); ++iterator)
                {
                    uids.push_back(iterator->first);
                }
            }

            if (needRefresh)
            {
                RequestRefresh();
            }

            return uids;
        }

        bool GetPaintedObject(const uint64_t uid, GB_ScreenPaintObject& paintObject)
        {
            bool needRefresh = false;
            bool succeeded = false;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                needRefresh = RemoveExpiredObjectsLocked();
                const auto iterator = paintObjects.find(uid);
                if (iterator != paintObjects.end())
                {
                    paintObject = BuildPublicObjectLocked(iterator->second);
                    succeeded = true;
                }
                else
                {
                    paintObject = GB_ScreenPaintObject();
                }
            }

            if (needRefresh)
            {
                RequestRefresh();
            }

            return succeeded;
        }

        std::vector<GB_ScreenPaintObject> GetPaintedObjects(const std::vector<uint64_t>& uids)
        {
            bool needRefresh = false;
            std::vector<GB_ScreenPaintObject> result;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                needRefresh = RemoveExpiredObjectsLocked();
                result.reserve(uids.size());
                for (size_t i = 0; i < uids.size(); i++)
                {
                    const auto iterator = paintObjects.find(uids[i]);
                    if (iterator != paintObjects.end())
                    {
                        result.push_back(BuildPublicObjectLocked(iterator->second));
                    }
                }
            }

            if (needRefresh)
            {
                RequestRefresh();
            }

            return result;
        }

        std::vector<GB_ScreenPaintObject> GetAllPaintedObjects()
        {
            bool needRefresh = false;
            std::vector<GB_ScreenPaintObject> result;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                needRefresh = RemoveExpiredObjectsLocked();
                result.reserve(paintObjects.size());
                for (auto iterator = paintObjects.begin(); iterator != paintObjects.end(); ++iterator)
                {
                    result.push_back(BuildPublicObjectLocked(iterator->second));
                }
            }

            if (needRefresh)
            {
                RequestRefresh();
            }

            return result;
        }

        bool IsPainting(const uint64_t uid)
        {
            bool needRefresh = false;
            bool isPainting = false;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                needRefresh = RemoveExpiredObjectsLocked();
                isPainting = paintObjects.find(uid) != paintObjects.end();
            }

            if (needRefresh)
            {
                RequestRefresh();
            }

            return isPainting;
        }

        bool RemovePaintedObject(const uint64_t uid)
        {
            bool removed = false;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                removed = paintObjects.erase(uid) > 0;
            }

            if (removed)
            {
                RequestRefresh();
            }

            return removed;
        }

        size_t RemovePaintedObjects(const std::vector<uint64_t>& uids)
        {
            size_t removeCount = 0;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                for (size_t i = 0; i < uids.size(); i++)
                {
                    removeCount += paintObjects.erase(uids[i]);
                }
            }

            if (removeCount > 0)
            {
                RequestRefresh();
            }

            return removeCount;
        }

        void Clear()
        {
            bool needRefresh = false;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                needRefresh = !paintObjects.empty();
                paintObjects.clear();
            }

            if (needRefresh)
            {
                RequestRefresh();
            }
        }

        void RenderWindow()
        {
            HWND currentWindowHandle = nullptr;
            std::vector<ScreenPainterObjectState> objectStates;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                (void)RemoveExpiredObjectsLocked();
                currentWindowHandle = windowHandle;
                objectStates.reserve(paintObjects.size());
                for (auto iterator = paintObjects.begin(); iterator != paintObjects.end(); ++iterator)
                {
                    objectStates.push_back(iterator->second);
                }
            }

            if (currentWindowHandle == nullptr)
            {
                return;
            }

            DpiAwarenessScope dpiAwarenessScope;

            RECT virtualScreenRect = { 0, 0, 0, 0 };
            if (!TryGetCurrentVirtualScreenRectForPainter(virtualScreenRect))
            {
                ::ShowWindow(currentWindowHandle, SW_HIDE);
                return;
            }

            const int virtualScreenWidth = virtualScreenRect.right - virtualScreenRect.left;
            const int virtualScreenHeight = virtualScreenRect.bottom - virtualScreenRect.top;
            if (virtualScreenWidth <= 0 || virtualScreenHeight <= 0)
            {
                ::ShowWindow(currentWindowHandle, SW_HIDE);
                return;
            }

            (void)::SetWindowPos(currentWindowHandle, HWND_TOPMOST, virtualScreenRect.left, virtualScreenRect.top, virtualScreenWidth, virtualScreenHeight, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);

            if (objectStates.empty())
            {
                ClearLayeredWindow(currentWindowHandle, virtualScreenRect);
                ::ShowWindow(currentWindowHandle, SW_HIDE);
                return;
            }

            cv::Mat canvasPremultipliedBgra(virtualScreenHeight, virtualScreenWidth, CV_8UC4, cv::Scalar(0, 0, 0, 0));
            for (size_t i = 0; i < objectStates.size(); i++)
            {
                const GB_ScreenPaintObject& paintObject = objectStates[i].paintObject;
                if (paintObject.objectType == GB_ScreenPaintObjectType::Polygon)
                {
                    (void)RenderPolygonObject(canvasPremultipliedBgra, paintObject, virtualScreenRect.left, virtualScreenRect.top);
                }
                else if (paintObject.objectType == GB_ScreenPaintObjectType::Image)
                {
                    (void)RenderImageObject(canvasPremultipliedBgra, paintObject, virtualScreenRect.left, virtualScreenRect.top);
                }
            }

            (void)UpdateLayeredWindowFromPremultipliedBgra(currentWindowHandle, virtualScreenRect, canvasPremultipliedBgra);
            ::ShowWindow(currentWindowHandle, SW_SHOWNOACTIVATE);
        }

        void OnTimer()
        {
            bool needRefresh = false;
            HWND currentWindowHandle = nullptr;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                needRefresh = RemoveExpiredObjectsLocked();
                currentWindowHandle = windowHandle;
            }

            if (currentWindowHandle != nullptr)
            {
                (void)::SetWindowPos(currentWindowHandle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }

            if (needRefresh)
            {
                RenderWindow();
            }
        }

    private:
        static const UINT kRefreshMessage = WM_APP + 0x432;
        static const UINT_PTR kRefreshTimerId = 1;
        static const UINT kRefreshTimerMilliseconds = 50;

        std::mutex mutex;
        std::map<uint64_t, ScreenPainterObjectState> paintObjects;
        std::atomic<uint64_t> nextUid{ 1 };
        std::thread windowThread;
        HWND windowHandle = nullptr;
        DWORD windowThreadId = 0;
        bool windowThreadStopping = false;
        bool windowThreadFinished = true;

        uint64_t AllocateUid()
        {
            uint64_t uid = nextUid.fetch_add(1, std::memory_order_relaxed);
            if (uid == 0)
            {
                uid = nextUid.fetch_add(1, std::memory_order_relaxed);
            }
            return uid;
        }

        void AddPaintObject(const GB_ScreenPaintObject& paintObject, const long long displayDurationMilliseconds)
        {
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                RemoveFinishedWindowThreadLocked();
                StartWindowThreadLocked();

                ScreenPainterObjectState objectState;
                objectState.paintObject = paintObject;
                objectState.expireTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(displayDurationMilliseconds);
                paintObjects[paintObject.uid] = objectState;
            }

            RequestRefresh();
        }

        void StartWindowThreadLocked()
        {
            if (windowThread.joinable() || windowThreadStopping)
            {
                return;
            }

            windowThreadFinished = false;
            windowThread = std::thread(&ScreenPainterManager::WindowThreadMain, this);
        }

        void RemoveFinishedWindowThreadLocked()
        {
            if (windowThreadFinished && windowThread.joinable())
            {
                windowThread.join();
            }
        }

        void StopWindowThread()
        {
            HWND currentWindowHandle = nullptr;
            DWORD currentWindowThreadId = 0;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                windowThreadStopping = true;
                currentWindowHandle = windowHandle;
                currentWindowThreadId = windowThreadId;
            }

            if (currentWindowHandle != nullptr)
            {
                (void)::PostMessageW(currentWindowHandle, WM_CLOSE, 0, 0);
            }
            else if (currentWindowThreadId != 0)
            {
                (void)::PostThreadMessageW(currentWindowThreadId, WM_QUIT, 0, 0);
            }

            if (windowThread.joinable())
            {
                windowThread.join();
            }

            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                windowHandle = nullptr;
                windowThreadId = 0;
                windowThreadStopping = false;
                windowThreadFinished = true;
            }
        }

        void RequestRefresh()
        {
            HWND currentWindowHandle = nullptr;
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                currentWindowHandle = windowHandle;
            }

            if (currentWindowHandle != nullptr)
            {
                (void)::PostMessageW(currentWindowHandle, kRefreshMessage, 0, 0);
            }
        }

        bool RemoveExpiredObjectsLocked()
        {
            if (paintObjects.empty())
            {
                return false;
            }

            const std::chrono::steady_clock::time_point currentTime = std::chrono::steady_clock::now();
            bool removed = false;
            for (auto iterator = paintObjects.begin(); iterator != paintObjects.end(); )
            {
                if (iterator->second.expireTime <= currentTime)
                {
                    iterator = paintObjects.erase(iterator);
                    removed = true;
                }
                else
                {
                    ++iterator;
                }
            }

            return removed;
        }

        GB_ScreenPaintObject BuildPublicObjectLocked(const ScreenPainterObjectState& objectState) const
        {
            GB_ScreenPaintObject paintObject = objectState.paintObject;
            if (paintObject.objectType == GB_ScreenPaintObjectType::Image && !objectState.paintObject.image.IsEmpty())
            {
                paintObject.image = GB_Image(objectState.paintObject.image, GB_ImageCopyMode::DeepCopy);
            }

            const std::chrono::steady_clock::time_point currentTime = std::chrono::steady_clock::now();
            if (objectState.expireTime <= currentTime)
            {
                paintObject.remainingMilliseconds = 0;
            }
            else
            {
                paintObject.remainingMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(objectState.expireTime - currentTime).count();
            }

            return paintObject;
        }

        static bool ClearLayeredWindow(HWND targetWindowHandle, const RECT& virtualScreenRect)
        {
            cv::Mat emptyImage(1, 1, CV_8UC4, cv::Scalar(0, 0, 0, 0));
            RECT clearRect = virtualScreenRect;
            clearRect.right = clearRect.left + 1;
            clearRect.bottom = clearRect.top + 1;
            return UpdateLayeredWindowFromPremultipliedBgra(targetWindowHandle, clearRect, emptyImage);
        }

        static bool UpdateLayeredWindowFromPremultipliedBgra(HWND targetWindowHandle, const RECT& virtualScreenRect, const cv::Mat& premultipliedBgraImage)
        {
            if (targetWindowHandle == nullptr || premultipliedBgraImage.empty() || premultipliedBgraImage.type() != CV_8UC4)
            {
                return false;
            }

            ScreenDcScope screenDc;
            if (!screenDc.IsValid())
            {
                return false;
            }

            CompatibleDcScope memoryDc(screenDc.Get());
            if (!memoryDc.IsValid())
            {
                return false;
            }

            BITMAPINFO bitmapInfo = {};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = premultipliedBgraImage.cols;
            bitmapInfo.bmiHeader.biHeight = -premultipliedBgraImage.rows;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            void* bitmapPixels = nullptr;
            BitmapScope dibBitmap(::CreateDIBSection(screenDc.Get(), &bitmapInfo, DIB_RGB_COLORS, &bitmapPixels, nullptr, 0));
            if (!dibBitmap.IsValid() || bitmapPixels == nullptr)
            {
                return false;
            }

            const size_t bitmapBytes = static_cast<size_t>(premultipliedBgraImage.rows) * static_cast<size_t>(premultipliedBgraImage.cols) * 4u;
            if (premultipliedBgraImage.isContinuous())
            {
                std::memcpy(bitmapPixels, premultipliedBgraImage.data, bitmapBytes);
            }
            else
            {
                unsigned char* targetBytes = static_cast<unsigned char*>(bitmapPixels);
                const size_t rowBytes = static_cast<size_t>(premultipliedBgraImage.cols) * 4u;
                for (int rowIndex = 0; rowIndex < premultipliedBgraImage.rows; rowIndex++)
                {
                    std::memcpy(targetBytes + static_cast<size_t>(rowIndex) * rowBytes, premultipliedBgraImage.ptr(rowIndex), rowBytes);
                }
            }

            SelectObjectScope selectBitmap(memoryDc.Get(), dibBitmap.Get());
            if (!selectBitmap.IsValid())
            {
                return false;
            }

            POINT sourcePoint = { 0, 0 };
            POINT targetPoint = { virtualScreenRect.left, virtualScreenRect.top };
            SIZE targetSize = { premultipliedBgraImage.cols, premultipliedBgraImage.rows };
            BLENDFUNCTION blendFunction = {};
            blendFunction.BlendOp = AC_SRC_OVER;
            blendFunction.BlendFlags = 0;
            blendFunction.SourceConstantAlpha = 255;
            blendFunction.AlphaFormat = AC_SRC_ALPHA;

            return ::UpdateLayeredWindow(targetWindowHandle, screenDc.Get(), &targetPoint, &targetSize, memoryDc.Get(), &sourcePoint, 0, &blendFunction, ULW_ALPHA) != FALSE;
        }

        static LRESULT CALLBACK WindowProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
        {
            ScreenPainterManager* manager = reinterpret_cast<ScreenPainterManager*>(::GetWindowLongPtrW(windowHandle, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                CREATESTRUCTW* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
                manager = reinterpret_cast<ScreenPainterManager*>(createStruct->lpCreateParams);
                (void)::SetWindowLongPtrW(windowHandle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(manager));
            }

            if (manager == nullptr)
            {
                return ::DefWindowProcW(windowHandle, message, wParam, lParam);
            }

            switch (message)
            {
            case kRefreshMessage:
                manager->RenderWindow();
                return 0;

            case WM_TIMER:
                if (wParam == kRefreshTimerId)
                {
                    manager->OnTimer();
                    return 0;
                }
                break;

            case WM_DISPLAYCHANGE:
            case WM_SETTINGCHANGE:
            case WM_DPICHANGED:
                manager->RenderWindow();
                return 0;

            case WM_CLOSE:
                ::DestroyWindow(windowHandle);
                return 0;

            case WM_DESTROY:
                ::PostQuitMessage(0);
                return 0;

            default:
                break;
            }

            return ::DefWindowProcW(windowHandle, message, wParam, lParam);
        }

        void WindowThreadMain()
        {
            DpiAwarenessScope dpiAwarenessScope;

            MSG initialMessage = {};
            (void)::PeekMessageW(&initialMessage, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                windowThreadId = ::GetCurrentThreadId();
                if (windowThreadStopping)
                {
                    windowThreadId = 0;
                    windowThreadFinished = true;
                    return;
                }
            }

            const wchar_t* className = L"GB_ScreenPainterLayeredWindow";
            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.style = CS_HREDRAW | CS_VREDRAW;
            windowClass.lpfnWndProc = &ScreenPainterManager::WindowProc;
            windowClass.hInstance = ::GetModuleHandleW(nullptr);
            windowClass.hCursor = nullptr;
            windowClass.hbrBackground = nullptr;
            windowClass.lpszClassName = className;
            (void)::RegisterClassExW(&windowClass);

            RECT virtualScreenRect = { 0, 0, 1, 1 };
            if (!TryGetCurrentVirtualScreenRectForPainter(virtualScreenRect))
            {
                virtualScreenRect.right = virtualScreenRect.left + 1;
                virtualScreenRect.bottom = virtualScreenRect.top + 1;
            }

            const DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            const HWND createdWindowHandle = ::CreateWindowExW(exStyle, className, L"GB_ScreenPainter", WS_POPUP, virtualScreenRect.left, virtualScreenRect.top, virtualScreenRect.right - virtualScreenRect.left, virtualScreenRect.bottom - virtualScreenRect.top, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
            if (createdWindowHandle == nullptr)
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                windowHandle = nullptr;
                windowThreadFinished = true;
                return;
            }

            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                if (windowThreadStopping)
                {
                    windowHandle = nullptr;
                    windowThreadId = 0;
                    windowThreadFinished = true;
                    ::DestroyWindow(createdWindowHandle);
                    return;
                }

                windowHandle = createdWindowHandle;
            }

            (void)::SetTimer(createdWindowHandle, kRefreshTimerId, kRefreshTimerMilliseconds, nullptr);
            (void)::SetWindowPos(createdWindowHandle, HWND_TOPMOST, virtualScreenRect.left, virtualScreenRect.top, virtualScreenRect.right - virtualScreenRect.left, virtualScreenRect.bottom - virtualScreenRect.top, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
            ::ShowWindow(createdWindowHandle, SW_SHOWNOACTIVATE);
            RenderWindow();

            MSG message = {};
            while (::GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }

            (void)::KillTimer(createdWindowHandle, kRefreshTimerId);
            {
                std::lock_guard<std::mutex> lockGuard(mutex);
                if (windowHandle == createdWindowHandle)
                {
                    windowHandle = nullptr;
                }
                windowThreadId = 0;
                windowThreadFinished = true;
            }
        }
    };

    static ScreenPainterManager& GetScreenPainterManager()
    {
        static ScreenPainterManager manager;
        return manager;
    }

#endif
}

std::vector<GB_ScreenInfo> GB_Screen::GetAllScreens()
{
#if !defined(_WIN32)
    return std::vector<GB_ScreenInfo>();
#else
    internal::DpiAwarenessScope dpiAwarenessScope;

    const std::map<std::wstring, internal::RuntimeMonitorInfo> runtimeMonitorMap = internal::CollectRuntimeMonitors();
    const std::vector<internal::PathScreenInfo> pathScreenInfos = internal::CollectPathScreenInfo();
    const std::unordered_map<std::wstring, internal::EdidInfo> edidInfoMap = internal::CollectEdidInfoByMonitorDevicePath();

    std::vector<GB_ScreenInfo> screenInfos;
    screenInfos.reserve(pathScreenInfos.empty() ? runtimeMonitorMap.size() : pathScreenInfos.size() + runtimeMonitorMap.size());

    if (!pathScreenInfos.empty())
    {
        std::unordered_set<std::wstring> matchedRuntimeMonitorNames;
        matchedRuntimeMonitorNames.reserve(pathScreenInfos.size());

        for (size_t i = 0; i < pathScreenInfos.size(); i++)
        {
            const internal::PathScreenInfo& pathScreenInfo = pathScreenInfos[i];

            GB_ScreenInfo screenInfo;
            screenInfo.gdiDeviceName = internal::WideStringToUtf8(pathScreenInfo.gdiDeviceName);
            screenInfo.monitorDevicePath = internal::WideStringToUtf8(pathScreenInfo.monitorDevicePath);
            screenInfo.adapterDevicePath = internal::WideStringToUtf8(pathScreenInfo.adapterDevicePath);
            screenInfo.monitorFriendlyName = internal::WideStringToUtf8(pathScreenInfo.monitorFriendlyName);
            screenInfo.currentPixelWidth = pathScreenInfo.currentPixelWidth;
            screenInfo.currentPixelHeight = pathScreenInfo.currentPixelHeight;
            screenInfo.preferredPixelWidth = pathScreenInfo.preferredPixelWidth;
            screenInfo.preferredPixelHeight = pathScreenInfo.preferredPixelHeight;
            screenInfo.refreshRateHz = pathScreenInfo.refreshRateHz;
            screenInfo.isInternal = pathScreenInfo.isInternal;

            const auto runtimeMonitorIterator = runtimeMonitorMap.find(pathScreenInfo.gdiDeviceName);
            if (runtimeMonitorIterator != runtimeMonitorMap.end())
            {
                const internal::RuntimeMonitorInfo& runtimeMonitorInfo = runtimeMonitorIterator->second;
                matchedRuntimeMonitorNames.insert(pathScreenInfo.gdiDeviceName);
                screenInfo.virtualScreenRectangle = GB_Rectangle(static_cast<double>(runtimeMonitorInfo.monitorRect.left), static_cast<double>(runtimeMonitorInfo.monitorRect.top), static_cast<double>(runtimeMonitorInfo.monitorRect.right), static_cast<double>(runtimeMonitorInfo.monitorRect.bottom));
                screenInfo.isPrimary = runtimeMonitorInfo.isPrimary;
                screenInfo.effectiveDpiX = static_cast<double>(runtimeMonitorInfo.effectiveDpiX);
                screenInfo.effectiveDpiY = static_cast<double>(runtimeMonitorInfo.effectiveDpiY);
                screenInfo.rawDpiX = static_cast<double>(runtimeMonitorInfo.rawDpiX);
                screenInfo.rawDpiY = static_cast<double>(runtimeMonitorInfo.rawDpiY);

                if (screenInfo.currentPixelWidth <= 0)
                {
                    screenInfo.currentPixelWidth = runtimeMonitorInfo.monitorRect.right - runtimeMonitorInfo.monitorRect.left;
                }
                if (screenInfo.currentPixelHeight <= 0)
                {
                    screenInfo.currentPixelHeight = runtimeMonitorInfo.monitorRect.bottom - runtimeMonitorInfo.monitorRect.top;
                }
            }
            else
            {
                screenInfo.virtualScreenRectangle = GB_Rectangle::Invalid;
            }

            if (screenInfo.preferredPixelWidth <= 0)
            {
                screenInfo.preferredPixelWidth = screenInfo.currentPixelWidth;
            }
            if (screenInfo.preferredPixelHeight <= 0)
            {
                screenInfo.preferredPixelHeight = screenInfo.currentPixelHeight;
            }

            internal::FillScaleFactors(screenInfo);

            const std::wstring normalizedMonitorDevicePath = internal::NormalizeDevicePath(pathScreenInfo.monitorDevicePath);
            const auto edidInfoIterator = edidInfoMap.find(normalizedMonitorDevicePath);
            if (edidInfoIterator != edidInfoMap.end())
            {
                const internal::EdidInfo& edidInfo = edidInfoIterator->second;
                screenInfo.manufacturerCode = edidInfo.manufacturerCode;
                screenInfo.brandName = internal::ExtractBrandName(screenInfo.monitorFriendlyName, edidInfo.brandName);
                screenInfo.productName = edidInfo.productName;
                screenInfo.serialNumber = edidInfo.serialNumber;
                screenInfo.physicalWidthMm = edidInfo.physicalWidthMm;
                screenInfo.physicalHeightMm = edidInfo.physicalHeightMm;
            }
            else
            {
                screenInfo.brandName = internal::ExtractBrandName(screenInfo.monitorFriendlyName, std::string());
            }

            internal::FillDiagonalInches(screenInfo);
            screenInfos.push_back(screenInfo);
        }

        for (auto iterator = runtimeMonitorMap.begin(); iterator != runtimeMonitorMap.end(); ++iterator)
        {
            if (matchedRuntimeMonitorNames.find(iterator->first) != matchedRuntimeMonitorNames.end())
            {
                continue;
            }

            screenInfos.push_back(internal::BuildRuntimeOnlyScreenInfo(iterator->first, iterator->second));
        }

        return screenInfos;
    }

    for (auto iterator = runtimeMonitorMap.begin(); iterator != runtimeMonitorMap.end(); ++iterator)
    {
        screenInfos.push_back(internal::BuildRuntimeOnlyScreenInfo(iterator->first, iterator->second));
    }

    return screenInfos;
#endif
}

GB_Rectangle GB_Screen::GetVirtualScreenRectangle()
{
#if !defined(_WIN32)
    return GB_Rectangle::Invalid;
#else
    internal::DpiAwarenessScope dpiAwarenessScope;
    return internal::GetVirtualScreenRectangleInternal();
#endif
}

bool GB_Screen::CaptureVirtualScreen(GB_Image& screenImage, const bool withCursor)
{
#if !defined(_WIN32)
    (void)withCursor;
    screenImage.Clear();
    return false;
#else
    return CaptureVirtualScreen(GetVirtualScreenRectangle(), screenImage, withCursor);
#endif
}

bool GB_Screen::CaptureVirtualScreen(const GB_Rectangle& virtualScreenRectangle, GB_Image& screenImage, const bool withCursor)
{
#if !defined(_WIN32)
    (void)virtualScreenRectangle;
    (void)withCursor;
    screenImage.Clear();
    return false;
#else
    if (!virtualScreenRectangle.IsValid())
    {
        screenImage.Clear();
        return false;
    }

    internal::DpiAwarenessScope dpiAwarenessScope;

    RECT captureRectangle = { 0, 0, 0, 0 };
    if (!internal::IntersectCaptureRectangle(virtualScreenRectangle, captureRectangle))
    {
        screenImage.Clear();
        return false;
    }

    GB_Image capturedImage;
    if (!internal::CaptureRectangleToImage(captureRectangle, capturedImage))
    {
        screenImage.Clear();
        return false;
    }

    if (withCursor && !internal::TryOverlayCurrentCursorOnImage(captureRectangle, capturedImage))
    {
        screenImage.Clear();
        return false;
    }

    screenImage = std::move(capturedImage);
    return !screenImage.IsEmpty();
#endif
}


GB_ScreenInfo GB_Screen::GetScreenFromPoint(const GB_Point2d& point)
{
#if !defined(_WIN32)
    (void)point;
    return GB_ScreenInfo();
#else
    if (!internal::IsFinitePoint(point))
    {
        return GB_ScreenInfo();
    }

    internal::DpiAwarenessScope dpiAwarenessScope;

    const std::vector<GB_ScreenInfo> screenInfos = GetAllScreens();
    int screenIndex = -1;
    if (!internal::TryFindContainingScreenIndex(screenInfos, point, screenIndex))
    {
        return GB_ScreenInfo();
    }

    return screenInfos[static_cast<size_t>(screenIndex)];
#endif
}

GB_ScreenInfo GB_Screen::GetPrimaryScreen()
{
#if !defined(_WIN32)
    return GB_ScreenInfo();
#else
    internal::DpiAwarenessScope dpiAwarenessScope;

    const std::vector<GB_ScreenInfo> screenInfos = GetAllScreens();
    GB_ScreenInfo primaryScreenInfo;
    if (!internal::TryGetPrimaryScreenInfo(screenInfos, primaryScreenInfo))
    {
        return GB_ScreenInfo();
    }

    return primaryScreenInfo;
#endif
}

bool GB_Screen::CaptureScreen(const int screenIndex, GB_Image& screenImage, const bool withCursor)
{
#if !defined(_WIN32)
    (void)screenIndex;
    (void)withCursor;
    screenImage.Clear();
    return false;
#else
    internal::DpiAwarenessScope dpiAwarenessScope;

    const std::vector<GB_ScreenInfo> screenInfos = GetAllScreens();
    GB_ScreenInfo screenInfo;
    if (!internal::TryGetScreenInfoByIndex(screenInfos, screenIndex, screenInfo))
    {
        screenImage.Clear();
        return false;
    }

    return internal::TryCaptureScreenImage(screenInfo, nullptr, screenImage, withCursor);
#endif
}

bool GB_Screen::CaptureScreen(const int screenIndex, const GB_Rectangle& screenLocalRectangle, GB_Image& screenImage, const bool withCursor)
{
#if !defined(_WIN32)
    (void)screenIndex;
    (void)screenLocalRectangle;
    (void)withCursor;
    screenImage.Clear();
    return false;
#else
    if (!screenLocalRectangle.IsValid())
    {
        screenImage.Clear();
        return false;
    }

    internal::DpiAwarenessScope dpiAwarenessScope;

    const std::vector<GB_ScreenInfo> screenInfos = GetAllScreens();
    GB_ScreenInfo screenInfo;
    if (!internal::TryGetScreenInfoByIndex(screenInfos, screenIndex, screenInfo))
    {
        screenImage.Clear();
        return false;
    }

    return internal::TryCaptureScreenImage(screenInfo, &screenLocalRectangle, screenImage, withCursor);
#endif
}

bool GB_Screen::CaptureScreen(const std::string& gdiDeviceName, GB_Image& screenImage, const bool withCursor)
{
#if !defined(_WIN32)
    (void)gdiDeviceName;
    (void)withCursor;
    screenImage.Clear();
    return false;
#else
    internal::DpiAwarenessScope dpiAwarenessScope;

    const std::vector<GB_ScreenInfo> screenInfos = GetAllScreens();
    GB_ScreenInfo screenInfo;
    if (!internal::TryGetScreenInfoByDeviceName(screenInfos, gdiDeviceName, screenInfo))
    {
        screenImage.Clear();
        return false;
    }

    return internal::TryCaptureScreenImage(screenInfo, nullptr, screenImage, withCursor);
#endif
}

bool GB_Screen::CaptureScreen(const std::string& gdiDeviceName, const GB_Rectangle& screenLocalRectangle, GB_Image& screenImage, const bool withCursor)
{
#if !defined(_WIN32)
    (void)gdiDeviceName;
    (void)screenLocalRectangle;
    (void)withCursor;
    screenImage.Clear();
    return false;
#else
    if (!screenLocalRectangle.IsValid())
    {
        screenImage.Clear();
        return false;
    }

    internal::DpiAwarenessScope dpiAwarenessScope;

    const std::vector<GB_ScreenInfo> screenInfos = GetAllScreens();
    GB_ScreenInfo screenInfo;
    if (!internal::TryGetScreenInfoByDeviceName(screenInfos, gdiDeviceName, screenInfo))
    {
        screenImage.Clear();
        return false;
    }

    return internal::TryCaptureScreenImage(screenInfo, &screenLocalRectangle, screenImage, withCursor);
#endif
}

bool GB_Screen::LogicalPixelToPhysicalPixel(const GB_Point2d& logicalPixelPoint, int& screenIndex, GB_Point2d& physicalPixelPointOnScreen)
{
    screenIndex = -1;
    physicalPixelPointOnScreen = GB_Point2d();

#if !defined(_WIN32)
    (void)logicalPixelPoint;
    return false;
#else
    if (!internal::IsFinitePoint(logicalPixelPoint))
    {
        return false;
    }

    internal::DpiAwarenessScope dpiAwarenessScope;

    double systemScaleFactorX = 1.0;
    double systemScaleFactorY = 1.0;
    if (!internal::TryGetSystemLogicalScale(systemScaleFactorX, systemScaleFactorY))
    {
        return false;
    }

    const GB_Point2d physicalPixelPoint(logicalPixelPoint.x * systemScaleFactorX, logicalPixelPoint.y * systemScaleFactorY);

    const std::vector<GB_ScreenInfo> screenInfos = GetAllScreens();
    if (!internal::TryFindContainingScreenIndex(screenInfos, physicalPixelPoint, screenIndex))
    {
        screenIndex = -1;
        return false;
    }

    const GB_ScreenInfo& screenInfo = screenInfos[static_cast<size_t>(screenIndex)];
    physicalPixelPointOnScreen.Set(physicalPixelPoint.x - screenInfo.virtualScreenRectangle.minX, physicalPixelPoint.y - screenInfo.virtualScreenRectangle.minY);
    return true;
#endif
}

bool GB_Screen::PhysicalPixelToLogicalPixel(const int screenIndex, const GB_Point2d& physicalPixelPointOnScreen, GB_Point2d& logicalPixelPoint)
{
    logicalPixelPoint = GB_Point2d();

#if !defined(_WIN32)
    (void)screenIndex;
    (void)physicalPixelPointOnScreen;
    return false;
#else
    if (!internal::IsFinitePoint(physicalPixelPointOnScreen))
    {
        return false;
    }

    internal::DpiAwarenessScope dpiAwarenessScope;

    const std::vector<GB_ScreenInfo> screenInfos = GetAllScreens();
    GB_ScreenInfo screenInfo;
    if (!internal::TryGetScreenInfoByIndex(screenInfos, screenIndex, screenInfo) || !screenInfo.virtualScreenRectangle.IsValid())
    {
        return false;
    }

    const double screenWidth = screenInfo.virtualScreenRectangle.Width();
    const double screenHeight = screenInfo.virtualScreenRectangle.Height();
    if (!(physicalPixelPointOnScreen.x >= 0.0 && physicalPixelPointOnScreen.x < screenWidth && physicalPixelPointOnScreen.y >= 0.0 && physicalPixelPointOnScreen.y < screenHeight))
    {
        return false;
    }

    double systemScaleFactorX = 1.0;
    double systemScaleFactorY = 1.0;
    if (!internal::TryGetSystemLogicalScale(systemScaleFactorX, systemScaleFactorY))
    {
        return false;
    }

    const double physicalX = screenInfo.virtualScreenRectangle.minX + physicalPixelPointOnScreen.x;
    const double physicalY = screenInfo.virtualScreenRectangle.minY + physicalPixelPointOnScreen.y;
    logicalPixelPoint.Set(physicalX / systemScaleFactorX, physicalY / systemScaleFactorY);
    return true;
#endif
}

uint64_t GB_ScreenPainter::PaintPolygon(const GB_Polygon& polygon, const GB_ScreenPaintPolygonOptions& paintOptions, const long long displayDurationMilliseconds)
{
#if !defined(_WIN32)
    (void)polygon;
    (void)paintOptions;
    (void)displayDurationMilliseconds;
    return 0;
#else
    return internal::GetScreenPainterManager().PaintPolygon(polygon, paintOptions, displayDurationMilliseconds);
#endif
}

uint64_t GB_ScreenPainter::PaintPolygon(const GB_Polygon& polygon, const GB_ColorRGBA& boundaryColor, const int boundaryThickness, const bool fill, const GB_ColorRGBA& fillColor, const long long displayDurationMilliseconds)
{
    GB_ScreenPaintPolygonOptions paintOptions;
    paintOptions.boundaryColor = boundaryColor;
    paintOptions.boundaryThickness = boundaryThickness;
    paintOptions.fill = fill;
    paintOptions.fillColor = fillColor;
    return PaintPolygon(polygon, paintOptions, displayDurationMilliseconds);
}

uint64_t GB_ScreenPainter::PaintImage(const GB_Image& image, const GB_ScreenPaintImageOptions& paintOptions, const long long displayDurationMilliseconds)
{
#if !defined(_WIN32)
    (void)image;
    (void)paintOptions;
    (void)displayDurationMilliseconds;
    return 0;
#else
    return internal::GetScreenPainterManager().PaintImage(image, paintOptions, displayDurationMilliseconds);
#endif
}

uint64_t GB_ScreenPainter::PaintImage(const GB_Image& image, const GB_Rectangle& screenRectangle, const long long displayDurationMilliseconds)
{
    GB_ScreenPaintImageOptions paintOptions;
    paintOptions.screenRectangle = screenRectangle;
    return PaintImage(image, paintOptions, displayDurationMilliseconds);
}

std::vector<uint64_t> GB_ScreenPainter::GetPaintedObjectUids()
{
#if !defined(_WIN32)
    return std::vector<uint64_t>();
#else
    return internal::GetScreenPainterManager().GetPaintedObjectUids();
#endif
}

bool GB_ScreenPainter::GetPaintedObject(const uint64_t uid, GB_ScreenPaintObject& paintObject)
{
#if !defined(_WIN32)
    (void)uid;
    paintObject = GB_ScreenPaintObject();
    return false;
#else
    return internal::GetScreenPainterManager().GetPaintedObject(uid, paintObject);
#endif
}

std::vector<GB_ScreenPaintObject> GB_ScreenPainter::GetPaintedObjects(const std::vector<uint64_t>& uids)
{
#if !defined(_WIN32)
    (void)uids;
    return std::vector<GB_ScreenPaintObject>();
#else
    return internal::GetScreenPainterManager().GetPaintedObjects(uids);
#endif
}

std::vector<GB_ScreenPaintObject> GB_ScreenPainter::GetAllPaintedObjects()
{
#if !defined(_WIN32)
    return std::vector<GB_ScreenPaintObject>();
#else
    return internal::GetScreenPainterManager().GetAllPaintedObjects();
#endif
}

bool GB_ScreenPainter::IsPainting(const uint64_t uid)
{
#if !defined(_WIN32)
    (void)uid;
    return false;
#else
    return internal::GetScreenPainterManager().IsPainting(uid);
#endif
}

bool GB_ScreenPainter::RemovePaintedObject(const uint64_t uid)
{
#if !defined(_WIN32)
    (void)uid;
    return false;
#else
    return internal::GetScreenPainterManager().RemovePaintedObject(uid);
#endif
}

size_t GB_ScreenPainter::RemovePaintedObjects(const std::vector<uint64_t>& uids)
{
#if !defined(_WIN32)
    (void)uids;
    return 0;
#else
    return internal::GetScreenPainterManager().RemovePaintedObjects(uids);
#endif
}

void GB_ScreenPainter::Clear()
{
#if defined(_WIN32)
    internal::GetScreenPainterManager().Clear();
#endif
}

