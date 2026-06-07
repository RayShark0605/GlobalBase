#include "GB_SystemClipboard.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <new>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#  define NOMINMAX
#  include <Windows.h>
#  include <Shellapi.h>
#  include <ShlObj_core.h>
#  pragma comment(lib, "Gdi32.lib")
#  pragma comment(lib, "Shell32.lib")
#  pragma comment(lib, "User32.lib")
#  ifndef BI_ALPHABITFIELDS
#    define BI_ALPHABITFIELDS 6L
#  endif
#endif

namespace
{
    const size_t GB_ClipboardMaximumInternalTextStorageBytes = static_cast<size_t>(512) * 1024 * 1024;
    const uint32_t GB_ClipboardMaximumRetryCount = 1000;

    static const char* const GB_ClipboardOperationClear = "GB_SystemClipboard::Clear";
    static const char* const GB_ClipboardOperationIsFormatAvailable = "GB_SystemClipboard::IsFormatAvailable";
    static const char* const GB_ClipboardOperationHasText = "GB_SystemClipboard::HasText";
    static const char* const GB_ClipboardOperationHasImage = "GB_SystemClipboard::HasImage";
    static const char* const GB_ClipboardOperationHasFilePaths = "GB_SystemClipboard::HasFilePaths";
    static const char* const GB_ClipboardOperationGetSequenceNumber = "GB_SystemClipboard::GetSequenceNumber";
    static const char* const GB_ClipboardOperationGetFormats = "GB_SystemClipboard::GetFormats";
    static const char* const GB_ClipboardOperationGetText = "GB_SystemClipboard::GetText";
    static const char* const GB_ClipboardOperationSetText = "GB_SystemClipboard::SetText";
    static const char* const GB_ClipboardOperationGetImage = "GB_SystemClipboard::GetImage";
    static const char* const GB_ClipboardOperationSetImage = "GB_SystemClipboard::SetImage";
    static const char* const GB_ClipboardOperationGetFilePaths = "GB_SystemClipboard::GetFilePaths";
    static const char* const GB_ClipboardOperationSetFilePaths = "GB_SystemClipboard::SetFilePaths";
    static const char* const GB_ClipboardOperationWatcherStart = "GB_SystemClipboardWatcher::Start";
    static const char* const GB_ClipboardOperationWatcherStop = "GB_SystemClipboardWatcher::Stop";

