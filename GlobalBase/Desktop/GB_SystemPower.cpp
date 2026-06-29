#include "GB_SystemPower.h"

#include "GB_PrivilegeScope.h"
#include "../GB_Utf8String.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <powerbase.h>
#  include <powrprof.h>
#  include <powersetting.h>
#  include <reason.h>
#  ifndef EWX_HYBRID_SHUTDOWN
#    define EWX_HYBRID_SHUTDOWN 0x00400000
#  endif
#  ifdef _MSC_VER
#    pragma comment(lib, "User32.lib")
#    pragma comment(lib, "PowrProf.lib")
#    pragma comment(lib, "Advapi32.lib")
#  endif
#endif

namespace
{
#if defined(_WIN32)
    const uint32_t DefaultShutdownReason = static_cast<uint32_t>(SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_MAINTENANCE);
#else
    const uint32_t DefaultShutdownReason = 0x80000000u | 0x00040000u | 0x00000001u;
#endif
    const uint32_t RequestFlagSystem = 0x00000001u;
    const uint32_t RequestFlagDisplay = 0x00000002u;
    const uint32_t RequestFlagAwayMode = 0x00000004u;
    const uint32_t MaxPowerSettingPayloadBytes = 4096u;
    const uint32_t MaxScheduledShutdownDelaySeconds = 10u * 365u * 24u * 60u * 60u;
    const size_t MaxPowerRequestReasonLength = 1024u;

    bool ContainsNullCharacter(const std::string& text)
    {
        return text.find('\0') != std::string::npos;
    }

    GB_SystemResult MakeUnsupportedPlatformResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, u8"当前平台不支持 Windows 电源管理能力。");
    }

    bool IsValidPowerActionType(const GB_SystemPowerActionType actionType)
    {
        switch (actionType)
        {
        case GB_SystemPowerActionType::Shutdown:
        case GB_SystemPowerActionType::PowerOff:
        case GB_SystemPowerActionType::Reboot:
        case GB_SystemPowerActionType::HybridShutdown:
            return true;
        default:
            break;
        }

        return false;
    }

    bool IsValidKeepAwakeMode(const GB_SystemPowerKeepAwakeMode keepAwakeMode)
    {
        switch (keepAwakeMode)
        {
        case GB_SystemPowerKeepAwakeMode::System:
        case GB_SystemPowerKeepAwakeMode::Display:
        case GB_SystemPowerKeepAwakeMode::SystemAndDisplay:
        case GB_SystemPowerKeepAwakeMode::AwayMode:
            return true;
        default:
            break;
        }

        return false;
    }

    uint32_t ResolveShutdownReasonCode(const uint32_t reasonCode)
    {
        return reasonCode == 0 ? DefaultShutdownReason : reasonCode;
    }

