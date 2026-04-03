#include "GB_DelayLoadRuntime.h"

#include <windows.h>
#include <delayimp.h>

#include <mutex>
#include <string>
#include <vector>

namespace
{
    HMODULE globalBaseModuleHandle = nullptr;

    std::once_flag runtimeInitOnce;
    bool runtimeInitialized = false;
    bool runtimeInitializeSucceeded = false;

    LONG messageBoxShownFlag = 0;

    bool EqualsIgnoreCaseAscii(const char* leftText, const char* rightText)
    {
        if (leftText == nullptr || rightText == nullptr)
        {
            return false;
        }

        while (*leftText != '\0' && *rightText != '\0')
        {
            char leftChar = *leftText;
            char rightChar = *rightText;

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

            leftText++;
            rightText++;
        }

        return *leftText == '\0' && *rightText == '\0';
    }

    bool EqualsIgnoreCaseAscii(const wchar_t* leftText, const wchar_t* rightText)
    {
        if (leftText == nullptr || rightText == nullptr)
        {
            return false;
        }

        while (*leftText != L'\0' && *rightText != L'\0')
        {
            wchar_t leftChar = *leftText;
            wchar_t rightChar = *rightText;

            if (leftChar >= L'A' && leftChar <= L'Z')
            {
                leftChar = static_cast<wchar_t>(leftChar - L'A' + L'a');
            }

            if (rightChar >= L'A' && rightChar <= L'Z')
            {
                rightChar = static_cast<wchar_t>(rightChar - L'A' + L'a');
            }

            if (leftChar != rightChar)
            {
                return false;
            }

            leftText++;
            rightText++;
        }

        return *leftText == L'\0' && *rightText == L'\0';
    }

    std::wstring Utf8OrAnsiToWide(const char* text)
    {
        if (text == nullptr || *text == '\0')
        {
            return L"";
        }

        const int utf8Length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (utf8Length > 0)
        {
            std::wstring wideText;
            wideText.resize(static_cast<size_t>(utf8Length - 1));
            MultiByteToWideChar(CP_UTF8, 0, text, -1, &wideText[0], utf8Length);
            return wideText;
        }

        const int ansiLength = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
        if (ansiLength > 0)
        {
            std::wstring wideText;
            wideText.resize(static_cast<size_t>(ansiLength - 1));
            MultiByteToWideChar(CP_ACP, 0, text, -1, &wideText[0], ansiLength);
            return wideText;
        }

        return L"";
    }

    std::wstring JoinPath(const std::wstring& leftPath, const std::wstring& rightPath)
    {
        if (leftPath.empty())
        {
            return rightPath;
        }

        if (rightPath.empty())
        {
            return leftPath;
        }

        if (leftPath.back() == L'\\' || leftPath.back() == L'/')
        {
            return leftPath + rightPath;
        }

        return leftPath + L"\\" + rightPath;
    }

    std::wstring GetDirectoryFromFilePath(const std::wstring& filePath)
    {
        const std::wstring::size_type pos = filePath.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
        {
            return L"";
        }

        return filePath.substr(0, pos);
    }

    const std::wstring& GetRuntimeDisplayName()
    {
#ifdef _DEBUG
        static const std::wstring runtimeDisplayName = L"GlobalBased";
#else
        static const std::wstring runtimeDisplayName = L"GlobalBase";
#endif
        return runtimeDisplayName;
    }

    std::wstring GetRuntimeFailureMessageBoxCaption()
    {
        return GetRuntimeDisplayName() + L" 运行时依赖加载失败";
    }

    const std::vector<std::wstring>& GetManagedDelayLoadDllNames()
    {
#ifdef _DEBUG
        static const std::vector<std::wstring> dllNames =
        {
            L"zlibd1.dll",
            L"libcurl-d.dll",
            L"libcrypto-3-x64.dll"
        };
#else
        static const std::vector<std::wstring> dllNames =
        {
            L"zlib1.dll",
            L"libcurl.dll",
            L"libcrypto-3-x64.dll"
        };
#endif
        return dllNames;
    }

