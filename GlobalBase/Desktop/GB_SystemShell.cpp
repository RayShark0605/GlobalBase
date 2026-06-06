#include "GB_SystemShell.h"
#include "GB_WindowsCommandLineInternal.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <new>
#include <stdexcept>
#include <cwctype>

#ifdef _WIN32
#  define NOMINMAX
#  include <Windows.h>
#  include <Objbase.h>
#  include <Shellapi.h>
#  include <ShlObj_core.h>
#  pragma comment(lib, "Ole32.lib")
#  pragma comment(lib, "Shell32.lib")
#endif

namespace
{
    static const char* const GB_ShellOperationOpenUrl = "GB_SystemShell::OpenUrl";
    static const char* const GB_ShellOperationOpenFile = "GB_SystemShell::OpenFile";
    static const char* const GB_ShellOperationOpenFolder = "GB_SystemShell::OpenFolder";
    static const char* const GB_ShellOperationExploreFolder = "GB_SystemShell::ExploreFolder";
    static const char* const GB_ShellOperationRevealInExplorer = "GB_SystemShell::RevealInExplorer";
    static const char* const GB_ShellOperationOpenSettings = "GB_SystemShell::OpenSettings";
    static const char* const GB_ShellOperationOpenSettingsPage = "GB_SystemShell::OpenSettingsPage";
    static const char* const GB_ShellOperationOpenSettingsUri = "GB_SystemShell::OpenSettingsUri";
    static const char* const GB_ShellOperationRunApplication = "GB_SystemShell::RunApplication";
    static const char* const GB_ShellOperationRunApplicationAsAdmin = "GB_SystemShell::RunApplicationAsAdmin";
    static const char* const GB_ShellOperationExecute = "GB_SystemShell::Execute";

    static GB_SystemResult MakeUnsupportedPlatformResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, "当前平台不支持 Windows Shell 执行能力。");
    }

    static bool HasEmbeddedNull(const std::string& text)
    {
        return text.find('\0') != std::string::npos;
    }

    static std::string ToAsciiLower(std::string text)
    {
        for (char& character : text)
        {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        return text;
    }

    static bool IsWindowsDrivePathLike(const std::string& text)
    {
        return text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) && text[1] == ':';
    }

    static bool IsExplicitFileSystemPathLike(const std::string& text)
    {
        return IsWindowsDrivePathLike(text) || text.find('\\') != std::string::npos || text.find('/') != std::string::npos;
    }

    static bool IsValidUriSchemeCharacter(char character, bool isFirstCharacter)
    {
        const unsigned char value = static_cast<unsigned char>(character);
        if (isFirstCharacter)
        {
            return std::isalpha(value) != 0;
        }
        return std::isalnum(value) != 0 || character == '+' || character == '-' || character == '.';
    }

    static bool TryGetUriScheme(const std::string& target, std::string& scheme)
    {
        scheme.clear();
        if (IsWindowsDrivePathLike(target))
        {
            return false;
        }

        const size_t colonPosition = target.find(':');
        if (colonPosition == std::string::npos || colonPosition == 0)
        {
            return false;
        }

        for (size_t characterIndex = 0; characterIndex < colonPosition; characterIndex++)
        {
            if (!IsValidUriSchemeCharacter(target[characterIndex], characterIndex == 0))
            {
                return false;
            }
        }

        scheme = ToAsciiLower(target.substr(0, colonPosition));
        return true;
    }

    static bool ContainsUnsafeUriCharacter(const std::string& uri)
    {
        for (const char character : uri)
        {
            const unsigned char value = static_cast<unsigned char>(character);
            if (value <= 0x20 || value == 0x7F)
            {
                return true;
            }
        }
        return false;
    }

    static bool IsSafeExecuteUriScheme(const std::string& scheme)
    {
        return scheme == "http" || scheme == "https" || scheme == "ms-settings";
    }

    static bool IsSafeOpenUrlScheme(const std::string& scheme)
    {
        return scheme == "http" || scheme == "https";
    }

    static bool IsHttpOrHttpsUrlShapeValid(const std::string& uri, const std::string& scheme)
    {
        const std::string prefix = scheme + "://";
        if (uri.size() <= prefix.size())
        {
            return false;
        }
        if (uri.compare(0, prefix.size(), prefix) != 0)
        {
            return false;
        }

        const char firstAuthorityCharacter = uri[prefix.size()];
        return firstAuthorityCharacter != '/' && firstAuthorityCharacter != '?' && firstAuthorityCharacter != '#';
    }

    static GB_SystemResult ValidateUriTarget(const std::string& uri, bool allowMsSettings, const std::string& operationName)
    {
        if (uri.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "URI 不能为空。");
        }
        if (HasEmbeddedNull(uri))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "URI 不能包含嵌入式 NUL 字符。");
        }
        if (ContainsUnsafeUriCharacter(uri))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "URI 不能包含 ASCII 控制字符或未转义空白字符。");
        }

        std::string scheme;
        if (!TryGetUriScheme(uri, scheme))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "URI 缺少有效 scheme。");
        }

        const bool schemeAllowed = IsSafeOpenUrlScheme(scheme) || (allowMsSettings && scheme == "ms-settings");
        if (!schemeAllowed)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "URI scheme 不在当前接口允许范围内。");
        }
        if (IsSafeOpenUrlScheme(scheme) && !IsHttpOrHttpsUrlShapeValid(uri, scheme))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "HTTP/HTTPS URL 必须使用 //authority 形式且 host 不能为空。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