#if defined(_WIN32)
    const UINT PowerWatcherStopMessage = WM_APP + 0x0626;

    const GUID GbPowerGuidMaxPowerSavings = { 0xA1841308, 0x3541, 0x4FAB, { 0xBC, 0x81, 0xF7, 0x15, 0x56, 0xF2, 0x0B, 0x4A } };
    const GUID GbPowerGuidTypicalPowerSavings = { 0x381B4222, 0xF694, 0x41F0, { 0x96, 0x85, 0xFF, 0x5B, 0xB2, 0x60, 0xDF, 0x2E } };
    const GUID GbPowerGuidMinPowerSavings = { 0x8C5E7FDA, 0xE8BF, 0x4A96, { 0x9A, 0x85, 0xA6, 0xE2, 0x3A, 0x8C, 0x63, 0x5C } };
    const GUID GbPowerGuidNoSubgroup = { 0xFEA3413E, 0x7E05, 0x4911, { 0x9A, 0x71, 0x70, 0x03, 0x31, 0xF1, 0xC2, 0x94 } };
    const GUID GbPowerGuidPowerSchemePersonality = { 0x245D8541, 0x3943, 0x4422, { 0xB0, 0x25, 0x13, 0xA7, 0x84, 0xF6, 0x79, 0xB7 } };
    const GUID GbPowerGuidAcdcPowerSource = { 0x5D3E9A59, 0xE9D5, 0x4B00, { 0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48 } };
    const GUID GbPowerGuidBatteryPercentageRemaining = { 0xA7AD8041, 0xB45A, 0x4CAE, { 0x87, 0xA3, 0xEE, 0xCB, 0xB4, 0x68, 0xA9, 0xE1 } };
    const GUID GbPowerGuidPowerSavingStatus = { 0xE00958C0, 0xC213, 0x4ACE, { 0xAC, 0x77, 0xFE, 0xCC, 0xED, 0x2E, 0xEE, 0xA5 } };
    const GUID GbPowerGuidActivePowerScheme = { 0x31F9F286, 0x5084, 0x42FE, { 0xB7, 0x20, 0x2B, 0x02, 0x64, 0x99, 0x37, 0x63 } };
    const GUID GbPowerGuidConsoleDisplayState = { 0x6FE69556, 0x704A, 0x47A0, { 0x8F, 0x24, 0xC2, 0x8D, 0x93, 0x6F, 0xDA, 0x47 } };
    const GUID GbPowerGuidMonitorPowerOn = { 0x02731015, 0x4510, 0x4526, { 0x99, 0xE6, 0xE5, 0xA1, 0x7E, 0xBD, 0x1A, 0xEA } };
    const GUID GbPowerGuidSessionDisplayStatus = { 0x2B84C20E, 0xAD23, 0x4DDF, { 0x93, 0xDB, 0x05, 0xFF, 0xBD, 0x7E, 0xFC, 0xA5 } };
    const GUID GbPowerGuidGlobalUserPresence = { 0x786E8A1D, 0xB427, 0x4344, { 0x92, 0x07, 0x09, 0xE7, 0x0B, 0xDC, 0xBE, 0xA9 } };
    const GUID GbPowerGuidSessionUserPresence = { 0x3C0F4548, 0xC03F, 0x4C4D, { 0xB9, 0xF2, 0x23, 0x7E, 0xDE, 0x68, 0x63, 0x76 } };

    GB_SystemResult Win32ErrorToResult(const DWORD errorCode, const std::string& operationName, const std::string& message)
    {
        return GB_SystemResult::FromWin32Error(static_cast<uint32_t>(errorCode), operationName, message);
    }

    GB_SystemResult PowerErrorToResult(const DWORD errorCode, const std::string& operationName, const std::string& message)
    {
        return GB_SystemResult::FromWin32Error(static_cast<uint32_t>(errorCode), operationName, message);
    }

    void ClearPowerRequestsByFlags(HANDLE requestHandle, const uint32_t appliedRequestFlags) noexcept
    {
        if (requestHandle == nullptr || requestHandle == INVALID_HANDLE_VALUE)
        {
            return;
        }

        if ((appliedRequestFlags & RequestFlagAwayMode) != 0)
        {
            (void)::PowerClearRequest(requestHandle, PowerRequestAwayModeRequired);
        }
        if ((appliedRequestFlags & RequestFlagDisplay) != 0)
        {
            (void)::PowerClearRequest(requestHandle, PowerRequestDisplayRequired);
        }
        if ((appliedRequestFlags & RequestFlagSystem) != 0)
        {
            (void)::PowerClearRequest(requestHandle, PowerRequestSystemRequired);
        }
    }

    bool TryUtf8ToWide(const std::string& text, std::wstring& wideText)
    {
        wideText.clear();
        if (ContainsNullCharacter(text))
        {
            return false;
        }

        try
        {
            wideText = GB_Utf8ToWString(text);
        }
        catch (...)
        {
            wideText.clear();
            return false;
        }

        return true;
    }

    int HexValue(const wchar_t character)
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

    bool ParseHexValue(const std::wstring& text, const size_t startIndex, const size_t length, uint64_t& value)
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

    bool TryParseGuidString(const std::string& guidText, GUID& guid)
    {
        guid = GUID();
        if (guidText.empty() || ContainsNullCharacter(guidText))
        {
            return false;
        }

        std::wstring text;
        if (!TryUtf8ToWide(guidText, text))
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

    std::string GuidToString(const GUID& guid)
    {
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", static_cast<unsigned int>(guid.Data1), static_cast<unsigned int>(guid.Data2), static_cast<unsigned int>(guid.Data3), static_cast<unsigned int>(guid.Data4[0]), static_cast<unsigned int>(guid.Data4[1]), static_cast<unsigned int>(guid.Data4[2]), static_cast<unsigned int>(guid.Data4[3]), static_cast<unsigned int>(guid.Data4[4]), static_cast<unsigned int>(guid.Data4[5]), static_cast<unsigned int>(guid.Data4[6]), static_cast<unsigned int>(guid.Data4[7]));
        return std::string(buffer);
    }

    bool EqualsGuid(const GUID& leftGuid, const GUID& rightGuid)
    {
        return ::IsEqualGUID(leftGuid, rightGuid) != FALSE;
    }

    std::string WidePowerBufferToUtf8(const std::vector<UCHAR>& buffer, const DWORD byteCount)
    {
        if (buffer.empty() || byteCount == 0 || byteCount > buffer.size() || (byteCount % sizeof(wchar_t)) != 0)
        {
            return std::string();
        }

        const size_t totalWcharCount = static_cast<size_t>(byteCount / sizeof(wchar_t));
        std::wstring wideText;
        try
        {
            wideText.assign(totalWcharCount, L'\0');
        }
        catch (...)
        {
            return std::string();
        }

        if (!wideText.empty())
        {
            std::memcpy(&wideText[0], buffer.data(), byteCount);
        }

        while (!wideText.empty() && wideText.back() == L'\0')
        {
            wideText.pop_back();
        }
        if (wideText.empty())
        {
            return std::string();
        }

        try
        {
            return GB_WStringToUtf8(wideText);
        }
        catch (...)
        {
            return std::string();
        }
    }

    typedef DWORD(WINAPI* PowerReadTextFunc)(HKEY, const GUID*, const GUID*, const GUID*, PUCHAR, LPDWORD);

    std::string ReadPowerTextValue(const GUID& schemeGuid, PowerReadTextFunc readFunc)
    {
        if (readFunc == nullptr)
        {
            return std::string();
        }

        DWORD bufferSize = 0;
        DWORD errorCode = readFunc(nullptr, &schemeGuid, nullptr, nullptr, nullptr, &bufferSize);
        if (errorCode != ERROR_SUCCESS && errorCode != ERROR_MORE_DATA)
        {
            return std::string();
        }

        for (size_t attemptIndex = 0; attemptIndex < 2; attemptIndex++)
        {
            if (bufferSize == 0 || bufferSize > 64u * 1024u)
            {
                return std::string();
            }

            std::vector<UCHAR> buffer;
            try
            {
                buffer.assign(bufferSize, 0);
            }
            catch (...)
            {
                return std::string();
            }

            DWORD actualBufferSize = bufferSize;
            errorCode = readFunc(nullptr, &schemeGuid, nullptr, nullptr, buffer.data(), &actualBufferSize);
            if (errorCode == ERROR_SUCCESS)
            {
                return WidePowerBufferToUtf8(buffer, actualBufferSize);
            }
            if (errorCode != ERROR_MORE_DATA || actualBufferSize <= bufferSize)
            {
                return std::string();
            }

            bufferSize = actualBufferSize;
        }

        return std::string();
    }

    GB_SystemPowerSleepState MapSystemPowerState(const SYSTEM_POWER_STATE powerState)
    {
        switch (powerState)
        {
        case PowerSystemWorking:
            return GB_SystemPowerSleepState::Working;
        case PowerSystemSleeping1:
            return GB_SystemPowerSleepState::Sleeping1;
        case PowerSystemSleeping2:
            return GB_SystemPowerSleepState::Sleeping2;
        case PowerSystemSleeping3:
            return GB_SystemPowerSleepState::Sleeping3;
        case PowerSystemHibernate:
            return GB_SystemPowerSleepState::Hibernate;
        case PowerSystemShutdown:
            return GB_SystemPowerSleepState::Shutdown;
        default:
            return GB_SystemPowerSleepState::Unknown;
        }
    }

    void FillPowerStatusFromNative(const SYSTEM_POWER_STATUS& nativeStatus, GB_SystemPowerStatus& powerStatus)
    {
        powerStatus = GB_SystemPowerStatus();
        powerStatus.rawAcLineStatus = static_cast<uint8_t>(nativeStatus.ACLineStatus);
        powerStatus.rawBatteryFlag = static_cast<uint8_t>(nativeStatus.BatteryFlag);
        powerStatus.rawBatteryLifePercent = static_cast<uint8_t>(nativeStatus.BatteryLifePercent);
        powerStatus.rawSystemStatusFlag = static_cast<uint8_t>(nativeStatus.SystemStatusFlag);
        powerStatus.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();

        if (nativeStatus.ACLineStatus == 1)
        {
            powerStatus.powerSource = GB_SystemPowerSource::AC;
            powerStatus.isAcOnline = true;
        }
        else if (nativeStatus.ACLineStatus == 0)
        {
            powerStatus.powerSource = GB_SystemPowerSource::Battery;
            powerStatus.isOnBattery = true;
        }
        else
        {
            powerStatus.powerSource = GB_SystemPowerSource::Unknown;
        }

        const bool hasKnownBatteryFlag = nativeStatus.BatteryFlag != 255;
        if (hasKnownBatteryFlag)
        {
            powerStatus.batteryPresent = (nativeStatus.BatteryFlag & 128) == 0;
            powerStatus.isBatteryCritical = (nativeStatus.BatteryFlag & 4) != 0;
            powerStatus.isBatteryLow = (nativeStatus.BatteryFlag & 2) != 0 || powerStatus.isBatteryCritical;
            powerStatus.isCharging = (nativeStatus.BatteryFlag & 8) != 0;
        }

        if (nativeStatus.ACLineStatus == 0 && hasKnownBatteryFlag && !powerStatus.batteryPresent)
        {
            powerStatus.powerSource = GB_SystemPowerSource::Offline;
        }

        if (nativeStatus.BatteryLifePercent != 255 && nativeStatus.BatteryLifePercent <= 100)
        {
            powerStatus.hasBatteryPercent = true;
            powerStatus.batteryPercent = static_cast<uint8_t>(nativeStatus.BatteryLifePercent);
            if (!hasKnownBatteryFlag)
            {
                powerStatus.batteryPresent = true;
            }
        }

        if (nativeStatus.BatteryLifeTime != static_cast<DWORD>(-1))
        {
            powerStatus.hasBatteryLifeSeconds = true;
            powerStatus.batteryLifeSeconds = static_cast<uint32_t>(nativeStatus.BatteryLifeTime);
        }

        if (nativeStatus.BatteryFullLifeTime != static_cast<DWORD>(-1))
        {
            powerStatus.hasBatteryFullLifeSeconds = true;
            powerStatus.batteryFullLifeSeconds = static_cast<uint32_t>(nativeStatus.BatteryFullLifeTime);
        }

        powerStatus.isBatterySaverOn = (nativeStatus.SystemStatusFlag & 1) != 0;
    }

    void FillPowerCapabilitiesFromNative(const SYSTEM_POWER_CAPABILITIES& nativeCapabilities, GB_SystemPowerCapabilities& powerCapabilities)
    {
        powerCapabilities = GB_SystemPowerCapabilities();
        powerCapabilities.hasPowerButton = nativeCapabilities.PowerButtonPresent != FALSE;
        powerCapabilities.hasSleepButton = nativeCapabilities.SleepButtonPresent != FALSE;
        powerCapabilities.hasLidSwitch = nativeCapabilities.LidPresent != FALSE;
        powerCapabilities.supportsS1 = nativeCapabilities.SystemS1 != FALSE;
        powerCapabilities.supportsS2 = nativeCapabilities.SystemS2 != FALSE;
        powerCapabilities.supportsS3 = nativeCapabilities.SystemS3 != FALSE;
        powerCapabilities.supportsS4 = nativeCapabilities.SystemS4 != FALSE;
        powerCapabilities.supportsS5 = nativeCapabilities.SystemS5 != FALSE;
        powerCapabilities.hasHibernationFile = nativeCapabilities.HiberFilePresent != FALSE;
        powerCapabilities.supportsFastS4 = nativeCapabilities.FastSystemS4 != FALSE;
        powerCapabilities.supportsHiberboot = nativeCapabilities.Hiberboot != FALSE;
        powerCapabilities.supportsWakeAlarm = nativeCapabilities.WakeAlarmPresent != FALSE;
        powerCapabilities.supportsAoAc = nativeCapabilities.AoAc != FALSE;
        powerCapabilities.supportsFullWake = nativeCapabilities.FullWake != FALSE;
        powerCapabilities.supportsVideoDimming = nativeCapabilities.VideoDimPresent != FALSE;
        powerCapabilities.hasApm = nativeCapabilities.ApmPresent != FALSE;
        powerCapabilities.hasUps = nativeCapabilities.UpsPresent != FALSE;
        powerCapabilities.hasBattery = nativeCapabilities.SystemBatteriesPresent != FALSE;
        powerCapabilities.batteriesAreShortTerm = nativeCapabilities.BatteriesAreShortTerm != FALSE;
        powerCapabilities.hasThermalControl = nativeCapabilities.ThermalControl != FALSE;
        powerCapabilities.supportsProcessorThrottle = nativeCapabilities.ProcessorThrottle != FALSE;
        powerCapabilities.supportsDiskSpinDown = nativeCapabilities.DiskSpinDown != FALSE;
        powerCapabilities.supportsSleep = powerCapabilities.supportsS1 || powerCapabilities.supportsS2 || powerCapabilities.supportsS3 || powerCapabilities.supportsAoAc;
        powerCapabilities.supportsHibernate = powerCapabilities.supportsS4 && powerCapabilities.hasHibernationFile;
        powerCapabilities.acOnlineWakeState = MapSystemPowerState(nativeCapabilities.AcOnLineWake);
        powerCapabilities.lidWakeState = MapSystemPowerState(nativeCapabilities.SoftLidWake);
        powerCapabilities.rtcWakeState = MapSystemPowerState(nativeCapabilities.RtcWake);
        powerCapabilities.minDeviceWakeState = MapSystemPowerState(nativeCapabilities.MinDeviceWakeState);
        powerCapabilities.defaultLowLatencyWakeState = MapSystemPowerState(nativeCapabilities.DefaultLowLatencyWake);
        powerCapabilities.processorMinThrottle = static_cast<uint8_t>(nativeCapabilities.ProcessorMinThrottle);
        powerCapabilities.processorMaxThrottle = static_cast<uint8_t>(nativeCapabilities.ProcessorMaxThrottle);
    }

    bool GetActiveSchemeGuid(GUID& activeSchemeGuid, DWORD& errorCode)
    {
        activeSchemeGuid = GUID();
        errorCode = ERROR_SUCCESS;

        GUID* nativeActiveSchemeGuid = nullptr;
        errorCode = ::PowerGetActiveScheme(nullptr, &nativeActiveSchemeGuid);
        if (errorCode != ERROR_SUCCESS)
        {
            return false;
        }
        if (nativeActiveSchemeGuid == nullptr)
        {
            errorCode = ERROR_INVALID_DATA;
            return false;
        }

        activeSchemeGuid = *nativeActiveSchemeGuid;
        (void)::LocalFree(nativeActiveSchemeGuid);
        return true;
    }

    bool TryReadPowerPlanPersonalityGuid(const GUID& schemeGuid, GUID& personalityGuid)
    {
        personalityGuid = GUID();

        DWORD valueType = 0;
        DWORD bufferSize = static_cast<DWORD>(sizeof(personalityGuid));
        DWORD errorCode = ::PowerReadACValue(nullptr, &schemeGuid, &GbPowerGuidNoSubgroup, &GbPowerGuidPowerSchemePersonality, &valueType, reinterpret_cast<UCHAR*>(&personalityGuid), &bufferSize);
        if (errorCode == ERROR_SUCCESS && bufferSize == sizeof(personalityGuid))
        {
            return true;
        }

        valueType = 0;
        bufferSize = static_cast<DWORD>(sizeof(personalityGuid));
        errorCode = ::PowerReadDCValue(nullptr, &schemeGuid, &GbPowerGuidNoSubgroup, &GbPowerGuidPowerSchemePersonality, &valueType, reinterpret_cast<UCHAR*>(&personalityGuid), &bufferSize);
        return errorCode == ERROR_SUCCESS && bufferSize == sizeof(personalityGuid);
    }

    GB_SystemPowerPlanPersonality ClassifyPowerPlanPersonality(const GUID& schemeGuid)
    {
        GUID personalityGuid = {};
        const GUID& effectivePersonalityGuid = TryReadPowerPlanPersonalityGuid(schemeGuid, personalityGuid) ? personalityGuid : schemeGuid;

        if (EqualsGuid(effectivePersonalityGuid, GbPowerGuidMaxPowerSavings))
        {
            return GB_SystemPowerPlanPersonality::PowerSaver;
        }
        if (EqualsGuid(effectivePersonalityGuid, GbPowerGuidTypicalPowerSavings))
        {
            return GB_SystemPowerPlanPersonality::Balanced;
        }
        if (EqualsGuid(effectivePersonalityGuid, GbPowerGuidMinPowerSavings))
        {
            return GB_SystemPowerPlanPersonality::HighPerformance;
        }

        return GB_SystemPowerPlanPersonality::Unknown;
    }

    void FillPowerPlanInfo(const GUID& schemeGuid, const GUID& activeSchemeGuid, const bool hasActiveSchemeGuid, GB_SystemPowerPlanInfo& powerPlan)
    {
        powerPlan = GB_SystemPowerPlanInfo();
        powerPlan.schemeGuid = GuidToString(schemeGuid);
        powerPlan.friendlyNameUtf8 = ReadPowerTextValue(schemeGuid, &PowerReadFriendlyName);
        powerPlan.descriptionUtf8 = ReadPowerTextValue(schemeGuid, &PowerReadDescription);
        powerPlan.isActive = hasActiveSchemeGuid && EqualsGuid(schemeGuid, activeSchemeGuid);
        powerPlan.personality = ClassifyPowerPlanPersonality(schemeGuid);
    }

    DWORD BuildExitWindowsFlags(const GB_SystemPowerActionOptions& options)
    {
        DWORD flags = 0;
        switch (options.actionType)
        {
        case GB_SystemPowerActionType::Shutdown:
            flags = EWX_SHUTDOWN;
            break;
        case GB_SystemPowerActionType::PowerOff:
            flags = EWX_POWEROFF;
            break;
        case GB_SystemPowerActionType::Reboot:
            flags = EWX_REBOOT;
            break;
        case GB_SystemPowerActionType::HybridShutdown:
            flags = EWX_SHUTDOWN | EWX_HYBRID_SHUTDOWN;
            break;
        default:
            flags = 0;
            break;
        }

        if (options.forceApplications)
        {
            flags |= EWX_FORCE;
        }
        if (options.forceIfHung)
        {
            flags |= EWX_FORCEIFHUNG;
        }
        if (options.rebootRegisteredApplications)
        {
            flags |= EWX_RESTARTAPPS;
        }

        return flags;
    }

    GB_SystemPowerEventType MapPowerSettingGuidToEventType(const GUID& settingGuid)
    {
        if (EqualsGuid(settingGuid, GbPowerGuidAcdcPowerSource))
        {
            return GB_SystemPowerEventType::PowerSourceChanged;
        }
        if (EqualsGuid(settingGuid, GbPowerGuidBatteryPercentageRemaining))
        {
            return GB_SystemPowerEventType::BatteryPercentageChanged;
        }
        if (EqualsGuid(settingGuid, GbPowerGuidPowerSavingStatus))
        {
            return GB_SystemPowerEventType::BatterySaverStatusChanged;
        }
        if (EqualsGuid(settingGuid, GbPowerGuidActivePowerScheme))
        {
            return GB_SystemPowerEventType::ActivePowerPlanChanged;
        }
        if (EqualsGuid(settingGuid, GbPowerGuidPowerSchemePersonality))
        {
            return GB_SystemPowerEventType::PowerPlanPersonalityChanged;
        }
        if (EqualsGuid(settingGuid, GbPowerGuidConsoleDisplayState) || EqualsGuid(settingGuid, GbPowerGuidMonitorPowerOn) || EqualsGuid(settingGuid, GbPowerGuidSessionDisplayStatus))
        {
            return GB_SystemPowerEventType::DisplayStateChanged;
        }
        if (EqualsGuid(settingGuid, GbPowerGuidGlobalUserPresence) || EqualsGuid(settingGuid, GbPowerGuidSessionUserPresence))
        {
            return GB_SystemPowerEventType::UserPresenceChanged;
        }

        return GB_SystemPowerEventType::PowerSettingChanged;
    }

    GB_SystemPowerEventType MapPowerBroadcastToEventType(const WPARAM wParam, const bool hasPowerSetting, const GUID& settingGuid)
    {
        if (wParam == PBT_POWERSETTINGCHANGE && hasPowerSetting)
        {
            return MapPowerSettingGuidToEventType(settingGuid);
        }

        switch (wParam)
        {
        case PBT_APMSUSPEND:
            return GB_SystemPowerEventType::Suspend;
        case PBT_APMRESUMEAUTOMATIC:
            return GB_SystemPowerEventType::ResumeAutomatic;
        case PBT_APMRESUMESUSPEND:
            return GB_SystemPowerEventType::ResumeUser;
        case PBT_APMRESUMECRITICAL:
            return GB_SystemPowerEventType::ResumeCritical;
        case PBT_APMPOWERSTATUSCHANGE:
            return GB_SystemPowerEventType::PowerStatusChanged;
#ifdef PBT_APMBATTERYLOW
        case PBT_APMBATTERYLOW:
            return GB_SystemPowerEventType::PowerStatusChanged;
#endif
        default:
            break;
        }

        return GB_SystemPowerEventType::Unknown;
    }
#endif
}

