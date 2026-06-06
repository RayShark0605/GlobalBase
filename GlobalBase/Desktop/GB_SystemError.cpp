#include "GB_SystemError.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace
{
    constexpr uint32_t GB_HResultSeverityFailure = 0x80000000u;
    constexpr uint32_t GB_HResultFacilityNtBit = 0x10000000u;
    constexpr uint32_t GB_HResultFacilityMask = 0x1FFFu;
    constexpr uint32_t GB_HResultFacilityWin32 = 7u;
    constexpr uint32_t GB_HResultCodeMask = 0x0000FFFFu;

    static std::string FormatNativeCode(const uint64_t errorCode)
    {
        if (errorCode <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
        {
            return GB_SystemError::FormatHex32(static_cast<uint32_t>(errorCode));
        }

        return GB_SystemError::FormatHex64(errorCode);
    }

    static std::string FormatNativeCodeWithDecimal(const uint64_t errorCode)
    {
        std::string result = FormatNativeCode(errorCode);
        result += " (";
        result += std::to_string(errorCode);
        result += ")";
        return result;
    }

    static bool CanRepresentUint32(const uint64_t value)
    {
        return value <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
    }

    static bool CanRepresentNonNegativeInt(const uint64_t value)
    {
        return value <= static_cast<uint64_t>(std::numeric_limits<int>::max());
    }

    static GB_NativeErrorSource NormalizeNativeErrorSource(const GB_NativeErrorSource errorSource)
    {
        const uint64_t errorSourceValue = static_cast<uint64_t>(errorSource);
        if (!GB_SystemError::IsValidNativeErrorSourceValue(errorSourceValue))
        {
            return GB_NativeErrorSource::None;
        }

        return errorSource;
    }

    static std::string BuildDecimalErrorMessage(const std::string& errorKindUtf8, const int errorCode)
    {
        std::string result = errorKindUtf8.empty() ? std::string(u8"Native error") : errorKindUtf8;
        result += " ";
        result += std::to_string(errorCode);
        return result;
    }

    static std::string BuildFallbackNativeErrorMessage(const std::string& errorKindUtf8, const uint64_t errorCode)
    {
        std::string result = errorKindUtf8.empty() ? std::string(u8"Native error") : errorKindUtf8;
        result += " ";
        result += FormatNativeCodeWithDecimal(errorCode);
        return result;
    }

    static bool IsAsciiTrailingWhitespace(const char value)
    {
        const unsigned char charValue = static_cast<unsigned char>(value);
        return charValue == '\r' || charValue == '\n' || charValue == '\t' || charValue == ' ' || charValue == '\0';
    }

    static std::string TrimTrailingAsciiWhitespace(std::string text)
    {
        size_t newSize = text.size();
        while (newSize > 0 && IsAsciiTrailingWhitespace(text[newSize - 1]))
        {
            newSize--;
        }

        if (newSize != text.size())
        {
            text.resize(newSize);
        }

        return text;
    }

    static bool TryGetGlobalBaseErrorCode(const uint64_t errorCodeValue, GB_SystemErrorCode& errorCode)
    {
        if (errorCodeValue > static_cast<uint64_t>(std::numeric_limits<uint16_t>::max()))
        {
            return false;
        }

        if (!GB_SystemError::IsValidErrorCodeValue(errorCodeValue))
        {
            return false;
        }

        errorCode = static_cast<GB_SystemErrorCode>(static_cast<uint16_t>(errorCodeValue));
        return true;
    }

    static std::string BuildHResultWin32Message(const int32_t hresult, const uint32_t win32ErrorCode)
    {
        std::string result = u8"HRESULT ";
        result += GB_SystemError::FormatHex32(static_cast<uint32_t>(hresult));
        result += u8" / Win32 ";
        result += GB_SystemError::FormatHex32(win32ErrorCode);
        result += ": ";
        result += GB_SystemError::GetWin32ErrorMessage(win32ErrorCode);
        return result;
    }

    static std::string BuildHResultNtStatusMessage(const int32_t hresult, const uint32_t ntStatus)
    {
        std::string result = u8"HRESULT ";
        result += GB_SystemError::FormatHex32(static_cast<uint32_t>(hresult));
        result += u8" / NTSTATUS ";
        result += GB_SystemError::FormatHex32(ntStatus);
        result += ": ";
        result += GB_SystemError::GetNtStatusMessage(ntStatus);
        return result;
    }

    static std::string BuildKnownHResultMessage(const int32_t hresult)
    {
        const uint32_t unsignedHResult = static_cast<uint32_t>(hresult);
        const char* hresultName = nullptr;
        const char* hresultDescription = nullptr;

        switch (unsignedHResult)
        {
        case 0x00000000u:
            hresultName = "S_OK";
            hresultDescription = u8"操作成功。";
            break;

        case 0x00000001u:
            hresultName = "S_FALSE";
            hresultDescription = u8"操作成功，但返回值表示 false 或无可用结果。";
            break;

        case 0x80004001u:
            hresultName = "E_NOTIMPL";
            hresultDescription = u8"未实现。";
            break;

        case 0x80004002u:
            hresultName = "E_NOINTERFACE";
            hresultDescription = u8"不支持该接口。";
            break;

        case 0x80004003u:
            hresultName = "E_POINTER";
            hresultDescription = u8"指针无效。";
            break;

        case 0x80004004u:
            hresultName = "E_ABORT";
            hresultDescription = u8"操作已中止。";
            break;

        case 0x80004005u:
            hresultName = "E_FAIL";
            hresultDescription = u8"未指定的失败。";
            break;

        case 0x8000FFFFu:
            hresultName = "E_UNEXPECTED";
            hresultDescription = u8"发生意外错误。";
            break;

        case 0x80070005u:
            hresultName = "E_ACCESSDENIED";
            hresultDescription = u8"访问被拒绝。";
            break;

        case 0x8007000Eu:
            hresultName = "E_OUTOFMEMORY";
            hresultDescription = u8"内存不足。";
            break;

        case 0x80070057u:
            hresultName = "E_INVALIDARG";
            hresultDescription = u8"参数无效。";
            break;

        case 0x80040154u:
            hresultName = "REGDB_E_CLASSNOTREG";
            hresultDescription = u8"COM 类未注册。";
            break;

        case 0x800401F0u:
            hresultName = "CO_E_NOTINITIALIZED";
            hresultDescription = u8"COM 尚未初始化。";
            break;

        case 0x800401F1u:
            hresultName = "CO_E_ALREADYINITIALIZED";
            hresultDescription = u8"COM 已经初始化。";
            break;

        case 0x800401F2u:
            hresultName = "CO_E_CANTDETERMINECLASS";
            hresultDescription = u8"无法确定 COM 类。";
            break;

        case 0x800401F3u:
            hresultName = "CO_E_CLASSSTRING";
            hresultDescription = u8"COM 类字符串无效。";
            break;

        case 0x80010106u:
            hresultName = "RPC_E_CHANGED_MODE";
            hresultDescription = u8"无法更改当前线程的 COM 并发模型。";
            break;

        case 0x8001010Eu:
            hresultName = "RPC_E_WRONG_THREAD";
            hresultDescription = u8"接口被错误地跨线程调用。";
            break;

        case 0x80010119u:
            hresultName = "RPC_E_TOO_LATE";
            hresultDescription = u8"COM 安全已经初始化，无法再次修改进程级 COM 安全设置。";
            break;

        case 0x8001011Au:
            hresultName = "RPC_E_NO_GOOD_SECURITY_PACKAGES";
            hresultDescription = u8"没有可用的 COM 安全包。";
            break;

        default:
            break;
        }

        if (hresultName == nullptr || hresultDescription == nullptr)
        {
            return std::string();
        }

        std::string result = hresultName;
        result += " ";
        result += GB_SystemError::FormatHex32(unsignedHResult);
        result += ": ";
        result += hresultDescription;
        return result;
    }

    static GB_SystemErrorCode GuessFailureErrorCodeFromHResult(const int32_t hresult, const GB_SystemErrorCode fallbackErrorCode)
    {
        if (GB_SystemError::IsHResultSucceeded(hresult))
        {
            return GB_SystemErrorCode::Succeeded;
        }

        bool converted = false;
        const uint32_t win32ErrorCode = GB_SystemError::HResultToWin32ErrorCode(hresult, converted);
        if (converted)
        {
            return GB_SystemError::GuessErrorCodeFromWin32ErrorCode(win32ErrorCode);
        }

        const uint32_t unsignedHResult = static_cast<uint32_t>(hresult);
        if ((unsignedHResult & GB_HResultFacilityNtBit) != 0)
        {
            const uint32_t ntStatus = unsignedHResult & ~GB_HResultFacilityNtBit;
            if (ntStatus != 0)
            {
                return GB_SystemError::GuessErrorCodeFromNtStatus(ntStatus);
            }
        }

        switch (unsignedHResult)
        {
        case 0x80004001u: // E_NOTIMPL
        case 0x80004002u: // E_NOINTERFACE
            return GB_SystemErrorCode::UnsupportedPlatform;

        case 0x80004003u: // E_POINTER
            return GB_SystemErrorCode::InvalidArgument;

        case 0x80004004u: // E_ABORT
            return GB_SystemErrorCode::Cancelled;

        case 0x80004005u: // E_FAIL
            return GB_SystemErrorCode::OperationFailed;

        case 0x8000FFFFu: // E_UNEXPECTED
            return GB_SystemErrorCode::InternalError;

        case 0x80070005u: // E_ACCESSDENIED
            return GB_SystemErrorCode::PermissionDenied;

        case 0x8007000Eu: // E_OUTOFMEMORY
            return GB_SystemErrorCode::ResourceAllocationFailed;

        case 0x80070057u: // E_INVALIDARG
            return GB_SystemErrorCode::InvalidArgument;

        case 0x80040154u: // REGDB_E_CLASSNOTREG
            return GB_SystemErrorCode::NotFound;

        case 0x800401F0u: // CO_E_NOTINITIALIZED
            return GB_SystemErrorCode::NotInitialized;

        case 0x800401F1u: // CO_E_ALREADYINITIALIZED
            return GB_SystemErrorCode::AlreadyInitialized;

        case 0x800401F2u: // CO_E_CANTDETERMINECLASS
        case 0x800401F3u: // CO_E_CLASSSTRING
            return GB_SystemErrorCode::InvalidArgument;

        case 0x80010106u: // RPC_E_CHANGED_MODE
        case 0x8001010Eu: // RPC_E_WRONG_THREAD
            return GB_SystemErrorCode::InvalidState;

        case 0x80010119u: // RPC_E_TOO_LATE
            return GB_SystemErrorCode::AlreadyInitialized;

        case 0x8001011Au: // RPC_E_NO_GOOD_SECURITY_PACKAGES
            return GB_SystemErrorCode::PermissionDenied;

        default:
            break;
        }

        return fallbackErrorCode;
    }

#if defined(_WIN32)
    class Win32LastErrorScope
    {
    public:
        Win32LastErrorScope() : lastErrorCode(::GetLastError())
        {
        }

        ~Win32LastErrorScope()
        {
            ::SetLastError(lastErrorCode);
        }

        Win32LastErrorScope(const Win32LastErrorScope&) = delete;
        Win32LastErrorScope& operator=(const Win32LastErrorScope&) = delete;

    private:
        DWORD lastErrorCode = 0;
    };

    static std::string WideStringToUtf8(const std::wstring& text)
    {
        const Win32LastErrorScope lastErrorScope;

        if (text.empty())
        {
            return std::string();
        }

        if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return std::string();
        }

        const int textLength = static_cast<int>(text.size());
#if defined(WC_ERR_INVALID_CHARS)
        DWORD convertFlags = WC_ERR_INVALID_CHARS;
#else
        DWORD convertFlags = 0;
#endif
        int requiredLength = ::WideCharToMultiByte(CP_UTF8, convertFlags, text.data(), textLength, nullptr, 0, nullptr, nullptr);
        if (requiredLength <= 0 && convertFlags != 0)
        {
            convertFlags = 0;
            requiredLength = ::WideCharToMultiByte(CP_UTF8, convertFlags, text.data(), textLength, nullptr, 0, nullptr, nullptr);
        }
        if (requiredLength <= 0)
        {
            return std::string();
        }

        std::string result(static_cast<size_t>(requiredLength), '\0');
        const int convertedLength = ::WideCharToMultiByte(CP_UTF8, convertFlags, text.data(), textLength, &result[0], requiredLength, nullptr, nullptr);
        if (convertedLength != requiredLength)
        {
            return std::string();
        }

        return result;
    }

    static std::string MultiByteStringToUtf8(const std::string& text, const UINT codePage)
    {
        const Win32LastErrorScope lastErrorScope;

        if (text.empty())
        {
            return std::string();
        }

        if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return std::string();
        }

        const int textLength = static_cast<int>(text.size());
        DWORD convertFlags = MB_ERR_INVALID_CHARS;
        int requiredWideLength = ::MultiByteToWideChar(codePage, convertFlags, text.data(), textLength, nullptr, 0);
        if (requiredWideLength <= 0)
        {
            convertFlags = 0;
            requiredWideLength = ::MultiByteToWideChar(codePage, convertFlags, text.data(), textLength, nullptr, 0);
        }
        if (requiredWideLength <= 0)
        {
            return text;
        }

        std::wstring wideText(static_cast<size_t>(requiredWideLength), L'\0');
        const int convertedWideLength = ::MultiByteToWideChar(codePage, convertFlags, text.data(), textLength, &wideText[0], requiredWideLength);
        if (convertedWideLength != requiredWideLength)
        {
            return text;
        }

        const std::string utf8Text = WideStringToUtf8(wideText);
        return utf8Text.empty() ? text : utf8Text;
    }

    class LocalMemoryScope
    {
    public:
        explicit LocalMemoryScope(void* memoryHandle) : memoryHandle(memoryHandle)
        {
        }

        ~LocalMemoryScope()
        {
            if (memoryHandle != nullptr)
            {
                (void)::LocalFree(static_cast<HLOCAL>(memoryHandle));
                memoryHandle = nullptr;
            }
        }

        LocalMemoryScope(const LocalMemoryScope&) = delete;
        LocalMemoryScope& operator=(const LocalMemoryScope&) = delete;

    private:
        void* memoryHandle = nullptr;
    };

    static bool TryFormatWindowsMessageByCode(const uint32_t messageCode, const DWORD sourceFlags, const void* messageSource, std::string& messageUtf8)
    {
        const Win32LastErrorScope lastErrorScope;

        messageUtf8.clear();

        LPWSTR messageBuffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | sourceFlags | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK;
        const DWORD formattedLength = ::FormatMessageW(flags, messageSource, static_cast<DWORD>(messageCode), 0, reinterpret_cast<LPWSTR>(&messageBuffer), 0, nullptr);
        LocalMemoryScope messageBufferScope(messageBuffer);

        if (formattedLength == 0 || messageBuffer == nullptr)
        {
            return false;
        }

        const std::wstring wideMessage(messageBuffer, messageBuffer + formattedLength);
        messageUtf8 = TrimTrailingAsciiWhitespace(WideStringToUtf8(wideMessage));
        return !messageUtf8.empty();
    }

    static std::string FormatWindowsMessageByCode(const uint32_t messageCode, const DWORD sourceFlags, const void* messageSource, const std::string& fallbackKindUtf8)
    {
        std::string messageUtf8;
        if (TryFormatWindowsMessageByCode(messageCode, sourceFlags, messageSource, messageUtf8))
        {
            return messageUtf8;
        }

        return BuildFallbackNativeErrorMessage(fallbackKindUtf8, messageCode);
    }

    static bool TryFormatSystemMessageByCode(const uint32_t messageCode, std::string& messageUtf8)
    {
        return TryFormatWindowsMessageByCode(messageCode, FORMAT_MESSAGE_FROM_SYSTEM, nullptr, messageUtf8);
    }

    static std::string FormatSystemMessageByCode(const uint32_t messageCode, const std::string& fallbackKindUtf8)
    {
        return FormatWindowsMessageByCode(messageCode, FORMAT_MESSAGE_FROM_SYSTEM, nullptr, fallbackKindUtf8);
    }

    static bool TryFormatNtStatusMessageByCode(const uint32_t ntStatus, std::string& messageUtf8)
    {
        const Win32LastErrorScope lastErrorScope;

        HMODULE ntdllModule = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdllModule == nullptr)
        {
            return false;
        }

        return TryFormatWindowsMessageByCode(ntStatus, FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM, ntdllModule, messageUtf8);
    }

    static std::string FormatNtStatusMessageByCode(const uint32_t ntStatus)
    {
        std::string messageUtf8;
        if (TryFormatNtStatusMessageByCode(ntStatus, messageUtf8))
        {
            return messageUtf8;
        }

        return BuildFallbackNativeErrorMessage(u8"NTSTATUS", ntStatus);
    }