#ifdef _WIN32
    static const size_t GB_MaxWindowsCommandLineCharacters = 32767;

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

    static GB_SystemResult Utf8ToWideResult(const std::string& textUtf8, std::wstring& textWide, const std::string& operationName, const std::string& fieldName)
    {
        if (HasEmbeddedNull(textUtf8))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, fieldName + " 不能包含嵌入式 NUL 字符。");
        }
        if (!Utf8ToWide(textUtf8, textWide))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, operationName, fieldName + " 不是有效 UTF-8 字符串。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    static bool IsDirectoryPath(const std::wstring& path)
    {
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    static bool IsExistingPath(const std::wstring& path)
    {
        return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    static std::wstring GetFileExtensionLower(const std::wstring& path)
    {
        const size_t separatorPosition = path.find_last_of(L"/\\");
        const size_t dotPosition = path.find_last_of(L'.');
        if (dotPosition == std::wstring::npos || (separatorPosition != std::wstring::npos && dotPosition < separatorPosition))
        {
            return std::wstring();
        }

        std::wstring extension = path.substr(dotPosition);
        for (wchar_t& character : extension)
        {
            character = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(character)));
        }
        return extension;
    }

    static bool IsExecutableLikeFilePath(const std::wstring& path)
    {
        const std::wstring extension = GetFileExtensionLower(path);
        return extension == L".exe" || extension == L".com" || extension == L".bat" || extension == L".cmd" || extension == L".ps1" || extension == L".msi" || extension == L".msc" || extension == L".cpl" || extension == L".scr" || extension == L".lnk" || extension == L".appref-ms";
    }

    static GB_SystemErrorCode GetShellExecuteErrorCodeFromHInstApp(UINT_PTR hInstAppValue)
    {
        switch (hInstAppValue)
        {
        case 0:
        case SE_ERR_FNF:
        case SE_ERR_PNF:
            return GB_SystemErrorCode::NotFound;
        case SE_ERR_ACCESSDENIED:
            return GB_SystemErrorCode::PermissionDenied;
        case SE_ERR_OOM:
            return GB_SystemErrorCode::ResourceAllocationFailed;
        case SE_ERR_SHARE:
            return GB_SystemErrorCode::ResourceBusy;
        case SE_ERR_ASSOCINCOMPLETE:
        case SE_ERR_NOASSOC:
            return GB_SystemErrorCode::InvalidState;
        case SE_ERR_DDEBUSY:
            return GB_SystemErrorCode::ResourceBusy;
        case SE_ERR_DDEFAIL:
        case SE_ERR_DDETIMEOUT:
            return GB_SystemErrorCode::NativeApiFailed;
        case SE_ERR_DLLNOTFOUND:
            return GB_SystemErrorCode::NotFound;
        default:
            return GB_SystemErrorCode::NativeApiFailed;
        }
    }

    static GB_SystemResult MakeShellExecuteHInstAppErrorResult(UINT_PTR hInstAppValue, const std::string& message)
    {
        GB_SystemResult result = GB_SystemResult::Failed(GetShellExecuteErrorCodeFromHInstApp(hInstAppValue), GB_ShellOperationExecute, message);
        result.errorSource = GB_NativeErrorSource::Win32;
        result.nativeErrorCode = static_cast<uint64_t>(hInstAppValue);
        result.nativeMessage = "ShellExecuteExW hInstApp failure value.";
        return result;
    }

    static bool IsValidKernelHandle(HANDLE handle)
    {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    }

    static GB_SystemResult ValidateFullCommandLineLength(const std::wstring& targetWide, const std::wstring& parametersWide, const std::string& operationName)
    {
        try
        {
            const size_t targetCommandLength = GB_WindowsCommandLineInternal::QuoteArgument(targetWide).size();
            size_t fullCommandLineLength = targetCommandLength;
            if (!parametersWide.empty())
            {
                if (fullCommandLineLength > std::numeric_limits<size_t>::max() - 1U - parametersWide.size())
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "命令行过长。");
                }
                fullCommandLineLength += 1U + parametersWide.size();
            }
            if (fullCommandLineLength >= GB_MaxWindowsCommandLineCharacters)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "完整命令行超过 Windows 命令行长度上限。");
            }
        }
        catch (const std::length_error&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "命令行过长。");
        }
        catch (const std::bad_alloc&)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, "校验命令行长度时内存分配失败。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult ValidateExistingFilePath(const std::string& pathUtf8, std::wstring& pathWide, const std::string& operationName)
    {
        if (pathUtf8.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "文件路径不能为空。");
        }

        GB_SystemResult result = Utf8ToWideResult(pathUtf8, pathWide, operationName, "文件路径");
        if (result.IsFailed())
        {
            return result;
        }

        const DWORD attributes = ::GetFileAttributesW(pathWide.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD errorCode = ::GetLastError();
            return GB_SystemResult::FromWin32Error(errorCode, operationName, "文件不存在或无法访问。");
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "目标路径是文件夹，不是文件。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult ValidateExistingFolderPath(const std::string& pathUtf8, std::wstring& pathWide, const std::string& operationName)
    {
        if (pathUtf8.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "文件夹路径不能为空。");
        }

        GB_SystemResult result = Utf8ToWideResult(pathUtf8, pathWide, operationName, "文件夹路径");
        if (result.IsFailed())
        {
            return result;
        }

        const DWORD attributes = ::GetFileAttributesW(pathWide.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD errorCode = ::GetLastError();
            return GB_SystemResult::FromWin32Error(errorCode, operationName, "文件夹不存在或无法访问。");
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "目标路径不是文件夹。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    static GB_SystemResult ValidateApplicationTarget(const std::string& applicationPathUtf8, const std::string& operationName)
    {
        if (applicationPathUtf8.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "应用程序路径不能为空。");
        }

        std::wstring applicationPathWide;
        GB_SystemResult result = Utf8ToWideResult(applicationPathUtf8, applicationPathWide, operationName, "应用程序路径");
        if (result.IsFailed())
        {
            return result;
        }

        const DWORD attributes = ::GetFileAttributesW(applicationPathWide.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            if (IsExplicitFileSystemPathLike(applicationPathUtf8))
            {
                const DWORD errorCode = ::GetLastError();
                return GB_SystemResult::FromWin32Error(errorCode, operationName, "应用程序路径不存在或无法访问。");
            }
            return GB_SystemResult::Succeeded(operationName);
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "应用程序路径不能是文件夹。");
        }
        if (!IsExecutableLikeFilePath(applicationPathWide))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, "RunApplication 只接受可执行类文件路径；打开普通文档请使用 OpenFile 或 Execute。");
        }
        return GB_SystemResult::Succeeded(operationName);
    }

    static int ToNativeShowMode(GB_ShellShowMode showMode)
    {
        switch (showMode)
        {
        case GB_ShellShowMode::Hide:
            return SW_HIDE;
        case GB_ShellShowMode::Normal:
            return SW_SHOWNORMAL;
        case GB_ShellShowMode::Minimized:
            return SW_SHOWMINIMIZED;
        case GB_ShellShowMode::Maximized:
            return SW_SHOWMAXIMIZED;
        case GB_ShellShowMode::Default:
        default:
            return SW_SHOWDEFAULT;
        }
    }

    static const wchar_t* ToNativeVerb(GB_ShellExecuteVerb verb)
    {
        switch (verb)
        {
        case GB_ShellExecuteVerb::Open:
            return L"open";
        case GB_ShellExecuteVerb::Explore:
            return L"explore";
        case GB_ShellExecuteVerb::Edit:
            return L"edit";
        case GB_ShellExecuteVerb::Print:
            return L"print";
        case GB_ShellExecuteVerb::RunAs:
            return L"runas";
        case GB_ShellExecuteVerb::Default:
        default:
            return nullptr;
        }
    }

    static GB_SystemResult ValidateExecuteOptions(const GB_ShellExecuteOptions& options)
    {
        if (options.target.empty())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "执行目标不能为空。");
        }
        if (HasEmbeddedNull(options.target))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "执行目标不能包含嵌入式 NUL 字符。");
        }
        if (HasEmbeddedNull(options.workingDirectory))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "工作目录不能包含嵌入式 NUL 字符。");
        }
        if (!GB_SystemShell::IsValidExecuteVerbValue(static_cast<uint64_t>(options.verb)))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "Shell verb 枚举值非法。");
        }
        if (!GB_SystemShell::IsValidShowModeValue(static_cast<uint64_t>(options.showMode)))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "窗口显示方式枚举值非法。");
        }
        if (options.waitTimeoutMilliseconds < -1)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "waitTimeoutMilliseconds 只能为 -1 或非负整数。");
        }
        if (options.waitTimeoutMilliseconds >= static_cast<int64_t>(INFINITE))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "waitTimeoutMilliseconds 的非负值不能等于或超过 Win32 INFINITE；无限等待请使用 -1。");
        }

        for (size_t argumentIndex = 0; argumentIndex < options.arguments.size(); argumentIndex++)
        {
            if (HasEmbeddedNull(options.arguments[argumentIndex]))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "命令行参数不能包含嵌入式 NUL 字符。");
            }
        }

        std::string scheme;
        if (TryGetUriScheme(options.target, scheme))
        {
            if (ContainsUnsafeUriCharacter(options.target))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "URI 不能包含 ASCII 控制字符或未转义空白字符。");
            }
            if (!options.arguments.empty())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "URI 目标不支持附加命令行参数，请把参数编码到 URI 自身。");
            }
            if (options.verb == GB_ShellExecuteVerb::RunAs)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "URI 目标不支持按 runas 语义启动。");
            }
            if (!options.allowUnsafeUriScheme && !IsSafeExecuteUriScheme(scheme))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "Execute 默认只允许 http、https 和 ms-settings URI。");
            }
            if ((scheme == "http" || scheme == "https") && !IsHttpOrHttpsUrlShapeValid(options.target, scheme))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "HTTP/HTTPS URL 必须使用 //authority 形式且 host 不能为空。");
            }
        }
        return GB_SystemResult::Succeeded(GB_ShellOperationExecute);
    }

    class GB_KernelHandleScope final
    {
    public:
        GB_KernelHandleScope() = default;
        explicit GB_KernelHandleScope(HANDLE inputHandle) : handle(inputHandle) {}

        ~GB_KernelHandleScope()
        {
            Reset(nullptr);
        }

        GB_KernelHandleScope(const GB_KernelHandleScope&) = delete;
        GB_KernelHandleScope& operator=(const GB_KernelHandleScope&) = delete;

        HANDLE Get() const
        {
            return handle;
        }

        void Reset(HANDLE newHandle)
        {
            if (IsValidKernelHandle(handle))
            {
                ::CloseHandle(handle);
            }
            handle = newHandle;
        }

    private:
        HANDLE handle = nullptr;
    };

    class GB_ShellComScope final
    {
    public:
        GB_ShellComScope()
        {
            hresult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            shouldUninitialize = hresult == S_OK || hresult == S_FALSE;
        }

        ~GB_ShellComScope()
        {
            if (shouldUninitialize)
            {
                ::CoUninitialize();
                shouldUninitialize = false;
            }
        }

        GB_ShellComScope(const GB_ShellComScope&) = delete;
        GB_ShellComScope& operator=(const GB_ShellComScope&) = delete;

        HRESULT GetHResult() const
        {
            return hresult;
        }

        bool IsComUsable() const
        {
            return hresult == S_OK || hresult == S_FALSE || hresult == RPC_E_CHANGED_MODE;
        }

    private:
        HRESULT hresult = S_OK;
        bool shouldUninitialize = false;
    };

    class GB_CoTaskMemScope final
    {
    public:
        GB_CoTaskMemScope() = default;
        explicit GB_CoTaskMemScope(void* inputPointer) : pointer(inputPointer) {}

        ~GB_CoTaskMemScope()
        {
            Reset(nullptr);
        }

        GB_CoTaskMemScope(const GB_CoTaskMemScope&) = delete;
        GB_CoTaskMemScope& operator=(const GB_CoTaskMemScope&) = delete;

        void* Get() const
        {
            return pointer;
        }

        void Reset(void* newPointer)
        {
            if (pointer != nullptr)
            {
                ::CoTaskMemFree(pointer);
            }
            pointer = newPointer;
        }

    private:
        void* pointer = nullptr;
    };

    static GB_SystemResult RevealPathWithShellApi(const std::wstring& pathWide)
    {
        GB_ShellComScope comScope;
        if (!comScope.IsComUsable())
        {
            return GB_SystemResult::FromHResult(static_cast<int32_t>(comScope.GetHResult()), GB_ShellOperationRevealInExplorer, "初始化 COM 失败，无法调用 SHOpenFolderAndSelectItems。");
        }

        PIDLIST_ABSOLUTE itemIdList = nullptr;
        SFGAOF attributes = 0;
        const HRESULT parseResult = ::SHParseDisplayName(pathWide.c_str(), nullptr, &itemIdList, 0, &attributes);
        GB_CoTaskMemScope itemIdListScope(itemIdList);
        if (FAILED(parseResult) || itemIdList == nullptr)
        {
            return GB_SystemResult::FromHResult(static_cast<int32_t>(parseResult), GB_ShellOperationRevealInExplorer, "解析 Shell 路径 PIDL 失败。");
        }

        const HRESULT openResult = ::SHOpenFolderAndSelectItems(itemIdList, 0, nullptr, 0);
        if (FAILED(openResult))
        {
            return GB_SystemResult::FromHResult(static_cast<int32_t>(openResult), GB_ShellOperationRevealInExplorer, "打开资源管理器并选中目标失败。");
        }

        return GB_SystemResult::Succeeded(GB_ShellOperationRevealInExplorer);
    }