    static GB_SystemResult MakeUnsupportedPlatformResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, "当前平台不支持 Windows 系统剪贴板能力。");
    }

    static bool HasEmbeddedNull(const std::string& text)
    {
        return text.find('\0') != std::string::npos;
    }

    static GB_SystemResult ValidateClipboardAccessOptions(const GB_SystemClipboardAccessOptions& options, const std::string& operationName)
    {
        if (options.retryCount > GB_ClipboardMaximumRetryCount)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "剪贴板重试次数超过允许上限。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    static bool CheckedAddSize(size_t leftValue, size_t rightValue, size_t& result)
    {
        if (rightValue > std::numeric_limits<size_t>::max() - leftValue)
        {
            return false;
        }
        result = leftValue + rightValue;
        return true;
    }

    static bool CheckedMultiplySize(size_t leftValue, size_t rightValue, size_t& result)
    {
        if (leftValue != 0 && rightValue > std::numeric_limits<size_t>::max() / leftValue)
        {
            return false;
        }
        result = leftValue * rightValue;
        return true;
    }

    static bool IsValidNewlineMode(GB_SystemClipboardNewlineMode newlineMode)
    {
        switch (newlineMode)
        {
        case GB_SystemClipboardNewlineMode::Preserve:
        case GB_SystemClipboardNewlineMode::Lf:
        case GB_SystemClipboardNewlineMode::CrLf:
            return true;
        default:
            return false;
        }
    }

    static std::string NormalizeNewlines(const std::string& text, GB_SystemClipboardNewlineMode newlineMode)
    {
        if (newlineMode == GB_SystemClipboardNewlineMode::Preserve)
        {
            return text;
        }

        std::string result;
        if (newlineMode == GB_SystemClipboardNewlineMode::CrLf && text.size() <= std::numeric_limits<size_t>::max() / 2U)
        {
            result.reserve(text.size() * 2U);
        }
        else
        {
            result.reserve(text.size());
        }
        for (size_t characterIndex = 0; characterIndex < text.size(); characterIndex++)
        {
            const char character = text[characterIndex];
            if (character == '\r')
            {
                if (characterIndex + 1 < text.size() && text[characterIndex + 1] == '\n')
                {
                    characterIndex++;
                }
                if (newlineMode == GB_SystemClipboardNewlineMode::Lf)
                {
                    result.push_back('\n');
                }
                else
                {
                    result.append("\r\n");
                }
                continue;
            }
            if (character == '\n')
            {
                if (newlineMode == GB_SystemClipboardNewlineMode::Lf)
                {
                    result.push_back('\n');
                }
                else
                {
                    result.append("\r\n");
                }
                continue;
            }
            result.push_back(character);
        }
        return result;
    }

#ifdef _WIN32
    static std::mutex& GetSelfSequenceMutex()
    {
        static std::mutex selfSequenceMutex;
        return selfSequenceMutex;
    }

    static std::deque<uint64_t>& GetSelfSequences()
    {
        static std::deque<uint64_t> selfSequences;
        return selfSequences;
    }

    static uint64_t ReadClipboardSequenceNumberRaw()
    {
        return static_cast<uint64_t>(::GetClipboardSequenceNumber());
    }

    static bool IsWin32SuccessCode(DWORD errorCode)
    {
        return errorCode == ERROR_SUCCESS;
    }

    static void RememberSelfWriteSequence()
    {
        const uint64_t sequenceNumber = ReadClipboardSequenceNumberRaw();
        if (sequenceNumber == 0)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(GetSelfSequenceMutex());
        std::deque<uint64_t>& selfSequences = GetSelfSequences();
        if (std::find(selfSequences.begin(), selfSequences.end(), sequenceNumber) == selfSequences.end())
        {
            selfSequences.push_back(sequenceNumber);
        }
        while (selfSequences.size() > 128)
        {
            selfSequences.pop_front();
        }
    }

    static bool IsSelfWriteSequence(uint64_t sequenceNumber)
    {
        if (sequenceNumber == 0)
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(GetSelfSequenceMutex());
        const std::deque<uint64_t>& selfSequences = GetSelfSequences();
        return std::find(selfSequences.begin(), selfSequences.end(), sequenceNumber) != selfSequences.end();
    }

    static bool Utf8ToWide(const std::string& textUtf8, std::wstring& textWide)
    {
        textWide.clear();
        if (textUtf8.empty())
        {
            return true;
        }
        if (textUtf8.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        const int inputLength = static_cast<int>(textUtf8.size());
        const int requiredLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, textUtf8.data(), inputLength, nullptr, 0);
        if (requiredLength <= 0)
        {
            return false;
        }

        try
        {
            textWide.resize(static_cast<size_t>(requiredLength));
        }
        catch (const std::bad_alloc&)
        {
            textWide.clear();
            return false;
        }

        const int convertedLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, textUtf8.data(), inputLength, &textWide[0], requiredLength);
        if (convertedLength != requiredLength)
        {
            textWide.clear();
            return false;
        }
        return true;
    }

    static bool WideCharsToUtf8(const wchar_t* textWide, size_t wideCharacterCount, std::string& textUtf8)
    {
        textUtf8.clear();
        if (wideCharacterCount == 0)
        {
            return true;
        }
        if (textWide == nullptr || wideCharacterCount > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        const int inputLength = static_cast<int>(wideCharacterCount);
        const int requiredLength = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, textWide, inputLength, nullptr, 0, nullptr, nullptr);
        if (requiredLength <= 0)
        {
            return false;
        }

        try
        {
            textUtf8.resize(static_cast<size_t>(requiredLength));
        }
        catch (const std::bad_alloc&)
        {
            textUtf8.clear();
            return false;
        }

        const int convertedLength = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, textWide, inputLength, &textUtf8[0], requiredLength, nullptr, nullptr);
        if (convertedLength != requiredLength)
        {
            textUtf8.clear();
            return false;
        }
        return true;
    }

    static bool WideToUtf8(const std::wstring& textWide, std::string& textUtf8)
    {
        return WideCharsToUtf8(textWide.data(), textWide.size(), textUtf8);
    }

    class GB_ClipboardOpenScope final
    {
    public:
        GB_ClipboardOpenScope() = default;

        ~GB_ClipboardOpenScope()
        {
            CloseSilently();
        }

        GB_ClipboardOpenScope(const GB_ClipboardOpenScope&) = delete;
        GB_ClipboardOpenScope& operator=(const GB_ClipboardOpenScope&) = delete;

        GB_SystemResult Open(const GB_SystemClipboardAccessOptions& options, const std::string& operationName, HWND ownerWindow = nullptr)
        {
            if (isOpened)
            {
                return GB_SystemResult::Succeeded(operationName);
            }

            const GB_SystemResult validationResult = ValidateClipboardAccessOptions(options, operationName);
            if (validationResult.IsFailed())
            {
                return validationResult;
            }

            DWORD lastErrorCode = ERROR_SUCCESS;
            DWORD retryDelay = options.initialRetryDelayMilliseconds;
            if (options.maxRetryDelayMilliseconds != 0 && retryDelay > options.maxRetryDelayMilliseconds)
            {
                retryDelay = options.maxRetryDelayMilliseconds;
            }
            uint64_t totalDelayMilliseconds = 0;
            const uint64_t totalAttemptCount = static_cast<uint64_t>(options.retryCount) + 1ULL;
            for (uint64_t attemptIndex = 0; attemptIndex < totalAttemptCount; attemptIndex++)
            {
                ::SetLastError(ERROR_SUCCESS);
                if (::OpenClipboard(ownerWindow))
                {
                    isOpened = true;
                    return GB_SystemResult::Succeeded(operationName);
                }

                lastErrorCode = ::GetLastError();
                if (attemptIndex + 1ULL == totalAttemptCount)
                {
                    break;
                }

                DWORD sleepMilliseconds = retryDelay;
                if (options.maxTotalRetryDelayMilliseconds != 0)
                {
                    if (totalDelayMilliseconds >= static_cast<uint64_t>(options.maxTotalRetryDelayMilliseconds))
                    {
                        break;
                    }
                    const uint64_t remainingDelay = static_cast<uint64_t>(options.maxTotalRetryDelayMilliseconds) - totalDelayMilliseconds;
                    sleepMilliseconds = static_cast<DWORD>(std::min<uint64_t>(static_cast<uint64_t>(sleepMilliseconds), remainingDelay));
                }

                ::Sleep(sleepMilliseconds);
                totalDelayMilliseconds += static_cast<uint64_t>(sleepMilliseconds);
                if (retryDelay == 0)
                {
                    retryDelay = 1;
                }
                else
                {
                    const DWORD doubledDelay = retryDelay > std::numeric_limits<DWORD>::max() / 2 ? std::numeric_limits<DWORD>::max() : retryDelay * 2;
                    retryDelay = options.maxRetryDelayMilliseconds == 0 ? doubledDelay : std::min<DWORD>(options.maxRetryDelayMilliseconds, doubledDelay);
                    if (retryDelay == 0)
                    {
                        retryDelay = 1;
                    }
                }
            }

            if (IsWin32SuccessCode(lastErrorCode) || lastErrorCode == ERROR_ACCESS_DENIED)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceBusy, operationName, "剪贴板正在被其他进程占用。");
            }
            return GB_SystemResult::FromWin32Error(lastErrorCode, operationName, "打开剪贴板失败。");
        }

        GB_SystemResult Close(const std::string& operationName)
        {
            if (!isOpened)
            {
                return GB_SystemResult::Succeeded(operationName);
            }
            if (!::CloseClipboard())
            {
                return GB_SystemResult::FromLastWin32Error(operationName, "关闭剪贴板失败。");
            }
            isOpened = false;
            return GB_SystemResult::Succeeded(operationName);
        }

    private:
        void CloseSilently() noexcept
        {
            if (isOpened)
            {
                ::CloseClipboard();
                isOpened = false;
            }
        }

    private:
        bool isOpened = false;
    };



    static const wchar_t* const GB_ClipboardOwnerWindowClassName = L"GB_SystemClipboardOwnerWindow";

    class GB_WindowHandleScope final
    {
    public:
        explicit GB_WindowHandleScope(HWND inputWindow = nullptr) : window(inputWindow)
        {
        }

        ~GB_WindowHandleScope()
        {
            Reset(nullptr);
        }

        GB_WindowHandleScope(const GB_WindowHandleScope&) = delete;
        GB_WindowHandleScope& operator=(const GB_WindowHandleScope&) = delete;

        HWND Get() const
        {
            return window;
        }

        void Reset(HWND newWindow)
        {
            if (window != nullptr)
            {
                ::DestroyWindow(window);
            }
            window = newWindow;
        }

    private:
        HWND window = nullptr;
    };

    static bool RegisterClipboardOwnerWindowClass()
    {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(WNDCLASSEXW);
        windowClass.lpfnWndProc = ::DefWindowProcW;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = GB_ClipboardOwnerWindowClassName;

        const ATOM classAtom = ::RegisterClassExW(&windowClass);
        return classAtom != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    static HWND CreateClipboardOwnerWindow()
    {
        if (!RegisterClipboardOwnerWindowClass())
        {
            return nullptr;
        }
        return ::CreateWindowExW(0, GB_ClipboardOwnerWindowClassName, L"GB_SystemClipboardOwner", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    }

    class GB_GlobalMemoryHandle final
    {
    public:
        GB_GlobalMemoryHandle() = default;
        explicit GB_GlobalMemoryHandle(HGLOBAL inputHandle) : handle(inputHandle) {}

        ~GB_GlobalMemoryHandle()
        {
            Reset(nullptr);
        }

        GB_GlobalMemoryHandle(const GB_GlobalMemoryHandle&) = delete;
        GB_GlobalMemoryHandle& operator=(const GB_GlobalMemoryHandle&) = delete;

        HGLOBAL Get() const
        {
            return handle;
        }

        HGLOBAL Detach()
        {
            HGLOBAL detachedHandle = handle;
            handle = nullptr;
            return detachedHandle;
        }

        void Reset(HGLOBAL newHandle)
        {
            if (handle != nullptr)
            {
                ::GlobalFree(handle);
            }
            handle = newHandle;
        }

    private:
        HGLOBAL handle = nullptr;
    };

    class GB_GlobalLockScope final
    {
    public:
        explicit GB_GlobalLockScope(HGLOBAL inputHandle) : handle(inputHandle)
        {
            if (handle != nullptr)
            {
                data = ::GlobalLock(handle);
            }
        }

        ~GB_GlobalLockScope()
        {
            if (data != nullptr && handle != nullptr)
            {
                ::GlobalUnlock(handle);
                data = nullptr;
            }
        }

        GB_GlobalLockScope(const GB_GlobalLockScope&) = delete;
        GB_GlobalLockScope& operator=(const GB_GlobalLockScope&) = delete;

        void* Get() const
        {
            return data;
        }

    private:
        HGLOBAL handle = nullptr;
        void* data = nullptr;
    };

    static GB_SystemClipboardFormatType GetFormatTypeFromIdAndName(UINT formatId, const std::string& formatName)
    {
        switch (formatId)
        {
        case CF_TEXT:
        case CF_OEMTEXT:
            return GB_SystemClipboardFormatType::Text;
        case CF_UNICODETEXT:
            return GB_SystemClipboardFormatType::UnicodeText;
        case CF_BITMAP:
            return GB_SystemClipboardFormatType::Bitmap;
        case CF_DIB:
            return GB_SystemClipboardFormatType::Dib;
        case CF_DIBV5:
            return GB_SystemClipboardFormatType::DibV5;
        case CF_HDROP:
            return GB_SystemClipboardFormatType::FilePaths;
        default:
            break;
        }

        if (formatName == "HTML Format")
        {
            return GB_SystemClipboardFormatType::Html;
        }
        if (formatName == "Rich Text Format")
        {
            return GB_SystemClipboardFormatType::RichText;
        }
        if (formatId >= 0xC000 && formatId <= 0xFFFF)
        {
            return GB_SystemClipboardFormatType::Registered;
        }
        if (formatId >= CF_PRIVATEFIRST && formatId <= CF_PRIVATELAST)
        {
            return GB_SystemClipboardFormatType::Private;
        }
        if (formatId >= CF_GDIOBJFIRST && formatId <= CF_GDIOBJLAST)
        {
            return GB_SystemClipboardFormatType::GdiObject;
        }
        return GB_SystemClipboardFormatType::Unknown;
    }

    static std::string GetStandardClipboardFormatName(UINT formatId)
    {
        switch (formatId)
        {
        case CF_TEXT:
            return "CF_TEXT";
        case CF_BITMAP:
            return "CF_BITMAP";
        case CF_METAFILEPICT:
            return "CF_METAFILEPICT";
        case CF_SYLK:
            return "CF_SYLK";
        case CF_DIF:
            return "CF_DIF";
        case CF_TIFF:
            return "CF_TIFF";
        case CF_OEMTEXT:
            return "CF_OEMTEXT";
        case CF_DIB:
            return "CF_DIB";
        case CF_PALETTE:
            return "CF_PALETTE";
        case CF_PENDATA:
            return "CF_PENDATA";
        case CF_RIFF:
            return "CF_RIFF";
        case CF_WAVE:
            return "CF_WAVE";
        case CF_UNICODETEXT:
            return "CF_UNICODETEXT";
        case CF_ENHMETAFILE:
            return "CF_ENHMETAFILE";
        case CF_HDROP:
            return "CF_HDROP";
        case CF_LOCALE:
            return "CF_LOCALE";
        case CF_DIBV5:
            return "CF_DIBV5";
        case CF_OWNERDISPLAY:
            return "CF_OWNERDISPLAY";
        case CF_DSPTEXT:
            return "CF_DSPTEXT";
        case CF_DSPBITMAP:
            return "CF_DSPBITMAP";
        case CF_DSPMETAFILEPICT:
            return "CF_DSPMETAFILEPICT";
        case CF_DSPENHMETAFILE:
            return "CF_DSPENHMETAFILE";
        default:
            return "";
        }
    }

    static bool TryGetClipboardFormatNameUtf8(UINT formatId, std::string& formatName)
    {
        formatName.clear();
        for (size_t bufferLength = 128; bufferLength <= 4096; bufferLength *= 2U)
        {
            std::vector<wchar_t> nameBuffer(bufferLength, L'\0');
            ::SetLastError(ERROR_SUCCESS);
            const int nameLength = ::GetClipboardFormatNameW(formatId, nameBuffer.data(), static_cast<int>(nameBuffer.size()));
            if (nameLength <= 0)
            {
                return false;
            }

            if (static_cast<size_t>(nameLength) + 1U < nameBuffer.size())
            {
                return WideCharsToUtf8(nameBuffer.data(), static_cast<size_t>(nameLength), formatName);
            }
        }
        return false;
    }

    static GB_SystemClipboardFormatInfo MakeFormatInfo(UINT formatId)
    {
        GB_SystemClipboardFormatInfo formatInfo;
        formatInfo.formatId = static_cast<uint32_t>(formatId);
        formatInfo.formatName = GetStandardClipboardFormatName(formatId);
        formatInfo.isStandardFormat = !formatInfo.formatName.empty();

        if (!formatInfo.isStandardFormat)
        {
            std::string formatName;
            if (TryGetClipboardFormatNameUtf8(formatId, formatName))
            {
                formatInfo.formatName = formatName;
            }
        }

        formatInfo.isRegisteredFormat = formatId >= 0xC000 && formatId <= 0xFFFF;
        formatInfo.isPrivateFormat = formatId >= CF_PRIVATEFIRST && formatId <= CF_PRIVATELAST;
        formatInfo.isGdiObjectFormat = formatId >= CF_GDIOBJFIRST && formatId <= CF_GDIOBJLAST;
        formatInfo.formatType = GetFormatTypeFromIdAndName(formatId, formatInfo.formatName);
        return formatInfo;
    }

    static bool IsClipboardImageFormat(UINT formatId)
    {
        return formatId == CF_DIBV5 || formatId == CF_DIB || formatId == CF_BITMAP;
    }

    static void AppendUniqueClipboardFormat(std::vector<UINT>& formats, UINT formatId)
    {
        if (std::find(formats.begin(), formats.end(), formatId) == formats.end())
        {
            formats.push_back(formatId);
        }
    }

    struct GB_ChannelMaskInfo
    {
        uint32_t mask = 0;
        uint32_t shift = 0;
        uint32_t bits = 0;
        bool valid = false;
    };

    static GB_ChannelMaskInfo MakeChannelMaskInfo(uint32_t mask)
    {
        GB_ChannelMaskInfo info;
        info.mask = mask;
        if (mask == 0)
        {
            return info;
        }

        while (info.shift < 32U && ((mask >> info.shift) & 1U) == 0U)
        {
            info.shift++;
        }
        if (info.shift >= 32U)
        {
            return info;
        }

        uint32_t shiftedMask = mask >> info.shift;
        while (info.bits < 32U && (shiftedMask & 1U) != 0U)
        {
            info.bits++;
            shiftedMask >>= 1U;
        }
        info.valid = info.bits > 0 && shiftedMask == 0;
        return info;
    }

    static unsigned char ExtractMaskedByte(uint32_t pixelValue, const GB_ChannelMaskInfo& maskInfo)
    {
        if (!maskInfo.valid)
        {
            return 0;
        }

        const uint32_t rawValue = (pixelValue & maskInfo.mask) >> maskInfo.shift;
        if (maskInfo.bits >= 8)
        {
            return static_cast<unsigned char>(rawValue >> (maskInfo.bits - 8));
        }

        const uint32_t maxValue = (1U << maskInfo.bits) - 1U;
        return static_cast<unsigned char>((rawValue * 255U + maxValue / 2U) / maxValue);
    }

    static bool DoesDibMaskFitBitCount(const GB_ChannelMaskInfo& maskInfo, WORD bitCount)
    {
        if (!maskInfo.valid || bitCount >= 32)
        {
            return true;
        }

        const uint32_t allowedMask = (1U << bitCount) - 1U;
        return (maskInfo.mask & ~allowedMask) == 0;
    }

    static bool AreDibChannelMasksValid(const GB_ChannelMaskInfo& redMaskInfo, const GB_ChannelMaskInfo& greenMaskInfo, const GB_ChannelMaskInfo& blueMaskInfo, const GB_ChannelMaskInfo& alphaMaskInfo, WORD bitCount)
    {
        if (!redMaskInfo.valid || !greenMaskInfo.valid || !blueMaskInfo.valid)
        {
            return false;
        }

        if (!DoesDibMaskFitBitCount(redMaskInfo, bitCount) || !DoesDibMaskFitBitCount(greenMaskInfo, bitCount) || !DoesDibMaskFitBitCount(blueMaskInfo, bitCount) || !DoesDibMaskFitBitCount(alphaMaskInfo, bitCount))
        {
            return false;
        }

        if ((redMaskInfo.mask & greenMaskInfo.mask) != 0 || (redMaskInfo.mask & blueMaskInfo.mask) != 0 || (greenMaskInfo.mask & blueMaskInfo.mask) != 0)
        {
            return false;
        }

        if (alphaMaskInfo.valid && ((alphaMaskInfo.mask & redMaskInfo.mask) != 0 || (alphaMaskInfo.mask & greenMaskInfo.mask) != 0 || (alphaMaskInfo.mask & blueMaskInfo.mask) != 0))
        {
            return false;
        }
        return true;
    }

    static size_t GetDibColorTableEntryCount(const BITMAPINFOHEADER* header)
    {
        if (header->biClrUsed != 0)
        {
            return static_cast<size_t>(header->biClrUsed);
        }

        if (header->biBitCount <= 8)
        {
            return static_cast<size_t>(1U) << header->biBitCount;
        }
        return 0;
    }

    static bool AddDibColorTableOffset(const BITMAPINFOHEADER* header, size_t byteSize, size_t& pixelOffset)
    {
        const size_t colorTableEntryCount = GetDibColorTableEntryCount(header);
        if (colorTableEntryCount == 0)
        {
            return pixelOffset <= byteSize;
        }

        size_t colorTableByteSize = 0;
        if (!CheckedMultiplySize(colorTableEntryCount, sizeof(RGBQUAD), colorTableByteSize))
        {
            return false;
        }
        return CheckedAddSize(pixelOffset, colorTableByteSize, pixelOffset) && pixelOffset <= byteSize;
    }

    static bool ReadUint32FromBytes(const unsigned char* bytes, size_t byteOffset, size_t byteSize, uint32_t& value)
    {
        if (byteOffset > byteSize || sizeof(uint32_t) > byteSize - byteOffset)
        {
            return false;
        }
        std::memcpy(&value, bytes + byteOffset, sizeof(value));
        return true;
    }

    static bool IsSupportedDibBitCount(WORD bitCount)
    {
        return bitCount == 1 || bitCount == 4 || bitCount == 8 || bitCount == 16 || bitCount == 24 || bitCount == 32;
    }

    static bool IsSupportedDibCompression(const BITMAPINFOHEADER* header)
    {
        if (header->biCompression == BI_RGB)
        {
            return true;
        }
        if (header->biCompression == BI_BITFIELDS || header->biCompression == BI_ALPHABITFIELDS)
        {
            return header->biBitCount == 16 || header->biBitCount == 32;
        }
        return false;
    }

    static const RGBQUAD* GetDibColorTable(const BITMAPINFOHEADER* header, const unsigned char* bytes, size_t byteSize, size_t maskByteSize)
    {
        const size_t colorTableEntryCount = GetDibColorTableEntryCount(header);
        if (colorTableEntryCount == 0)
        {
            return nullptr;
        }

        size_t colorTableOffset = 0;
        if (!CheckedAddSize(static_cast<size_t>(header->biSize), maskByteSize, colorTableOffset) || colorTableOffset > byteSize)
        {
            return nullptr;
        }

        size_t colorTableByteSize = 0;
        if (!CheckedMultiplySize(colorTableEntryCount, sizeof(RGBQUAD), colorTableByteSize) || colorTableByteSize > byteSize - colorTableOffset)
        {
            return nullptr;
        }
        return reinterpret_cast<const RGBQUAD*>(bytes + colorTableOffset);
    }

    static bool ReadDibMaskFromHeaderExtension(const unsigned char* bytes, size_t byteSize, size_t maskIndex, uint32_t& mask)
    {
        size_t maskOffset = 0;
        if (!CheckedAddSize(sizeof(BITMAPINFOHEADER), maskIndex * sizeof(uint32_t), maskOffset))
        {
            return false;
        }
        return ReadUint32FromBytes(bytes, maskOffset, byteSize, mask);
    }

    static bool GetDibLayout(const BITMAPINFOHEADER* header, const unsigned char* bytes, size_t byteSize, uint32_t& redMask, uint32_t& greenMask, uint32_t& blueMask, uint32_t& alphaMask, const RGBQUAD*& colorTable, size_t& pixelOffset)
    {
        redMask = 0x00FF0000U;
        greenMask = 0x0000FF00U;
        blueMask = 0x000000FFU;
        alphaMask = header->biBitCount == 32 ? 0xFF000000U : 0x00000000U;
        colorTable = nullptr;
        pixelOffset = static_cast<size_t>(header->biSize);

        size_t maskByteSize = 0;
        if (header->biCompression == BI_RGB)
        {
            if (header->biBitCount == 16)
            {
                redMask = 0x00007C00U;
                greenMask = 0x000003E0U;
                blueMask = 0x0000001FU;
                alphaMask = 0x00000000U;
            }
        }
        else
        {
            if (header->biSize >= sizeof(BITMAPV5HEADER))
            {
                const BITMAPV5HEADER* headerV5 = reinterpret_cast<const BITMAPV5HEADER*>(header);
                redMask = headerV5->bV5RedMask;
                greenMask = headerV5->bV5GreenMask;
                blueMask = headerV5->bV5BlueMask;
                alphaMask = headerV5->bV5AlphaMask;
            }
            else if (header->biSize >= sizeof(BITMAPV4HEADER))
            {
                const BITMAPV4HEADER* headerV4 = reinterpret_cast<const BITMAPV4HEADER*>(header);
                redMask = headerV4->bV4RedMask;
                greenMask = headerV4->bV4GreenMask;
                blueMask = headerV4->bV4BlueMask;
                alphaMask = headerV4->bV4AlphaMask;
            }
            else if (header->biSize >= sizeof(BITMAPINFOHEADER) + sizeof(uint32_t) * 3U)
            {
                if (!ReadDibMaskFromHeaderExtension(bytes, byteSize, 0U, redMask) || !ReadDibMaskFromHeaderExtension(bytes, byteSize, 1U, greenMask) || !ReadDibMaskFromHeaderExtension(bytes, byteSize, 2U, blueMask))
                {
                    return false;
                }
                alphaMask = 0x00000000U;
                if (header->biSize >= sizeof(BITMAPINFOHEADER) + sizeof(uint32_t) * 4U && !ReadDibMaskFromHeaderExtension(bytes, byteSize, 3U, alphaMask))
                {
                    return false;
                }
                pixelOffset = static_cast<size_t>(header->biSize);
            }
            else
            {
                const size_t maskCount = header->biCompression == BI_ALPHABITFIELDS ? 4U : 3U;
                if (!CheckedMultiplySize(maskCount, sizeof(uint32_t), maskByteSize))
                {
                    return false;
                }
                size_t requiredByteSize = 0;
                if (!CheckedAddSize(static_cast<size_t>(header->biSize), maskByteSize, requiredByteSize) || requiredByteSize > byteSize)
                {
                    return false;
                }

                if (!ReadUint32FromBytes(bytes, static_cast<size_t>(header->biSize), byteSize, redMask) || !ReadUint32FromBytes(bytes, static_cast<size_t>(header->biSize) + sizeof(uint32_t), byteSize, greenMask) || !ReadUint32FromBytes(bytes, static_cast<size_t>(header->biSize) + sizeof(uint32_t) * 2U, byteSize, blueMask))
                {
                    return false;
                }
                alphaMask = 0x00000000U;
                if (maskCount >= 4U && !ReadUint32FromBytes(bytes, static_cast<size_t>(header->biSize) + sizeof(uint32_t) * 3U, byteSize, alphaMask))
                {
                    return false;
                }
                pixelOffset = requiredByteSize;
            }
        }

        colorTable = GetDibColorTable(header, bytes, byteSize, maskByteSize);
        if (GetDibColorTableEntryCount(header) != 0 && colorTable == nullptr)
        {
            return false;
        }
        return AddDibColorTableOffset(header, byteSize, pixelOffset);
    }

    static bool GetDibPaletteColor(const RGBQUAD* colorTable, size_t colorTableEntryCount, uint32_t colorIndex, unsigned char& blue, unsigned char& green, unsigned char& red)
    {
        if (colorTable == nullptr || colorIndex >= colorTableEntryCount)
        {
            return false;
        }

        const RGBQUAD& color = colorTable[colorIndex];
        blue = color.rgbBlue;
        green = color.rgbGreen;
        red = color.rgbRed;
        return true;
    }

    static void SetBgraRowAlphaOpaque(unsigned char* rowData, size_t width)
    {
        for (size_t col = 0; col < width; col++)
        {
            rowData[col * 4U + 3U] = 255;
        }
    }

    static bool CopyBgra32DibFast(const unsigned char* bytes, size_t pixelOffset, size_t rowStrideBytes, size_t width, size_t height, bool topDown, bool readSourceAlpha, bool forceOpaqueAlpha, bool setOpaqueWhenAllAlphaZero, GB_Image& decodedImage)
    {
        const size_t copyRowBytes = width * 4U;
        bool hasNonZeroAlpha = false;

        for (size_t sourceRow = 0; sourceRow < height; sourceRow++)
        {
            const size_t destinationRow = topDown ? sourceRow : height - 1U - sourceRow;
            const unsigned char* sourceData = bytes + pixelOffset + sourceRow * rowStrideBytes;
            unsigned char* destinationData = decodedImage.GetRowData(destinationRow);
            if (destinationData == nullptr)
            {
                return false;
            }

            std::memcpy(destinationData, sourceData, copyRowBytes);
            if (forceOpaqueAlpha)
            {
                SetBgraRowAlphaOpaque(destinationData, width);
            }
            else if (readSourceAlpha && setOpaqueWhenAllAlphaZero)
            {
                for (size_t col = 0; col < width; col++)
                {
                    hasNonZeroAlpha = hasNonZeroAlpha || destinationData[col * 4U + 3U] != 0;
                }
            }
        }

        if (readSourceAlpha && setOpaqueWhenAllAlphaZero && !hasNonZeroAlpha)
        {
            for (size_t row = 0; row < height; row++)
            {
                unsigned char* rowData = decodedImage.GetRowData(row);
                if (rowData == nullptr)
                {
                    return false;
                }
                SetBgraRowAlphaOpaque(rowData, width);
            }
        }
        return true;
    }

    static bool DecodeDibMemory(const void* dibData, size_t dibByteSize, size_t maxImageBytes, GB_Image& image)
    {
        if (dibData == nullptr || dibByteSize < sizeof(BITMAPINFOHEADER))
        {
            return false;
        }

        const unsigned char* bytes = static_cast<const unsigned char*>(dibData);
        const BITMAPINFOHEADER* header = reinterpret_cast<const BITMAPINFOHEADER*>(bytes);
        if (header->biSize < sizeof(BITMAPINFOHEADER) || header->biSize > dibByteSize)
        {
            return false;
        }
        if (header->biPlanes != 1 || header->biWidth <= 0 || header->biHeight == 0)
        {
            return false;
        }
        if (!IsSupportedDibBitCount(header->biBitCount) || !IsSupportedDibCompression(header))
        {
            return false;
        }
        if (header->biHeight < 0 && header->biCompression != BI_RGB && header->biCompression != BI_BITFIELDS && header->biCompression != BI_ALPHABITFIELDS)
        {
            return false;
        }

        uint32_t redMask = 0;
        uint32_t greenMask = 0;
        uint32_t blueMask = 0;
        uint32_t alphaMask = 0;
        const RGBQUAD* colorTable = nullptr;
        size_t pixelOffset = 0;
        if (!GetDibLayout(header, bytes, dibByteSize, redMask, greenMask, blueMask, alphaMask, colorTable, pixelOffset))
        {
            return false;
        }

        const size_t width = static_cast<size_t>(header->biWidth);
        const size_t height = header->biHeight < 0 ? static_cast<size_t>(-static_cast<int64_t>(header->biHeight)) : static_cast<size_t>(header->biHeight);
        const bool topDown = header->biHeight < 0;

        size_t bitsPerRow = 0;
        if (!CheckedMultiplySize(width, static_cast<size_t>(header->biBitCount), bitsPerRow))
        {
            return false;
        }
        size_t rowStrideUnits = 0;
        if (!CheckedAddSize(bitsPerRow, 31U, rowStrideUnits))
        {
            return false;
        }
        size_t rowStrideBytes = 0;
        if (!CheckedMultiplySize(rowStrideUnits / 32U, 4U, rowStrideBytes))
        {
            return false;
        }

        size_t pixelByteSize = 0;
        if (!CheckedMultiplySize(rowStrideBytes, height, pixelByteSize))
        {
            return false;
        }
        size_t requiredByteSize = 0;
        if (!CheckedAddSize(pixelOffset, pixelByteSize, requiredByteSize) || requiredByteSize > dibByteSize)
        {
            return false;
        }

        size_t decodedPixelCount = 0;
        size_t decodedByteSize = 0;
        if (!CheckedMultiplySize(width, height, decodedPixelCount) || !CheckedMultiplySize(decodedPixelCount, 4U, decodedByteSize) || decodedByteSize > maxImageBytes)
        {
            return false;
        }

        GB_Image decodedImage(height, width, GB_ImageDepth::UInt8, 4, false);
        if (decodedImage.IsEmpty())
        {
            return false;
        }

        bool hasNonZeroAlpha = false;
        const size_t colorTableEntryCount = GetDibColorTableEntryCount(header);
        const GB_ChannelMaskInfo redMaskInfo = MakeChannelMaskInfo(redMask);
        const GB_ChannelMaskInfo greenMaskInfo = MakeChannelMaskInfo(greenMask);
        const GB_ChannelMaskInfo blueMaskInfo = MakeChannelMaskInfo(blueMask);
        const GB_ChannelMaskInfo alphaMaskInfo = MakeChannelMaskInfo(alphaMask);
        const bool hasExplicitAlphaMask = (header->biCompression == BI_BITFIELDS || header->biCompression == BI_ALPHABITFIELDS) && alphaMaskInfo.valid;
        if ((header->biBitCount == 16 || (header->biBitCount == 32 && header->biCompression != BI_RGB)) && !AreDibChannelMasksValid(redMaskInfo, greenMaskInfo, blueMaskInfo, alphaMaskInfo, header->biBitCount))
        {
            return false;
        }

        if (header->biBitCount == 32)
        {
            const bool isDefaultBgraRgbDib = header->biCompression == BI_RGB;
            const bool isBgraBitFieldsDib = (header->biCompression == BI_BITFIELDS || header->biCompression == BI_ALPHABITFIELDS) && redMask == 0x00FF0000U && greenMask == 0x0000FF00U && blueMask == 0x000000FFU && (alphaMask == 0xFF000000U || alphaMask == 0x00000000U);
            if (isDefaultBgraRgbDib || isBgraBitFieldsDib)
            {
                const bool readSourceAlpha = isDefaultBgraRgbDib || alphaMask == 0xFF000000U;
                const bool forceOpaqueAlpha = !isDefaultBgraRgbDib && alphaMask == 0x00000000U;
                const bool setOpaqueWhenAllAlphaZero = isDefaultBgraRgbDib;
                if (!CopyBgra32DibFast(bytes, pixelOffset, rowStrideBytes, width, height, topDown, readSourceAlpha, forceOpaqueAlpha, setOpaqueWhenAllAlphaZero, decodedImage))
                {
                    return false;
                }
                image = std::move(decodedImage);
                return true;
            }
        }

        for (size_t sourceRow = 0; sourceRow < height; sourceRow++)
        {
            const size_t destinationRow = topDown ? sourceRow : height - 1U - sourceRow;
            const unsigned char* sourceData = bytes + pixelOffset + sourceRow * rowStrideBytes;
            unsigned char* destinationData = decodedImage.GetRowData(destinationRow);
            if (destinationData == nullptr)
            {
                return false;
            }

            for (size_t col = 0; col < width; col++)
            {
                unsigned char blue = 0;
                unsigned char green = 0;
                unsigned char red = 0;
                unsigned char alpha = 255;

                if (header->biBitCount == 1)
                {
                    const uint32_t colorIndex = static_cast<uint32_t>((sourceData[col / 8U] >> (7U - (col % 8U))) & 0x01U);
                    if (!GetDibPaletteColor(colorTable, colorTableEntryCount, colorIndex, blue, green, red))
                    {
                        return false;
                    }
                }
                else if (header->biBitCount == 4)
                {
                    const unsigned char packedValue = sourceData[col / 2U];
                    const uint32_t colorIndex = (col % 2U == 0) ? static_cast<uint32_t>((packedValue >> 4U) & 0x0FU) : static_cast<uint32_t>(packedValue & 0x0FU);
                    if (!GetDibPaletteColor(colorTable, colorTableEntryCount, colorIndex, blue, green, red))
                    {
                        return false;
                    }
                }
                else if (header->biBitCount == 8)
                {
                    const uint32_t colorIndex = static_cast<uint32_t>(sourceData[col]);
                    if (!GetDibPaletteColor(colorTable, colorTableEntryCount, colorIndex, blue, green, red))
                    {
                        return false;
                    }
                }
                else if (header->biBitCount == 16)
                {
                    uint16_t pixelValue16 = 0;
                    std::memcpy(&pixelValue16, sourceData + col * 2U, sizeof(pixelValue16));
                    const uint32_t pixelValue = static_cast<uint32_t>(pixelValue16);
                    red = ExtractMaskedByte(pixelValue, redMaskInfo);
                    green = ExtractMaskedByte(pixelValue, greenMaskInfo);
                    blue = ExtractMaskedByte(pixelValue, blueMaskInfo);
                    alpha = alphaMaskInfo.valid ? ExtractMaskedByte(pixelValue, alphaMaskInfo) : 255;
                    hasNonZeroAlpha = hasNonZeroAlpha || alpha != 0;
                }
                else if (header->biBitCount == 24)
                {
                    const unsigned char* pixel = sourceData + col * 3U;
                    blue = pixel[0];
                    green = pixel[1];
                    red = pixel[2];
                }
                else if (header->biCompression == BI_RGB)
                {
                    const unsigned char* pixel = sourceData + col * 4U;
                    blue = pixel[0];
                    green = pixel[1];
                    red = pixel[2];
                    alpha = pixel[3];
                    hasNonZeroAlpha = hasNonZeroAlpha || alpha != 0;
                }
                else
                {
                    uint32_t pixelValue = 0;
                    std::memcpy(&pixelValue, sourceData + col * 4U, sizeof(pixelValue));
                    red = ExtractMaskedByte(pixelValue, redMaskInfo);
                    green = ExtractMaskedByte(pixelValue, greenMaskInfo);
                    blue = ExtractMaskedByte(pixelValue, blueMaskInfo);
                    alpha = alphaMaskInfo.valid ? ExtractMaskedByte(pixelValue, alphaMaskInfo) : 255;
                    hasNonZeroAlpha = hasNonZeroAlpha || alpha != 0;
                }

                unsigned char* destinationPixel = destinationData + col * 4U;
                destinationPixel[0] = blue;
                destinationPixel[1] = green;
                destinationPixel[2] = red;
                destinationPixel[3] = alpha;
            }
        }

        if ((header->biBitCount == 16 || header->biBitCount == 32) && !hasNonZeroAlpha && !hasExplicitAlphaMask)
        {
            for (size_t row = 0; row < height; row++)
            {
                unsigned char* rowData = decodedImage.GetRowData(row);
                if (rowData == nullptr)
                {
                    return false;
                }
                SetBgraRowAlphaOpaque(rowData, width);
            }
        }

        image = std::move(decodedImage);
        return true;
    }

    static bool DecodeBitmapHandle(HBITMAP bitmapHandle, size_t maxImageBytes, GB_Image& image)
    {
        if (bitmapHandle == nullptr)
        {
            return false;
        }

        BITMAP bitmap;
        ::ZeroMemory(&bitmap, sizeof(bitmap));
        if (::GetObjectW(bitmapHandle, sizeof(bitmap), &bitmap) != sizeof(bitmap))
        {
            return false;
        }
        if (bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0)
        {
            return false;
        }

        const size_t width = static_cast<size_t>(bitmap.bmWidth);
        const size_t height = static_cast<size_t>(bitmap.bmHeight);
        size_t pixelCount = 0;
        size_t pixelByteSize = 0;
        if (!CheckedMultiplySize(width, height, pixelCount) || !CheckedMultiplySize(pixelCount, 4U, pixelByteSize) || pixelByteSize > maxImageBytes)
        {
            return false;
        }
        if (height > static_cast<size_t>(std::numeric_limits<UINT>::max()))
        {
            return false;
        }

        GB_Image decodedImage(height, width, GB_ImageDepth::UInt8, 4, false);
        if (decodedImage.IsEmpty())
        {
            return false;
        }

        const size_t rowBytes = width * 4U;
        unsigned char* directPixels = nullptr;
        std::vector<unsigned char> temporaryPixels;
        if (decodedImage.IsContinuous() && decodedImage.GetRowStrideBytes() == rowBytes)
        {
            directPixels = decodedImage.GetData();
        }
        else
        {
            try
            {
                temporaryPixels.resize(pixelByteSize);
            }
            catch (const std::bad_alloc&)
            {
                return false;
            }
            directPixels = temporaryPixels.data();
        }
        if (directPixels == nullptr)
        {
            return false;
        }

        BITMAPINFO bitmapInfo;
        ::ZeroMemory(&bitmapInfo, sizeof(bitmapInfo));
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = bitmap.bmWidth;
        bitmapInfo.bmiHeader.biHeight = -bitmap.bmHeight;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        HDC screenDc = ::GetDC(nullptr);
        if (screenDc == nullptr)
        {
            return false;
        }
        const int scanLines = ::GetDIBits(screenDc, bitmapHandle, 0, static_cast<UINT>(height), directPixels, &bitmapInfo, DIB_RGB_COLORS);
        ::ReleaseDC(nullptr, screenDc);
        if (scanLines != static_cast<int>(height))
        {
            return false;
        }

        bool hasNonZeroAlpha = false;
        for (size_t pixelIndex = 0; pixelIndex < pixelCount; pixelIndex++)
        {
            if (directPixels[pixelIndex * 4U + 3U] != 0)
            {
                hasNonZeroAlpha = true;
                break;
            }
        }
        if (!hasNonZeroAlpha)
        {
            for (size_t pixelIndex = 0; pixelIndex < pixelCount; pixelIndex++)
            {
                directPixels[pixelIndex * 4U + 3U] = 255;
            }
        }

        if (!temporaryPixels.empty())
        {
            for (size_t row = 0; row < height; row++)
            {
                unsigned char* destinationRow = decodedImage.GetRowData(row);
                if (destinationRow == nullptr)
                {
                    return false;
                }
                std::memcpy(destinationRow, temporaryPixels.data() + row * rowBytes, rowBytes);
            }
        }

        image = std::move(decodedImage);
        return true;
    }

    static bool CopyBgraImageRowsToMemory(const GB_Image& bgraImage, unsigned char* destinationPixels)
    {
        if (destinationPixels == nullptr)
        {
            return false;
        }

        const size_t width = bgraImage.GetWidth();
        const size_t height = bgraImage.GetHeight();
        const size_t rowBytes = width * 4U;
        for (size_t row = 0; row < height; row++)
        {
            const unsigned char* sourceRow = bgraImage.GetRowData(row);
            if (sourceRow == nullptr)
            {
                return false;
            }
            std::memcpy(destinationPixels + row * rowBytes, sourceRow, rowBytes);
        }
        return true;
    }

    static bool CopyBgraImageRowsToOpaqueMemory(const GB_Image& bgraImage, unsigned char* destinationPixels)
    {
        if (!CopyBgraImageRowsToMemory(bgraImage, destinationPixels))
        {
            return false;
        }

        const size_t width = bgraImage.GetWidth();
        const size_t height = bgraImage.GetHeight();
        const size_t rowBytes = width * 4U;
        for (size_t row = 0; row < height; row++)
        {
            SetBgraRowAlphaOpaque(destinationPixels + row * rowBytes, width);
        }
        return true;
    }

    static HGLOBAL MakeDibV5GlobalMemory(const GB_Image& bgraImage)
    {
        const size_t width = bgraImage.GetWidth();
        const size_t height = bgraImage.GetHeight();
        if (width == 0 || height == 0 || width > static_cast<size_t>(std::numeric_limits<LONG>::max()) || height > static_cast<size_t>(std::numeric_limits<LONG>::max()))
        {
            ::SetLastError(ERROR_INVALID_DATA);
            return nullptr;
        }

        size_t totalByteSize = 0;
        size_t pixelByteSize = 0;
        if (!CheckedMultiplySize(width, height, pixelByteSize) || !CheckedMultiplySize(pixelByteSize, 4U, pixelByteSize) || pixelByteSize > static_cast<size_t>(std::numeric_limits<DWORD>::max()))
        {
            ::SetLastError(ERROR_INVALID_DATA);
            return nullptr;
        }
        if (!CheckedAddSize(sizeof(BITMAPV5HEADER), pixelByteSize, totalByteSize))
        {
            ::SetLastError(ERROR_INVALID_DATA);
            return nullptr;
        }

        GB_GlobalMemoryHandle memoryHandle(::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalByteSize));
        if (memoryHandle.Get() == nullptr)
        {
            return nullptr;
        }

        GB_GlobalLockScope lockScope(memoryHandle.Get());
        void* lockedMemory = lockScope.Get();
        if (lockedMemory == nullptr)
        {
            return nullptr;
        }

        BITMAPV5HEADER* header = static_cast<BITMAPV5HEADER*>(lockedMemory);
        header->bV5Size = sizeof(BITMAPV5HEADER);
        header->bV5Width = static_cast<LONG>(width);
        header->bV5Height = -static_cast<LONG>(height);
        header->bV5Planes = 1;
        header->bV5BitCount = 32;
        header->bV5Compression = BI_BITFIELDS;
        header->bV5SizeImage = static_cast<DWORD>(pixelByteSize);
        header->bV5RedMask = 0x00FF0000U;
        header->bV5GreenMask = 0x0000FF00U;
        header->bV5BlueMask = 0x000000FFU;
        header->bV5AlphaMask = 0xFF000000U;
        header->bV5CSType = LCS_sRGB;
        if (!CopyBgraImageRowsToMemory(bgraImage, static_cast<unsigned char*>(lockedMemory) + sizeof(BITMAPV5HEADER)))
        {
            ::SetLastError(ERROR_INVALID_DATA);
            return nullptr;
        }
        return memoryHandle.Detach();
    }

    static HGLOBAL MakeDibGlobalMemory(const GB_Image& bgraImage)
    {
        const size_t width = bgraImage.GetWidth();
        const size_t height = bgraImage.GetHeight();
        if (width == 0 || height == 0 || width > static_cast<size_t>(std::numeric_limits<LONG>::max()) || height > static_cast<size_t>(std::numeric_limits<LONG>::max()))
        {
            ::SetLastError(ERROR_INVALID_DATA);
            return nullptr;
        }

        size_t totalByteSize = 0;
        size_t pixelByteSize = 0;
        if (!CheckedMultiplySize(width, height, pixelByteSize) || !CheckedMultiplySize(pixelByteSize, 4U, pixelByteSize) || pixelByteSize > static_cast<size_t>(std::numeric_limits<DWORD>::max()))
        {
            ::SetLastError(ERROR_INVALID_DATA);
            return nullptr;
        }
        if (!CheckedAddSize(sizeof(BITMAPINFOHEADER), pixelByteSize, totalByteSize))
        {
            ::SetLastError(ERROR_INVALID_DATA);
            return nullptr;
        }

        GB_GlobalMemoryHandle memoryHandle(::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalByteSize));
        if (memoryHandle.Get() == nullptr)
        {
            return nullptr;
        }

        GB_GlobalLockScope lockScope(memoryHandle.Get());
        void* lockedMemory = lockScope.Get();
        if (lockedMemory == nullptr)
        {
            return nullptr;
        }

        BITMAPINFOHEADER* header = static_cast<BITMAPINFOHEADER*>(lockedMemory);
        header->biSize = sizeof(BITMAPINFOHEADER);
        header->biWidth = static_cast<LONG>(width);
        header->biHeight = -static_cast<LONG>(height);
        header->biPlanes = 1;
        header->biBitCount = 32;
        header->biCompression = BI_RGB;
        header->biSizeImage = static_cast<DWORD>(pixelByteSize);
        if (!CopyBgraImageRowsToOpaqueMemory(bgraImage, static_cast<unsigned char*>(lockedMemory) + sizeof(BITMAPINFOHEADER)))
        {
            ::SetLastError(ERROR_INVALID_DATA);
            return nullptr;
        }
        return memoryHandle.Detach();
    }