#endif

#if !defined(_MSC_VER) && (defined(__unix__) || defined(__APPLE__))
    static std::string GetPosixStrErrorMessage(const int errorNumber)
    {
        char buffer[512] = {};

#if defined(__GLIBC__) && defined(_GNU_SOURCE)
        const char* result = ::strerror_r(errorNumber, buffer, sizeof(buffer));
        if (result == nullptr || result[0] == '\0')
        {
            return std::string();
        }

        return TrimTrailingAsciiWhitespace(std::string(result));
#else
        if (::strerror_r(errorNumber, buffer, sizeof(buffer)) != 0 || buffer[0] == '\0')
        {
            return std::string();
        }

        return TrimTrailingAsciiWhitespace(std::string(buffer));
#endif
    }
#endif
}

bool GB_SystemError::IsSucceeded(const GB_SystemErrorCode errorCode)
{
    return errorCode == GB_SystemErrorCode::Succeeded;
}

bool GB_SystemError::IsFailed(const GB_SystemErrorCode errorCode)
{
    return !IsSucceeded(errorCode);
}

bool GB_SystemError::IsValidErrorCodeValue(const uint64_t errorCodeValue)
{
    switch (errorCodeValue)
    {
    case static_cast<uint64_t>(GB_SystemErrorCode::Succeeded):
    case static_cast<uint64_t>(GB_SystemErrorCode::UnsupportedPlatform):
    case static_cast<uint64_t>(GB_SystemErrorCode::InvalidArgument):
    case static_cast<uint64_t>(GB_SystemErrorCode::InvalidState):
    case static_cast<uint64_t>(GB_SystemErrorCode::NotInitialized):
    case static_cast<uint64_t>(GB_SystemErrorCode::AlreadyInitialized):
    case static_cast<uint64_t>(GB_SystemErrorCode::PermissionDenied):
    case static_cast<uint64_t>(GB_SystemErrorCode::NotFound):
    case static_cast<uint64_t>(GB_SystemErrorCode::AlreadyExists):
    case static_cast<uint64_t>(GB_SystemErrorCode::Timeout):
    case static_cast<uint64_t>(GB_SystemErrorCode::Cancelled):
    case static_cast<uint64_t>(GB_SystemErrorCode::OperationFailed):
    case static_cast<uint64_t>(GB_SystemErrorCode::NativeApiFailed):
    case static_cast<uint64_t>(GB_SystemErrorCode::ComApiFailed):
    case static_cast<uint64_t>(GB_SystemErrorCode::WinRtApiFailed):
    case static_cast<uint64_t>(GB_SystemErrorCode::ResourceAllocationFailed):
    case static_cast<uint64_t>(GB_SystemErrorCode::EncodingConversionFailed):
    case static_cast<uint64_t>(GB_SystemErrorCode::ParseFailed):
    case static_cast<uint64_t>(GB_SystemErrorCode::InternalError):
    case static_cast<uint64_t>(GB_SystemErrorCode::UnknownError):
    case static_cast<uint64_t>(GB_SystemErrorCode::ResourceBusy):
        return true;

    default:
        break;
    }

    return false;
}