GB_SystemPowerKeepAwakeRequest::GB_SystemPowerKeepAwakeRequest() = default;

GB_SystemPowerKeepAwakeRequest::~GB_SystemPowerKeepAwakeRequest() noexcept
{
    ReleaseNoThrow();
}

GB_SystemPowerKeepAwakeRequest::GB_SystemPowerKeepAwakeRequest(GB_SystemPowerKeepAwakeRequest&& other) noexcept
{
    MoveFrom(other);
}

GB_SystemPowerKeepAwakeRequest& GB_SystemPowerKeepAwakeRequest::operator=(GB_SystemPowerKeepAwakeRequest&& other) noexcept
{
    if (this != &other)
    {
        ReleaseNoThrow();
        MoveFrom(other);
    }

    return *this;
}

GB_SystemResult GB_SystemPowerKeepAwakeRequest::Release()
{
#if defined(_WIN32)
    if (requestHandle == nullptr || requestHandle == INVALID_HANDLE_VALUE)
    {
        ClearState();
        return GB_SystemResult::Succeeded(u8"GB_SystemPowerKeepAwakeRequest::Release");
    }

    HANDLE nativeHandle = static_cast<HANDLE>(requestHandle);
    const uint32_t localRequestFlags = appliedRequestFlags;
    GB_SystemResult firstFailure = GB_SystemResult::Succeeded(u8"GB_SystemPowerKeepAwakeRequest::Release");

    if ((localRequestFlags & RequestFlagAwayMode) != 0 && ::PowerClearRequest(nativeHandle, PowerRequestAwayModeRequired) == FALSE && firstFailure.IsSucceeded())
    {
        firstFailure = GB_SystemResult::FromLastWin32Error(u8"GB_SystemPowerKeepAwakeRequest::Release", u8"清理 Away Mode 保持唤醒请求失败。");
    }
    if ((localRequestFlags & RequestFlagDisplay) != 0 && ::PowerClearRequest(nativeHandle, PowerRequestDisplayRequired) == FALSE && firstFailure.IsSucceeded())
    {
        firstFailure = GB_SystemResult::FromLastWin32Error(u8"GB_SystemPowerKeepAwakeRequest::Release", u8"清理显示器保持唤醒请求失败。");
    }
    if ((localRequestFlags & RequestFlagSystem) != 0 && ::PowerClearRequest(nativeHandle, PowerRequestSystemRequired) == FALSE && firstFailure.IsSucceeded())
    {
        firstFailure = GB_SystemResult::FromLastWin32Error(u8"GB_SystemPowerKeepAwakeRequest::Release", u8"清理系统保持唤醒请求失败。");
    }

    if (::CloseHandle(nativeHandle) == FALSE && firstFailure.IsSucceeded())
    {
        firstFailure = GB_SystemResult::FromLastWin32Error(u8"GB_SystemPowerKeepAwakeRequest::Release", u8"关闭 Power Request 句柄失败。");
    }

    ClearState();
    return firstFailure;
#else
    ClearState();
    return GB_SystemResult::Succeeded("GB_SystemPowerKeepAwakeRequest::Release");
#endif
}

void GB_SystemPowerKeepAwakeRequest::ReleaseNoThrow() noexcept
{
#if defined(_WIN32)
    if (requestHandle != nullptr && requestHandle != INVALID_HANDLE_VALUE)
    {
        HANDLE nativeHandle = static_cast<HANDLE>(requestHandle);
        const uint32_t localRequestFlags = appliedRequestFlags;

        if ((localRequestFlags & RequestFlagAwayMode) != 0)
        {
            (void)::PowerClearRequest(nativeHandle, PowerRequestAwayModeRequired);
        }
        if ((localRequestFlags & RequestFlagDisplay) != 0)
        {
            (void)::PowerClearRequest(nativeHandle, PowerRequestDisplayRequired);
        }
        if ((localRequestFlags & RequestFlagSystem) != 0)
        {
            (void)::PowerClearRequest(nativeHandle, PowerRequestSystemRequired);
        }
        (void)::CloseHandle(nativeHandle);
    }
#endif
    ClearState();
}

bool GB_SystemPowerKeepAwakeRequest::IsActive() const
{
#if defined(_WIN32)
    return requestHandle != nullptr && requestHandle != INVALID_HANDLE_VALUE;
#else
    return false;
#endif
}

GB_SystemPowerKeepAwakeRequest::operator bool() const
{
    return IsActive();
}

GB_SystemPowerKeepAwakeOptions GB_SystemPowerKeepAwakeRequest::GetOptions() const
{
    return options;
}

void GB_SystemPowerKeepAwakeRequest::MoveFrom(GB_SystemPowerKeepAwakeRequest& other) noexcept
{
    requestHandle = other.requestHandle;
    appliedRequestFlags = other.appliedRequestFlags;
    options.mode = other.options.mode;
    options.reasonUtf8.swap(other.options.reasonUtf8);
    other.ClearState();
}

void GB_SystemPowerKeepAwakeRequest::ClearState() noexcept
{
    requestHandle = nullptr;
    appliedRequestFlags = 0;
    options.mode = GB_SystemPowerKeepAwakeMode::System;
    options.reasonUtf8.clear();
}