#endif
}

GB_SystemResult GB_SystemClipboard::Clear(const GB_SystemClipboardAccessOptions& options)
{
#ifdef _WIN32
    GB_WindowHandleScope ownerWindow(CreateClipboardOwnerWindow());
    if (ownerWindow.Get() == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationClear, "创建剪贴板所有者窗口失败。");
    }

    GB_ClipboardOpenScope clipboardScope;
    GB_SystemResult result = clipboardScope.Open(options, GB_ClipboardOperationClear, ownerWindow.Get());
    if (result.IsFailed())
    {
        return result;
    }
    if (!::EmptyClipboard())
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationClear, "清空剪贴板失败。");
    }
    result = clipboardScope.Close(GB_ClipboardOperationClear);
    if (result.IsFailed())
    {
        return result;
    }
    RememberSelfWriteSequence();
    return GB_SystemResult::Succeeded(GB_ClipboardOperationClear);
#else
    (void)options;
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationClear);
#endif
}

GB_SystemResult GB_SystemClipboard::IsFormatAvailable(uint32_t formatId, bool& available)
{
#ifdef _WIN32
    if (formatId == 0 || formatId > static_cast<uint32_t>(std::numeric_limits<UINT>::max()))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationIsFormatAvailable, "剪贴板格式 ID 非法。");
    }
    available = ::IsClipboardFormatAvailable(static_cast<UINT>(formatId)) != FALSE;
    return GB_SystemResult::Succeeded(GB_ClipboardOperationIsFormatAvailable);