#endif
}

GB_SystemResult GB_SystemShell::OpenUrl(const std::string& url)
{
#ifdef _WIN32
    GB_SystemResult result = ValidateUriTarget(url, false, GB_ShellOperationOpenUrl);
    if (result.IsFailed())
    {
        return result;
    }

    GB_ShellExecuteOptions options;
    options.target = url;
    options.verb = GB_ShellExecuteVerb::Open;
    options.showMode = GB_ShellShowMode::Default;
    return Execute(options, nullptr).WithOperationName(GB_ShellOperationOpenUrl);
#else
    (void)url;
    return MakeUnsupportedPlatformResult(GB_ShellOperationOpenUrl);
#endif
}

GB_SystemResult GB_SystemShell::OpenFile(const std::string& filePath)
{
#ifdef _WIN32
    std::wstring filePathWide;
    GB_SystemResult result = ValidateExistingFilePath(filePath, filePathWide, GB_ShellOperationOpenFile);
    if (result.IsFailed())
    {
        return result;
    }

    GB_ShellExecuteOptions options;
    options.target = filePath;
    options.verb = GB_ShellExecuteVerb::Open;
    options.showMode = GB_ShellShowMode::Default;
    return Execute(options, nullptr).WithOperationName(GB_ShellOperationOpenFile);
#else
    (void)filePath;
    return MakeUnsupportedPlatformResult(GB_ShellOperationOpenFile);
#endif
}

