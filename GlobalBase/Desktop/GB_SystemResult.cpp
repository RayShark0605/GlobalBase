#include "GB_SystemResult.h"

#include <limits>
#include <sstream>

namespace
{
    const uint32_t GB_FacilityNtBit = 0x10000000u;

    static int32_t MakeHResult(const uint32_t value)
    {
        return static_cast<int32_t>(value);
    }

    static uint64_t MakeUnsignedHResultCode(const int32_t hresult)
    {
        return static_cast<uint64_t>(static_cast<uint32_t>(hresult));
    }

    static uint64_t MakeUnsignedCRuntimeErrorCode(const int errorNumber)
    {
        return errorNumber > 0 ? static_cast<uint64_t>(errorNumber) : 0;
    }

    static GB_SystemErrorCode NormalizeErrorCode(const GB_SystemErrorCode errorCode)
    {
        const uint64_t errorCodeValue = static_cast<uint64_t>(errorCode);
        if (!GB_SystemError::IsValidErrorCodeValue(errorCodeValue))
        {
            return GB_SystemErrorCode::UnknownError;
        }

        return errorCode;
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

    static GB_SystemErrorCode NormalizeFailureErrorCode(const GB_SystemErrorCode errorCode)
    {
        const GB_SystemErrorCode normalizedErrorCode = NormalizeErrorCode(errorCode);
        if (normalizedErrorCode == GB_SystemErrorCode::Succeeded)
        {
            return GB_SystemErrorCode::OperationFailed;
        }

        return normalizedErrorCode;
    }

    static std::string ResolveMessage(const GB_SystemErrorCode errorCode, const std::string& message)
    {
        if (!message.empty())
        {
            return message;
        }

        return GB_SystemError::GetErrorCodeDescription(NormalizeErrorCode(errorCode));
    }

    static int32_t NtStatusToFailureHResult(const uint32_t ntStatus)
    {
        if (ntStatus == 0)
        {
            return 0;
        }

        if (static_cast<int32_t>(ntStatus) >= 0)
        {
            return 0;
        }

        return static_cast<int32_t>(ntStatus | GB_FacilityNtBit);
    }

    static bool CanRepresentUint32(const uint64_t value)
    {
        return value <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
    }

    static int32_t MakeRelatedHResult(const GB_NativeErrorSource errorSource, const uint64_t nativeErrorCode)
    {
        switch (errorSource)
        {
        case GB_NativeErrorSource::Win32:
            return CanRepresentUint32(nativeErrorCode) ? GB_SystemError::Win32ErrorCodeToHResult(static_cast<uint32_t>(nativeErrorCode)) : 0;

        case GB_NativeErrorSource::HResult:
        case GB_NativeErrorSource::Com:
        case GB_NativeErrorSource::WinRt:
            return CanRepresentUint32(nativeErrorCode) ? static_cast<int32_t>(static_cast<uint32_t>(nativeErrorCode)) : 0;

        case GB_NativeErrorSource::NtStatus:
            return CanRepresentUint32(nativeErrorCode) ? NtStatusToFailureHResult(static_cast<uint32_t>(nativeErrorCode)) : 0;

        default:
            break;
        }

        return 0;
    }

    static void AppendPart(std::ostringstream& stream, const std::string& name, const std::string& value)
    {
        if (value.empty())
        {
            return;
        }

        stream << ", " << name << "=" << value;
    }

    static bool ShouldBuildNativeMessage(const GB_SystemErrorCode errorCode, const GB_NativeErrorSource errorSource, const uint64_t nativeErrorCode)
    {
        if (GB_SystemError::IsSucceeded(errorCode))
        {
            return false;
        }

        if (errorSource == GB_NativeErrorSource::None)
        {
            return nativeErrorCode != 0;
        }

        return nativeErrorCode != 0;
    }

    static std::string BuildNativeMessage(const GB_SystemErrorCode errorCode, const GB_NativeErrorSource errorSource, const uint64_t nativeErrorCode)
    {
        if (!ShouldBuildNativeMessage(errorCode, errorSource, nativeErrorCode))
        {
            return std::string();
        }

        return GB_SystemError::FormatNativeErrorMessage(errorSource, nativeErrorCode);
    }
}

GB_SystemResult::GB_SystemResult()
{
    Reset();
}

GB_SystemResult::GB_SystemResult(const GB_SystemErrorCode errorCode, const std::string& operationName, const std::string& message)
{
    const GB_SystemErrorCode normalizedErrorCode = NormalizeErrorCode(errorCode);
    if (normalizedErrorCode == GB_SystemErrorCode::Succeeded)
    {
        *this = GB_SystemResult::Succeeded(operationName, message);
        return;
    }

    *this = GB_SystemResult::Failed(normalizedErrorCode, operationName, message);
}

GB_SystemResult GB_SystemResult::Succeeded(const std::string& operationName, const std::string& message)
{
    GB_SystemResult result;
    result.errorCode = GB_SystemErrorCode::Succeeded;
    result.errorSource = GB_NativeErrorSource::None;
    result.nativeErrorCode = 0;
    result.hresult = 0;
    result.operationName = operationName;
    result.message = message;
    result.nativeMessage.clear();
    return result;
}

GB_SystemResult GB_SystemResult::Failed(const GB_SystemErrorCode errorCode, const std::string& operationName, const std::string& message)
{
    GB_SystemResult result;
    result.errorCode = NormalizeFailureErrorCode(errorCode);
    result.errorSource = GB_NativeErrorSource::None;
    result.nativeErrorCode = 0;
    result.hresult = ErrorCodeToHResult(result.errorCode);
    result.operationName = operationName;
    result.message = ResolveMessage(result.errorCode, message);
    result.nativeMessage.clear();
    return result;
}

GB_SystemResult GB_SystemResult::FromGlobalBaseError(const GB_SystemErrorCode errorCode, const std::string& operationName, const std::string& message)
{
    const GB_SystemErrorCode normalizedErrorCode = NormalizeErrorCode(errorCode);
    if (normalizedErrorCode == GB_SystemErrorCode::Succeeded)
    {
        return GB_SystemResult::Succeeded(operationName, message);
    }

    GB_SystemResult result;
    result.errorCode = NormalizeFailureErrorCode(normalizedErrorCode);
    result.errorSource = GB_NativeErrorSource::GlobalBase;
    result.nativeErrorCode = static_cast<uint64_t>(static_cast<uint16_t>(result.errorCode));
    result.hresult = ErrorCodeToHResult(result.errorCode);
    result.operationName = operationName;
    result.message = ResolveMessage(result.errorCode, message);
    result.nativeMessage = BuildNativeMessage(result.errorCode, result.errorSource, result.nativeErrorCode);
    return result;
}

GB_SystemResult GB_SystemResult::FromLastWin32Error(const std::string& operationName, const std::string& message)
{
    const GB_NativeErrorInfo nativeErrorInfo = GB_SystemError::MakeLastWin32ErrorInfo();
    return FromNativeErrorInfo(nativeErrorInfo, operationName, message);
}

GB_SystemResult GB_SystemResult::FromWin32Error(const uint32_t win32ErrorCode, const std::string& operationName, const std::string& message)
{
    return FromNativeError(GB_NativeErrorSource::Win32, win32ErrorCode, operationName, message);
}

GB_SystemResult GB_SystemResult::FromHResult(const int32_t hresult, const std::string& operationName, const std::string& message)
{
    return FromNativeError(GB_NativeErrorSource::HResult, MakeUnsignedHResultCode(hresult), operationName, message);
}

GB_SystemResult GB_SystemResult::FromComHResult(const int32_t hresult, const std::string& operationName, const std::string& message)
{
    return FromNativeError(GB_NativeErrorSource::Com, MakeUnsignedHResultCode(hresult), operationName, message);
}

GB_SystemResult GB_SystemResult::FromWinRtHResult(const int32_t hresult, const std::string& operationName, const std::string& message)
{
    return FromNativeError(GB_NativeErrorSource::WinRt, MakeUnsignedHResultCode(hresult), operationName, message);
}

GB_SystemResult GB_SystemResult::FromNtStatus(const uint32_t ntStatus, const std::string& operationName, const std::string& message)
{
    return FromNativeError(GB_NativeErrorSource::NtStatus, ntStatus, operationName, message);
}

GB_SystemResult GB_SystemResult::FromCRuntimeError(const int errorNumber, const std::string& operationName, const std::string& message)
{
    if (errorNumber == 0)
    {
        return GB_SystemResult::Succeeded(operationName, message);
    }

    if (errorNumber < 0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, message.empty() ? std::string(u8"errno 不能为负数。") : message);
    }