GB_SystemResult GB_SystemPower::GetPowerStatus(GB_SystemPowerStatus& powerStatus)
{
    powerStatus = GB_SystemPowerStatus();
#if defined(_WIN32)
    SYSTEM_POWER_STATUS nativeStatus = {};
    if (::GetSystemPowerStatus(&nativeStatus) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error(u8"GB_SystemPower::GetPowerStatus", u8"读取系统电源状态失败。");
    }

    FillPowerStatusFromNative(nativeStatus, powerStatus);
    return GB_SystemResult::Succeeded(u8"GB_SystemPower::GetPowerStatus");
#else
    return MakeUnsupportedPlatformResult("GB_SystemPower::GetPowerStatus");
#endif
}

GB_SystemResult GB_SystemPower::GetPowerCapabilities(GB_SystemPowerCapabilities& powerCapabilities)
{
    powerCapabilities = GB_SystemPowerCapabilities();
#if defined(_WIN32)
    SYSTEM_POWER_CAPABILITIES nativeCapabilities = {};
    if (::GetPwrCapabilities(&nativeCapabilities) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error(u8"GB_SystemPower::GetPowerCapabilities", u8"读取系统电源能力失败。");
    }

    FillPowerCapabilitiesFromNative(nativeCapabilities, powerCapabilities);
    return GB_SystemResult::Succeeded(u8"GB_SystemPower::GetPowerCapabilities");
#else
    return MakeUnsupportedPlatformResult("GB_SystemPower::GetPowerCapabilities");
#endif
}

GB_SystemResult GB_SystemPower::RequestPowerAction(const GB_SystemPowerActionOptions& options)
{
#if defined(_WIN32)
    if (!IsValidPowerActionType(options.actionType))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPower::RequestPowerAction", u8"电源动作类型无效。");
    }
    if (options.forceApplications && options.forceIfHung)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPower::RequestPowerAction", u8"forceApplications 与 forceIfHung 不能同时启用。");
    }
    if (options.rebootRegisteredApplications && options.actionType != GB_SystemPowerActionType::Reboot)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPower::RequestPowerAction", u8"rebootRegisteredApplications 只能用于重启动作。");
    }

    GB_PrivilegeScope privilegeScope;
    GB_SystemResult privilegeResult = privilegeScope.Enable(GB_WindowsPrivilege::Shutdown, u8"GB_SystemPower::RequestPowerAction");
    if (privilegeResult.IsFailed())
    {
        return privilegeResult.WithOperationName(u8"GB_SystemPower::RequestPowerAction").WithMessage(u8"启用 SeShutdownPrivilege 权限失败。");
    }

    const DWORD flags = BuildExitWindowsFlags(options);
    const DWORD reasonCode = ResolveShutdownReasonCode(options.reasonCode);
#if defined(_MSC_VER)
#  pragma warning(suppress: 28159)
#endif
    if (::ExitWindowsEx(flags, reasonCode) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error(u8"GB_SystemPower::RequestPowerAction", u8"发起系统电源动作失败。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemPower::RequestPowerAction", u8"系统已接受电源动作请求；该结果不代表动作最终一定完成。");
#else
    (void)options;
    return MakeUnsupportedPlatformResult("GB_SystemPower::RequestPowerAction");
#endif
}

GB_SystemResult GB_SystemPower::Shutdown(const bool forceApplications)
{
    GB_SystemPowerActionOptions options;
    options.actionType = GB_SystemPowerActionType::Shutdown;
    options.forceApplications = forceApplications;
    return RequestPowerAction(options).WithOperationName(u8"GB_SystemPower::Shutdown");
}

GB_SystemResult GB_SystemPower::PowerOff(const bool forceApplications)
{
    GB_SystemPowerActionOptions options;
    options.actionType = GB_SystemPowerActionType::PowerOff;
    options.forceApplications = forceApplications;
    return RequestPowerAction(options).WithOperationName(u8"GB_SystemPower::PowerOff");
}

GB_SystemResult GB_SystemPower::Reboot(const bool forceApplications)
{
    GB_SystemPowerActionOptions options;
    options.actionType = GB_SystemPowerActionType::Reboot;
    options.forceApplications = forceApplications;
    return RequestPowerAction(options).WithOperationName(u8"GB_SystemPower::Reboot");
}

GB_SystemResult GB_SystemPower::HybridShutdown(const bool forceApplications)
{
    GB_SystemPowerActionOptions options;
    options.actionType = GB_SystemPowerActionType::HybridShutdown;
    options.forceApplications = forceApplications;
    return RequestPowerAction(options).WithOperationName(u8"GB_SystemPower::HybridShutdown");
}

GB_SystemResult GB_SystemPower::Sleep(const GB_SystemSuspendOptions& options)
{
#if defined(_WIN32)
    if (options.checkCapabilityBeforeCall)
    {
        GB_SystemPowerCapabilities powerCapabilities;
        GB_SystemResult capabilitiesResult = GetPowerCapabilities(powerCapabilities);
        if (capabilitiesResult.IsFailed())
        {
            return capabilitiesResult.WithOperationName(u8"GB_SystemPower::Sleep");
        }
        if (!powerCapabilities.supportsSleep)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemPower::Sleep", u8"当前系统不报告可用的睡眠能力。");
        }
    }

    GB_PrivilegeScope privilegeScope;
    GB_SystemResult privilegeResult = privilegeScope.Enable(GB_WindowsPrivilege::Shutdown, u8"GB_SystemPower::Sleep");
    if (privilegeResult.IsFailed())
    {
        return privilegeResult.WithOperationName(u8"GB_SystemPower::Sleep").WithMessage(u8"启用 SeShutdownPrivilege 权限失败。");
    }

    if (::SetSuspendState(FALSE, options.force ? TRUE : FALSE, options.disableWakeEvents ? TRUE : FALSE) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error(u8"GB_SystemPower::Sleep", u8"发起系统睡眠失败。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemPower::Sleep", u8"系统已接受睡眠请求；该结果不代表睡眠最终一定完成。");
#else
    (void)options;
    return MakeUnsupportedPlatformResult("GB_SystemPower::Sleep");
#endif
}

GB_SystemResult GB_SystemPower::Hibernate(const GB_SystemSuspendOptions& options)
{
#if defined(_WIN32)
    if (options.checkCapabilityBeforeCall)
    {
        GB_SystemPowerCapabilities powerCapabilities;
        GB_SystemResult capabilitiesResult = GetPowerCapabilities(powerCapabilities);
        if (capabilitiesResult.IsFailed())
        {
            return capabilitiesResult.WithOperationName(u8"GB_SystemPower::Hibernate");
        }
        if (!powerCapabilities.supportsHibernate)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemPower::Hibernate", u8"当前系统不报告可用的休眠能力。");
        }
    }

    GB_PrivilegeScope privilegeScope;
    GB_SystemResult privilegeResult = privilegeScope.Enable(GB_WindowsPrivilege::Shutdown, u8"GB_SystemPower::Hibernate");
    if (privilegeResult.IsFailed())
    {
        return privilegeResult.WithOperationName(u8"GB_SystemPower::Hibernate").WithMessage(u8"启用 SeShutdownPrivilege 权限失败。");
    }

    if (::SetSuspendState(TRUE, options.force ? TRUE : FALSE, options.disableWakeEvents ? TRUE : FALSE) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error(u8"GB_SystemPower::Hibernate", u8"发起系统休眠失败。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemPower::Hibernate", u8"系统已接受休眠请求；该结果不代表休眠最终一定完成。");
#else
    (void)options;
    return MakeUnsupportedPlatformResult("GB_SystemPower::Hibernate");
#endif
}

GB_SystemResult GB_SystemPower::ScheduleShutdown(const GB_SystemScheduledShutdownOptions& options)
{
#if defined(_WIN32)
    if (options.delaySeconds > MaxScheduledShutdownDelaySeconds)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPower::ScheduleShutdown", u8"计划关机延迟时间不能超过 Windows MAX_SHUTDOWN_TIMEOUT 限制。");
    }

    const std::string messageUtf8 = options.messageUtf8.empty() ? std::string(u8"GlobalBase 计划关机或计划重启请求。") : options.messageUtf8;
    std::wstring messageWide;
    if (!TryUtf8ToWide(messageUtf8, messageWide))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, u8"GB_SystemPower::ScheduleShutdown", u8"计划关机提示消息不是有效 UTF-8 文本或包含 NUL 字符。");
    }

    GB_PrivilegeScope privilegeScope;
    GB_SystemResult privilegeResult = privilegeScope.Enable(GB_WindowsPrivilege::Shutdown, u8"GB_SystemPower::ScheduleShutdown");
    if (privilegeResult.IsFailed())
    {
        return privilegeResult.WithOperationName(u8"GB_SystemPower::ScheduleShutdown").WithMessage(u8"启用 SeShutdownPrivilege 权限失败。");
    }

    LPWSTR messagePointer = const_cast<LPWSTR>(messageWide.c_str());
    const DWORD reasonCode = ResolveShutdownReasonCode(options.reasonCode);
#if defined(_MSC_VER)
#  pragma warning(suppress: 28159 28160)
#endif
    if (::InitiateSystemShutdownExW(nullptr, messagePointer, static_cast<DWORD>(options.delaySeconds), options.forceApplications ? TRUE : FALSE, options.rebootAfterShutdown ? TRUE : FALSE, reasonCode) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error(u8"GB_SystemPower::ScheduleShutdown", u8"发起计划关机或计划重启失败。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemPower::ScheduleShutdown", u8"系统已接受计划关机或计划重启请求。");
#else
    (void)options;
    return MakeUnsupportedPlatformResult("GB_SystemPower::ScheduleShutdown");
#endif
}