bool GB_SystemError::IsValidNativeErrorSourceValue(const uint64_t errorSourceValue)
{
    switch (errorSourceValue)
    {
    case static_cast<uint64_t>(GB_NativeErrorSource::None):
    case static_cast<uint64_t>(GB_NativeErrorSource::GlobalBase):
    case static_cast<uint64_t>(GB_NativeErrorSource::Win32):
    case static_cast<uint64_t>(GB_NativeErrorSource::HResult):
    case static_cast<uint64_t>(GB_NativeErrorSource::NtStatus):
    case static_cast<uint64_t>(GB_NativeErrorSource::Com):
    case static_cast<uint64_t>(GB_NativeErrorSource::WinRt):
    case static_cast<uint64_t>(GB_NativeErrorSource::CRuntime):
        return true;

    default:
        break;
    }

    return false;
}

std::string GB_SystemError::GetErrorCodeName(const GB_SystemErrorCode errorCode)
{
    switch (errorCode)
    {
    case GB_SystemErrorCode::Succeeded:
        return u8"Succeeded";
    case GB_SystemErrorCode::UnsupportedPlatform:
        return u8"UnsupportedPlatform";
    case GB_SystemErrorCode::InvalidArgument:
        return u8"InvalidArgument";
    case GB_SystemErrorCode::InvalidState:
        return u8"InvalidState";
    case GB_SystemErrorCode::NotInitialized:
        return u8"NotInitialized";
    case GB_SystemErrorCode::AlreadyInitialized:
        return u8"AlreadyInitialized";
    case GB_SystemErrorCode::PermissionDenied:
        return u8"PermissionDenied";
    case GB_SystemErrorCode::NotFound:
        return u8"NotFound";
    case GB_SystemErrorCode::AlreadyExists:
        return u8"AlreadyExists";
    case GB_SystemErrorCode::Timeout:
        return u8"Timeout";
    case GB_SystemErrorCode::Cancelled:
        return u8"Cancelled";
    case GB_SystemErrorCode::OperationFailed:
        return u8"OperationFailed";
    case GB_SystemErrorCode::NativeApiFailed:
        return u8"NativeApiFailed";
    case GB_SystemErrorCode::ComApiFailed:
        return u8"ComApiFailed";
    case GB_SystemErrorCode::WinRtApiFailed:
        return u8"WinRtApiFailed";
    case GB_SystemErrorCode::ResourceAllocationFailed:
        return u8"ResourceAllocationFailed";
    case GB_SystemErrorCode::EncodingConversionFailed:
        return u8"EncodingConversionFailed";
    case GB_SystemErrorCode::ParseFailed:
        return u8"ParseFailed";
    case GB_SystemErrorCode::InternalError:
        return u8"InternalError";
    case GB_SystemErrorCode::UnknownError:
        return u8"UnknownError";
    case GB_SystemErrorCode::ResourceBusy:
        return u8"ResourceBusy";
    }

    return u8"UnknownError";
}