    std::wstring GetGlobalBaseDirectory()
    {
        if (globalBaseModuleHandle == nullptr)
        {
            return L"";
        }

        std::vector<wchar_t> filePathBuffer(MAX_PATH, L'\0');

        while (true)
        {
            const DWORD copiedLength = GetModuleFileNameW(globalBaseModuleHandle, filePathBuffer.data(), static_cast<DWORD>(filePathBuffer.size()));

            if (copiedLength == 0)
            {
                return L"";
            }

            if (copiedLength < filePathBuffer.size() - 1)
            {
                const std::wstring filePath(filePathBuffer.data(), copiedLength);
                return GetDirectoryFromFilePath(filePath);
            }

            filePathBuffer.resize(filePathBuffer.size() * 2, L'\0');
        }
    }

    bool IsManagedDelayLoadDll(const char* dllName)
    {
        if (dllName == nullptr || *dllName == '\0')
        {
            return false;
        }

        const std::wstring dllNameWide = Utf8OrAnsiToWide(dllName);
        if (dllNameWide.empty())
        {
            return false;
        }

        const std::vector<std::wstring>& managedDelayLoadDllNames = GetManagedDelayLoadDllNames();
        for (size_t i = 0; i < managedDelayLoadDllNames.size(); i++)
        {
            if (EqualsIgnoreCaseAscii(dllNameWide.c_str(), managedDelayLoadDllNames[i].c_str()))
            {
                return true;
            }
        }

        return false;
    }