GB_SystemResult GB_SystemPower::AbortScheduledShutdown()
{
#if defined(_WIN32)
    GB_PrivilegeScope privilegeScope;
    GB_SystemResult privilegeResult = privilegeScope.Enable(GB_WindowsPrivilege::Shutdown, u8"GB_SystemPower::AbortScheduledShutdown");
    if (privilegeResult.IsFailed())
    {
        return privilegeResult.WithOperationName(u8"GB_SystemPower::AbortScheduledShutdown").WithMessage(u8"启用 SeShutdownPrivilege 权限失败。");
    }

    if (::AbortSystemShutdownW(nullptr) == FALSE)
    {
        return GB_SystemResult::FromLastWin32Error(u8"GB_SystemPower::AbortScheduledShutdown", u8"取消计划关机失败。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemPower::AbortScheduledShutdown");
#else
    return MakeUnsupportedPlatformResult("GB_SystemPower::AbortScheduledShutdown");
#endif
}

GB_SystemResult GB_SystemPower::CreateKeepAwakeRequest(const GB_SystemPowerKeepAwakeOptions& options, GB_SystemPowerKeepAwakeRequest& request)
{
#if defined(_WIN32)
    if (!IsValidKeepAwakeMode(options.mode))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPower::CreateKeepAwakeRequest", u8"保持唤醒模式无效。");
    }
    if (options.reasonUtf8.empty() || ContainsNullCharacter(options.reasonUtf8))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPower::CreateKeepAwakeRequest", u8"保持唤醒请求必须提供非空 UTF-8 reason，且不能包含 NUL 字符。");
    }

    std::wstring reasonWide;
    if (!TryUtf8ToWide(options.reasonUtf8, reasonWide) || reasonWide.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, u8"GB_SystemPower::CreateKeepAwakeRequest", u8"保持唤醒 reason 从 UTF-8 转 UTF-16 失败。");
    }

    if (reasonWide.length() > MaxPowerRequestReasonLength)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPower::CreateKeepAwakeRequest", u8"保持唤醒 reason 过长。");
    }

    GB_SystemResult releaseResult = request.Release();
    if (releaseResult.IsFailed())
    {
        return releaseResult.WithOperationName(u8"GB_SystemPower::CreateKeepAwakeRequest").WithMessage(u8"创建新保持唤醒请求前释放旧请求失败。");
    }

    REASON_CONTEXT context = {};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = const_cast<PWSTR>(reasonWide.c_str());

    HANDLE requestHandle = ::PowerCreateRequest(&context);
    if (requestHandle == INVALID_HANDLE_VALUE || requestHandle == nullptr)
    {
        const DWORD lastError = ::GetLastError();
        const DWORD effectiveError = lastError == ERROR_SUCCESS ? ERROR_INVALID_HANDLE : lastError;
        return GB_SystemResult::FromWin32Error(effectiveError, u8"GB_SystemPower::CreateKeepAwakeRequest", u8"创建 Power Request 句柄失败。");
    }

    GB_SystemPowerKeepAwakeOptions storedOptions;
    storedOptions.mode = options.mode;
    try
    {
        storedOptions.reasonUtf8 = options.reasonUtf8;
    }
    catch (const std::bad_alloc&)
    {
        (void)::CloseHandle(requestHandle);
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemPower::CreateKeepAwakeRequest", u8"保存保持唤醒请求参数时内存不足。");
    }
    catch (...)
    {
        (void)::CloseHandle(requestHandle);
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemPower::CreateKeepAwakeRequest", u8"保存保持唤醒请求参数时发生未知异常。");
    }

    uint32_t appliedRequestFlags = 0;
    const bool needDisplay = options.mode == GB_SystemPowerKeepAwakeMode::Display || options.mode == GB_SystemPowerKeepAwakeMode::SystemAndDisplay;
    const bool needAwayMode = options.mode == GB_SystemPowerKeepAwakeMode::AwayMode;
    const bool needSystem = options.mode == GB_SystemPowerKeepAwakeMode::System || needDisplay || needAwayMode;

    if (needSystem && ::PowerSetRequest(requestHandle, PowerRequestSystemRequired) == FALSE)
    {
        const GB_SystemResult result = GB_SystemResult::FromLastWin32Error(u8"GB_SystemPower::CreateKeepAwakeRequest", u8"设置系统保持唤醒请求失败。");
        (void)::CloseHandle(requestHandle);
        return result;
    }
    if (needSystem)
    {
        appliedRequestFlags |= RequestFlagSystem;
    }

    if (needDisplay && ::PowerSetRequest(requestHandle, PowerRequestDisplayRequired) == FALSE)
    {
        const GB_SystemResult result = GB_SystemResult::FromLastWin32Error(u8"GB_SystemPower::CreateKeepAwakeRequest", u8"设置显示器保持唤醒请求失败。");
        ClearPowerRequestsByFlags(requestHandle, appliedRequestFlags);
        (void)::CloseHandle(requestHandle);
        return result;
    }
    if (needDisplay)
    {
        appliedRequestFlags |= RequestFlagDisplay;
    }

    if (needAwayMode && ::PowerSetRequest(requestHandle, PowerRequestAwayModeRequired) == FALSE)
    {
        const GB_SystemResult result = GB_SystemResult::FromLastWin32Error(u8"GB_SystemPower::CreateKeepAwakeRequest", u8"设置 Away Mode 保持唤醒请求失败。");
        ClearPowerRequestsByFlags(requestHandle, appliedRequestFlags);
        (void)::CloseHandle(requestHandle);
        return result;
    }
    if (needAwayMode)
    {
        appliedRequestFlags |= RequestFlagAwayMode;
    }

    request.options.mode = storedOptions.mode;
    request.options.reasonUtf8.swap(storedOptions.reasonUtf8);
    request.requestHandle = requestHandle;
    request.appliedRequestFlags = appliedRequestFlags;
    return GB_SystemResult::Succeeded(u8"GB_SystemPower::CreateKeepAwakeRequest");
#else
    (void)options;
    (void)request;
    return MakeUnsupportedPlatformResult("GB_SystemPower::CreateKeepAwakeRequest");
#endif
}

GB_SystemResult GB_SystemPower::EnumeratePowerPlans(std::vector<GB_SystemPowerPlanInfo>& powerPlans)
{
    powerPlans.clear();
#if defined(_WIN32)
    GUID activeSchemeGuid = {};
    DWORD activeErrorCode = ERROR_SUCCESS;
    const bool hasActiveSchemeGuid = GetActiveSchemeGuid(activeSchemeGuid, activeErrorCode);

    for (ULONG index = 0; index < (std::numeric_limits<ULONG>::max)(); index++)
    {
        GUID schemeGuid = {};
        DWORD bufferSize = static_cast<DWORD>(sizeof(schemeGuid));
        const DWORD errorCode = ::PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, reinterpret_cast<UCHAR*>(&schemeGuid), &bufferSize);
        if (errorCode == ERROR_NO_MORE_ITEMS)
        {
            break;
        }
        if (errorCode != ERROR_SUCCESS)
        {
            powerPlans.clear();
            return PowerErrorToResult(errorCode, u8"GB_SystemPower::EnumeratePowerPlans", u8"枚举电源方案失败。");
        }
        if (bufferSize != sizeof(schemeGuid))
        {
            powerPlans.clear();
            return GB_SystemResult::Failed(GB_SystemErrorCode::NativeApiFailed, u8"GB_SystemPower::EnumeratePowerPlans", u8"PowerEnumerate 返回的电源方案 GUID 大小无效。");
        }

        try
        {
            GB_SystemPowerPlanInfo powerPlan;
            FillPowerPlanInfo(schemeGuid, activeSchemeGuid, hasActiveSchemeGuid, powerPlan);
            powerPlans.push_back(std::move(powerPlan));
        }
        catch (const std::bad_alloc&)
        {
            powerPlans.clear();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemPower::EnumeratePowerPlans", u8"保存电源方案列表时内存不足。");
        }
        catch (...)
        {
            powerPlans.clear();
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemPower::EnumeratePowerPlans", u8"保存电源方案列表时发生未知异常。");
        }
    }

    if (!hasActiveSchemeGuid && powerPlans.empty())
    {
        return PowerErrorToResult(activeErrorCode, u8"GB_SystemPower::EnumeratePowerPlans", u8"读取活动电源方案失败，且未枚举到任何电源方案。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemPower::EnumeratePowerPlans");
#else
    return MakeUnsupportedPlatformResult("GB_SystemPower::EnumeratePowerPlans");
#endif
}

GB_SystemResult GB_SystemPower::GetActivePowerPlan(GB_SystemPowerPlanInfo& powerPlan)
{
    powerPlan = GB_SystemPowerPlanInfo();
#if defined(_WIN32)
    GUID activeSchemeGuid = {};
    DWORD errorCode = ERROR_SUCCESS;
    if (!GetActiveSchemeGuid(activeSchemeGuid, errorCode))
    {
        return PowerErrorToResult(errorCode, u8"GB_SystemPower::GetActivePowerPlan", u8"读取活动电源方案失败。");
    }

    try
    {
        FillPowerPlanInfo(activeSchemeGuid, activeSchemeGuid, true, powerPlan);
    }
    catch (const std::bad_alloc&)
    {
        powerPlan = GB_SystemPowerPlanInfo();
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemPower::GetActivePowerPlan", u8"保存活动电源方案信息时内存不足。");
    }
    catch (...)
    {
        powerPlan = GB_SystemPowerPlanInfo();
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemPower::GetActivePowerPlan", u8"保存活动电源方案信息时发生未知异常。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemPower::GetActivePowerPlan");
#else
    return MakeUnsupportedPlatformResult("GB_SystemPower::GetActivePowerPlan");
#endif
}

GB_SystemResult GB_SystemPower::SetActivePowerPlan(const std::string& schemeGuid)
{
#if defined(_WIN32)
    GUID parsedSchemeGuid = {};
    if (!TryParseGuidString(schemeGuid, parsedSchemeGuid))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPower::SetActivePowerPlan", u8"电源方案 GUID 格式无效。");
    }

    const DWORD errorCode = ::PowerSetActiveScheme(nullptr, &parsedSchemeGuid);
    if (errorCode != ERROR_SUCCESS)
    {
        return PowerErrorToResult(errorCode, u8"GB_SystemPower::SetActivePowerPlan", u8"设置活动电源方案失败。");
    }

    return GB_SystemResult::Succeeded(u8"GB_SystemPower::SetActivePowerPlan");
#else
    (void)schemeGuid;
    return MakeUnsupportedPlatformResult("GB_SystemPower::SetActivePowerPlan");
#endif
}