#else
    (void)formatId;
    available = false;
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationIsFormatAvailable);
#endif
}

GB_SystemResult GB_SystemClipboard::HasText(bool& hasText)
{
#ifdef _WIN32
    hasText = ::IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
    return GB_SystemResult::Succeeded(GB_ClipboardOperationHasText);
#else
    hasText = false;
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationHasText);
#endif
}

GB_SystemResult GB_SystemClipboard::HasImage(bool& hasImage)
{
#ifdef _WIN32
    hasImage = ::IsClipboardFormatAvailable(CF_DIBV5) != FALSE || ::IsClipboardFormatAvailable(CF_DIB) != FALSE || ::IsClipboardFormatAvailable(CF_BITMAP) != FALSE;
    return GB_SystemResult::Succeeded(GB_ClipboardOperationHasImage);
#else
    hasImage = false;
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationHasImage);
#endif
}

GB_SystemResult GB_SystemClipboard::HasFilePaths(bool& hasFilePaths)
{
#ifdef _WIN32
    hasFilePaths = ::IsClipboardFormatAvailable(CF_HDROP) != FALSE;
    return GB_SystemResult::Succeeded(GB_ClipboardOperationHasFilePaths);
#else
    hasFilePaths = false;
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationHasFilePaths);
#endif
}