std::string GB_SystemError::GetErrorCodeDescription(const GB_SystemErrorCode errorCode)
{
    switch (errorCode)
    {
    case GB_SystemErrorCode::Succeeded:
        return u8"操作成功。";
    case GB_SystemErrorCode::UnsupportedPlatform:
        return u8"当前平台、当前系统版本或当前环境不支持该能力。";
    case GB_SystemErrorCode::InvalidArgument:
        return u8"参数非法。";
    case GB_SystemErrorCode::InvalidState:
        return u8"当前对象或系统状态不满足操作前置条件。";
    case GB_SystemErrorCode::NotInitialized:
        return u8"对象或子系统尚未初始化。";
    case GB_SystemErrorCode::AlreadyInitialized:
        return u8"对象或子系统已经初始化。";
    case GB_SystemErrorCode::PermissionDenied:
        return u8"权限不足或访问被拒绝。";
    case GB_SystemErrorCode::NotFound:
        return u8"目标对象不存在。";
    case GB_SystemErrorCode::AlreadyExists:
        return u8"目标对象已经存在。";
    case GB_SystemErrorCode::Timeout:
        return u8"操作超时。";
    case GB_SystemErrorCode::Cancelled:
        return u8"操作被取消。";
    case GB_SystemErrorCode::OperationFailed:
        return u8"操作失败。";
    case GB_SystemErrorCode::NativeApiFailed:
        return u8"原生 API 调用失败。";
    case GB_SystemErrorCode::ComApiFailed:
        return u8"COM API 调用失败。";
    case GB_SystemErrorCode::WinRtApiFailed:
        return u8"Windows Runtime API 调用失败。";
    case GB_SystemErrorCode::ResourceAllocationFailed:
        return u8"资源分配失败。";
    case GB_SystemErrorCode::EncodingConversionFailed:
        return u8"字符编码转换失败。";
    case GB_SystemErrorCode::ParseFailed:
        return u8"解析失败。";
    case GB_SystemErrorCode::InternalError:
        return u8"内部逻辑错误。";
    case GB_SystemErrorCode::UnknownError:
        return u8"未知错误。";
    case GB_SystemErrorCode::ResourceBusy:
        return u8"目标资源当前正被其他操作占用。";
    }

    return u8"未知错误。";
}

std::string GB_SystemError::GetNativeErrorSourceName(const GB_NativeErrorSource errorSource)
{
    switch (errorSource)
    {
    case GB_NativeErrorSource::None:
        return u8"None";
    case GB_NativeErrorSource::GlobalBase:
        return u8"GlobalBase";
    case GB_NativeErrorSource::Win32:
        return u8"Win32";
    case GB_NativeErrorSource::HResult:
        return u8"HRESULT";
    case GB_NativeErrorSource::NtStatus:
        return u8"NTSTATUS";
    case GB_NativeErrorSource::Com:
        return u8"COM";
    case GB_NativeErrorSource::WinRt:
        return u8"WinRT";
    case GB_NativeErrorSource::CRuntime:
        return u8"C Runtime";
    }

    return u8"Unknown";
}