    return FromNativeError(GB_NativeErrorSource::CRuntime, MakeUnsignedCRuntimeErrorCode(errorNumber), operationName, message);
}

GB_SystemResult GB_SystemResult::FromNativeError(const GB_NativeErrorSource errorSource, const uint64_t nativeErrorCode, const std::string& operationName, const std::string& message)
{
    const GB_NativeErrorSource normalizedErrorSource = NormalizeNativeErrorSource(errorSource);
    const GB_SystemErrorCode guessedErrorCode = NormalizeErrorCode(GB_SystemError::GuessErrorCodeFromNativeError(normalizedErrorSource, nativeErrorCode));
    if (guessedErrorCode == GB_SystemErrorCode::Succeeded)
    {
        GB_SystemResult result = GB_SystemResult::Succeeded(operationName, message);
        result.errorSource = normalizedErrorSource;
        result.nativeErrorCode = nativeErrorCode;
        result.hresult = MakeRelatedHResult(normalizedErrorSource, nativeErrorCode);
        result.nativeMessage.clear();
        return result;
    }

    GB_SystemResult result;
    result.errorCode = NormalizeFailureErrorCode(guessedErrorCode);
    result.errorSource = normalizedErrorSource;
    result.nativeErrorCode = nativeErrorCode;
    result.hresult = MakeRelatedHResult(normalizedErrorSource, nativeErrorCode);
    if (result.hresult == 0 || GB_SystemError::IsHResultSucceeded(result.hresult))
    {
        result.hresult = ErrorCodeToHResult(result.errorCode);
    }
    result.operationName = operationName;
    result.message = ResolveMessage(result.errorCode, message);
    result.nativeMessage = BuildNativeMessage(result.errorCode, normalizedErrorSource, nativeErrorCode);
    return result;
}