GB_SystemResult GB_SystemClipboard::GetSequenceNumber(uint64_t& sequenceNumber)
{
#ifdef _WIN32
    sequenceNumber = ReadClipboardSequenceNumberRaw();
    return GB_SystemResult::Succeeded(GB_ClipboardOperationGetSequenceNumber);
#else
    sequenceNumber = 0;
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationGetSequenceNumber);
#endif
}

GB_SystemResult GB_SystemClipboard::GetFormats(std::vector<GB_SystemClipboardFormatInfo>& formats, const GB_SystemClipboardAccessOptions& options)
{
#ifdef _WIN32
    GB_ClipboardOpenScope clipboardScope;
    GB_SystemResult result = clipboardScope.Open(options, GB_ClipboardOperationGetFormats);
    if (result.IsFailed())
    {
        return result;
    }

    std::vector<GB_SystemClipboardFormatInfo> tempFormats;
    const int formatCount = ::CountClipboardFormats();
    try
    {
        if (formatCount > 0)
        {
            tempFormats.reserve(static_cast<size_t>(formatCount));
        }
    }
    catch (const std::bad_alloc&)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationGetFormats, "为剪贴板格式列表分配内存失败。");
    }

    UINT formatId = 0;
    ::SetLastError(ERROR_SUCCESS);
    while ((formatId = ::EnumClipboardFormats(formatId)) != 0)
    {
        try
        {
            tempFormats.push_back(MakeFormatInfo(formatId));
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationGetFormats, "保存剪贴板格式信息时内存分配失败。");
        }
    }

    const DWORD errorCode = ::GetLastError();
    if (errorCode != ERROR_SUCCESS)
    {
        return GB_SystemResult::FromWin32Error(errorCode, GB_ClipboardOperationGetFormats, "枚举剪贴板格式失败。");
    }

    formats.swap(tempFormats);
    return GB_SystemResult::Succeeded(GB_ClipboardOperationGetFormats);
#else
    (void)options;
    formats.clear();
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationGetFormats);
#endif
}