std::string GB_SystemError::GetNativeErrorSourceDescription(const GB_NativeErrorSource errorSource)
{
    switch (errorSource)
    {
    case GB_NativeErrorSource::None:
        return u8"无原生错误。";
    case GB_NativeErrorSource::GlobalBase:
        return u8"GlobalBase 自身定义的错误。";
    case GB_NativeErrorSource::Win32:
        return u8"Win32 GetLastError / 系统错误码。";
    case GB_NativeErrorSource::HResult:
        return u8"HRESULT 错误码。";
    case GB_NativeErrorSource::NtStatus:
        return u8"NTSTATUS 错误码。";
    case GB_NativeErrorSource::Com:
        return u8"COM 错误。";
    case GB_NativeErrorSource::WinRt:
        return u8"Windows Runtime 错误。";
    case GB_NativeErrorSource::CRuntime:
        return u8"C 运行时错误。";
    }

    return u8"未知原生错误来源。";
}

uint32_t GB_SystemError::GetLastWin32ErrorCode()
{
#if defined(_WIN32)
    return static_cast<uint32_t>(::GetLastError());
#else
    return 0;
#endif
}

std::string GB_SystemError::GetLastWin32ErrorMessage()
{
    const uint32_t win32ErrorCode = GetLastWin32ErrorCode();
    return GetWin32ErrorMessage(win32ErrorCode);
}

std::string GB_SystemError::GetWin32ErrorMessage(const uint32_t win32ErrorCode)
{
#if defined(_WIN32)
    return FormatSystemMessageByCode(win32ErrorCode, u8"Win32 error");
#else
    return BuildFallbackNativeErrorMessage(u8"Win32 error", win32ErrorCode);
#endif
}

std::string GB_SystemError::GetHResultMessage(const int32_t hresult)
{
    const std::string knownHResultMessage = BuildKnownHResultMessage(hresult);
    if (!knownHResultMessage.empty())
    {
        return knownHResultMessage;
    }

    if (hresult > 0)
    {
        return BuildFallbackNativeErrorMessage(u8"HRESULT success", static_cast<uint32_t>(hresult));
    }

    bool converted = false;
    const uint32_t win32ErrorCode = HResultToWin32ErrorCode(hresult, converted);
    if (converted)
    {
        return BuildHResultWin32Message(hresult, win32ErrorCode);
    }

    const uint32_t unsignedHResult = static_cast<uint32_t>(hresult);
#if defined(_WIN32)
    std::string systemMessageUtf8;
    if (TryFormatSystemMessageByCode(unsignedHResult, systemMessageUtf8))
    {
        std::string result = u8"HRESULT ";
        result += FormatHex32(unsignedHResult);
        result += ": ";
        result += systemMessageUtf8;
        return result;
    }
#endif

    if ((unsignedHResult & GB_HResultFacilityNtBit) != 0)
    {
        const uint32_t ntStatus = unsignedHResult & ~GB_HResultFacilityNtBit;
        if (ntStatus != 0)
        {
#if defined(_WIN32)
            std::string ntStatusMessageUtf8;
            if (TryFormatNtStatusMessageByCode(ntStatus, ntStatusMessageUtf8))
            {
                std::string result = u8"HRESULT ";
                result += FormatHex32(unsignedHResult);
                result += u8" / NTSTATUS ";
                result += FormatHex32(ntStatus);
                result += ": ";
                result += ntStatusMessageUtf8;
                return result;
            }
#endif
            return BuildHResultNtStatusMessage(hresult, ntStatus);
        }
    }

    return BuildFallbackNativeErrorMessage(u8"HRESULT", unsignedHResult);
}

std::string GB_SystemError::GetNtStatusMessage(const uint32_t ntStatus)
{
#if defined(_WIN32)
    return FormatNtStatusMessageByCode(ntStatus);
#else
    return BuildFallbackNativeErrorMessage(u8"NTSTATUS", ntStatus);
#endif
}

std::string GB_SystemError::GetCRuntimeErrorMessage(const int errorNumber)
{
    if (errorNumber == 0)
    {
        return u8"No error.";
    }

    if (errorNumber < 0)
    {
        return BuildDecimalErrorMessage(u8"C runtime error", errorNumber);
    }

#if defined(_MSC_VER)
#if defined(_WIN32)
    wchar_t wideBuffer[512] = {};
    if (::_wcserror_s(wideBuffer, sizeof(wideBuffer) / sizeof(wideBuffer[0]), errorNumber) == 0 && wideBuffer[0] != L'\0')
    {
        std::string message = TrimTrailingAsciiWhitespace(WideStringToUtf8(wideBuffer));
        if (!message.empty())
        {
            return message;
        }
    }
#endif

    char buffer[512] = {};
    if (::strerror_s(buffer, sizeof(buffer), errorNumber) == 0 && buffer[0] != '\0')
    {
        std::string message = TrimTrailingAsciiWhitespace(buffer);
#if defined(_WIN32)
        message = MultiByteStringToUtf8(message, CP_ACP);
#endif
        if (!message.empty())
        {
            return message;
        }
    }
#elif defined(__unix__) || defined(__APPLE__)
    const std::string message = GetPosixStrErrorMessage(errorNumber);
    if (!message.empty())
    {
        return message;
    }
#else
    const char* message = std::strerror(errorNumber);
    if (message != nullptr && message[0] != '\0')
    {
        return TrimTrailingAsciiWhitespace(message);
    }
#endif

    return BuildFallbackNativeErrorMessage(u8"C runtime error", static_cast<uint32_t>(errorNumber));
}

int32_t GB_SystemError::Win32ErrorCodeToHResult(const uint32_t win32ErrorCode)
{
    const int32_t signedErrorCode = static_cast<int32_t>(win32ErrorCode);
    if (signedErrorCode <= 0)
    {
        return signedErrorCode;
    }

    const uint32_t hresult = (win32ErrorCode & GB_HResultCodeMask) | (GB_HResultFacilityWin32 << 16) | GB_HResultSeverityFailure;
    return static_cast<int32_t>(hresult);
}