GB_SystemResult GB_SystemResult::FromNativeErrorInfo(const GB_NativeErrorInfo& nativeErrorInfo, const std::string& operationName, const std::string& message)
{
    GB_SystemResult result = FromNativeError(nativeErrorInfo.errorSource, nativeErrorInfo.errorCode, operationName, message);
    if (!nativeErrorInfo.message.empty())
    {
        result.nativeMessage = nativeErrorInfo.message;
    }

    return result;
}

bool GB_SystemResult::IsSucceeded() const
{
    return GB_SystemError::IsSucceeded(NormalizeErrorCode(errorCode));
}

bool GB_SystemResult::IsFailed() const
{
    return GB_SystemError::IsFailed(NormalizeErrorCode(errorCode));
}

bool GB_SystemResult::HasNativeCode() const
{
    return NormalizeNativeErrorSource(errorSource) != GB_NativeErrorSource::None || nativeErrorCode != 0;
}

bool GB_SystemResult::HasNativeError() const
{
    return IsFailed() && (NormalizeNativeErrorSource(errorSource) != GB_NativeErrorSource::None || nativeErrorCode != 0);
}

bool GB_SystemResult::HasOperationName() const
{
    return !operationName.empty();
}

bool GB_SystemResult::HasMessage() const
{
    return !message.empty();
}

bool GB_SystemResult::HasNativeMessage() const
{
    return !nativeMessage.empty();
}

GB_SystemResult::operator bool() const
{
    return IsSucceeded();
}

GB_NativeErrorInfo GB_SystemResult::GetNativeErrorInfo() const
{
    GB_NativeErrorInfo nativeErrorInfo;
    nativeErrorInfo.errorSource = NormalizeNativeErrorSource(errorSource);
    nativeErrorInfo.errorCode = nativeErrorCode;
    nativeErrorInfo.message = nativeMessage;
    return nativeErrorInfo;
}

std::string GB_SystemResult::GetErrorCodeName() const
{
    return GB_SystemError::GetErrorCodeName(NormalizeErrorCode(errorCode));
}

std::string GB_SystemResult::GetErrorCodeDescription() const
{
    return GB_SystemError::GetErrorCodeDescription(NormalizeErrorCode(errorCode));
}

std::string GB_SystemResult::GetNativeErrorSourceName() const
{
    return GB_SystemError::GetNativeErrorSourceName(NormalizeNativeErrorSource(errorSource));
}

std::string GB_SystemResult::GetNativeErrorSourceDescription() const
{
    return GB_SystemError::GetNativeErrorSourceDescription(NormalizeNativeErrorSource(errorSource));
}

std::string GB_SystemResult::GetDisplayMessage() const
{
    std::ostringstream stream;

    if (!operationName.empty())
    {
        stream << operationName << ": ";
    }

    if (!message.empty())
    {
        stream << message;
    }
    else
    {
        stream << GetErrorCodeDescription();
    }

    if (IsFailed() && !nativeMessage.empty())
    {
        stream << u8" 原生错误：" << nativeMessage;
    }

    return stream.str();
}