GB_SystemResult GB_SystemShell::OpenFolder(const std::string& folderPath)
{
#ifdef _WIN32
    std::wstring folderPathWide;
    GB_SystemResult result = ValidateExistingFolderPath(folderPath, folderPathWide, GB_ShellOperationOpenFolder);
    if (result.IsFailed())
    {
        return result;
    }

    GB_ShellExecuteOptions options;
    options.target = folderPath;
    options.verb = GB_ShellExecuteVerb::Open;
    options.showMode = GB_ShellShowMode::Default;
    return Execute(options, nullptr).WithOperationName(GB_ShellOperationOpenFolder);
#else
    (void)folderPath;
    return MakeUnsupportedPlatformResult(GB_ShellOperationOpenFolder);
#endif
}

GB_SystemResult GB_SystemShell::ExploreFolder(const std::string& folderPath)
{
#ifdef _WIN32
    std::wstring folderPathWide;
    GB_SystemResult result = ValidateExistingFolderPath(folderPath, folderPathWide, GB_ShellOperationExploreFolder);
    if (result.IsFailed())
    {
        return result;
    }

    GB_ShellExecuteOptions options;
    options.target = folderPath;
    options.verb = GB_ShellExecuteVerb::Explore;
    options.showMode = GB_ShellShowMode::Default;
    return Execute(options, nullptr).WithOperationName(GB_ShellOperationExploreFolder);
#else
    (void)folderPath;
    return MakeUnsupportedPlatformResult(GB_ShellOperationExploreFolder);
#endif
}