uint32_t GB_SystemError::HResultToWin32ErrorCode(const int32_t hresult, bool& converted)
{
    converted = false;

    if (hresult == 0)
    {
        converted = true;
        return 0;
    }

    if (hresult > 0)
    {
        return 0;
    }

    const uint32_t unsignedHResult = static_cast<uint32_t>(hresult);
    if ((unsignedHResult & GB_HResultSeverityFailure) == 0)
    {
        return 0;
    }

    const uint32_t facility = (unsignedHResult >> 16) & GB_HResultFacilityMask;
    if (facility != GB_HResultFacilityWin32)
    {
        return 0;
    }

    const uint32_t win32ErrorCode = unsignedHResult & GB_HResultCodeMask;
    if (win32ErrorCode == 0)
    {
        return 0;
    }

    converted = true;
    return win32ErrorCode;
}

bool GB_SystemError::IsHResultSucceeded(const int32_t hresult)
{
    return hresult >= 0;
}

bool GB_SystemError::IsHResultFailed(const int32_t hresult)
{
    return hresult < 0;
}

std::string GB_SystemError::FormatHex32(const uint32_t value, const bool withPrefix)
{
    char buffer[16] = {};
    if (withPrefix)
    {
        std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%08X", value);
    }

    return std::string(buffer);
}

std::string GB_SystemError::FormatHex64(const uint64_t value, const bool withPrefix)
{
    char buffer[32] = {};
    if (withPrefix)
    {
        std::snprintf(buffer, sizeof(buffer), "0x%016llX", static_cast<unsigned long long>(value));
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(value));
    }

    return std::string(buffer);
}

std::string GB_SystemError::FormatNativeErrorCode(const GB_NativeErrorSource errorSource, const uint64_t errorCode)
{
    const GB_NativeErrorSource normalizedErrorSource = NormalizeNativeErrorSource(errorSource);

    switch (normalizedErrorSource)
    {
    case GB_NativeErrorSource::Win32:
    case GB_NativeErrorSource::HResult:
    case GB_NativeErrorSource::NtStatus:
    case GB_NativeErrorSource::Com:
    case GB_NativeErrorSource::WinRt:
        return CanRepresentUint32(errorCode) ? FormatHex32(static_cast<uint32_t>(errorCode)) : FormatHex64(errorCode);

    case GB_NativeErrorSource::CRuntime:
        return CanRepresentNonNegativeInt(errorCode) ? std::to_string(static_cast<int>(errorCode)) : FormatHex64(errorCode);

    case GB_NativeErrorSource::GlobalBase:
    case GB_NativeErrorSource::None:
    default:
        break;
    }

    return FormatNativeCode(errorCode);
}

std::string GB_SystemError::FormatNativeErrorMessage(const GB_NativeErrorSource errorSource, const uint64_t errorCode)
{
    const GB_NativeErrorSource normalizedErrorSource = NormalizeNativeErrorSource(errorSource);

    switch (normalizedErrorSource)
    {
    case GB_NativeErrorSource::None:
        return errorCode == 0 ? std::string() : BuildFallbackNativeErrorMessage(u8"Native error", errorCode);

    case GB_NativeErrorSource::GlobalBase:
    {
        GB_SystemErrorCode systemErrorCode = GB_SystemErrorCode::UnknownError;
        if (TryGetGlobalBaseErrorCode(errorCode, systemErrorCode))
        {
            std::string result = GetErrorCodeName(systemErrorCode);
            result += " ";
            result += FormatHex32(static_cast<uint32_t>(errorCode));
            result += ": ";
            result += GetErrorCodeDescription(systemErrorCode);
            return result;
        }

        return BuildFallbackNativeErrorMessage(u8"GlobalBase error", errorCode);
    }

    case GB_NativeErrorSource::Win32:
        return CanRepresentUint32(errorCode) ? GetWin32ErrorMessage(static_cast<uint32_t>(errorCode)) : BuildFallbackNativeErrorMessage(u8"Win32 error", errorCode);

    case GB_NativeErrorSource::HResult:
        return CanRepresentUint32(errorCode) ? GetHResultMessage(static_cast<int32_t>(static_cast<uint32_t>(errorCode))) : BuildFallbackNativeErrorMessage(u8"HRESULT", errorCode);

    case GB_NativeErrorSource::Com:
        return CanRepresentUint32(errorCode) ? GetHResultMessage(static_cast<int32_t>(static_cast<uint32_t>(errorCode))) : BuildFallbackNativeErrorMessage(u8"COM HRESULT", errorCode);

    case GB_NativeErrorSource::WinRt:
        return CanRepresentUint32(errorCode) ? GetHResultMessage(static_cast<int32_t>(static_cast<uint32_t>(errorCode))) : BuildFallbackNativeErrorMessage(u8"WinRT HRESULT", errorCode);

    case GB_NativeErrorSource::NtStatus:
        return CanRepresentUint32(errorCode) ? GetNtStatusMessage(static_cast<uint32_t>(errorCode)) : BuildFallbackNativeErrorMessage(u8"NTSTATUS", errorCode);

    case GB_NativeErrorSource::CRuntime:
        return CanRepresentNonNegativeInt(errorCode) ? GetCRuntimeErrorMessage(static_cast<int>(errorCode)) : BuildFallbackNativeErrorMessage(u8"C runtime error", errorCode);
    }

    return BuildFallbackNativeErrorMessage(u8"Native error", errorCode);
}

GB_NativeErrorInfo GB_SystemError::MakeNativeErrorInfo(const GB_NativeErrorSource errorSource, const uint64_t errorCode)
{
    GB_NativeErrorInfo errorInfo;
    errorInfo.errorSource = NormalizeNativeErrorSource(errorSource);
    errorInfo.errorCode = errorCode;
    errorInfo.message = FormatNativeErrorMessage(errorInfo.errorSource, errorCode);
    return errorInfo;
}

GB_NativeErrorInfo GB_SystemError::MakeLastWin32ErrorInfo()
{
    const uint32_t win32ErrorCode = GetLastWin32ErrorCode();
    return MakeNativeErrorInfo(GB_NativeErrorSource::Win32, win32ErrorCode);
}