GB_SystemResult GB_SystemPower::ReadPowerSettingIndex(const std::string& schemeGuid, const std::string& subgroupGuid, const std::string& settingGuid, const bool readAcValue, uint32_t& valueIndex)
{
    valueIndex = 0;
#if defined(_WIN32)
    GUID parsedSchemeGuid = {};
    if (schemeGuid.empty())
    {
        DWORD activeErrorCode = ERROR_SUCCESS;
        if (!GetActiveSchemeGuid(parsedSchemeGuid, activeErrorCode))
        {
            return PowerErrorToResult(activeErrorCode, u8"GB_SystemPower::ReadPowerSettingIndex", u8"读取活动电源方案失败。");
        }
    }
    else if (!TryParseGuidString(schemeGuid, parsedSchemeGuid))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPower::ReadPowerSettingIndex", u8"电源方案 GUID 格式无效。");
    }

    GUID parsedSubgroupGuid = {};
    GUID parsedSettingGuid = {};
    if (subgroupGuid.empty())
    {
        parsedSubgroupGuid = GbPowerGuidNoSubgroup;
    }
    else if (!TryParseGuidString(subgroupGuid, parsedSubgroupGuid))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPower::ReadPowerSettingIndex", u8"电源设置 subgroup GUID 格式无效。");
    }
    if (!TryParseGuidString(settingGuid, parsedSettingGuid))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPower::ReadPowerSettingIndex", u8"电源设置 GUID 格式无效。");
    }

    DWORD nativeValueIndex = 0;
    const DWORD errorCode = readAcValue ? ::PowerReadACValueIndex(nullptr, &parsedSchemeGuid, &parsedSubgroupGuid, &parsedSettingGuid, &nativeValueIndex) : ::PowerReadDCValueIndex(nullptr, &parsedSchemeGuid, &parsedSubgroupGuid, &parsedSettingGuid, &nativeValueIndex);
    if (errorCode != ERROR_SUCCESS)
    {
        return PowerErrorToResult(errorCode, u8"GB_SystemPower::ReadPowerSettingIndex", readAcValue ? u8"读取 AC 电源设置索引失败。" : u8"读取 DC 电源设置索引失败。");
    }

    valueIndex = static_cast<uint32_t>(nativeValueIndex);
    return GB_SystemResult::Succeeded(u8"GB_SystemPower::ReadPowerSettingIndex");
#else
    (void)schemeGuid;
    (void)subgroupGuid;
    (void)settingGuid;
    (void)readAcValue;
    return MakeUnsupportedPlatformResult("GB_SystemPower::ReadPowerSettingIndex");
#endif
}

std::string GB_SystemPower::GetPowerSourceName(const GB_SystemPowerSource powerSource)
{
    switch (powerSource)
    {
    case GB_SystemPowerSource::AC:
        return "AC";
    case GB_SystemPowerSource::Battery:
        return "Battery";
    case GB_SystemPowerSource::Offline:
        return "Offline";
    case GB_SystemPowerSource::Unknown:
    default:
        return "Unknown";
    }
}

std::string GB_SystemPower::GetSleepStateName(const GB_SystemPowerSleepState sleepState)
{
    switch (sleepState)
    {
    case GB_SystemPowerSleepState::Working:
        return "Working";
    case GB_SystemPowerSleepState::Sleeping1:
        return "Sleeping1";
    case GB_SystemPowerSleepState::Sleeping2:
        return "Sleeping2";
    case GB_SystemPowerSleepState::Sleeping3:
        return "Sleeping3";
    case GB_SystemPowerSleepState::Hibernate:
        return "Hibernate";
    case GB_SystemPowerSleepState::Shutdown:
        return "Shutdown";
    case GB_SystemPowerSleepState::Unknown:
    default:
        return "Unknown";
    }
}

std::string GB_SystemPower::GetPowerActionTypeName(const GB_SystemPowerActionType actionType)
{
    switch (actionType)
    {
    case GB_SystemPowerActionType::Shutdown:
        return "Shutdown";
    case GB_SystemPowerActionType::PowerOff:
        return "PowerOff";
    case GB_SystemPowerActionType::Reboot:
        return "Reboot";
    case GB_SystemPowerActionType::HybridShutdown:
        return "HybridShutdown";
    default:
        return "Unknown";
    }
}

std::string GB_SystemPower::GetKeepAwakeModeName(const GB_SystemPowerKeepAwakeMode keepAwakeMode)
{
    switch (keepAwakeMode)
    {
    case GB_SystemPowerKeepAwakeMode::System:
        return "System";
    case GB_SystemPowerKeepAwakeMode::Display:
        return "Display";
    case GB_SystemPowerKeepAwakeMode::SystemAndDisplay:
        return "SystemAndDisplay";
    case GB_SystemPowerKeepAwakeMode::AwayMode:
        return "AwayMode";
    default:
        return "Unknown";
    }
}

std::string GB_SystemPower::GetPowerPlanPersonalityName(const GB_SystemPowerPlanPersonality personality)
{
    switch (personality)
    {
    case GB_SystemPowerPlanPersonality::PowerSaver:
        return "PowerSaver";
    case GB_SystemPowerPlanPersonality::Balanced:
        return "Balanced";
    case GB_SystemPowerPlanPersonality::HighPerformance:
        return "HighPerformance";
    case GB_SystemPowerPlanPersonality::Unknown:
    default:
        return "Unknown";
    }
}

std::string GB_SystemPower::GetPowerEventTypeName(const GB_SystemPowerEventType eventType)
{
    switch (eventType)
    {
    case GB_SystemPowerEventType::Suspend:
        return "Suspend";
    case GB_SystemPowerEventType::ResumeAutomatic:
        return "ResumeAutomatic";
    case GB_SystemPowerEventType::ResumeUser:
        return "ResumeUser";
    case GB_SystemPowerEventType::ResumeCritical:
        return "ResumeCritical";
    case GB_SystemPowerEventType::PowerStatusChanged:
        return "PowerStatusChanged";
    case GB_SystemPowerEventType::PowerSourceChanged:
        return "PowerSourceChanged";
    case GB_SystemPowerEventType::BatteryPercentageChanged:
        return "BatteryPercentageChanged";
    case GB_SystemPowerEventType::ActivePowerPlanChanged:
        return "ActivePowerPlanChanged";
    case GB_SystemPowerEventType::DisplayStateChanged:
        return "DisplayStateChanged";
    case GB_SystemPowerEventType::UserPresenceChanged:
        return "UserPresenceChanged";
    case GB_SystemPowerEventType::PowerSettingChanged:
        return "PowerSettingChanged";
    case GB_SystemPowerEventType::BatterySaverStatusChanged:
        return "BatterySaverStatusChanged";
    case GB_SystemPowerEventType::PowerPlanPersonalityChanged:
        return "PowerPlanPersonalityChanged";
    case GB_SystemPowerEventType::Unknown:
    default:
        return "Unknown";
    }
}

class GB_SystemPowerWatcher::Impl final
{
public:
    explicit Impl(const GB_SystemPowerWatcherOptions& inputOptions) : options(inputOptions), eventDispatcher(GB_EventDispatcher::MakeQueuedOptions(inputOptions.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropNewest, u8"GB_SystemPowerWatcher"))
    {
        callbackSetupResult = eventDispatcher.SubscribeAll([this](const GB_Event& event)
            {
                DispatchTypedCallback(event);
            }, typedSubscriptionToken);
    }

    ~Impl() noexcept
    {
        try
        {
            (void)Stop();
        }
        catch (...)
        {
        }
    }

    GB_SystemResult Start()
    {
#if !defined(_WIN32)
        return MakeUnsupportedPlatformResult("GB_SystemPowerWatcher::Start");
#else
        std::unique_lock<std::mutex> operationLock(operationMutex);
        if (callbackSetupResult.IsFailed())
        {
            return callbackSetupResult.WithOperationName(u8"GB_SystemPowerWatcher::Start");
        }
        if (options.maxPendingNativeEvents == 0 || options.maxDispatchQueueSize == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_SystemPowerWatcher::Start", u8"电源监听器队列容量必须大于 0。");
        }
        if (IsRunning())
        {
            return GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Start");
        }

        GB_SystemResult joinResult = JoinPreviousThreadsBeforeStart();
        if (joinResult.IsFailed())
        {
            return joinResult;
        }

        GB_SystemResult dispatcherStartResult = eventDispatcher.Start();
        if (dispatcherStartResult.IsFailed())
        {
            return dispatcherStartResult.WithOperationName(u8"GB_SystemPowerWatcher::Start");
        }

        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            running = false;
            stopRequested = false;
            eventWorkerStopRequested = false;
            startCompleted = false;
            startResult = GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Start");
            nativeEventQueue.clear();
        }

        try
        {
            eventWorkerThread = std::thread(&Impl::EventWorkerMainSafe, this);
            messageThread = std::thread(&Impl::MessageThreadMainSafe, this);
        }
        catch (const std::bad_alloc&)
        {
            RequestEventWorkerStop();
            JoinThreadIfNeeded(eventWorkerThread);
            (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemPowerWatcher::Start", u8"创建电源监听线程时内存不足。");
        }
        catch (...)
        {
            RequestEventWorkerStop();
            JoinThreadIfNeeded(eventWorkerThread);
            (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemPowerWatcher::Start", u8"创建电源监听线程失败。");
        }

        {
            std::unique_lock<std::mutex> stateLock(stateMutex);
            const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
            const bool ready = startCondition.wait_until(stateLock, deadline, [this]() { return startCompleted; });
            if (!ready)
            {
                stateLock.unlock();
                (void)RequestMessageThreadStop();
                RequestEventWorkerStop();
                JoinThreadIfNeeded(messageThread);
                JoinThreadIfNeeded(eventWorkerThread);
                (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
                return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, u8"GB_SystemPowerWatcher::Start", u8"等待电源监听隐藏窗口创建超时。");
            }

            if (startResult.IsFailed())
            {
                GB_SystemResult failedResult = startResult;
                stateLock.unlock();
                RequestEventWorkerStop();
                JoinThreadIfNeeded(messageThread);
                JoinThreadIfNeeded(eventWorkerThread);
                (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
                return failedResult;
            }
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Start");
#endif
    }

    GB_SystemResult Stop()
    {
#if !defined(_WIN32)
        return GB_SystemResult::Succeeded("GB_SystemPowerWatcher::Stop");
#else
        std::unique_lock<std::mutex> operationLock(operationMutex);
        if (messageThread.joinable() && std::this_thread::get_id() == messageThread.get_id())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemPowerWatcher::Stop", u8"不能在电源消息线程内停止监听器。请在外部线程或事件回调线程中停止。");
        }
        if (eventWorkerThread.joinable() && std::this_thread::get_id() == eventWorkerThread.get_id())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemPowerWatcher::Stop", u8"不能在电源事件工作线程内停止监听器。请在外部线程或事件回调线程中停止。");
        }

        bool hadThreads = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            hadThreads = messageThread.joinable() || eventWorkerThread.joinable();
            if (!running && !hadThreads)
            {
                (void)eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
                return GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Stop");
            }
            running = false;
            stopRequested = true;
        }

        GB_SystemResult stopMessageResult = RequestMessageThreadStop();
        RequestEventWorkerStop();
        JoinThreadIfNeeded(messageThread);
        JoinThreadIfNeeded(eventWorkerThread);

        GB_SystemResult dispatcherStopResult = eventDispatcher.Stop(GB_EventDispatcherStopMode::Drain);

        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            nativeEventQueue.clear();
            windowHandle = nullptr;
            messageThreadId = 0;
            running = false;
            stopRequested = false;
            eventWorkerStopRequested = false;
            startCompleted = false;
        }