GB_SystemResult GB_SystemShell::RevealInExplorer(const std::string& path)
{
#ifdef _WIN32
    if (path.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationRevealInExplorer, "路径不能为空。");
    }

    std::wstring pathWide;
    GB_SystemResult result = Utf8ToWideResult(path, pathWide, GB_ShellOperationRevealInExplorer, "路径");
    if (result.IsFailed())
    {
        return result;
    }
    if (!IsExistingPath(pathWide))
    {
        const DWORD errorCode = ::GetLastError();
        return GB_SystemResult::FromWin32Error(errorCode, GB_ShellOperationRevealInExplorer, "路径不存在或无法访问。");
    }

    return RevealPathWithShellApi(pathWide);
#else
    (void)path;
    return MakeUnsupportedPlatformResult(GB_ShellOperationRevealInExplorer);
#endif
}

GB_SystemResult GB_SystemShell::OpenSettings()
{
    return OpenSettingsUri("ms-settings:").WithOperationName(GB_ShellOperationOpenSettings);
}

GB_SystemResult GB_SystemShell::OpenSettingsPage(GB_SystemSettingsPage settingsPage)
{
    if (!IsValidSettingsPageValue(static_cast<uint64_t>(settingsPage)))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationOpenSettingsPage, "Windows 设置页枚举值非法。");
    }
    const std::string settingsUri = GetSettingsPageUri(settingsPage);
    if (settingsUri.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationOpenSettingsPage, "Windows 设置页未映射到有效 URI。");
    }
    return OpenSettingsUri(settingsUri).WithOperationName(GB_ShellOperationOpenSettingsPage);
}