GB_SystemErrorCode GB_SystemError::GuessErrorCodeFromWin32ErrorCode(const uint32_t win32ErrorCode)
{
    if (win32ErrorCode == 0)
    {
        return GB_SystemErrorCode::Succeeded;
    }

    switch (win32ErrorCode)
    {
    case 11:    // ERROR_BAD_FORMAT
    case 13:    // ERROR_INVALID_DATA
        return GB_SystemErrorCode::ParseFailed;

    case 2:     // ERROR_FILE_NOT_FOUND
    case 3:     // ERROR_PATH_NOT_FOUND
    case 15:    // ERROR_INVALID_DRIVE
    case 18:    // ERROR_NO_MORE_FILES
    case 53:    // ERROR_BAD_NETPATH
    case 67:    // ERROR_BAD_NET_NAME
    case 126:   // ERROR_MOD_NOT_FOUND
    case 127:   // ERROR_PROC_NOT_FOUND
    case 1157:  // ERROR_DLL_NOT_FOUND
    case 161:   // ERROR_BAD_PATHNAME
    case 203:   // ERROR_ENVVAR_NOT_FOUND
    case 267:   // ERROR_DIRECTORY
    case 1060:  // ERROR_SERVICE_DOES_NOT_EXIST
    case 1168:  // ERROR_NOT_FOUND
    case 1169:  // ERROR_NO_MATCH
        return GB_SystemErrorCode::NotFound;

    case 5:     // ERROR_ACCESS_DENIED
    case 1300:  // ERROR_NOT_ALL_ASSIGNED
    case 1314:  // ERROR_PRIVILEGE_NOT_HELD
        return GB_SystemErrorCode::PermissionDenied;

    case 6:     // ERROR_INVALID_HANDLE
    case 87:    // ERROR_INVALID_PARAMETER
    case 124:   // ERROR_INVALID_LEVEL
    case 160:   // ERROR_BAD_ARGUMENTS
    case 1004:  // ERROR_INVALID_FLAGS
    case 1008:  // ERROR_NO_TOKEN
        return GB_SystemErrorCode::InvalidArgument;

    case 123:   // ERROR_INVALID_NAME
    case 206:   // ERROR_FILENAME_EXCED_RANGE
        return GB_SystemErrorCode::InvalidArgument;

    case 1113:  // ERROR_NO_UNICODE_TRANSLATION
        return GB_SystemErrorCode::EncodingConversionFailed;

    case 170:   // ERROR_BUSY
    case 231:   // ERROR_PIPE_BUSY
    case 32:    // ERROR_SHARING_VIOLATION
    case 33:    // ERROR_LOCK_VIOLATION
        return GB_SystemErrorCode::ResourceBusy;

    case 21:    // ERROR_NOT_READY
    case 31:    // ERROR_GEN_FAILURE
    case 1051:  // ERROR_DEPENDENT_SERVICES_RUNNING
    case 1056:  // ERROR_SERVICE_ALREADY_RUNNING
    case 1058:  // ERROR_SERVICE_DISABLED
    case 1061:  // ERROR_SERVICE_CANNOT_ACCEPT_CTRL
    case 1115:  // ERROR_SHUTDOWN_IN_PROGRESS
    case 1167:  // ERROR_DEVICE_NOT_CONNECTED
    case 1247:  // ERROR_ALREADY_INITIALIZED
    case 5023:  // ERROR_INVALID_STATE
        return GB_SystemErrorCode::InvalidState;

    case 8:     // ERROR_NOT_ENOUGH_MEMORY
    case 14:    // ERROR_OUTOFMEMORY
    case 111:   // ERROR_BUFFER_OVERFLOW
    case 122:   // ERROR_INSUFFICIENT_BUFFER
    case 1450:  // ERROR_NO_SYSTEM_RESOURCES
    case 1130:  // ERROR_NOT_ENOUGH_SERVER_MEMORY
    case 1816:  // ERROR_NOT_ENOUGH_QUOTA
        return GB_SystemErrorCode::ResourceAllocationFailed;

    case 1:     // ERROR_INVALID_FUNCTION
    case 50:    // ERROR_NOT_SUPPORTED
    case 120:   // ERROR_CALL_NOT_IMPLEMENTED
    case 1150:  // ERROR_OLD_WIN_VERSION
        return GB_SystemErrorCode::UnsupportedPlatform;

    case 80:    // ERROR_FILE_EXISTS
    case 183:   // ERROR_ALREADY_EXISTS
    case 1073:  // ERROR_SERVICE_EXISTS
        return GB_SystemErrorCode::AlreadyExists;

    case 121:   // ERROR_SEM_TIMEOUT
    case 258:   // WAIT_TIMEOUT
    case 1053:  // ERROR_SERVICE_REQUEST_TIMEOUT
    case 1460:  // ERROR_TIMEOUT
        return GB_SystemErrorCode::Timeout;

    case 995:   // ERROR_OPERATION_ABORTED
    case 1223:  // ERROR_CANCELLED
        return GB_SystemErrorCode::Cancelled;

    default:
        break;
    }

    return GB_SystemErrorCode::NativeApiFailed;
}

GB_SystemErrorCode GB_SystemError::GuessErrorCodeFromHResult(const int32_t hresult)
{
    return GuessFailureErrorCodeFromHResult(hresult, GB_SystemErrorCode::NativeApiFailed);
}

GB_SystemErrorCode GB_SystemError::GuessErrorCodeFromNtStatus(const uint32_t ntStatus)
{
    if (ntStatus == 0x00000000u) // STATUS_SUCCESS
    {
        return GB_SystemErrorCode::Succeeded;
    }

    switch (ntStatus)
    {
    case 0x00000102u: // STATUS_TIMEOUT
    case 0xC00000B5u: // STATUS_IO_TIMEOUT
        return GB_SystemErrorCode::Timeout;

    case 0xC0000120u: // STATUS_CANCELLED
        return GB_SystemErrorCode::Cancelled;

    case 0xC0000121u: // STATUS_CANNOT_DELETE
        return GB_SystemErrorCode::InvalidState;

    case 0xC000000Du: // STATUS_INVALID_PARAMETER
    case 0xC0000008u: // STATUS_INVALID_HANDLE
    case 0xC00000EFu: // STATUS_INVALID_PARAMETER_1
    case 0xC00000F0u: // STATUS_INVALID_PARAMETER_2
    case 0xC00000F1u: // STATUS_INVALID_PARAMETER_3
    case 0xC00000F2u: // STATUS_INVALID_PARAMETER_4
        return GB_SystemErrorCode::InvalidArgument;

    case 0xC0000022u: // STATUS_ACCESS_DENIED
    case 0xC0000061u: // STATUS_PRIVILEGE_NOT_HELD
        return GB_SystemErrorCode::PermissionDenied;

    case 0xC000000Fu: // STATUS_NO_SUCH_FILE
    case 0xC0000034u: // STATUS_OBJECT_NAME_NOT_FOUND
    case 0xC000003Au: // STATUS_OBJECT_PATH_NOT_FOUND
    case 0xC0000225u: // STATUS_NOT_FOUND
        return GB_SystemErrorCode::NotFound;

    case 0xC0000035u: // STATUS_OBJECT_NAME_COLLISION
        return GB_SystemErrorCode::AlreadyExists;

    case 0xC0000017u: // STATUS_NO_MEMORY
    case 0xC000009Au: // STATUS_INSUFFICIENT_RESOURCES
        return GB_SystemErrorCode::ResourceAllocationFailed;

    case 0xC00000BBu: // STATUS_NOT_SUPPORTED
    case 0xC0000002u: // STATUS_NOT_IMPLEMENTED
        return GB_SystemErrorCode::UnsupportedPlatform;

    default:
        break;
    }

    if (static_cast<int32_t>(ntStatus) >= 0)
    {
        return GB_SystemErrorCode::Succeeded;
    }

    return GB_SystemErrorCode::NativeApiFailed;
}