        if (dispatcherStopResult.IsFailed())
        {
            return dispatcherStopResult.WithOperationName(u8"GB_SystemPowerWatcher::Stop");
        }
        if (stopMessageResult.IsFailed())
        {
            return stopMessageResult;
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Stop");
#endif
    }

    bool IsRunning() const
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        return running && !stopRequested;
    }

    void SetPowerEventCallback(const GB_SystemPowerWatcher::PowerEventCallback& callback)
    {
        std::lock_guard<std::mutex> callbackLock(callbackMutex);
        powerEventCallback = callback;
    }

    GB_EventDispatcher& GetEventDispatcher()
    {
        return eventDispatcher;
    }

    uint64_t GetDroppedNativeEventCount() const
    {
        return droppedNativeEventCount.load(std::memory_order_acquire);
    }

private:
#if defined(_WIN32)
    struct NativePowerEvent
    {
        uint32_t nativeMessage = 0;
        uint64_t nativeWParam = 0;
        uint64_t timestampMilliseconds = 0;
        bool hasPowerSetting = false;
        GUID settingGuidNative = {};
        std::vector<uint8_t> settingData;
    };
#endif

    void DispatchTypedCallback(const GB_Event& event)
    {
        const GB_SystemPowerEvent* powerEvent = event.payload.AnyCast<GB_SystemPowerEvent>();
        if (powerEvent == nullptr)
        {
            return;
        }

        GB_SystemPowerWatcher::PowerEventCallback callback;
        {
            std::lock_guard<std::mutex> callbackLock(callbackMutex);
            callback = powerEventCallback;
        }
        if (callback)
        {
            callback(*powerEvent);
        }
    }