GB_SystemResult GB_SystemShell::OpenSettingsUri(const std::string& settingsUri)
{
#ifdef _WIN32
    GB_SystemResult result = ValidateUriTarget(settingsUri, true, GB_ShellOperationOpenSettingsUri);
    if (result.IsFailed())
    {
        return result;
    }

    std::string scheme;
    if (!TryGetUriScheme(settingsUri, scheme) || scheme != "ms-settings")
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationOpenSettingsUri, "Windows 设置 URI 必须使用 ms-settings scheme。");
    }

    GB_ShellExecuteOptions options;
    options.target = settingsUri;
    options.verb = GB_ShellExecuteVerb::Open;
    options.showMode = GB_ShellShowMode::Default;
    return Execute(options, nullptr).WithOperationName(GB_ShellOperationOpenSettingsUri);
#else
    (void)settingsUri;
    return MakeUnsupportedPlatformResult(GB_ShellOperationOpenSettingsUri);
#endif
}

GB_SystemResult GB_SystemShell::RunApplication(const std::string& applicationPath, const std::vector<std::string>& arguments)
{
#ifdef _WIN32
    if (applicationPath.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationRunApplication, "应用程序路径不能为空。");
    }
    if (HasEmbeddedNull(applicationPath))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationRunApplication, "应用程序路径不能包含嵌入式 NUL 字符。");
    }
    std::string scheme;
    if (TryGetUriScheme(applicationPath, scheme))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationRunApplication, "RunApplication 只接受应用程序路径或可执行文件名，URI 请使用 OpenUrl 或 Execute。");
    }
    GB_SystemResult validationResult = ValidateApplicationTarget(applicationPath, GB_ShellOperationRunApplication);
    if (validationResult.IsFailed())
    {
        return validationResult;
    }

    GB_ShellExecuteOptions options;
    options.target = applicationPath;
    options.arguments = arguments;
    options.verb = GB_ShellExecuteVerb::Open;
    options.showMode = GB_ShellShowMode::Default;
    return Execute(options, nullptr).WithOperationName(GB_ShellOperationRunApplication);
#else
    (void)applicationPath;
    (void)arguments;
    return MakeUnsupportedPlatformResult(GB_ShellOperationRunApplication);
#endif
}

GB_SystemResult GB_SystemShell::RunApplicationAsAdmin(const std::string& applicationPath, const std::vector<std::string>& arguments)
{
#ifdef _WIN32
    if (applicationPath.empty())
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationRunApplicationAsAdmin, "应用程序路径不能为空。");
    }
    if (HasEmbeddedNull(applicationPath))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationRunApplicationAsAdmin, "应用程序路径不能包含嵌入式 NUL 字符。");
    }
    std::string scheme;
    if (TryGetUriScheme(applicationPath, scheme))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationRunApplicationAsAdmin, "RunApplicationAsAdmin 只接受应用程序路径或可执行文件名，URI 不能按 runas 语义启动。");
    }
    GB_SystemResult validationResult = ValidateApplicationTarget(applicationPath, GB_ShellOperationRunApplicationAsAdmin);
    if (validationResult.IsFailed())
    {
        return validationResult;
    }

    GB_ShellExecuteOptions options;
    options.target = applicationPath;
    options.arguments = arguments;
    options.verb = GB_ShellExecuteVerb::RunAs;
    options.showMode = GB_ShellShowMode::Default;
    return Execute(options, nullptr).WithOperationName(GB_ShellOperationRunApplicationAsAdmin);
#else
    (void)applicationPath;
    (void)arguments;
    return MakeUnsupportedPlatformResult(GB_ShellOperationRunApplicationAsAdmin);
#endif
}