std::string GB_SystemResult::ToString() const
{
    const GB_NativeErrorSource normalizedErrorSource = NormalizeNativeErrorSource(errorSource);

    std::ostringstream stream;
    stream << (IsSucceeded() ? "Succeeded" : "Failed");
    stream << "[errorCode=" << GetErrorCodeName();
    stream << ", errorDescription=" << GetErrorCodeDescription();

    if (!operationName.empty())
    {
        stream << ", operationName=" << operationName;
    }

    if (!message.empty())
    {
        stream << ", message=" << message;
    }

    if (HasNativeCode())
    {
        stream << ", errorSource=" << GetNativeErrorSourceName();
        stream << ", nativeErrorCode=" << GB_SystemError::FormatNativeErrorCode(normalizedErrorSource, nativeErrorCode);
    }

    if (hresult != 0)
    {
        stream << ", hresult=" << GB_SystemError::FormatHex32(static_cast<uint32_t>(hresult));
    }

    AppendPart(stream, "nativeMessage", nativeMessage);
    stream << "]";
    return stream.str();
}

int32_t GB_SystemResult::ToHResult() const
{
    if (IsSucceeded())
    {
        return GB_SystemError::IsHResultSucceeded(hresult) ? hresult : 0;
    }

    if (hresult != 0 && GB_SystemError::IsHResultFailed(hresult))
    {
        return hresult;
    }

    return ErrorCodeToHResult(NormalizeFailureErrorCode(errorCode));
}

void GB_SystemResult::Reset()
{
    errorCode = GB_SystemErrorCode::Succeeded;
    errorSource = GB_NativeErrorSource::None;
    nativeErrorCode = 0;
    hresult = 0;
    operationName.clear();
    message.clear();
    nativeMessage.clear();
}

GB_SystemResult& GB_SystemResult::WithOperationName(const std::string& operationName)
{
    this->operationName = operationName;
    return *this;
}

GB_SystemResult& GB_SystemResult::WithMessage(const std::string& message)
{
    this->message = message;
    return *this;
}

GB_SystemResult& GB_SystemResult::WithNativeMessage(const std::string& nativeMessage)
{
    this->nativeMessage = nativeMessage;
    return *this;
}

int32_t GB_SystemResult::ErrorCodeToHResult(const GB_SystemErrorCode errorCode)
{
    switch (NormalizeErrorCode(errorCode))
    {
    case GB_SystemErrorCode::Succeeded:
        return 0;

    case GB_SystemErrorCode::UnsupportedPlatform:
        return MakeHResult(0x80004001u); // E_NOTIMPL

    case GB_SystemErrorCode::InvalidArgument:
        return MakeHResult(0x80070057u); // E_INVALIDARG / HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER)

    case GB_SystemErrorCode::NotInitialized:
        return MakeHResult(0x800401F0u); // CO_E_NOTINITIALIZED

    case GB_SystemErrorCode::AlreadyInitialized:
        return MakeHResult(0x800401F1u); // CO_E_ALREADYINITIALIZED

    case GB_SystemErrorCode::PermissionDenied:
        return MakeHResult(0x80070005u); // E_ACCESSDENIED / HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)

    case GB_SystemErrorCode::NotFound:
        return GB_SystemError::Win32ErrorCodeToHResult(1168u); // ERROR_NOT_FOUND

    case GB_SystemErrorCode::AlreadyExists:
        return GB_SystemError::Win32ErrorCodeToHResult(183u); // ERROR_ALREADY_EXISTS

    case GB_SystemErrorCode::Timeout:
        return GB_SystemError::Win32ErrorCodeToHResult(1460u); // ERROR_TIMEOUT

    case GB_SystemErrorCode::Cancelled:
        return MakeHResult(0x80004004u); // E_ABORT

    case GB_SystemErrorCode::ResourceAllocationFailed:
        return MakeHResult(0x8007000Eu); // E_OUTOFMEMORY / HRESULT_FROM_WIN32(ERROR_OUTOFMEMORY)

    case GB_SystemErrorCode::InvalidState:
        return GB_SystemError::Win32ErrorCodeToHResult(5023u); // ERROR_INVALID_STATE

    case GB_SystemErrorCode::EncodingConversionFailed:
        return GB_SystemError::Win32ErrorCodeToHResult(1113u); // ERROR_NO_UNICODE_TRANSLATION

    case GB_SystemErrorCode::ParseFailed:
        return GB_SystemError::Win32ErrorCodeToHResult(13u); // ERROR_INVALID_DATA

    case GB_SystemErrorCode::ResourceBusy:
        return GB_SystemError::Win32ErrorCodeToHResult(170u); // ERROR_BUSY

    case GB_SystemErrorCode::InternalError:
        return MakeHResult(0x8000FFFFu); // E_UNEXPECTED

    case GB_SystemErrorCode::OperationFailed:
    case GB_SystemErrorCode::NativeApiFailed:
    case GB_SystemErrorCode::ComApiFailed:
    case GB_SystemErrorCode::WinRtApiFailed:
    case GB_SystemErrorCode::UnknownError:
    default:
        break;
    }

    return MakeHResult(0x80004005u); // E_FAIL
}