#if defined(_WIN32)
    GB_SystemResult JoinPreviousThreadsBeforeStart()
    {
        if (messageThread.joinable())
        {
            if (std::this_thread::get_id() == messageThread.get_id())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemPowerWatcher::Start", u8"不能在电源消息线程内重新启动监听器。");
            }
            messageThread.join();
        }
        if (eventWorkerThread.joinable())
        {
            if (std::this_thread::get_id() == eventWorkerThread.get_id())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_SystemPowerWatcher::Start", u8"不能在电源事件工作线程内重新启动监听器。");
            }
            eventWorkerThread.join();
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Start");
    }

    void JoinThreadIfNeeded(std::thread& targetThread)
    {
        if (!targetThread.joinable())
        {
            return;
        }
        if (std::this_thread::get_id() == targetThread.get_id())
        {
            targetThread.detach();
            return;
        }
        targetThread.join();
    }

    GB_SystemResult RequestMessageThreadStop()
    {
        HWND localWindowHandle = nullptr;
        DWORD localThreadId = 0;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            localWindowHandle = windowHandle;
            localThreadId = messageThreadId;
        }

        DWORD postWindowErrorCode = ERROR_SUCCESS;
        if (localWindowHandle != nullptr)
        {
            if (::PostMessageW(localWindowHandle, PowerWatcherStopMessage, 0, 0) != FALSE)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Stop");
            }
            postWindowErrorCode = ::GetLastError();
        }

        if (localThreadId == 0)
        {
            if (postWindowErrorCode == ERROR_SUCCESS || postWindowErrorCode == ERROR_INVALID_WINDOW_HANDLE)
            {
                return GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Stop");
            }
            return GB_SystemResult::FromWin32Error(postWindowErrorCode, u8"GB_SystemPowerWatcher::Stop", u8"向电源监听隐藏窗口投递停止消息失败，且消息线程 ID 不可用。");
        }

        if (localThreadId == ::GetCurrentThreadId())
        {
            ::PostQuitMessage(0);
            return GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Stop");
        }

        if (::PostThreadMessageW(localThreadId, WM_QUIT, 0, 0) != FALSE)
        {
            return GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Stop");
        }

        const DWORD postThreadErrorCode = ::GetLastError();
        if (postThreadErrorCode == ERROR_INVALID_THREAD_ID)
        {
            return GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Stop", u8"电源监听消息线程已经退出。");
        }
        if (postWindowErrorCode != ERROR_SUCCESS && postWindowErrorCode != ERROR_INVALID_WINDOW_HANDLE)
        {
            return GB_SystemResult::FromWin32Error(postWindowErrorCode, u8"GB_SystemPowerWatcher::Stop", u8"向电源监听隐藏窗口投递停止消息失败；线程级停止消息也失败，线程 Win32 错误码：" + std::to_string(postThreadErrorCode) + u8"。");
        }

        return GB_SystemResult::FromWin32Error(postThreadErrorCode, u8"GB_SystemPowerWatcher::Stop", u8"向电源监听消息线程投递停止消息失败。");
    }

    void RequestEventWorkerStop()
    {
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            eventWorkerStopRequested = true;
        }
        nativeEventCondition.notify_all();
    }

    void SignalStartResult(const GB_SystemResult& result)
    {
        bool shouldNotify = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (!startCompleted)
            {
                startResult = result;
                startCompleted = true;
                shouldNotify = true;
            }
        }

        if (shouldNotify)
        {
            startCondition.notify_all();
        }
    }

    std::wstring MakeWindowClassName() const
    {
        const uintptr_t thisValue = reinterpret_cast<uintptr_t>(this);
        return L"GB_SystemPowerWatcher_" + std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(::GetTickCount64()) + L"_" + std::to_wstring(static_cast<unsigned long long>(thisValue));
    }

    void MessageThreadMainSafe()
    {
        try
        {
            MessageThreadMain();
        }
        catch (const std::bad_alloc&)
        {
            SignalStartResult(GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemPowerWatcher::Start", u8"电源监听消息线程内存不足，线程已退出。"));
            ClearMessageThreadState();
        }
        catch (const std::exception& exception)
        {
            SignalStartResult(GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemPowerWatcher::Start", std::string(u8"电源监听消息线程异常退出：") + exception.what()));
            ClearMessageThreadState();
        }
        catch (...)
        {
            SignalStartResult(GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, u8"GB_SystemPowerWatcher::Start", u8"电源监听消息线程发生未知异常并退出。"));
            ClearMessageThreadState();
        }
    }

    GB_SystemResult RegisterPowerNotifications(HWND createdWindowHandle, std::vector<HPOWERNOTIFY>& notificationHandles)
    {
        const GUID* settingGuids[] =
        {
            &GbPowerGuidAcdcPowerSource,
            &GbPowerGuidBatteryPercentageRemaining,
            &GbPowerGuidPowerSavingStatus,
            &GbPowerGuidActivePowerScheme,
            &GbPowerGuidPowerSchemePersonality,
            &GbPowerGuidConsoleDisplayState,
            &GbPowerGuidMonitorPowerOn,
            &GbPowerGuidSessionDisplayStatus,
            &GbPowerGuidGlobalUserPresence,
            &GbPowerGuidSessionUserPresence
        };

        try
        {
            notificationHandles.reserve(sizeof(settingGuids) / sizeof(settingGuids[0]));
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemPowerWatcher::Start", u8"预分配电源设置通知句柄数组时内存不足。");
        }

        DWORD firstRegisterErrorCode = ERROR_SUCCESS;
        for (size_t index = 0; index < sizeof(settingGuids) / sizeof(settingGuids[0]); index++)
        {
            HPOWERNOTIFY notifyHandle = ::RegisterPowerSettingNotification(createdWindowHandle, settingGuids[index], DEVICE_NOTIFY_WINDOW_HANDLE);
            if (notifyHandle == nullptr)
            {
                const DWORD lastError = ::GetLastError();
                if (firstRegisterErrorCode == ERROR_SUCCESS)
                {
                    firstRegisterErrorCode = lastError == ERROR_SUCCESS ? ERROR_INVALID_DATA : lastError;
                }
                continue;
            }

            try
            {
                notificationHandles.push_back(notifyHandle);
            }
            catch (...)
            {
                (void)::UnregisterPowerSettingNotification(notifyHandle);
                UnregisterPowerNotifications(notificationHandles);
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, u8"GB_SystemPowerWatcher::Start", u8"保存电源设置通知句柄时内存不足。");
            }
        }

        if (notificationHandles.empty() && firstRegisterErrorCode != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(firstRegisterErrorCode, u8"GB_SystemPowerWatcher::Start", u8"所有电源设置通知注册均失败。");
        }

        return GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Start");
    }

    static void UnregisterPowerNotifications(std::vector<HPOWERNOTIFY>& notificationHandles)
    {
        for (size_t index = 0; index < notificationHandles.size(); index++)
        {
            if (notificationHandles[index] != nullptr)
            {
                (void)::UnregisterPowerSettingNotification(notificationHandles[index]);
            }
        }
        notificationHandles.clear();
    }

    void MessageThreadMain()
    {
        MSG initialMessage = {};
        (void)::PeekMessageW(&initialMessage, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            messageThreadId = ::GetCurrentThreadId();
        }

        const std::wstring className = MakeWindowClassName();
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Impl::WindowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className.c_str();

        if (::RegisterClassExW(&windowClass) == 0)
        {
            SignalStartResult(GB_SystemResult::FromLastWin32Error(u8"GB_SystemPowerWatcher::Start", u8"注册电源监听隐藏窗口类失败。"));
            ClearMessageThreadState();
            return;
        }

        HWND createdWindowHandle = ::CreateWindowExW(WS_EX_TOOLWINDOW, className.c_str(), L"GB_SystemPowerWatcher", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, windowClass.hInstance, this);
        if (createdWindowHandle == nullptr)
        {
            const GB_SystemResult result = GB_SystemResult::FromLastWin32Error(u8"GB_SystemPowerWatcher::Start", u8"创建电源监听隐藏窗口失败。");
            (void)::UnregisterClassW(className.c_str(), windowClass.hInstance);
            SignalStartResult(result);
            ClearMessageThreadState();
            return;
        }

        std::vector<HPOWERNOTIFY> localNotificationHandles;
        const GB_SystemResult registerResult = RegisterPowerNotifications(createdWindowHandle, localNotificationHandles);
        if (registerResult.IsFailed())
        {
            (void)::DestroyWindow(createdWindowHandle);
            (void)::UnregisterClassW(className.c_str(), windowClass.hInstance);
            SignalStartResult(registerResult);
            ClearMessageThreadState();
            return;
        }

        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            windowHandle = createdWindowHandle;
            notificationHandles.swap(localNotificationHandles);
            running = true;
        }
        SignalStartResult(GB_SystemResult::Succeeded(u8"GB_SystemPowerWatcher::Start"));

        MSG message = {};
        while (::GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            (void)::TranslateMessage(&message);
            (void)::DispatchMessageW(&message);
        }

        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            UnregisterPowerNotifications(notificationHandles);
        }
        (void)::DestroyWindow(createdWindowHandle);
        (void)::UnregisterClassW(className.c_str(), windowClass.hInstance);
        ClearMessageThreadState();
    }

    void ClearMessageThreadState()
    {
        bool shouldNotifyEventWorker = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            windowHandle = nullptr;
            messageThreadId = 0;
            running = false;
            if (!stopRequested)
            {
                eventWorkerStopRequested = true;
                shouldNotifyEventWorker = true;
            }
        }

        if (shouldNotifyEventWorker)
        {
            nativeEventCondition.notify_all();
        }
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
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

        if (impl != nullptr && message == PowerWatcherStopMessage)
        {
            ::PostQuitMessage(0);
            return 0;
        }

        if (impl != nullptr && message == WM_POWERBROADCAST)
        {
            impl->QueuePowerBroadcast(message, wParam, lParam);
            return TRUE;
        }

        if (message == WM_NCDESTROY)
        {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }

        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void QueuePowerBroadcast(const UINT nativeMessage, const WPARAM wParam, const LPARAM lParam)
    {
        try
        {
            NativePowerEvent event;
            event.nativeMessage = static_cast<uint32_t>(nativeMessage);
            event.nativeWParam = static_cast<uint64_t>(wParam);
            event.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();

            if (wParam == PBT_POWERSETTINGCHANGE && lParam != 0)
            {
                const POWERBROADCAST_SETTING* powerSetting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                event.hasPowerSetting = true;
                event.settingGuidNative = powerSetting->PowerSetting;
                if (powerSetting->DataLength <= MaxPowerSettingPayloadBytes)
                {
                    if (powerSetting->DataLength > 0)
                    {
                        const uint8_t* dataBegin = reinterpret_cast<const uint8_t*>(powerSetting->Data);
                        event.settingData.assign(dataBegin, dataBegin + powerSetting->DataLength);
                    }
                }
                else
                {
                    droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
                    return;
                }
            }

            {
                std::lock_guard<std::mutex> stateLock(stateMutex);
                if (stopRequested)
                {
                    return;
                }
                if (options.maxPendingNativeEvents != 0 && nativeEventQueue.size() >= options.maxPendingNativeEvents)
                {
                    nativeEventQueue.pop_front();
                    droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
                }
                nativeEventQueue.push_back(std::move(event));
            }
        }
        catch (...)
        {
            droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
            return;
        }

        nativeEventCondition.notify_one();
    }

    void EventWorkerMainSafe()
    {
        try
        {
            EventWorkerMain();
        }
        catch (...)
        {
            droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> stateLock(stateMutex);
            eventWorkerStopRequested = true;
        }
    }

    void EventWorkerMain()
    {
        while (true)
        {
            NativePowerEvent nativeEvent;
            {
                std::unique_lock<std::mutex> stateLock(stateMutex);
                nativeEventCondition.wait(stateLock, [this]() { return eventWorkerStopRequested || !nativeEventQueue.empty(); });
                if (eventWorkerStopRequested && nativeEventQueue.empty())
                {
                    break;
                }
                nativeEvent = std::move(nativeEventQueue.front());
                nativeEventQueue.pop_front();
            }

            try
            {
                const GB_SystemPowerEvent powerEvent = BuildPowerEvent(nativeEvent);
                if (powerEvent.eventType == GB_SystemPowerEventType::Unknown)
                {
                    continue;
                }

                GB_Event typedEvent(powerEvent.eventName, GB_Variant(powerEvent), powerEvent.sourceName);
                typedEvent.timestampMilliseconds = powerEvent.timestampMilliseconds;
                AddCommonAttributes(powerEvent, typedEvent);
                const GB_SystemResult postResult = eventDispatcher.Post(typedEvent);
                if (postResult.IsFailed())
                {
                    droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            catch (...)
            {
                droppedNativeEventCount.fetch_add(1, std::memory_order_acq_rel);
            }
        }
    }

    void AddCommonAttributes(const GB_SystemPowerEvent& powerEvent, GB_Event& event)
    {
        event.SetAttribute("eventType", GB_Variant(static_cast<unsigned int>(powerEvent.eventType)));
        event.SetAttribute("eventTypeName", GB_Variant(GB_SystemPower::GetPowerEventTypeName(powerEvent.eventType)));
        event.SetAttribute("sourceName", GB_Variant(powerEvent.sourceName));
        event.SetAttribute("nativeMessage", GB_Variant(powerEvent.nativeMessage));
        event.SetAttribute("nativeWParam", GB_Variant(static_cast<unsigned long long>(powerEvent.nativeWParam)));
        event.SetAttribute("settingGuid", GB_Variant(powerEvent.settingGuid));
        if (powerEvent.hasSettingDataUInt32)
        {
            event.SetAttribute("settingDataUInt32", GB_Variant(static_cast<unsigned int>(powerEvent.settingDataUInt32)));
        }
        if (powerEvent.hasSettingDataGuid)
        {
            event.SetAttribute("settingDataGuid", GB_Variant(powerEvent.settingDataGuid));
        }
    }

    GB_SystemPowerEvent BuildPowerEvent(NativePowerEvent& nativeEvent)
    {
        GB_SystemPowerEvent powerEvent;
        powerEvent.eventType = MapPowerBroadcastToEventType(static_cast<WPARAM>(nativeEvent.nativeWParam), nativeEvent.hasPowerSetting, nativeEvent.settingGuidNative);
        if (powerEvent.eventType == GB_SystemPowerEventType::Unknown)
        {
            return powerEvent;
        }

        powerEvent.eventName = "SystemPower." + GB_SystemPower::GetPowerEventTypeName(powerEvent.eventType);
        powerEvent.sourceName = nativeEvent.hasPowerSetting ? "RegisterPowerSettingNotification" : "WM_POWERBROADCAST";
        powerEvent.timestampMilliseconds = nativeEvent.timestampMilliseconds;
        powerEvent.nativeMessage = nativeEvent.nativeMessage;
        powerEvent.nativeWParam = nativeEvent.nativeWParam;
        if (nativeEvent.hasPowerSetting)
        {
            powerEvent.settingGuid = GuidToString(nativeEvent.settingGuidNative);
        }
        powerEvent.settingData = std::move(nativeEvent.settingData);

        if (powerEvent.settingData.size() == sizeof(uint32_t))
        {
            uint32_t dataValue = 0;
            std::memcpy(&dataValue, powerEvent.settingData.data(), sizeof(dataValue));
            powerEvent.hasSettingDataUInt32 = true;
            powerEvent.settingDataUInt32 = dataValue;
        }
        else if (powerEvent.settingData.size() == sizeof(GUID))
        {
            GUID dataGuid = {};
            std::memcpy(&dataGuid, powerEvent.settingData.data(), sizeof(dataGuid));
            powerEvent.hasSettingDataGuid = true;
            powerEvent.settingDataGuid = GuidToString(dataGuid);
        }

        if (options.capturePowerStatusSnapshot)
        {
            GB_SystemPowerStatus powerStatus;
            const GB_SystemResult statusResult = GB_SystemPower::GetPowerStatus(powerStatus);
            if (statusResult.IsSucceeded())
            {
                powerEvent.powerStatus = powerStatus;
                powerEvent.hasPowerStatus = true;
            }
        }

        return powerEvent;
    }
#endif

private:
    GB_SystemPowerWatcherOptions options;
    GB_EventDispatcher eventDispatcher;
    GB_EventSubscriptionToken typedSubscriptionToken;
    GB_SystemResult callbackSetupResult;
    GB_SystemPowerWatcher::PowerEventCallback powerEventCallback;
    mutable std::mutex callbackMutex;
    mutable std::mutex operationMutex;
    mutable std::mutex stateMutex;
#if defined(_WIN32)
    std::condition_variable startCondition;
    std::condition_variable nativeEventCondition;
    std::deque<NativePowerEvent> nativeEventQueue;
    std::thread messageThread;
    std::thread eventWorkerThread;
#endif
    std::atomic<uint64_t> droppedNativeEventCount{ 0 };
    bool running = false;
    bool stopRequested = false;
#if defined(_WIN32)
    bool eventWorkerStopRequested = false;
    bool startCompleted = false;
    GB_SystemResult startResult;
    HWND windowHandle = nullptr;
    DWORD messageThreadId = 0;
    std::vector<HPOWERNOTIFY> notificationHandles;
#endif
};

GB_SystemPowerWatcher::GB_SystemPowerWatcher() : impl(new Impl(GB_SystemPowerWatcherOptions()))
{
}

GB_SystemPowerWatcher::GB_SystemPowerWatcher(const GB_SystemPowerWatcherOptions& options) : impl(new Impl(options))
{
}

GB_SystemPowerWatcher::~GB_SystemPowerWatcher() noexcept = default;

GB_SystemResult GB_SystemPowerWatcher::Start()
{
    return impl->Start();
}

GB_SystemResult GB_SystemPowerWatcher::Stop()
{
    return impl->Stop();
}

bool GB_SystemPowerWatcher::IsRunning() const
{
    return impl->IsRunning();
}

void GB_SystemPowerWatcher::SetPowerEventCallback(const PowerEventCallback& callback)
{
    impl->SetPowerEventCallback(callback);
}

GB_EventDispatcher& GB_SystemPowerWatcher::GetEventDispatcher()
{
    return impl->GetEventDispatcher();
}

uint64_t GB_SystemPowerWatcher::GetDroppedNativeEventCount() const
{
    return impl->GetDroppedNativeEventCount();
}