GB_SystemResult GB_SystemShell::Execute(const GB_ShellExecuteOptions& options, GB_ShellExecuteResult* executeResult)
{
#ifdef _WIN32
    if (executeResult != nullptr)
    {
        *executeResult = GB_ShellExecuteResult();
    }

    GB_SystemResult validationResult = ValidateExecuteOptions(options);
    if (validationResult.IsFailed())
    {
        return validationResult;
    }

    std::wstring targetWide;
    GB_SystemResult conversionResult = Utf8ToWideResult(options.target, targetWide, GB_ShellOperationExecute, "执行目标");
    if (conversionResult.IsFailed())
    {
        return conversionResult;
    }

    std::wstring workingDirectoryWide;
    if (!options.workingDirectory.empty())
    {
        conversionResult = Utf8ToWideResult(options.workingDirectory, workingDirectoryWide, GB_ShellOperationExecute, "工作目录");
        if (conversionResult.IsFailed())
        {
            return conversionResult;
        }
        if (!IsDirectoryPath(workingDirectoryWide))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_ShellOperationExecute, "工作目录不存在或不是文件夹。");
        }
    }

    std::string targetScheme;
    const bool targetIsUri = TryGetUriScheme(options.target, targetScheme);
    const DWORD targetAttributes = targetIsUri ? INVALID_FILE_ATTRIBUTES : ::GetFileAttributesW(targetWide.c_str());
    const bool targetExistsAsFileSystemPath = targetAttributes != INVALID_FILE_ATTRIBUTES;
    const bool targetIsDirectory = targetExistsAsFileSystemPath && (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!options.arguments.empty() && targetExistsAsFileSystemPath && (targetIsDirectory || !IsExecutableLikeFilePath(targetWide)))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "ShellExecuteExW 只有在目标是可执行类文件时才允许传入 lpParameters；文件夹或普通文档请不要附加 arguments。");
    }

    std::vector<std::wstring> argumentWideList;
    try
    {
        argumentWideList.reserve(options.arguments.size());
        for (size_t argumentIndex = 0; argumentIndex < options.arguments.size(); argumentIndex++)
        {
            std::wstring argumentWide;
            conversionResult = Utf8ToWideResult(options.arguments[argumentIndex], argumentWide, GB_ShellOperationExecute, "命令行参数");
            if (conversionResult.IsFailed())
            {
                return conversionResult;
            }
            argumentWideList.push_back(argumentWide);
        }
    }
    catch (const std::bad_alloc&)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ShellOperationExecute, "转换命令行参数时内存分配失败。");
    }

    std::wstring parametersWide;
    try
    {
        parametersWide = GB_WindowsCommandLineInternal::BuildParameters(argumentWideList);
    }
    catch (const std::length_error&)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "命令行参数过长。");
    }
    catch (const std::bad_alloc&)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_ShellOperationExecute, "构造命令行参数时内存分配失败。");
    }
    if (parametersWide.size() >= GB_MaxWindowsCommandLineCharacters)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_ShellOperationExecute, "命令行参数超过 Windows 命令行长度上限。");
    }

    GB_SystemResult commandLineLengthResult = ValidateFullCommandLineLength(targetWide, parametersWide, GB_ShellOperationExecute);
    if (commandLineLengthResult.IsFailed())
    {
        return commandLineLengthResult;
    }

    GB_ShellComScope comScope;
    if (!comScope.IsComUsable())
    {
        return GB_SystemResult::FromHResult(static_cast<int32_t>(comScope.GetHResult()), GB_ShellOperationExecute, "初始化 COM 失败，无法安全调用 ShellExecuteExW。");
    }

    SHELLEXECUTEINFOW shellExecuteInfo;
    ::ZeroMemory(&shellExecuteInfo, sizeof(shellExecuteInfo));
    shellExecuteInfo.cbSize = sizeof(shellExecuteInfo);
    shellExecuteInfo.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC | SEE_MASK_UNICODE;
    if (options.waitForExit || executeResult != nullptr)
    {
        shellExecuteInfo.fMask |= SEE_MASK_NOCLOSEPROCESS;
    }
    shellExecuteInfo.hwnd = nullptr;
    shellExecuteInfo.lpVerb = ToNativeVerb(options.verb);
    shellExecuteInfo.lpFile = targetWide.c_str();
    shellExecuteInfo.lpParameters = parametersWide.empty() ? nullptr : parametersWide.c_str();
    shellExecuteInfo.lpDirectory = workingDirectoryWide.empty() ? nullptr : workingDirectoryWide.c_str();
    shellExecuteInfo.nShow = ToNativeShowMode(options.showMode);

    if (!::ShellExecuteExW(&shellExecuteInfo))
    {
        const DWORD errorCode = ::GetLastError();
        if (errorCode == ERROR_CANCELLED)
        {
            return GB_SystemResult::FromWin32Error(errorCode, GB_ShellOperationExecute, "Shell 执行被用户或系统取消。");
        }
        if (errorCode != ERROR_SUCCESS)
        {
            return GB_SystemResult::FromWin32Error(errorCode, GB_ShellOperationExecute, "ShellExecuteExW 调用失败。");
        }

        const UINT_PTR hInstAppValue = reinterpret_cast<UINT_PTR>(shellExecuteInfo.hInstApp);
        if (hInstAppValue <= 32)
        {
            return MakeShellExecuteHInstAppErrorResult(hInstAppValue, "ShellExecuteExW 调用失败，且 GetLastError 未提供有效错误码。");
        }
        return GB_SystemResult::Failed(GB_SystemErrorCode::NativeApiFailed, GB_ShellOperationExecute, "ShellExecuteExW 调用失败，且无法取得有效原生错误码。");
    }

    const UINT_PTR successHInstAppValue = reinterpret_cast<UINT_PTR>(shellExecuteInfo.hInstApp);
    if (successHInstAppValue <= 32)
    {
        return MakeShellExecuteHInstAppErrorResult(successHInstAppValue, "ShellExecuteExW 返回成功，但 hInstApp 未返回有效成功值。");
    }

    GB_KernelHandleScope processHandleScope(shellExecuteInfo.hProcess);
    HANDLE processHandle = processHandleScope.Get();
    if (executeResult != nullptr && IsValidKernelHandle(processHandle))
    {
        executeResult->processHandleReturned = true;
        const DWORD processId = ::GetProcessId(processHandle);
        if (processId != 0)
        {
            executeResult->processId = static_cast<uint32_t>(processId);
            executeResult->hasProcessId = true;
        }
    }

    if (options.waitForExit)
    {
        if (!IsValidKernelHandle(processHandle))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, GB_ShellOperationExecute, "已请求等待进程退出，但 ShellExecuteExW 未返回可等待的进程句柄。");
        }

        const DWORD timeoutMilliseconds = options.waitTimeoutMilliseconds < 0 ? INFINITE : static_cast<DWORD>(options.waitTimeoutMilliseconds);
        const DWORD waitResult = ::WaitForSingleObject(processHandle, timeoutMilliseconds);
        if (waitResult == WAIT_OBJECT_0)
        {
            if (executeResult != nullptr)
            {
                executeResult->waitCompleted = true;
                DWORD exitCode = 0;
                if (!::GetExitCodeProcess(processHandle, &exitCode))
                {
                    return GB_SystemResult::FromLastWin32Error(GB_ShellOperationExecute, "获取 Shell 启动进程的退出码失败。");
                }
                executeResult->exitCode = static_cast<uint32_t>(exitCode);
                executeResult->hasExitCode = true;
            }
        }
        else if (waitResult == WAIT_TIMEOUT)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, GB_ShellOperationExecute, "等待 Shell 启动的进程退出超时。");
        }
        else
        {
            const DWORD errorCode = ::GetLastError();
            return GB_SystemResult::FromWin32Error(errorCode, GB_ShellOperationExecute, "等待 Shell 启动的进程退出失败。");
        }
    }
    return GB_SystemResult::Succeeded(GB_ShellOperationExecute);