GB_SystemResult GB_SystemClipboard::GetText(std::string& text, const GB_SystemClipboardTextReadOptions& options)
{
#ifdef _WIN32
    if (!IsValidNewlineMode(options.newlineMode))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationGetText, "剪贴板文本换行处理策略枚举值非法。");
    }

    GB_ClipboardOpenScope clipboardScope;
    GB_SystemResult result = clipboardScope.Open(options.accessOptions, GB_ClipboardOperationGetText);
    if (result.IsFailed())
    {
        return result;
    }
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_ClipboardOperationGetText, "剪贴板中没有 Unicode 文本格式。");
    }

    HGLOBAL textHandle = static_cast<HGLOBAL>(::GetClipboardData(CF_UNICODETEXT));
    if (textHandle == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationGetText, "获取剪贴板文本句柄失败。");
    }

    const SIZE_T globalSize = ::GlobalSize(textHandle);
    if (globalSize == 0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NativeApiFailed, GB_ClipboardOperationGetText, "剪贴板文本内存块大小为 0。");
    }
    if (globalSize < sizeof(wchar_t) || (globalSize % sizeof(wchar_t)) != 0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ParseFailed, GB_ClipboardOperationGetText, "剪贴板 Unicode 文本内存块大小不是合法 UTF-16 字节数。");
    }
    if (globalSize > GB_ClipboardMaximumInternalTextStorageBytes)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationGetText, "剪贴板 Unicode 文本内部存储超过模块安全上限。");
    }

    GB_GlobalLockScope lockScope(textHandle);
    const wchar_t* textWideData = static_cast<const wchar_t*>(lockScope.Get());
    if (textWideData == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationGetText, "锁定剪贴板文本内存失败。");
    }

    const size_t maxWideCharacters = static_cast<size_t>(globalSize / sizeof(wchar_t));
    size_t actualWideCharacters = 0;
    while (actualWideCharacters < maxWideCharacters && textWideData[actualWideCharacters] != L'\0')
    {
        actualWideCharacters++;
    }
    if (actualWideCharacters == maxWideCharacters)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ParseFailed, GB_ClipboardOperationGetText, "剪贴板 Unicode 文本缺少终止 NUL。");
    }

    std::string textUtf8;
    if (!WideCharsToUtf8(textWideData, actualWideCharacters, textUtf8))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_ClipboardOperationGetText, "剪贴板 Unicode 文本转换为 UTF-8 失败。");
    }

    if (options.newlineMode != GB_SystemClipboardNewlineMode::Preserve)
    {
        try
        {
            textUtf8 = NormalizeNewlines(textUtf8, options.newlineMode);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationGetText, "规范化剪贴板文本换行时内存分配失败。");
        }
    }
    if (textUtf8.size() > options.maxTextBytes)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationGetText, "剪贴板文本转换后超过读取上限。");
    }

    text.swap(textUtf8);
    return GB_SystemResult::Succeeded(GB_ClipboardOperationGetText);
#else
    (void)options;
    text.clear();
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationGetText);
#endif
}

GB_SystemResult GB_SystemClipboard::SetText(const std::string& text, const GB_SystemClipboardTextWriteOptions& options)
{
#ifdef _WIN32
    if (!IsValidNewlineMode(options.newlineMode))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetText, "剪贴板文本换行处理策略枚举值非法。");
    }
    if (text.empty() && !options.allowEmptyText)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetText, "不允许写入空文本。");
    }
    if (HasEmbeddedNull(text))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetText, "剪贴板文本不能包含嵌入式 NUL 字符。");
    }
    if (text.size() > options.maxTextBytes)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetText, "文本超过写入上限。");
    }

    std::string normalizedText;
    const std::string* textForConversion = &text;
    if (options.newlineMode != GB_SystemClipboardNewlineMode::Preserve)
    {
        try
        {
            normalizedText = NormalizeNewlines(text, options.newlineMode);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationSetText, "规范化剪贴板文本换行时内存分配失败。");
        }
        if (normalizedText.size() > options.maxTextBytes)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetText, "文本换行规范化后超过写入上限。");
        }
        textForConversion = &normalizedText;
    }

    std::wstring textWide;
    if (!Utf8ToWide(*textForConversion, textWide))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetText, "输入文本不是有效 UTF-8 字符串。");
    }

    size_t wideCharacterCountWithNull = 0;
    if (!CheckedAddSize(textWide.size(), 1U, wideCharacterCountWithNull))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetText, "文本过长。");
    }
    size_t byteSize = 0;
    if (!CheckedMultiplySize(wideCharacterCountWithNull, sizeof(wchar_t), byteSize) || byteSize > GB_ClipboardMaximumInternalTextStorageBytes)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationSetText, "文本转换后的 UTF-16 内部存储超过模块安全上限。");
    }

    GB_GlobalMemoryHandle memoryHandle(::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, byteSize));
    if (memoryHandle.Get() == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetText, "为剪贴板文本分配全局内存失败。");
    }
    {
        GB_GlobalLockScope lockScope(memoryHandle.Get());
        void* lockedMemory = lockScope.Get();
        if (lockedMemory == nullptr)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetText, "锁定剪贴板文本内存失败。");
        }
        if (!textWide.empty())
        {
            std::memcpy(lockedMemory, textWide.data(), textWide.size() * sizeof(wchar_t));
        }
        static_cast<wchar_t*>(lockedMemory)[textWide.size()] = L'\0';
    }

    GB_WindowHandleScope ownerWindow(CreateClipboardOwnerWindow());
    if (ownerWindow.Get() == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetText, "创建剪贴板所有者窗口失败。");
    }

    GB_ClipboardOpenScope clipboardScope;
    GB_SystemResult result = clipboardScope.Open(options.accessOptions, GB_ClipboardOperationSetText, ownerWindow.Get());
    if (result.IsFailed())
    {
        return result;
    }
    if (!::EmptyClipboard())
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetText, "清空剪贴板失败。");
    }
    if (::SetClipboardData(CF_UNICODETEXT, memoryHandle.Get()) == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetText, "写入 CF_UNICODETEXT 失败。");
    }
    memoryHandle.Detach();
    result = clipboardScope.Close(GB_ClipboardOperationSetText);
    if (result.IsFailed())
    {
        return result;
    }
    RememberSelfWriteSequence();
    return GB_SystemResult::Succeeded(GB_ClipboardOperationSetText);
#else
    (void)text;
    (void)options;
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationSetText);
#endif
}

GB_SystemResult GB_SystemClipboard::GetImage(GB_Image& image, const GB_SystemClipboardImageReadOptions& options)
{
#ifdef _WIN32
    GB_ClipboardOpenScope clipboardScope;
    GB_SystemResult result = clipboardScope.Open(options.accessOptions, GB_ClipboardOperationGetImage);
    if (result.IsFailed())
    {
        return result;
    }

    std::vector<UINT> imageFormats;
    imageFormats.reserve(3U);
    UINT enumeratedFormatId = 0;
    ::SetLastError(ERROR_SUCCESS);
    while ((enumeratedFormatId = ::EnumClipboardFormats(enumeratedFormatId)) != 0)
    {
        if (IsClipboardImageFormat(enumeratedFormatId))
        {
            AppendUniqueClipboardFormat(imageFormats, enumeratedFormatId);
        }
    }

    const DWORD enumerateErrorCode = ::GetLastError();
    if (enumerateErrorCode != ERROR_SUCCESS)
    {
        return GB_SystemResult::FromWin32Error(enumerateErrorCode, GB_ClipboardOperationGetImage, "枚举剪贴板图片格式失败。");
    }

    if (imageFormats.empty())
    {
        if (::IsClipboardFormatAvailable(CF_DIBV5))
        {
            AppendUniqueClipboardFormat(imageFormats, CF_DIBV5);
        }
        if (::IsClipboardFormatAvailable(CF_DIB))
        {
            AppendUniqueClipboardFormat(imageFormats, CF_DIB);
        }
        if (::IsClipboardFormatAvailable(CF_BITMAP))
        {
            AppendUniqueClipboardFormat(imageFormats, CF_BITMAP);
        }
    }

    if (imageFormats.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_ClipboardOperationGetImage, "剪贴板中没有图片格式。");
    }

    bool foundImageFormat = false;
    for (const UINT formatId : imageFormats)
    {
        foundImageFormat = true;
        if (formatId == CF_DIBV5 || formatId == CF_DIB)
        {
            HGLOBAL dibHandle = static_cast<HGLOBAL>(::GetClipboardData(formatId));
            if (dibHandle == nullptr)
            {
                continue;
            }

            const SIZE_T dibSize = ::GlobalSize(dibHandle);
            if (dibSize > options.maxImageBytes)
            {
                const char* formatName = formatId == CF_DIBV5 ? "CF_DIBV5" : "CF_DIB";
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationGetImage, std::string("剪贴板 ") + formatName + " 图片超过读取上限。");
            }
            if (dibSize == 0)
            {
                continue;
            }

            GB_GlobalLockScope lockScope(dibHandle);
            if (lockScope.Get() == nullptr)
            {
                continue;
            }

            GB_Image decodedImage;
            if (DecodeDibMemory(lockScope.Get(), static_cast<size_t>(dibSize), options.maxImageBytes, decodedImage))
            {
                image = std::move(decodedImage);
                return GB_SystemResult::Succeeded(GB_ClipboardOperationGetImage);
            }
            continue;
        }

        if (formatId == CF_BITMAP)
        {
            HBITMAP bitmapHandle = static_cast<HBITMAP>(::GetClipboardData(CF_BITMAP));
            GB_Image decodedImage;
            if (DecodeBitmapHandle(bitmapHandle, options.maxImageBytes, decodedImage))
            {
                if (decodedImage.GetTotalByteSize() <= options.maxImageBytes)
                {
                    image = std::move(decodedImage);
                    return GB_SystemResult::Succeeded(GB_ClipboardOperationGetImage);
                }
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationGetImage, "剪贴板图片超过读取上限。");
            }
        }
    }

    if (foundImageFormat)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ParseFailed, GB_ClipboardOperationGetImage, "剪贴板中存在图片格式，但当前模块无法解析其内容。");
    }
    return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_ClipboardOperationGetImage, "剪贴板中没有可解析的图片格式。");
#else
    (void)image;
    (void)options;
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationGetImage);
#endif
}

GB_SystemResult GB_SystemClipboard::SetImage(const GB_Image& image, const GB_SystemClipboardImageWriteOptions& options)
{
#ifdef _WIN32
    if (image.IsEmpty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetImage, "不能写入空图像。");
    }
    if (image.GetTotalByteSize() > options.maxImageBytes)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetImage, "图像超过写入上限。");
    }

    GB_Image bgraImage = image.ConvertToBgra8();
    if (bgraImage.IsEmpty() || bgraImage.GetDepth() != GB_ImageDepth::UInt8 || bgraImage.GetChannels() != 4)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetImage, "当前仅支持可转换为 8 位 BGRA 的图像写入剪贴板。");
    }

    size_t pixelCount = 0;
    size_t pixelByteSize = 0;
    if (!CheckedMultiplySize(bgraImage.GetWidth(), bgraImage.GetHeight(), pixelCount) || !CheckedMultiplySize(pixelCount, 4U, pixelByteSize) || pixelByteSize > options.maxImageBytes)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetImage, "图像转换后超过写入上限。");
    }

    size_t dibV5ByteSize = 0;
    if (!CheckedAddSize(sizeof(BITMAPV5HEADER), pixelByteSize, dibV5ByteSize) || dibV5ByteSize > options.maxImageBytes)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetImage, "CF_DIBV5 数据超过写入上限。");
    }
    if (options.publishCompatibilityDib)
    {
        size_t dibByteSize = 0;
        if (!CheckedAddSize(sizeof(BITMAPINFOHEADER), pixelByteSize, dibByteSize) || dibByteSize > options.maxImageBytes)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetImage, "CF_DIB 兼容数据超过写入上限。");
        }
    }

    GB_GlobalMemoryHandle dibV5Handle(MakeDibV5GlobalMemory(bgraImage));
    if (dibV5Handle.Get() == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetImage, "创建 CF_DIBV5 内存失败。");
    }

    GB_GlobalMemoryHandle dibHandle;
    if (options.publishCompatibilityDib)
    {
        dibHandle.Reset(MakeDibGlobalMemory(bgraImage));
        if (dibHandle.Get() == nullptr)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetImage, "创建 CF_DIB 兼容内存失败。");
        }
    }

    GB_WindowHandleScope ownerWindow(CreateClipboardOwnerWindow());
    if (ownerWindow.Get() == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetImage, "创建剪贴板所有者窗口失败。");
    }

    GB_ClipboardOpenScope clipboardScope;
    GB_SystemResult result = clipboardScope.Open(options.accessOptions, GB_ClipboardOperationSetImage, ownerWindow.Get());
    if (result.IsFailed())
    {
        return result;
    }
    if (!::EmptyClipboard())
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetImage, "清空剪贴板失败。");
    }
    if (::SetClipboardData(CF_DIBV5, dibV5Handle.Get()) == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetImage, "写入 CF_DIBV5 失败。");
    }
    dibV5Handle.Detach();

    if (options.publishCompatibilityDib)
    {
        if (::SetClipboardData(CF_DIB, dibHandle.Get()) == nullptr)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetImage, "写入 CF_DIB 兼容格式失败。");
        }
        dibHandle.Detach();
    }

    result = clipboardScope.Close(GB_ClipboardOperationSetImage);
    if (result.IsFailed())
    {
        return result;
    }
    RememberSelfWriteSequence();
    return GB_SystemResult::Succeeded(GB_ClipboardOperationSetImage);