    HMODULE LoadDependencyByPreferredRule(const wchar_t* dllName)
    {
        if (dllName == nullptr || *dllName == L'\0')
        {
            return nullptr;
        }

        const std::wstring globalBaseDirectory = GetGlobalBaseDirectory();
        if (globalBaseDirectory.empty())
        {
            return nullptr;
        }

        const std::wstring dependencyDirectory = JoinPath(globalBaseDirectory, L"GlobalBaseDependencies");

        const std::wstring preferredPath = JoinPath(dependencyDirectory, dllName);

        HMODULE moduleHandle = LoadLibraryExW(preferredPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

        if (moduleHandle != nullptr)
        {
            return moduleHandle;
        }

        const std::wstring fallbackPath = JoinPath(globalBaseDirectory, dllName);

        moduleHandle = LoadLibraryExW(fallbackPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

        if (moduleHandle != nullptr)
        {
            return moduleHandle;
        }

        return nullptr;
    }

    HMODULE LoadDependencyByPreferredRule(const char* dllName)
    {
        return LoadDependencyByPreferredRule(Utf8OrAnsiToWide(dllName).c_str());
    }

    void ShowMessageBoxOnce(const std::wstring& messageText)
    {
        if (InterlockedCompareExchange(&messageBoxShownFlag, 1, 0) != 0)
        {
            return;
        }

        MessageBoxW(nullptr, messageText.c_str(), GetRuntimeFailureMessageBoxCaption().c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST);
    }

    bool PreloadManagedDependencies(std::wstring* failedDllList)
    {
        std::vector<std::wstring> failedNames;

        const std::vector<std::wstring>& dllNames = GetManagedDelayLoadDllNames();
        for (size_t i = 0; i < dllNames.size(); i++)
        {
            const std::wstring& dllName = dllNames[i];

            const HMODULE moduleHandle = LoadDependencyByPreferredRule(dllName.c_str());
            if (moduleHandle == nullptr)
            {
                failedNames.push_back(dllName);
            }
        }

        if (!failedNames.empty())
        {
            if (failedDllList != nullptr)
            {
                failedDllList->clear();

                for (size_t i = 0; i < failedNames.size(); i++)
                {
                    if (i > 0)
                    {
                        *failedDllList += L"\r\n";
                    }

                    *failedDllList += failedNames[i];
                }
            }

            return false;
        }

        return true;
    }

    void EnsureRuntimeInitializedInternal()
    {
        std::call_once(runtimeInitOnce, []() {
            runtimeInitialized = true;

            std::wstring failedDllList;
            runtimeInitializeSucceeded = PreloadManagedDependencies(&failedDllList);

            if (!runtimeInitializeSucceeded)
            {
                const std::wstring& runtimeDisplayName = GetRuntimeDisplayName();

                std::wstring messageText =
                    runtimeDisplayName +
                    L" 无法加载以下关键运行时依赖：\r\n\r\n" +
                    failedDllList +
                    L"\r\n\r\n"
                    L"加载顺序已按以下规则尝试：\r\n"
                    L"1. GlobalBaseDependencies 子目录\r\n"
                    L"2. 当前模块所在目录\r\n\r\n"
                    L"请检查部署目录、位数是否一致，以及依赖 DLL 自身的下级依赖是否齐全。";

                ShowMessageBoxOnce(messageText);
            }
        });
    }

    FARPROC WINAPI DelayLoadNotifyHook(unsigned notification, PDelayLoadInfo delayLoadInfo)
    {
        if (delayLoadInfo == nullptr)
        {
            return nullptr;
        }

        if (notification == dliNotePreLoadLibrary)
        {
            if (!IsManagedDelayLoadDll(delayLoadInfo->szDll))
            {
                return nullptr;
            }

            const HMODULE moduleHandle = LoadDependencyByPreferredRule(delayLoadInfo->szDll);
            if (moduleHandle != nullptr)
            {
                return reinterpret_cast<FARPROC>(moduleHandle);
            }
        }

        return nullptr;
    }

    FARPROC WINAPI DelayLoadFailureHook(unsigned notification, PDelayLoadInfo delayLoadInfo)
    {
        if (delayLoadInfo == nullptr)
        {
            return nullptr;
        }

        if (!IsManagedDelayLoadDll(delayLoadInfo->szDll))
        {
            return nullptr;
        }

        if (notification == dliFailLoadLib)
        {
            const std::wstring& runtimeDisplayName = GetRuntimeDisplayName();
            std::wstring dllName = Utf8OrAnsiToWide(delayLoadInfo->szDll);

            std::wstring messageText =
                runtimeDisplayName +
                L" 在延迟加载阶段仍然无法加载依赖 DLL：\r\n\r\n" +
                dllName +
                L"\r\n\r\n"
                L"已尝试以下位置：\r\n"
                L"1. GlobalBaseDependencies 子目录\r\n"
                L"2. 当前模块所在目录";

            ShowMessageBoxOnce(messageText);
            return nullptr;
        }

        if (notification == dliFailGetProc)
        {
            const std::wstring& runtimeDisplayName = GetRuntimeDisplayName();
            std::wstring dllName = Utf8OrAnsiToWide(delayLoadInfo->szDll);
            std::wstring functionName;

            if (delayLoadInfo->dlp.fImportByName && delayLoadInfo->dlp.szProcName != nullptr)
            {
                functionName = Utf8OrAnsiToWide(delayLoadInfo->dlp.szProcName);
            }
            else
            {
                functionName = L"(ordinal import)";
            }

            std::wstring messageText =
                runtimeDisplayName +
                L" 已加载 DLL，但无法解析导入符号：\r\n\r\nDLL: " +
                dllName +
                L"\r\n符号: " +
                functionName +
                L"\r\n\r\n这通常表示 DLL 版本不匹配。";

            ShowMessageBoxOnce(messageText);
            return nullptr;
        }

        return nullptr;
    }
}

ExternC const PfnDliHook __pfnDliNotifyHook2 = DelayLoadNotifyHook;
ExternC const PfnDliHook __pfnDliFailureHook2 = DelayLoadFailureHook;

void GB_SetSelfModuleHandle(HMODULE moduleHandle)
{
    globalBaseModuleHandle = moduleHandle;
}

bool GB_InitializeRuntime()
{
    EnsureRuntimeInitializedInternal();
    return runtimeInitializeSucceeded;
}