GB_SystemErrorCode GB_SystemError::GuessErrorCodeFromCRuntimeErrorCode(const int errorNumber)
{
    if (errorNumber == 0)
    {
        return GB_SystemErrorCode::Succeeded;
    }

    switch (errorNumber)
    {
#if defined(EINVAL)
    case EINVAL:
        return GB_SystemErrorCode::InvalidArgument;
#endif
#if defined(ERANGE) && (!defined(EINVAL) || ERANGE != EINVAL)
    case ERANGE:
        return GB_SystemErrorCode::InvalidArgument;
#endif
#if defined(EILSEQ) && (!defined(EINVAL) || EILSEQ != EINVAL) && (!defined(ERANGE) || EILSEQ != ERANGE)
    case EILSEQ:
        return GB_SystemErrorCode::EncodingConversionFailed;
#endif
#if defined(EPERM)
    case EPERM:
        return GB_SystemErrorCode::PermissionDenied;
#endif
#if defined(EACCES) && (!defined(EPERM) || EACCES != EPERM)
    case EACCES:
        return GB_SystemErrorCode::PermissionDenied;
#endif
#if defined(ENOENT)
    case ENOENT:
        return GB_SystemErrorCode::NotFound;
#endif
#if defined(ENODEV) && (!defined(ENOENT) || ENODEV != ENOENT)
    case ENODEV:
        return GB_SystemErrorCode::NotFound;
#endif
#if defined(ENOTDIR) && (!defined(ENOENT) || ENOTDIR != ENOENT)
    case ENOTDIR:
        return GB_SystemErrorCode::NotFound;
#endif
#if defined(EEXIST)
    case EEXIST:
        return GB_SystemErrorCode::AlreadyExists;
#endif
#if defined(ENOMEM)
    case ENOMEM:
        return GB_SystemErrorCode::ResourceAllocationFailed;
#endif
#if defined(ETIMEDOUT)
    case ETIMEDOUT:
        return GB_SystemErrorCode::Timeout;
#endif
#if defined(ECANCELED)
    case ECANCELED:
        return GB_SystemErrorCode::Cancelled;
#endif
#if defined(ENOTSUP)
    case ENOTSUP:
        return GB_SystemErrorCode::UnsupportedPlatform;
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
    case EOPNOTSUPP:
        return GB_SystemErrorCode::UnsupportedPlatform;
#endif
#if defined(ENOSYS) && (!defined(ENOTSUP) || ENOSYS != ENOTSUP)
    case ENOSYS:
        return GB_SystemErrorCode::UnsupportedPlatform;
#endif
#if defined(EBUSY)
    case EBUSY:
        return GB_SystemErrorCode::InvalidState;
#endif
#if defined(EAGAIN) && (!defined(EBUSY) || EAGAIN != EBUSY)
    case EAGAIN:
        return GB_SystemErrorCode::InvalidState;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || EWOULDBLOCK != EAGAIN) && (!defined(EBUSY) || EWOULDBLOCK != EBUSY)
    case EWOULDBLOCK:
        return GB_SystemErrorCode::InvalidState;
#endif
    default:
        break;
    }

    return GB_SystemErrorCode::OperationFailed;
}

GB_SystemErrorCode GB_SystemError::GuessErrorCodeFromNativeError(const GB_NativeErrorSource errorSource, const uint64_t errorCode)
{
    const GB_NativeErrorSource normalizedErrorSource = NormalizeNativeErrorSource(errorSource);

    switch (normalizedErrorSource)
    {
    case GB_NativeErrorSource::None:
        return errorCode == 0 ? GB_SystemErrorCode::Succeeded : GB_SystemErrorCode::UnknownError;

    case GB_NativeErrorSource::GlobalBase:
    {
        GB_SystemErrorCode systemErrorCode = GB_SystemErrorCode::UnknownError;
        if (TryGetGlobalBaseErrorCode(errorCode, systemErrorCode))
        {
            return systemErrorCode;
        }

        return GB_SystemErrorCode::UnknownError;
    }

    case GB_NativeErrorSource::Win32:
        if (!CanRepresentUint32(errorCode))
        {
            return GB_SystemErrorCode::NativeApiFailed;
        }
        return GuessErrorCodeFromWin32ErrorCode(static_cast<uint32_t>(errorCode));

    case GB_NativeErrorSource::HResult:
        if (!CanRepresentUint32(errorCode))
        {
            return GB_SystemErrorCode::NativeApiFailed;
        }
        return GuessFailureErrorCodeFromHResult(static_cast<int32_t>(static_cast<uint32_t>(errorCode)), GB_SystemErrorCode::NativeApiFailed);

    case GB_NativeErrorSource::Com:
        if (!CanRepresentUint32(errorCode))
        {
            return GB_SystemErrorCode::ComApiFailed;
        }
        return GuessFailureErrorCodeFromHResult(static_cast<int32_t>(static_cast<uint32_t>(errorCode)), GB_SystemErrorCode::ComApiFailed);

    case GB_NativeErrorSource::WinRt:
        if (!CanRepresentUint32(errorCode))
        {
            return GB_SystemErrorCode::WinRtApiFailed;
        }
        return GuessFailureErrorCodeFromHResult(static_cast<int32_t>(static_cast<uint32_t>(errorCode)), GB_SystemErrorCode::WinRtApiFailed);

    case GB_NativeErrorSource::NtStatus:
        if (!CanRepresentUint32(errorCode))
        {
            return GB_SystemErrorCode::NativeApiFailed;
        }
        return GuessErrorCodeFromNtStatus(static_cast<uint32_t>(errorCode));

    case GB_NativeErrorSource::CRuntime:
        if (!CanRepresentNonNegativeInt(errorCode))
        {
            return GB_SystemErrorCode::OperationFailed;
        }
        return GuessErrorCodeFromCRuntimeErrorCode(static_cast<int>(errorCode));
    }

    return GB_SystemErrorCode::UnknownError;
}