#else
    (void)image;
    (void)options;
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationSetImage);
#endif
}

GB_SystemResult GB_SystemClipboard::GetFilePaths(std::vector<std::string>& filePaths, const GB_SystemClipboardFileReadOptions& options)
{
#ifdef _WIN32
    GB_ClipboardOpenScope clipboardScope;
    GB_SystemResult result = clipboardScope.Open(options.accessOptions, GB_ClipboardOperationGetFilePaths);
    if (result.IsFailed())
    {
        return result;
    }
    if (!::IsClipboardFormatAvailable(CF_HDROP))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_ClipboardOperationGetFilePaths, "剪贴板中没有 CF_HDROP 文件路径列表。");
    }

    HDROP dropHandle = static_cast<HDROP>(::GetClipboardData(CF_HDROP));
    if (dropHandle == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationGetFilePaths, "获取 CF_HDROP 句柄失败。");
    }

    const UINT fileCount = ::DragQueryFileW(dropHandle, 0xFFFFFFFFU, nullptr, 0);
    if (fileCount > options.maxFileCount)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationGetFilePaths, "剪贴板文件数量超过读取上限。");
    }

    std::vector<std::string> tempFilePaths;
    try
    {
        tempFilePaths.reserve(static_cast<size_t>(fileCount));
    }
    catch (const std::bad_alloc&)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationGetFilePaths, "为剪贴板文件路径列表分配内存失败。");
    }
    size_t totalPathCharacters = 1;
    for (UINT fileIndex = 0; fileIndex < fileCount; fileIndex++)
    {
        const UINT pathLength = ::DragQueryFileW(dropHandle, fileIndex, nullptr, 0);
        if (pathLength == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ParseFailed, GB_ClipboardOperationGetFilePaths, "剪贴板 CF_HDROP 中存在空文件路径。");
        }
        if (pathLength == std::numeric_limits<UINT>::max())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationGetFilePaths, "剪贴板文件路径长度超过 Win32 API 可表达范围。");
        }

        size_t pathCharactersWithNull = 0;
        size_t newTotalPathCharacters = 0;
        if (!CheckedAddSize(static_cast<size_t>(pathLength), 1U, pathCharactersWithNull) || !CheckedAddSize(totalPathCharacters, pathCharactersWithNull, newTotalPathCharacters) || newTotalPathCharacters > options.maxTotalPathCharacters)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationGetFilePaths, "剪贴板文件路径总长度超过读取上限。");
        }
        totalPathCharacters = newTotalPathCharacters;

        std::wstring pathWide;
        try
        {
            pathWide.resize(static_cast<size_t>(pathLength) + 1U);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationGetFilePaths, "为单个剪贴板文件路径分配内存失败。");
        }
        const UINT copiedLength = ::DragQueryFileW(dropHandle, fileIndex, &pathWide[0], pathLength + 1U);
        if (copiedLength != pathLength)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NativeApiFailed, GB_ClipboardOperationGetFilePaths, "读取 CF_HDROP 文件路径失败。");
        }
        pathWide.resize(static_cast<size_t>(pathLength));

        std::string pathUtf8;
        if (!WideToUtf8(pathWide, pathUtf8))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_ClipboardOperationGetFilePaths, "文件路径转换为 UTF-8 失败。");
        }
        try
        {
            tempFilePaths.push_back(pathUtf8);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationGetFilePaths, "保存剪贴板文件路径时内存分配失败。");
        }
    }

    filePaths.swap(tempFilePaths);
    return GB_SystemResult::Succeeded(GB_ClipboardOperationGetFilePaths);
#else
    (void)options;
    filePaths.clear();
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationGetFilePaths);
#endif
}

GB_SystemResult GB_SystemClipboard::SetFilePaths(const std::vector<std::string>& filePaths, const GB_SystemClipboardFileWriteOptions& options)
{
#ifdef _WIN32
    if (filePaths.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetFilePaths, "文件路径列表不能为空；如需清空剪贴板请调用 Clear()。");
    }
    if (filePaths.size() > options.maxFileCount)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetFilePaths, "文件数量超过写入上限。");
    }

    std::vector<std::wstring> pathWideList;
    try
    {
        pathWideList.reserve(filePaths.size());
    }
    catch (const std::bad_alloc&)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationSetFilePaths, "为待写入文件路径列表分配内存失败。");
    }
    size_t totalWideCharacters = 1;
    for (const std::string& filePath : filePaths)
    {
        if (filePath.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetFilePaths, "文件路径不能为空。");
        }
        if (HasEmbeddedNull(filePath))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetFilePaths, "文件路径不能包含嵌入式 NUL 字符。");
        }

        std::wstring pathWide;
        if (!Utf8ToWide(filePath, pathWide))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_ClipboardOperationSetFilePaths, "文件路径不是有效 UTF-8 字符串。");
        }

        size_t pathCharactersWithNull = 0;
        if (!CheckedAddSize(pathWide.size(), 1U, pathCharactersWithNull) || !CheckedAddSize(totalWideCharacters, pathCharactersWithNull, totalWideCharacters) || totalWideCharacters > options.maxTotalPathCharacters)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetFilePaths, "文件路径总长度超过写入上限。");
        }
        try
        {
            pathWideList.push_back(pathWide);
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationSetFilePaths, "保存待写入文件路径时内存分配失败。");
        }
    }

    size_t pathBytes = 0;
    if (!CheckedMultiplySize(totalWideCharacters, sizeof(wchar_t), pathBytes))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetFilePaths, "文件路径数据过大。");
    }
    size_t totalByteSize = 0;
    if (!CheckedAddSize(sizeof(DROPFILES), pathBytes, totalByteSize))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ClipboardOperationSetFilePaths, "CF_HDROP 数据过大。");
    }

    GB_GlobalMemoryHandle memoryHandle(::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalByteSize));
    if (memoryHandle.Get() == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetFilePaths, "为 CF_HDROP 分配全局内存失败。");
    }
    {
        GB_GlobalLockScope lockScope(memoryHandle.Get());
        void* lockedMemory = lockScope.Get();
        if (lockedMemory == nullptr)
        {
            return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetFilePaths, "锁定 CF_HDROP 内存失败。");
        }

        DROPFILES* dropFiles = static_cast<DROPFILES*>(lockedMemory);
        dropFiles->pFiles = sizeof(DROPFILES);
        dropFiles->pt.x = 0;
        dropFiles->pt.y = 0;
        dropFiles->fNC = FALSE;
        dropFiles->fWide = TRUE;

        wchar_t* outputPath = reinterpret_cast<wchar_t*>(static_cast<unsigned char*>(lockedMemory) + sizeof(DROPFILES));
        for (const std::wstring& pathWide : pathWideList)
        {
            std::memcpy(outputPath, pathWide.c_str(), pathWide.size() * sizeof(wchar_t));
            outputPath += pathWide.size();
            *outputPath = L'\0';
            outputPath++;
        }
        *outputPath = L'\0';
    }

    GB_WindowHandleScope ownerWindow(CreateClipboardOwnerWindow());
    if (ownerWindow.Get() == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetFilePaths, "创建剪贴板所有者窗口失败。");
    }

    GB_ClipboardOpenScope clipboardScope;
    GB_SystemResult result = clipboardScope.Open(options.accessOptions, GB_ClipboardOperationSetFilePaths, ownerWindow.Get());
    if (result.IsFailed())
    {
        return result;
    }
    if (!::EmptyClipboard())
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetFilePaths, "清空剪贴板失败。");
    }
    if (::SetClipboardData(CF_HDROP, memoryHandle.Get()) == nullptr)
    {
        return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationSetFilePaths, "写入 CF_HDROP 失败。");
    }
    memoryHandle.Detach();
    result = clipboardScope.Close(GB_ClipboardOperationSetFilePaths);
    if (result.IsFailed())
    {
        return result;
    }
    RememberSelfWriteSequence();
    return GB_SystemResult::Succeeded(GB_ClipboardOperationSetFilePaths);
#else
    (void)filePaths;
    (void)options;
    return MakeUnsupportedPlatformResult(GB_ClipboardOperationSetFilePaths);
#endif
}

std::string GB_SystemClipboard::GetFormatTypeName(GB_SystemClipboardFormatType formatType)
{
    switch (formatType)
    {
    case GB_SystemClipboardFormatType::Unknown:
        return "Unknown";
    case GB_SystemClipboardFormatType::Text:
        return "Text";
    case GB_SystemClipboardFormatType::UnicodeText:
        return "UnicodeText";
    case GB_SystemClipboardFormatType::Bitmap:
        return "Bitmap";
    case GB_SystemClipboardFormatType::Dib:
        return "Dib";
    case GB_SystemClipboardFormatType::DibV5:
        return "DibV5";
    case GB_SystemClipboardFormatType::FilePaths:
        return "FilePaths";
    case GB_SystemClipboardFormatType::Html:
        return "Html";
    case GB_SystemClipboardFormatType::RichText:
        return "RichText";
    case GB_SystemClipboardFormatType::Registered:
        return "Registered";
    case GB_SystemClipboardFormatType::Private:
        return "Private";
    case GB_SystemClipboardFormatType::GdiObject:
        return "GdiObject";
    default:
        return "Unknown";
    }
}

class GB_SystemClipboardWatcher::Impl final
{
public:
    explicit Impl(const GB_SystemClipboardWatcherOptions& inputOptions)
        : options(inputOptions), eventDispatcher(GB_EventDispatcher::MakeQueuedOptions(inputOptions.maxQueueSize, GB_EventQueueOverflowPolicy::DropOldest, "SystemClipboardWatcher"))
    {
    }

    ~Impl() noexcept
    {
        Stop();
    }