#else
    (void)options;
    if (executeResult != nullptr)
    {
        *executeResult = GB_ShellExecuteResult();
    }
    return MakeUnsupportedPlatformResult(GB_ShellOperationExecute);
#endif
}

bool GB_SystemShell::IsValidShowModeValue(uint64_t showModeValue)
{
    return showModeValue <= static_cast<uint64_t>(GB_ShellShowMode::Maximized);
}

bool GB_SystemShell::IsValidExecuteVerbValue(uint64_t executeVerbValue)
{
    return executeVerbValue <= static_cast<uint64_t>(GB_ShellExecuteVerb::RunAs);
}

bool GB_SystemShell::IsValidSettingsPageValue(uint64_t settingsPageValue)
{
    return settingsPageValue <= static_cast<uint64_t>(GB_SystemSettingsPage::PrivacyCamera);
}

std::string GB_SystemShell::GetSettingsPageUri(GB_SystemSettingsPage settingsPage)
{
    switch (settingsPage)
    {
    case GB_SystemSettingsPage::Home:
        return "ms-settings:";
    case GB_SystemSettingsPage::Display:
        return "ms-settings:display";
    case GB_SystemSettingsPage::Sound:
        return "ms-settings:sound";
    case GB_SystemSettingsPage::Bluetooth:
        return "ms-settings:bluetooth";
    case GB_SystemSettingsPage::Wifi:
        return "ms-settings:network-wifi";
    case GB_SystemSettingsPage::Network:
        return "ms-settings:network";
    case GB_SystemSettingsPage::DefaultApps:
        return "ms-settings:defaultapps";
    case GB_SystemSettingsPage::AppsFeatures:
        return "ms-settings:appsfeatures";
    case GB_SystemSettingsPage::Clipboard:
        return "ms-settings:clipboard";
    case GB_SystemSettingsPage::PowerSleep:
        return "ms-settings:powersleep";
    case GB_SystemSettingsPage::PrivacyMicrophone:
        return "ms-settings:privacy-microphone";
    case GB_SystemSettingsPage::PrivacyCamera:
        return "ms-settings:privacy-webcam";
    default:
        return "";
    }
}