    GB_SystemResult Start()
    {
#ifdef _WIN32
        if (options.includeFormats)
        {
            const GB_SystemResult validationResult = ValidateClipboardAccessOptions(options.formatAccessOptions, GB_ClipboardOperationWatcherStart);
            if (validationResult.IsFailed())
            {
                return validationResult;
            }
        }

        std::unique_lock<std::mutex> lock(stateMutex);
        if (running)
        {
            return GB_SystemResult::Succeeded(GB_ClipboardOperationWatcherStart, "剪贴板监听器已经启动。");
        }

        if (watcherThread.joinable() || eventWorkerThread.joinable())
        {
            lock.unlock();
            GB_SystemResult joinPreviousThreadResult = JoinPreviousThreadsBeforeStart();
            if (joinPreviousThreadResult.IsFailed())
            {
                return joinPreviousThreadResult;
            }
            lock.lock();
            if (running)
            {
                return GB_SystemResult::Succeeded(GB_ClipboardOperationWatcherStart, "剪贴板监听器已经启动。");
            }
        }

        createSucceeded = false;
        createCompleted = false;
        createResult = GB_SystemResult::Succeeded(GB_ClipboardOperationWatcherStart);
        stopRequested = false;

        {
            std::lock_guard<std::mutex> queueLock(eventQueueMutex);
            eventWorkerStopRequested = false;
            pendingClipboardEvents.clear();
        }

        GB_SystemResult dispatcherResult = eventDispatcher.Start();
        if (dispatcherResult.IsFailed())
        {
            return dispatcherResult.WithOperationName(GB_ClipboardOperationWatcherStart);
        }

        try
        {
            eventWorkerThread = std::thread(&Impl::EventWorkerMain, this);
            watcherThread = std::thread(&Impl::ThreadMain, this);
        }
        catch (const std::system_error& exception)
        {
            lock.unlock();
            StopEventWorker(false);
            eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationWatcherStart, std::string("创建剪贴板监听线程失败：") + exception.what());
        }
        catch (const std::bad_alloc&)
        {
            lock.unlock();
            StopEventWorker(false);
            eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ClipboardOperationWatcherStart, "创建剪贴板监听线程时内存分配失败。");
        }

        createCondition.wait(lock, [this]() { return createCompleted; });
        if (!createSucceeded)
        {
            lock.unlock();
            if (watcherThread.joinable())
            {
                watcherThread.join();
            }
            StopEventWorker(false);
            eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            return createResult;
        }

        running = true;
        return GB_SystemResult::Succeeded(GB_ClipboardOperationWatcherStart);
#else
        return MakeUnsupportedPlatformResult(GB_ClipboardOperationWatcherStart);
#endif
    }

    GB_SystemResult Stop()
    {
#ifdef _WIN32
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!running && !watcherThread.joinable() && !eventWorkerThread.joinable())
            {
                eventDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
                return GB_SystemResult::Succeeded(GB_ClipboardOperationWatcherStop);
            }

            stopRequested = true;
            bool stopMessagePosted = false;
            if (windowHandle != nullptr)
            {
                stopMessagePosted = ::PostMessageW(windowHandle, WM_APP + 101, 0, 0) != FALSE;
            }
            if (!stopMessagePosted && threadId != 0)
            {
                stopMessagePosted = ::PostThreadMessageW(threadId, WM_QUIT, 0, 0) != FALSE;
            }
            if (!stopMessagePosted && (windowHandle != nullptr || threadId != 0))
            {
                return GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationWatcherStop, "投递剪贴板监听线程停止消息失败。");
            }
        }

        if (watcherThread.joinable() && std::this_thread::get_id() != watcherThread.get_id())
        {
            watcherThread.join();
        }

        StopEventWorker(true);

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            running = false;
            windowHandle = nullptr;
            threadId = 0;
        }
        return eventDispatcher.Stop(GB_EventDispatcherStopMode::Drain).WithOperationName(GB_ClipboardOperationWatcherStop);
#else
        return MakeUnsupportedPlatformResult(GB_ClipboardOperationWatcherStop);
#endif
    }

    bool IsRunning() const
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        return running;
    }

    void SetClipboardEventCallback(const ClipboardEventCallback& inputCallback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        callback = inputCallback;
    }

    GB_EventDispatcher& GetEventDispatcher()
    {
        return eventDispatcher;
    }

private:
#ifdef _WIN32
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            CREATESTRUCTW* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            Impl* impl = createStruct == nullptr ? nullptr : static_cast<Impl*>(createStruct->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
        }

        Impl* impl = reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (impl != nullptr)
        {
            if (message == WM_CLIPBOARDUPDATE)
            {
                impl->QueueClipboardUpdate();
                return 0;
            }
            if (message == WM_APP + 101)
            {
                ::RemoveClipboardFormatListener(hwnd);
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (message == WM_DESTROY)
            {
                ::PostQuitMessage(0);
                return 0;
            }
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void SetThreadId(DWORD inputThreadId)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        threadId = inputThreadId;
    }

    void SignalCreateResult(bool succeeded, const GB_SystemResult& result, HWND hwnd)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        windowHandle = hwnd;
        createSucceeded = succeeded;
        createResult = result;
        createCompleted = true;
        createCondition.notify_all();
    }

    void ThreadMain()
    {
        SetThreadId(::GetCurrentThreadId());

        const wchar_t* className = L"GB_SystemClipboardWatcherWindow";
        WNDCLASSEXW windowClass;
        ::ZeroMemory(&windowClass, sizeof(windowClass));
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Impl::WindowProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className;
        const ATOM classAtom = ::RegisterClassExW(&windowClass);
        if (classAtom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            SignalCreateResult(false, GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationWatcherStart, "注册剪贴板监听窗口类失败。"), nullptr);
            SetThreadId(0);
            return;
        }

        HWND hwnd = ::CreateWindowExW(0, className, L"GB_SystemClipboardWatcher", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, this);
        if (hwnd == nullptr)
        {
            SignalCreateResult(false, GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationWatcherStart, "创建剪贴板监听隐藏窗口失败。"), nullptr);
            SetThreadId(0);
            return;
        }

        if (!::AddClipboardFormatListener(hwnd))
        {
            const GB_SystemResult result = GB_SystemResult::FromLastWin32Error(GB_ClipboardOperationWatcherStart, "注册剪贴板变化监听失败。");
            ::DestroyWindow(hwnd);
            SignalCreateResult(false, result, nullptr);
            SetThreadId(0);
            return;
        }

        SignalCreateResult(true, GB_SystemResult::Succeeded(GB_ClipboardOperationWatcherStart), hwnd);

        MSG message;
        while (::GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        std::lock_guard<std::mutex> lock(stateMutex);
        windowHandle = nullptr;
        running = false;
        threadId = 0;
    }

    void QueueClipboardUpdate()
    {
        GB_SystemClipboardEvent clipboardEvent;
        clipboardEvent.sequenceNumber = ReadClipboardSequenceNumberRaw();
        clipboardEvent.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
        clipboardEvent.isSelfWrite = IsSelfWriteSequence(clipboardEvent.sequenceNumber);

        {
            std::lock_guard<std::mutex> lock(eventQueueMutex);
            if (options.maxQueueSize != 0 && pendingClipboardEvents.size() >= options.maxQueueSize)
            {
                pendingClipboardEvents.pop_front();
            }
            pendingClipboardEvents.push_back(std::move(clipboardEvent));
        }
        eventQueueCondition.notify_one();
    }

    GB_SystemResult JoinPreviousThreadsBeforeStart()
    {
        if (watcherThread.joinable())
        {
            if (std::this_thread::get_id() == watcherThread.get_id())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, GB_ClipboardOperationWatcherStart, "不能在剪贴板监听线程内部重新启动监听器。");
            }
            watcherThread.join();
        }

        if (eventWorkerThread.joinable())
        {
            if (std::this_thread::get_id() == eventWorkerThread.get_id())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, GB_ClipboardOperationWatcherStart, "不能在剪贴板事件回调线程内部重新启动监听器。");
            }
            eventWorkerThread.join();
        }
        return GB_SystemResult::Succeeded(GB_ClipboardOperationWatcherStart);
    }

    void StopEventWorker(bool drainQueuedEvents)
    {
        {
            std::lock_guard<std::mutex> lock(eventQueueMutex);
            eventWorkerStopRequested = true;
            if (!drainQueuedEvents)
            {
                pendingClipboardEvents.clear();
            }
        }
        eventQueueCondition.notify_all();

        if (eventWorkerThread.joinable() && std::this_thread::get_id() != eventWorkerThread.get_id())
        {
            eventWorkerThread.join();
        }
    }

    void EventWorkerMain()
    {
        while (true)
        {
            GB_SystemClipboardEvent clipboardEvent;
            {
                std::unique_lock<std::mutex> lock(eventQueueMutex);
                eventQueueCondition.wait(lock, [this]() { return eventWorkerStopRequested || !pendingClipboardEvents.empty(); });
                if (pendingClipboardEvents.empty())
                {
                    if (eventWorkerStopRequested)
                    {
                        break;
                    }
                    continue;
                }
                clipboardEvent = std::move(pendingClipboardEvents.front());
                pendingClipboardEvents.pop_front();
            }

            CompleteClipboardEvent(clipboardEvent);
            DispatchClipboardEvent(clipboardEvent);
        }
    }

    void CompleteClipboardEvent(GB_SystemClipboardEvent& clipboardEvent)
    {
        if (!options.includeFormats)
        {
            return;
        }

        uint64_t beforeSequenceNumber = 0;
        GB_SystemClipboard::GetSequenceNumber(beforeSequenceNumber);
        std::vector<GB_SystemClipboardFormatInfo> formats;
        GB_SystemResult formatsResult = GB_SystemClipboard::GetFormats(formats, options.formatAccessOptions);
        uint64_t afterSequenceNumber = 0;
        GB_SystemClipboard::GetSequenceNumber(afterSequenceNumber);
        if (formatsResult.IsSucceeded())
        {
            clipboardEvent.formats.swap(formats);
            clipboardEvent.formatsConsistent = beforeSequenceNumber == clipboardEvent.sequenceNumber && afterSequenceNumber == clipboardEvent.sequenceNumber;
        }
    }

    void DispatchClipboardEvent(const GB_SystemClipboardEvent& clipboardEvent)
    {
        ClipboardEventCallback callbackCopy;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callbackCopy = callback;
        }
        if (callbackCopy)
        {
            try
            {
                callbackCopy(clipboardEvent);
            }
            catch (...)
            {
            }
        }

        GB_Event event(clipboardEvent.eventName);
        event.sourceName = "GB_SystemClipboardWatcher";
        event.timestampMilliseconds = clipboardEvent.timestampMilliseconds;
        eventDispatcher.Post(event);
    }
#endif

private:
    GB_SystemClipboardWatcherOptions options;
    GB_EventDispatcher eventDispatcher;
    ClipboardEventCallback callback;
    mutable std::mutex callbackMutex;
    mutable std::mutex stateMutex;
    std::condition_variable createCondition;
    std::thread watcherThread;
    std::thread eventWorkerThread;
    mutable std::mutex eventQueueMutex;
    std::condition_variable eventQueueCondition;
    std::deque<GB_SystemClipboardEvent> pendingClipboardEvents;
    bool running = false;
    bool stopRequested = false;
    bool createSucceeded = false;
    bool createCompleted = false;
    bool eventWorkerStopRequested = false;
    GB_SystemResult createResult;
#ifdef _WIN32
    HWND windowHandle = nullptr;
    DWORD threadId = 0;
#endif
};

GB_SystemClipboardWatcher::GB_SystemClipboardWatcher()
    : impl(new Impl(GB_SystemClipboardWatcherOptions()))
{
}

GB_SystemClipboardWatcher::GB_SystemClipboardWatcher(const GB_SystemClipboardWatcherOptions& options)
    : impl(new Impl(options))
{
}

GB_SystemClipboardWatcher::~GB_SystemClipboardWatcher() noexcept
{
}

GB_SystemResult GB_SystemClipboardWatcher::Start()
{
    return impl->Start();
}

GB_SystemResult GB_SystemClipboardWatcher::Stop()
{
    return impl->Stop();
}

bool GB_SystemClipboardWatcher::IsRunning() const
{
    return impl->IsRunning();
}

void GB_SystemClipboardWatcher::SetClipboardEventCallback(const ClipboardEventCallback& callback)
{
    impl->SetClipboardEventCallback(callback);
}

GB_EventDispatcher& GB_SystemClipboardWatcher::GetEventDispatcher()
{
    return impl->GetEventDispatcher();
}
