#include "GB_FileSystem.h"
#include "GB_Utf8String.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <sys/stat.h>

#if defined(_WIN32)
#    define NOMINMAX
#    include <windows.h>
#    include <shlobj.h>
#    include <KnownFolders.h>
#    include <objbase.h>
#    include <shobjidl.h>
#    include <cwchar>
#    if defined(_MSC_VER)
#        pragma comment(lib, "Ole32.lib")
#        pragma comment(lib, "Shell32.lib")
#    endif
#else
#    include <dirent.h>
#    include <fcntl.h>
#    include <pwd.h>
#    include <sys/types.h>
#    include <sys/statvfs.h>
#    include <sys/time.h>
#    include <unistd.h>
#    include <utime.h>
#    include <fstream>
#endif

namespace internal
{
    static bool IsSlash(char ch)
    {
        return ch == '/' || ch == '\\';
    }

    static bool IsAsciiAlpha(char ch)
    {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    }


#if defined(_WIN32)
    static unsigned char ToLowerAscii(unsigned char ch)
    {
        if (ch >= static_cast<unsigned char>('A') && ch <= static_cast<unsigned char>('Z'))
        {
            return static_cast<unsigned char>(ch - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
        }
        return ch;
    }

#endif
    static void ReplaceBackslashWithSlash(std::string& text)
    {
        for (size_t i = 0; i < text.size(); i++)
        {
            if (text[i] == '\\')
            {
                text[i] = '/';
            }
        }
    }

    static std::string ToOutputNorm(const std::string& pathUtf8)
    {
        std::string out = pathUtf8;
        ReplaceBackslashWithSlash(out);
        return out;
    }

#if defined(_WIN32)
    static std::string ToWindowsNative(const std::string& pathUtf8)
    {
        std::string out = pathUtf8;
        for (size_t i = 0; i < out.size(); i++)
        {
            if (out[i] == '/')
            {
                out[i] = '\\';
            }
        }
        return out;
    }

#endif
    static size_t FindUncShareEnd(const std::string& normalizedPath)
    {
        // normalizedPath uses '/'
        // format: //server/share[/...]
        if (normalizedPath.size() < 2 || normalizedPath[0] != '/' || normalizedPath[1] != '/')
        {
            return std::string::npos;
        }

        const size_t serverEnd = normalizedPath.find('/', 2);
        if (serverEnd == std::string::npos)
        {
            return std::string::npos;
        }

        const size_t shareEnd = normalizedPath.find('/', serverEnd + 1);
        if (shareEnd == std::string::npos)
        {
            return normalizedPath.size();
        }

        return shareEnd;
    }

    static std::string StripTrailingSlashesButKeepRoot(const std::string& pathUtf8)
    {
        std::string s = ToOutputNorm(pathUtf8);
        if (s.empty())
        {
            return s;
        }

        // Unix root
        if (s == "/")
        {
            return s;
        }

        // Windows drive root: "C:/"
        if (s.size() == 3 && IsAsciiAlpha(s[0]) && s[1] == ':' && s[2] == '/')
        {
            return s;
        }

        // UNC share root: "//server/share" or "//server/share/"
        const size_t uncRootEnd = FindUncShareEnd(s);
        const size_t minLen = (uncRootEnd != std::string::npos) ? uncRootEnd : 0;

        while (s.size() > 1 && s.back() == '/')
        {
            if (minLen > 0 && s.size() <= minLen)
            {
                break;
            }
            s.pop_back();
        }

        return s;
    }

    static std::string EnsureTrailingSlash(const std::string& pathUtf8)
    {
        if (pathUtf8.empty())
        {
            return "";
        }

        // 先做分隔符统一与末尾多余分隔符清理，再确保末尾只有一个 '/'。
        std::string out = StripTrailingSlashesButKeepRoot(pathUtf8);
        if (out.empty())
        {
            return "";
        }

        // Special: "C:" -> "C:/"
        if (out.size() == 2 && IsAsciiAlpha(out[0]) && out[1] == ':')
        {
            out.push_back('/');
            return out;
        }

        if (out.back() != '/')
        {
            out.push_back('/');
        }
        return out;
    }

    static bool EndsWithSlash(const std::string& pathUtf8)
    {
        if (pathUtf8.empty())
        {
            return false;
        }
        return IsSlash(pathUtf8.back());
    }


    static bool IsDirectoryHint(const std::string& pathUtf8)
    {
        if (pathUtf8.empty())
        {
            return false;
        }

        if (EndsWithSlash(pathUtf8))
        {
            return true;
        }

        const std::string normalized = ToOutputNorm(pathUtf8);
        if (normalized == "." || normalized == "..")
        {
            return true;
        }

        // Windows drive root: "C:"
        if (normalized.size() == 2 && IsAsciiAlpha(normalized[0]) && normalized[1] == ':')
        {
            return true;
        }

        const size_t pos = normalized.find_last_of('/');
        const std::string lastSegment = (pos == std::string::npos) ? normalized : normalized.substr(pos + 1);
        return (lastSegment == "." || lastSegment == "..");
    }

    struct PathParts
    {
        std::string root; // "", "/", "c:", "//server/share"
        bool isAbsolute = false;
        bool isDrive = false;
        bool isUnc = false;
        std::vector<std::string> segments;
    };

    static bool StartsWith(const std::string& text, const std::string& prefix)
    {
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    static bool ContainsNullByte(const std::string& text)
    {
        return text.find('\0') != std::string::npos;
    }

    static bool IsValidNativePathString(const std::string& pathUtf8)
    {
        return !pathUtf8.empty() && !ContainsNullByte(pathUtf8);
    }

    static PathParts ParseAndNormalizePathLexical(const std::string& rawPathUtf8)
    {
        PathParts path;
        std::string s = ToOutputNorm(rawPathUtf8);

        // Handle Windows "extended" prefixes in a tolerant way (keep lexical behavior).
        // - "\\?\C:\..."  -> "C:/..."
        // - "\\?\UNC\server\share\..." -> "//server/share/..."
        // After ToOutputNorm: "//?/C:/..." and "//?/UNC/server/share/..."
        if (StartsWith(s, "//?/UNC/"))
        {
            s = "//" + s.substr(std::strlen("//?/UNC/"));
        }
        else if (StartsWith(s, "//?/"))
        {
            s = s.substr(std::strlen("//?/"));
        }
        else if (StartsWith(s, "//./"))
        {
            s = s.substr(std::strlen("//./"));
        }

        size_t index = 0;

        // Windows drive absolute path.
        // "C:/xxx" is absolute; "C:" is kept as a drive-root shorthand for historical compatibility;
        // "C:xxx" is drive-relative on Windows and must not be silently normalized to "C:/xxx".
        if ((s.size() == 2 && IsAsciiAlpha(s[0]) && s[1] == ':')
            || (s.size() >= 3 && IsAsciiAlpha(s[0]) && s[1] == ':' && s[2] == '/'))
        {
            path.isAbsolute = true;
            path.isDrive = true;
            char driveLower = s[0];
            if (driveLower >= 'A' && driveLower <= 'Z')
            {
                driveLower = static_cast<char>(driveLower - 'A' + 'a');
            }
            path.root = std::string(1, driveLower) + ":";
            index = 2;
            if (index < s.size() && s[index] == '/')
            {
                index++;
            }
        }
        // UNC
        else if (StartsWith(s, "//"))
        {
            const size_t uncRootEnd = FindUncShareEnd(s);
            if (uncRootEnd != std::string::npos)
            {
                path.isAbsolute = true;
                path.isUnc = true;
                path.root = s.substr(0, uncRootEnd);
                index = uncRootEnd;
                if (index < s.size() && s[index] == '/')
                {
                    index++;
                }
            }
        }
        // Unix absolute
        else if (!s.empty() && s[0] == '/')
        {
            path.isAbsolute = true;
            path.root = "/";
            index = 1;
        }

        // Segments
        std::vector<std::string> rawSegments;
        std::string current;
        for (size_t i = index; i <= s.size(); i++)
        {
            const char ch = (i < s.size()) ? s[i] : '/';
            if (ch == '/')
            {
                if (!current.empty())
                {
                    rawSegments.push_back(current);
                    current.clear();
                }
            }
            else
            {
                current.push_back(ch);
            }
        }

        // Normalize dot segments (lexical, no symlink resolution)
        std::vector<std::string> outSegments;
        outSegments.reserve(rawSegments.size());
        for (size_t i = 0; i < rawSegments.size(); i++)
        {
            const std::string& seg = rawSegments[i];
            if (seg.empty() || seg == ".")
            {
                continue;
            }
            if (seg == "..")
            {
                if (!outSegments.empty() && outSegments.back() != "..")
                {
                    outSegments.pop_back();
                }
                else
                {
                    if (!path.isAbsolute)
                    {
                        outSegments.push_back("..");
                    }
                }
                continue;
            }
            outSegments.push_back(seg);
        }

        path.segments.swap(outSegments);
        return path;
    }

    static bool IsAbsoluteRootPath(const std::string& pathUtf8)
    {
        const PathParts path = ParseAndNormalizePathLexical(pathUtf8);
        return path.isAbsolute && path.segments.empty();
    }

    static bool IsRootPathString(const std::string& pathUtf8)
    {
        if (pathUtf8.empty())
        {
            return false;
        }

        const std::string trimmedPath = StripTrailingSlashesButKeepRoot(pathUtf8);
        return IsAbsoluteRootPath(trimmedPath);
    }

    static bool IsUnsafeDeleteTargetPath(const std::string& pathUtf8)
    {
        const PathParts path = ParseAndNormalizePathLexical(pathUtf8);
        if (path.isAbsolute && path.segments.empty())
        {
            return true;
        }

        // Do not allow deleting the process current directory through "." or paths normalized to ".".
        if (!path.isAbsolute && path.segments.empty())
        {
            return true;
        }

        // Deleting ".." is too error-prone for a low-level helper.
        if (!path.isAbsolute && path.segments.size() == 1 && path.segments[0] == "..")
        {
            return true;
        }

        return false;
    }

    static std::string BuildPathString(const PathParts& path, bool forceDir)
    {
        std::string out;

        if (path.isAbsolute)
        {
            if (path.isDrive)
            {
                out = path.root;
                out.push_back('/');
            }
            else if (path.isUnc)
            {
                out = path.root;
                out.push_back('/');
            }
            else
            {
                out = "/";
            }
        }

        for (size_t i = 0; i < path.segments.size(); i++)
        {
            if (!out.empty() && out.back() != '/')
            {
                out.push_back('/');
            }
            out += path.segments[i];
        }

        if (out.empty())
        {
            out = ".";
        }

        if (forceDir)
        {
            if (out == ".")
            {
                out = "./";
            }
            else if (out == "..")
            {
                out = "../";
            }
            else if (out.back() != '/')
            {
                out.push_back('/');
            }
        }
        else
        {
            // Keep trailing slash only for explicit roots.
            if (out != "/")
            {
                const bool isRootDrive = (out.size() == 3 && IsAsciiAlpha(out[0]) && out[1] == ':' && out[2] == '/');
                const bool isRootUnc = (path.isUnc && out == path.root + "/");
                if (!isRootDrive && !isRootUnc)
                {
                    while (out.size() > 1 && out.back() == '/')
                    {
                        out.pop_back();
                    }
                }
            }
        }

        return out;
    }

    static bool EqualSegment(const std::string& a, const std::string& b)
    {
#if defined(_WIN32)
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); i++)
        {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[i]);
            const unsigned char la = ToLowerAscii(ca);
            const unsigned char lb = ToLowerAscii(cb);
            if (la != lb)
            {
                return false;
            }
        }
        return true;
#else
        return a == b;
#endif
    }

    static bool EqualRoot(const PathParts& a, const PathParts& b)
    {
        if (a.isAbsolute != b.isAbsolute)
        {
            return false;
        }
        if (a.isDrive != b.isDrive)
        {
            return false;
        }
        if (a.isUnc != b.isUnc)
        {
            return false;
        }
        return EqualSegment(a.root, b.root);
    }

    static std::string JoinSegmentsWithSlash(const std::vector<std::string>& segments)
    {
        if (segments.empty())
        {
            return "";
        }
        std::string out = segments[0];
        for (size_t i = 1; i < segments.size(); i++)
        {
            out.push_back('/');
            out += segments[i];
        }
        return out;
    }

    static std::string NormalizeDirectoryPathUtf8(const std::string& pathUtf8)
    {
        if (pathUtf8.empty())
        {
            return "";
        }

        const PathParts path = ParseAndNormalizePathLexical(pathUtf8);
        return BuildPathString(path, true);
    }

#if defined(_WIN32)
    static bool StartsWithWide(const std::wstring& text, const std::wstring& prefix)
    {
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    static bool IsDriveAbsoluteNativePath(const std::wstring& nativePath)
    {
        return nativePath.size() >= 3
            && IsAsciiAlpha(static_cast<char>(nativePath[0]))
            && nativePath[1] == L':'
            && (nativePath[2] == L'\\' || nativePath[2] == L'/');
    }

    static bool IsUncNativePath(const std::wstring& nativePath)
    {
        return nativePath.size() >= 5
            && nativePath[0] == L'\\'
            && nativePath[1] == L'\\'
            && nativePath[2] != L'?'
            && nativePath[2] != L'.';
    }

    static std::wstring AddExtendedPathPrefixIfNeeded(const std::wstring& nativePath)
    {
        if (nativePath.empty())
        {
            return nativePath;
        }

        if (StartsWithWide(nativePath, L"\\\\?\\") || StartsWithWide(nativePath, L"\\\\.\\"))
        {
            return nativePath;
        }

        if (IsDriveAbsoluteNativePath(nativePath))
        {
            return L"\\\\?\\" + nativePath;
        }

        if (IsUncNativePath(nativePath))
        {
            return L"\\\\?\\UNC\\" + nativePath.substr(2);
        }

        // Root-relative and pure relative paths cannot use the "\\\\?\\" prefix.
        return nativePath;
    }

    static std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty() || ContainsNullByte(utf8))
        {
            return L"";
        }

        std::string pathForApiUtf8 = utf8;
        const std::string normalizedOriginal = ToOutputNorm(utf8);
        if (StartsWith(normalizedOriginal, "//?/") || StartsWith(normalizedOriginal, "//./"))
        {
            pathForApiUtf8 = normalizedOriginal;
        }
        else
        {
            const PathParts parsedPath = ParseAndNormalizePathLexical(utf8);
            if (parsedPath.isAbsolute)
            {
                pathForApiUtf8 = BuildPathString(parsedPath, EndsWithSlash(utf8));
            }
        }

        const std::string native = ToWindowsNative(pathForApiUtf8);
        if (native.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return L"";
        }

        const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, native.c_str(),
            static_cast<int>(native.size()), nullptr, 0);
        if (required <= 0)
        {
            return L"";
        }

        std::wstring out;
        out.resize(static_cast<size_t>(required));
        const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, native.c_str(),
            static_cast<int>(native.size()), &out[0], required);
        if (written != required)
        {
            return L"";
        }

        return AddExtendedPathPrefixIfNeeded(out);
    }

    static std::string WideToUtf8(const std::wstring& wide)
    {
        if (wide.empty())
        {
            return "";
        }

        if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return "";
        }

        const int required = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
            nullptr, 0, nullptr, nullptr);
        if (required <= 0)
        {
            return "";
        }

        std::string out;
        out.resize(static_cast<size_t>(required));
        const int written = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
            &out[0], required, nullptr, nullptr);
        if (written != required)
        {
            return "";
        }

        return ToOutputNorm(out);
    }

    static DWORD GetFindFirstFileExFlagsForDirectoryScan()
    {
#if defined(FIND_FIRST_EX_LARGE_FETCH)
        // 大目录遍历时让系统使用更大的内部缓冲，减少内核态/用户态往返。
        return FIND_FIRST_EX_LARGE_FETCH;
#else
        return 0;
#endif
    }

    static HANDLE FindFirstFileExBasicWithFallback(const std::wstring& patternW, WIN32_FIND_DATAW& outData)
    {
        HANDLE findHandle = FindFirstFileExW(patternW.c_str(), FindExInfoBasic, &outData, FindExSearchNameMatch, nullptr, GetFindFirstFileExFlagsForDirectoryScan());
#if defined(FIND_FIRST_EX_LARGE_FETCH)
        if (findHandle == INVALID_HANDLE_VALUE && GetFindFirstFileExFlagsForDirectoryScan() != 0)
        {
            const DWORD errorCode = GetLastError();
            if (errorCode == ERROR_INVALID_PARAMETER || errorCode == ERROR_NOT_SUPPORTED)
            {
                // 部分旧系统、特殊文件系统或网络文件系统可能不支持 FIND_FIRST_EX_LARGE_FETCH；
                // 此时回退为普通枚举，避免因为性能优化标志导致功能失败。
                findHandle = FindFirstFileExW(patternW.c_str(), FindExInfoBasic, &outData, FindExSearchNameMatch, nullptr, 0);
            }
        }
#endif
        return findHandle;
    }

    static bool IsDotOrDotDotW(const wchar_t* name)
    {
        if (!name)
        {
            return false;
        }
        return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
    }
#else
    static bool IsDotOrDotDotA(const char* name)
    {
        if (!name)
        {
            return false;
        }
        return std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0;
    }
#endif

    static bool IsDirByStat(const std::string& pathUtf8, bool& outExists, bool& outIsDir)
    {
        outExists = false;
        outIsDir = false;

        if (pathUtf8.empty())
        {
            return true;
        }

#if defined(_WIN32)
        const std::wstring pathW = Utf8ToWide(pathUtf8);
        if (pathW.empty())
        {
            return false;
        }

        const DWORD attrs = GetFileAttributesW(pathW.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            outExists = false;
            outIsDir = false;
            return true;
        }

        outExists = true;
        outIsDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return true;
#else
        const std::string normalized = ToOutputNorm(pathUtf8);
        if (ContainsNullByte(normalized))
        {
            return false;
        }

        struct stat st;
        if (::stat(normalized.c_str(), &st) != 0)
        {
            outExists = false;
            outIsDir = false;
            return true;
        }

        outExists = true;
        outIsDir = S_ISDIR(st.st_mode) != 0;
        return true;
#endif
    }

    static bool TryGetFileSize64(const std::string& filePathUtf8, unsigned long long& outSize)
    {
        outSize = 0;

#if defined(_WIN32)
        const std::wstring pathW = Utf8ToWide(filePathUtf8);
        if (pathW.empty())
        {
            return false;
        }

        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(pathW.c_str(), GetFileExInfoStandard, &data))
        {
            return false;
        }

        const DWORD attrs = data.dwFileAttributes;
        if ((attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        {
            return false;
        }

        outSize = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | static_cast<unsigned long long>(data.nFileSizeLow);
        return true;
#else
        const std::string normalized = ToOutputNorm(filePathUtf8);
        if (ContainsNullByte(normalized))
        {
            return false;
        }

        struct stat st;
        if (::stat(normalized.c_str(), &st) != 0)
        {
            return false;
        }
        if (!S_ISREG(st.st_mode))
        {
            return false;
        }

        outSize = static_cast<unsigned long long>(st.st_size);
        return true;
#endif
    }

    static bool MakeDirsRecursive(const std::string& dirPathUtf8)
    {
        const std::string trimmed = StripTrailingSlashesButKeepRoot(dirPathUtf8);
        if (trimmed.empty())
        {
            return false;
        }

        // quick return if already exists
        bool exists = false;
        bool isDir = false;
        if (!IsDirByStat(trimmed, exists, isDir))
        {
            return false;
        }
        if (exists)
        {
            return isDir;
        }

        // Parse without forcing output dir
        const PathParts path = ParseAndNormalizePathLexical(trimmed);

#if defined(_WIN32)
        // Drive root should always be treated as existing.
        if (path.isDrive && path.segments.empty())
        {
            return true;
        }
        // UNC root should always be treated as existing.
        if (path.isUnc && path.segments.empty())
        {
            return true;
        }

        std::string current;
        if (path.isAbsolute)
        {
            if (path.isDrive)
            {
                current = path.root + "/";
            }
            else if (path.isUnc)
            {
                current = path.root + "/";
            }
            else
            {
                current = "/";
            }
        }

        for (size_t i = 0; i < path.segments.size(); i++)
        {
            if (!current.empty() && current.back() != '/')
            {
                current.push_back('/');
            }
            current += path.segments[i];

            bool stepExists = false;
            bool stepIsDir = false;
            if (!IsDirByStat(current, stepExists, stepIsDir))
            {
                return false;
            }
            if (stepExists)
            {
                if (!stepIsDir)
                {
                    return false;
                }
                continue;
            }

            const std::wstring w = Utf8ToWide(current);
            if (w.empty())
            {
                return false;
            }
            if (!CreateDirectoryW(w.c_str(), nullptr))
            {
                const DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS)
                {
                    return false;
                }

                bool racedExists = false;
                bool racedIsDir = false;
                if (!IsDirByStat(current, racedExists, racedIsDir) || !racedExists || !racedIsDir)
                {
                    return false;
                }
            }
        }
        return true;
#else
        std::string current;
        if (path.isAbsolute)
        {
            current = "/";
        }

        for (size_t i = 0; i < path.segments.size(); i++)
        {
            if (!current.empty() && current.back() != '/')
            {
                current.push_back('/');
            }
            current += path.segments[i];

            struct stat st;
            if (::stat(current.c_str(), &st) == 0)
            {
                if (!S_ISDIR(st.st_mode))
                {
                    return false;
                }
                continue;
            }

            if (::mkdir(current.c_str(), 0755) != 0)
            {
                if (errno == EEXIST)
                {
                    struct stat st2;
                    if (::stat(current.c_str(), &st2) == 0 && S_ISDIR(st2.st_mode))
                    {
                        continue;
                    }
                    return false;
                }
                return false;
            }
        }
        return true;
#endif
    }

#if defined(_WIN32)
    static std::wstring StripTrailingSlashForDirectoryApi(const std::wstring& path)
    {
        if (path.empty())
        {
            return path;
        }
        std::wstring out = path;
        while (out.size() > 1)
        {
            const wchar_t last = out.back();
            if (last != L'/' && last != L'\\')
            {
                break;
            }
            // Keep drive root (e.g. "C:\\")
            if (out.size() == 3 && IsAsciiAlpha(static_cast<char>(out[0])) && out[1] == L':' && (out[2] == L'\\' || out[2] == L'/'))
            {
                break;
            }
            out.pop_back();
        }
        return out;
    }

    static DWORD GetSettableFileAttributes(DWORD attrs)
    {
        DWORD settableAttrs = attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM
            | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_OFFLINE);
        if (settableAttrs == 0)
        {
            settableAttrs = FILE_ATTRIBUTE_NORMAL;
        }
        return settableAttrs;
    }

    static bool SetSettableFileAttributes(const std::wstring& pathW, DWORD attrs)
    {
        if (pathW.empty())
        {
            return false;
        }
        return SetFileAttributesW(pathW.c_str(), GetSettableFileAttributes(attrs)) != 0;
    }

    static bool ClearFileAttributesIfNeeded(const std::wstring& pathW, DWORD attributesToClear)
    {
        if (pathW.empty())
        {
            return false;
        }

        const DWORD attrs = GetFileAttributesW(pathW.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            return true;
        }

        const DWORD newAttrs = attrs & ~attributesToClear;
        if (newAttrs == attrs)
        {
            return true;
        }

        return SetSettableFileAttributes(pathW, newAttrs);
    }

    static bool ClearReadOnlyAttributeIfNeeded(const std::wstring& pathW)
    {
        return ClearFileAttributesIfNeeded(pathW, FILE_ATTRIBUTE_READONLY);
    }

    static bool ClearOverwriteBlockingAttributesIfNeeded(const std::wstring& pathW)
    {
        return ClearFileAttributesIfNeeded(pathW, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    }

    static bool DeleteOneFile(const std::string& filePathUtf8)
    {
        const std::wstring w = Utf8ToWide(filePathUtf8);
        if (w.empty())
        {
            return false;
        }

        ClearReadOnlyAttributeIfNeeded(w);
        return DeleteFileW(w.c_str()) != 0;
    }

    static bool RemoveEmptyDir(const std::string& dirPathUtf8)
    {
        const std::wstring w = StripTrailingSlashForDirectoryApi(Utf8ToWide(dirPathUtf8));
        if (w.empty())
        {
            return false;
        }

        ClearReadOnlyAttributeIfNeeded(w);
        return RemoveDirectoryW(w.c_str()) != 0;
    }

    static bool DeleteDirContents(const std::string& dirPathUtf8)
    {
        const std::string dirWithSlash = EnsureTrailingSlash(dirPathUtf8);
        if (dirWithSlash.empty())
        {
            return false;
        }

        const std::wstring patternW = Utf8ToWide(dirWithSlash + "*");
        if (patternW.empty())
        {
            return false;
        }

        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileExBasicWithFallback(patternW, data);
        if (find == INVALID_HANDLE_VALUE)
        {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND)
            {
                return true;
            }
            return false;
        }

        do
        {
            const wchar_t* name = data.cFileName;
            if (!name)
            {
                continue;
            }
            if (IsDotOrDotDotW(name))
            {
                continue;
            }

            const std::string childNameUtf8 = WideToUtf8(std::wstring(name));
            if (childNameUtf8.empty())
            {
                continue;
            }
            const std::string child = dirWithSlash + childNameUtf8;
            const DWORD attrs = data.dwFileAttributes;
            const bool isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool isReparsePoint = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (isDir)
            {
                // 目录符号链接/联接点（reparse point）不应递归进入，避免误删目标目录。
                if (isReparsePoint)
                {
                    if (!RemoveEmptyDir(child))
                    {
                        FindClose(find);
                        return false;
                    }
                }
                else
                {
                    if (!DeleteDirContents(child))
                    {
                        FindClose(find);
                        return false;
                    }
                    if (!RemoveEmptyDir(child))
                    {
                        FindClose(find);
                        return false;
                    }
                }
            }
            else
            {
                if (!DeleteOneFile(child))
                {
                    FindClose(find);
                    return false;
                }
            }
        } while (FindNextFileW(find, &data));

        const DWORD findErr = GetLastError();
        FindClose(find);
        return findErr == ERROR_NO_MORE_FILES;
    }
#else
    static bool DeleteDirContents(const std::string& dirPathUtf8)
    {
        const std::string dirWithSlash = EnsureTrailingSlash(dirPathUtf8);
        if (dirWithSlash.empty())
        {
            return false;
        }

        DIR* dir = opendir(dirWithSlash.c_str());
        if (!dir)
        {
            return false;
        }

        for (;;)
        {
            errno = 0;
            struct dirent* entry = readdir(dir);
            if (!entry)
            {
                const int readDirErrno = errno;
                const int closeDirResult = closedir(dir);
                return readDirErrno == 0 && closeDirResult == 0;
            }

            const char* name = entry->d_name;
            if (IsDotOrDotDotA(name))
            {
                continue;
            }

            const std::string fullPath = dirWithSlash + name;
            struct stat st;
            if (lstat(fullPath.c_str(), &st) != 0)
            {
                if (errno == ENOENT)
                {
                    continue;
                }
                closedir(dir);
                return false;
            }

            if (S_ISDIR(st.st_mode))
            {
                if (!DeleteDirContents(fullPath))
                {
                    closedir(dir);
                    return false;
                }
                if (rmdir(fullPath.c_str()) != 0)
                {
                    closedir(dir);
                    return false;
                }
            }
            else
            {
                if (unlink(fullPath.c_str()) != 0)
                {
                    closedir(dir);
                    return false;
                }
            }
        }
    }
#endif

    static void ListFilesRecursive(const std::string& dirPathUtf8, bool recursive, std::vector<std::string>& outFiles)
    {
        if (dirPathUtf8.empty())
        {
            return;
        }

        bool exists = false;
        bool isDir = false;
        if (!IsDirByStat(dirPathUtf8, exists, isDir) || !exists || !isDir)
        {
            return;
        }

        const std::string dirWithSlash = EnsureTrailingSlash(dirPathUtf8);
        if (dirWithSlash.empty())
        {
            return;
        }

#if defined(_WIN32)
        const std::wstring patternW = Utf8ToWide(dirWithSlash + "*");
        if (patternW.empty())
        {
            return;
        }

        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileExBasicWithFallback(patternW, data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;
        }

        do
        {
            const wchar_t* name = data.cFileName;
            if (!name)
            {
                continue;
            }
            if (IsDotOrDotDotW(name))
            {
                continue;
            }

            const std::string itemNameUtf8 = WideToUtf8(std::wstring(name));
            if (itemNameUtf8.empty())
            {
                continue;
            }
            const std::string item = dirWithSlash + itemNameUtf8;
            const DWORD attrs = data.dwFileAttributes;
            const bool entryIsDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool isReparsePoint = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (entryIsDir)
            {
                if (recursive && !isReparsePoint)
                {
                    ListFilesRecursive(item, true, outFiles);
                }
            }
            else
            {
                outFiles.push_back(ToOutputNorm(item));
            }
        } while (FindNextFileW(find, &data));

        FindClose(find);
#else
        DIR* dir = opendir(dirWithSlash.c_str());
        if (!dir)
        {
            return;
        }

        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr)
        {
            const char* name = entry->d_name;
            if (IsDotOrDotDotA(name))
            {
                continue;
            }

            const std::string item = dirWithSlash + name;
            struct stat st;
            if (lstat(item.c_str(), &st) != 0)
            {
                continue;
            }

            if (S_ISDIR(st.st_mode))
            {
                if (recursive)
                {
                    ListFilesRecursive(item, true, outFiles);
                }
            }
            else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
            {
                outFiles.push_back(ToOutputNorm(item));
            }
        }

        closedir(dir);
#endif
    }

#if !defined(_WIN32)
    static std::string GetEnvVarUtf8(const char* name)
    {
        if (!name)
        {
            return "";
        }
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string();
    }

    static std::string GetHomeDirectoryUtf8_NoThrow()
    {
        const std::string fromEnv = GetEnvVarUtf8("HOME");
        if (!fromEnv.empty())
        {
            return NormalizeDirectoryPathUtf8(fromEnv);
        }

        const uid_t userId = getuid();
        long bufferSize = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufferSize < 0)
        {
            bufferSize = 16384;
        }

        std::vector<char> buffer;
        buffer.resize(static_cast<size_t>(bufferSize));

        struct passwd pwd;
        struct passwd* result = nullptr;
        if (getpwuid_r(userId, &pwd, buffer.data(), buffer.size(), &result) != 0 || !result || !result->pw_dir)
        {
            return "";
        }

        return NormalizeDirectoryPathUtf8(std::string(result->pw_dir));
    }

    static std::string TrimAscii(const std::string& text)
    {
        size_t beginIndex = 0;
        while (beginIndex < text.size())
        {
            const char ch = text[beginIndex];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            {
                break;
            }
            beginIndex++;
        }

        size_t endIndex = text.size();
        while (endIndex > beginIndex)
        {
            const char ch = text[endIndex - 1];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            {
                break;
            }
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    static std::string UnescapeXdgValue(const std::string& value)
    {
        std::string out;
        out.reserve(value.size());

        for (size_t i = 0; i < value.size(); i++)
        {
            const char ch = value[i];
            if (ch == '\\' && i + 1 < value.size())
            {
                const char nextCh = value[i + 1];
                if (nextCh == '\\' || nextCh == '"' || nextCh == '$')
                {
                    out.push_back(nextCh);
                    i++;
                    continue;
                }
            }
            out.push_back(ch);
        }

        return out;
    }

    static std::string ExpandHomeToken(const std::string& path, const std::string& homeDirWithSlash)
    {
        if (homeDirWithSlash.empty())
        {
            return path;
        }

        std::string homeDirNoSlash = homeDirWithSlash;
        if (!homeDirNoSlash.empty() && homeDirNoSlash.back() == '/')
        {
            homeDirNoSlash.pop_back();
        }

        std::string out = path;

        // Support $HOME and ${HOME}
        const std::string token1 = "$HOME";
        const std::string token2 = "${HOME}";

        size_t pos = 0;
        while ((pos = out.find(token2, pos)) != std::string::npos)
        {
            out.replace(pos, token2.size(), homeDirNoSlash);
            pos += homeDirNoSlash.size();
        }

        pos = 0;
        while ((pos = out.find(token1, pos)) != std::string::npos)
        {
            out.replace(pos, token1.size(), homeDirNoSlash);
            pos += homeDirNoSlash.size();
        }

        if (StartsWith(out, "~/"))
        {
            out = homeDirNoSlash + out.substr(1);
        }

        return out;
    }

    static std::string GetXdgUserDirFromConfig(const std::string& xdgKeyName, const std::string& homeDirWithSlash)
    {
        if (homeDirWithSlash.empty())
        {
            return "";
        }

        std::string configHome = GetEnvVarUtf8("XDG_CONFIG_HOME");
        if (configHome.empty())
        {
            std::string homeDirNoSlash = homeDirWithSlash;
            if (!homeDirNoSlash.empty() && homeDirNoSlash.back() == '/')
            {
                homeDirNoSlash.pop_back();
            }
            configHome = homeDirNoSlash + "/.config";
        }

        ReplaceBackslashWithSlash(configHome);
        configHome = StripTrailingSlashesButKeepRoot(configHome);
        const std::string configFilePath = configHome + "/user-dirs.dirs";

        std::ifstream input(configFilePath.c_str(), std::ios::in);
        if (!input.is_open())
        {
            return "";
        }

        std::string line;
        const std::string prefix = xdgKeyName + "=";
        while (std::getline(input, line))
        {
            const std::string trimmed = TrimAscii(line);
            if (trimmed.empty() || trimmed[0] == '#')
            {
                continue;
            }
            if (!StartsWith(trimmed, prefix))
            {
                continue;
            }

            std::string value = TrimAscii(trimmed.substr(prefix.size()));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            {
                value = value.substr(1, value.size() - 2);
            }

            value = UnescapeXdgValue(value);
            value = ExpandHomeToken(value, homeDirWithSlash);
            value = NormalizeDirectoryPathUtf8(value);
            return value;
        }

        return "";
    }
#endif

#if defined(_WIN32)
    static bool GetKnownFolderPathUtf8(const KNOWNFOLDERID& folderId, std::string& outPathUtf8)
    {
        outPathUtf8.clear();

        PWSTR rawPath = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &rawPath);
        if (FAILED(hr) || !rawPath)
        {
            return false;
        }

        const std::wstring pathW(rawPath);
        CoTaskMemFree(rawPath);

        outPathUtf8 = GB_WStringToUtf8(pathW);
        outPathUtf8 = NormalizeDirectoryPathUtf8(outPathUtf8);
        return !outPathUtf8.empty();
    }

    static std::wstring GetEnvVarW(const wchar_t* name)
    {
        if (!name)
        {
            return L"";
        }

        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0)
        {
            return L"";
        }

        std::wstring value;
        value.resize(static_cast<size_t>(required), L'\0');
        const DWORD written = GetEnvironmentVariableW(name, &value[0], required);
        if (written == 0 || written >= required)
        {
            return L"";
        }
        value.resize(static_cast<size_t>(written));
        return value;
    }

    static std::string GetEnvVarUtf8FromWide(const wchar_t* name)
    {
        const std::wstring valueW = GetEnvVarW(name);
        if (valueW.empty())
        {
            return "";
        }
        return GB_WStringToUtf8(valueW);
    }

    static DWORD GetTempPathBySystemPolicyW(DWORD bufferLength, wchar_t* buffer)
    {
        typedef DWORD(WINAPI* GetTempPath2WFunc)(DWORD, LPWSTR);
        static GetTempPath2WFunc getTempPath2WFunc = reinterpret_cast<GetTempPath2WFunc>(GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "GetTempPath2W"));
        if (getTempPath2WFunc)
        {
            return getTempPath2WFunc(bufferLength, buffer);
        }
        return GetTempPathW(bufferLength, buffer);
    }
#endif

    static bool ContainsPathSlash(const std::string& text)
    {
        for (size_t i = 0; i < text.size(); i++)
        {
            if (IsSlash(text[i]))
            {
                return true;
            }
        }
        return false;
    }

    static bool IsDotOrDotDotName(const std::string& name)
    {
        return name == "." || name == "..";
    }

    static bool IsExistingDirectoryPath(const std::string& pathUtf8)
    {
        bool exists = false;
        bool isDir = false;
        if (!IsDirByStat(pathUtf8, exists, isDir))
        {
            return false;
        }
        return exists && isDir;
    }

    static bool IsSamePathLexically(const std::string& leftPathUtf8, const std::string& rightPathUtf8)
    {
        const PathParts left = ParseAndNormalizePathLexical(StripTrailingSlashesButKeepRoot(leftPathUtf8));
        const PathParts right = ParseAndNormalizePathLexical(StripTrailingSlashesButKeepRoot(rightPathUtf8));
        if (!EqualRoot(left, right))
        {
            return false;
        }
        if (left.segments.size() != right.segments.size())
        {
            return false;
        }
        for (size_t i = 0; i < left.segments.size(); i++)
        {
            if (!EqualSegment(left.segments[i], right.segments[i]))
            {
                return false;
            }
        }
        return true;
    }

    static bool TryCheckSameExistingPath(const std::string& leftPathUtf8, const std::string& rightPathUtf8, bool& outIsSamePath)
    {
        outIsSamePath = false;

#if defined(_WIN32)
        const std::wstring leftW = Utf8ToWide(leftPathUtf8);
        const std::wstring rightW = Utf8ToWide(rightPathUtf8);
        if (leftW.empty() || rightW.empty())
        {
            return false;
        }

        const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
        const HANDLE leftHandle = CreateFileW(leftW.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr);
        if (leftHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        const HANDLE rightHandle = CreateFileW(rightW.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr);
        if (rightHandle == INVALID_HANDLE_VALUE)
        {
            CloseHandle(leftHandle);
            return false;
        }

        BY_HANDLE_FILE_INFORMATION leftInfo;
        BY_HANDLE_FILE_INFORMATION rightInfo;
        const BOOL leftOk = GetFileInformationByHandle(leftHandle, &leftInfo);
        const BOOL rightOk = GetFileInformationByHandle(rightHandle, &rightInfo);
        CloseHandle(rightHandle);
        CloseHandle(leftHandle);

        if (!leftOk || !rightOk)
        {
            return false;
        }

        outIsSamePath = leftInfo.dwVolumeSerialNumber == rightInfo.dwVolumeSerialNumber
            && leftInfo.nFileIndexHigh == rightInfo.nFileIndexHigh
            && leftInfo.nFileIndexLow == rightInfo.nFileIndexLow;
        return true;
#else
        const std::string leftNormalized = ToOutputNorm(leftPathUtf8);
        const std::string rightNormalized = ToOutputNorm(rightPathUtf8);
        if (ContainsNullByte(leftNormalized) || ContainsNullByte(rightNormalized))
        {
            return false;
        }

        struct stat leftStat;
        struct stat rightStat;
        if (lstat(leftNormalized.c_str(), &leftStat) != 0 || lstat(rightNormalized.c_str(), &rightStat) != 0)
        {
            return false;
        }

        outIsSamePath = leftStat.st_dev == rightStat.st_dev && leftStat.st_ino == rightStat.st_ino;
        return true;
#endif
    }

    static bool IsSubPathLexically(const std::string& parentPathUtf8, const std::string& childPathUtf8)
    {
        const PathParts parent = ParseAndNormalizePathLexical(StripTrailingSlashesButKeepRoot(parentPathUtf8));
        const PathParts child = ParseAndNormalizePathLexical(StripTrailingSlashesButKeepRoot(childPathUtf8));
        if (!EqualRoot(parent, child))
        {
            return false;
        }
        if (child.segments.size() <= parent.segments.size())
        {
            return false;
        }
        for (size_t i = 0; i < parent.segments.size(); i++)
        {
            if (!EqualSegment(parent.segments[i], child.segments[i]))
            {
                return false;
            }
        }
        return true;
    }

    static bool IsPathReplaceCompatible(GB_FileType srcType, const std::string& dstPathUtf8)
    {
        const GB_FileType dstType = GB_GetFileType(dstPathUtf8);
        if (dstType == GB_FileType::NotExists)
        {
            return true;
        }

        const bool srcIsDir = (srcType == GB_FileType::Directory);
        const bool dstIsDir = IsExistingDirectoryPath(dstPathUtf8);
        if (srcIsDir != dstIsDir)
        {
            return false;
        }
        return true;
    }

    static bool IsDestinationParentDirectoryReady(const std::string& dstPathUtf8)
    {
        std::string parentPathUtf8 = GB_GetDirectoryPath(dstPathUtf8);
        if (parentPathUtf8.empty())
        {
            return true;
        }

        parentPathUtf8 = StripTrailingSlashesButKeepRoot(parentPathUtf8);
        if (parentPathUtf8.empty())
        {
            return true;
        }

        return IsExistingDirectoryPath(parentPathUtf8);
    }

    static std::string MakeMoveTempPath(const std::string& dstPathUtf8)
    {
        const std::string trimmedDst = StripTrailingSlashesButKeepRoot(dstPathUtf8);
        if (trimmedDst.empty())
        {
            return "";
        }

        const std::string parentPathUtf8 = GB_GetDirectoryPath(trimmedDst);
        const std::string fileNameUtf8 = GB_GetFileName(trimmedDst, true);
        if (fileNameUtf8.empty())
        {
            return "";
        }

#if defined(_WIN32)
        const unsigned long processId = static_cast<unsigned long>(GetCurrentProcessId());
#else
        const unsigned long processId = static_cast<unsigned long>(getpid());
#endif

        for (int i = 0; i < 1000; i++)
        {
            char suffix[128];
#if defined(_MSC_VER)
            sprintf_s(suffix, sizeof(suffix), ".gb_move_tmp_%lu_%d", processId, i);
#else
            std::snprintf(suffix, sizeof(suffix), ".gb_move_tmp_%lu_%d", processId, i);
#endif
            const std::string candidate = parentPathUtf8 + fileNameUtf8 + suffix;
            if (GB_GetFileType(candidate) == GB_FileType::NotExists)
            {
                return candidate;
            }
        }

        return "";
    }

    static bool SystemRenamePathNoFallback(const std::string& srcPathUtf8, const std::string& dstPathUtf8, bool overwriteIfExists, bool allowCopyAcrossVolume = false)
    {
        if (srcPathUtf8.empty() || dstPathUtf8.empty())
        {
            return false;
        }

#if defined(_WIN32)
        const std::wstring srcW = Utf8ToWide(srcPathUtf8);
        const std::wstring dstW = Utf8ToWide(dstPathUtf8);
        if (srcW.empty() || dstW.empty())
        {
            return false;
        }

        DWORD flags = 0;
        if (overwriteIfExists)
        {
            flags |= MOVEFILE_REPLACE_EXISTING;
        }
        if (allowCopyAcrossVolume)
        {
            flags |= MOVEFILE_COPY_ALLOWED;
        }
        return MoveFileExW(srcW.c_str(), dstW.c_str(), flags) != 0;
#else
        (void)allowCopyAcrossVolume;
        if (!overwriteIfExists && GB_GetFileType(dstPathUtf8) != GB_FileType::NotExists)
        {
            errno = EEXIST;
            return false;
        }

        const std::string srcNormalized = ToOutputNorm(srcPathUtf8);
        const std::string dstNormalized = ToOutputNorm(dstPathUtf8);
        return std::rename(srcNormalized.c_str(), dstNormalized.c_str()) == 0;
#endif
    }

    static bool DeletePathAnyType(const std::string& pathUtf8)
    {
        if (pathUtf8.empty())
        {
            return false;
        }

#if defined(_WIN32)
        const std::wstring pathW = Utf8ToWide(pathUtf8);
        if (pathW.empty())
        {
            return false;
        }

        const DWORD attrs = GetFileAttributesW(pathW.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            return false;
        }

        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return GB_DeleteDirectory(pathUtf8);
        }

        // 文件符号链接也是重解析点。GB_DeleteFile 按接口语义只接受常规文件，
        // 因此这里必须直接使用 DeleteFileW 删除路径项本身，而不能再转回 GB_DeleteFile。
        return DeleteOneFile(pathUtf8);
#else
        const std::string normalized = ToOutputNorm(pathUtf8);
        struct stat st;
        if (lstat(normalized.c_str(), &st) != 0)
        {
            return false;
        }

        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
        {
            return GB_DeleteDirectory(pathUtf8);
        }
        return unlink(normalized.c_str()) == 0;
#endif
    }

#if defined(_WIN32)
    static unsigned long long FileTimeToUInt64(const FILETIME& fileTime)
    {
        ULARGE_INTEGER value;
        value.LowPart = fileTime.dwLowDateTime;
        value.HighPart = fileTime.dwHighDateTime;
        return static_cast<unsigned long long>(value.QuadPart);
    }

    static GB_DateTime FileTimeToDateTime(const FILETIME& fileTime)
    {
        static const unsigned long long windowsToUnixEpoch100Ns = 116444736000000000ULL;
        const unsigned long long fileTimeValue = FileTimeToUInt64(fileTime);
        if (fileTimeValue < windowsToUnixEpoch100Ns)
        {
            return GB_DateTime::Invalid;
        }

        const unsigned long long unixMillisecondsUnsigned = (fileTimeValue - windowsToUnixEpoch100Ns) / 10000ULL;
        if (unixMillisecondsUnsigned > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
        {
            return GB_DateTime::Invalid;
        }

        return GB_DateTime::CreateFromUnixMilliseconds(static_cast<long long>(unixMillisecondsUnsigned), GB_DateTimeSpec::UtcTime);
    }

    static bool DateTimeToFileTime(const GB_DateTime& dateTime, FILETIME& outFileTime)
    {
        static const long long windowsToUnixEpoch100Ns = 116444736000000000LL;
        if (!dateTime.IsValid())
        {
            return false;
        }

        const long long unixMilliseconds = dateTime.ToUnixMilliseconds();
        if (unixMilliseconds < -11644473600000LL)
        {
            return false;
        }
        if (unixMilliseconds > (std::numeric_limits<long long>::max() - windowsToUnixEpoch100Ns) / 10000LL)
        {
            return false;
        }

        const long long fileTimeValueSigned = unixMilliseconds * 10000LL + windowsToUnixEpoch100Ns;
        if (fileTimeValueSigned < 0)
        {
            return false;
        }

        ULARGE_INTEGER value;
        value.QuadPart = static_cast<ULONGLONG>(fileTimeValueSigned);
        outFileTime.dwLowDateTime = value.LowPart;
        outFileTime.dwHighDateTime = value.HighPart;
        return true;
    }

    static std::wstring StripTrailingSlashForFindFile(const std::wstring& path)
    {
        if (path.empty())
        {
            return path;
        }

        std::wstring out = path;
        while (out.size() > 1)
        {
            const wchar_t last = out.back();
            if (last != L'/' && last != L'\\')
            {
                break;
            }
            if (out.size() == 3 && IsAsciiAlpha(static_cast<char>(out[0])) && out[1] == L':' && (out[2] == L'\\' || out[2] == L'/'))
            {
                break;
            }
            out.pop_back();
        }
        return out;
    }

    static bool IsWindowsSymbolicLink(const std::wstring& pathW, DWORD attrs)
    {
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
        {
            return false;
        }

        WIN32_FIND_DATAW data;
        const std::wstring pathForFind = StripTrailingSlashForFindFile(pathW);
        const HANDLE find = FindFirstFileW(pathForFind.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        const DWORD reparseTag = data.dwReserved0;
        FindClose(find);
        return reparseTag == IO_REPARSE_TAG_SYMLINK;
    }

    static GB_FileType GetWindowsFileTypeByAttributes(const std::wstring& pathW, DWORD attrs)
    {
        if (IsWindowsSymbolicLink(pathW, attrs))
        {
            return GB_FileType::SymbolicLink;
        }
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            return GB_FileType::Other;
        }
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return GB_FileType::Directory;
        }
        if ((attrs & FILE_ATTRIBUTE_DEVICE) != 0)
        {
            return GB_FileType::Other;
        }
        return GB_FileType::RegularFile;
    }

    static bool TryOpenFileWithAccess(const std::string& filePathUtf8, DWORD desiredAccess)
    {
        const std::wstring pathW = Utf8ToWide(filePathUtf8);
        if (pathW.empty())
        {
            return false;
        }

        const HANDLE handle = CreateFileW(pathW.c_str(), desiredAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        CloseHandle(handle);
        return true;
    }

    static bool ExpandEnvironmentString(const std::wstring& text, std::wstring& outText)
    {
        outText.clear();
        if (text.empty())
        {
            return false;
        }

        const DWORD required = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
        if (required == 0)
        {
            return false;
        }

        std::wstring buffer;
        buffer.resize(static_cast<size_t>(required), L'\0');
        const DWORD written = ExpandEnvironmentStringsW(text.c_str(), &buffer[0], required);
        if (written == 0 || written > required)
        {
            return false;
        }

        if (written > 0)
        {
            buffer.resize(static_cast<size_t>(written - 1));
        }
        outText.swap(buffer);
        return true;
    }
#else
    static GB_DateTime PosixTimeToDateTime(std::time_t seconds)
    {
        if (seconds > static_cast<std::time_t>(std::numeric_limits<long long>::max())
            || seconds < static_cast<std::time_t>(std::numeric_limits<long long>::min()))
        {
            return GB_DateTime::Invalid;
        }
        return GB_DateTime::CreateFromUnixSeconds(static_cast<long long>(seconds), GB_DateTimeSpec::UtcTime);
    }

    static bool DateTimeToTimeval(const GB_DateTime& dateTime, timeval& outTimeValue)
    {
        if (!dateTime.IsValid())
        {
            return false;
        }

        const long long unixMilliseconds = dateTime.ToUnixMilliseconds();
        long long seconds = unixMilliseconds / 1000LL;
        long long milliseconds = unixMilliseconds % 1000LL;
        if (milliseconds < 0)
        {
            milliseconds += 1000LL;
            seconds--;
        }

        if (seconds > static_cast<long long>(std::numeric_limits<time_t>::max())
            || seconds < static_cast<long long>(std::numeric_limits<time_t>::min()))
        {
            return false;
        }

        outTimeValue.tv_sec = static_cast<time_t>(seconds);
        outTimeValue.tv_usec = static_cast<suseconds_t>(milliseconds * 1000LL);
        return true;
    }

    static GB_FileType GetPosixFileTypeFromMode(mode_t mode)
    {
        if (S_ISLNK(mode))
        {
            return GB_FileType::SymbolicLink;
        }
        if (S_ISREG(mode))
        {
            return GB_FileType::RegularFile;
        }
        if (S_ISDIR(mode))
        {
            return GB_FileType::Directory;
        }
        return GB_FileType::Other;
    }

    static bool TryOpenFileWithFlags(const std::string& filePathUtf8, int flags)
    {
        const std::string normalized = ToOutputNorm(filePathUtf8);
        if (ContainsNullByte(normalized))
        {
            return false;
        }

        const int fd = open(normalized.c_str(), flags);
        if (fd < 0)
        {
            return false;
        }

        close(fd);
        return true;
    }
#endif

    static bool CopyRegularFilePreserveMetadata(const std::string& srcFilePathUtf8, const std::string& dstFilePathUtf8, bool overwriteIfExists)
    {
        if (srcFilePathUtf8.empty() || dstFilePathUtf8.empty())
        {
            return false;
        }

#if defined(_WIN32)
        const std::wstring srcW = Utf8ToWide(srcFilePathUtf8);
        const std::wstring dstW = Utf8ToWide(dstFilePathUtf8);
        if (srcW.empty() || dstW.empty())
        {
            return false;
        }

        if (overwriteIfExists && !ClearOverwriteBlockingAttributesIfNeeded(dstW))
        {
            return false;
        }

        DWORD copyFlags = COPY_FILE_RESTARTABLE;
        if (!overwriteIfExists)
        {
            copyFlags |= COPY_FILE_FAIL_IF_EXISTS;
        }

        return CopyFileExW(srcW.c_str(), dstW.c_str(), nullptr, nullptr, nullptr, copyFlags) != 0;
#else
        const std::string srcNormalized = ToOutputNorm(srcFilePathUtf8);
        const std::string dstNormalized = ToOutputNorm(dstFilePathUtf8);
        if (ContainsNullByte(srcNormalized) || ContainsNullByte(dstNormalized))
        {
            return false;
        }

        struct stat srcStat;
        if (lstat(srcNormalized.c_str(), &srcStat) != 0 || !S_ISREG(srcStat.st_mode))
        {
            return false;
        }

        struct stat dstStat;
        if (lstat(dstNormalized.c_str(), &dstStat) == 0)
        {
            if (srcStat.st_dev == dstStat.st_dev && srcStat.st_ino == dstStat.st_ino)
            {
                return true;
            }
            if (!overwriteIfExists || !S_ISREG(dstStat.st_mode))
            {
                return false;
            }
            if ((dstStat.st_mode & S_IWUSR) == 0)
            {
                chmod(dstNormalized.c_str(), dstStat.st_mode | S_IWUSR);
            }
        }

        const int srcFd = open(srcNormalized.c_str(), O_RDONLY);
        if (srcFd < 0)
        {
            return false;
        }

        struct stat openedSrcStat;
        if (fstat(srcFd, &openedSrcStat) != 0 || !S_ISREG(openedSrcStat.st_mode))
        {
            close(srcFd);
            return false;
        }
        if (openedSrcStat.st_dev != srcStat.st_dev || openedSrcStat.st_ino != srcStat.st_ino)
        {
            close(srcFd);
            return false;
        }

        int dstFlags = O_WRONLY | O_CREAT;
        if (overwriteIfExists)
        {
            dstFlags |= O_TRUNC;
        }
        else
        {
            dstFlags |= O_EXCL;
        }

        const int dstFd = open(dstNormalized.c_str(), dstFlags, srcStat.st_mode & 07777);
        if (dstFd < 0)
        {
            close(srcFd);
            return false;
        }

        std::vector<char> buffer;
        buffer.resize(4 * 1024 * 1024);

        bool ok = true;
        for (;;)
        {
            const ssize_t bytesRead = read(srcFd, buffer.data(), buffer.size());
            if (bytesRead < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                ok = false;
                break;
            }
            if (bytesRead == 0)
            {
                break;
            }

            ssize_t totalWritten = 0;
            while (totalWritten < bytesRead)
            {
                const ssize_t bytesWritten = write(dstFd, buffer.data() + totalWritten, static_cast<size_t>(bytesRead - totalWritten));
                if (bytesWritten < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    ok = false;
                    break;
                }
                if (bytesWritten == 0)
                {
                    ok = false;
                    break;
                }
                totalWritten += bytesWritten;
            }
            if (!ok)
            {
                break;
            }
        }

        if (ok && fchmod(dstFd, srcStat.st_mode & 07777) != 0)
        {
            ok = false;
        }

        if (close(srcFd) != 0)
        {
            ok = false;
        }
        if (close(dstFd) != 0)
        {
            ok = false;
        }

        if (!ok)
        {
            unlink(dstNormalized.c_str());
            return false;
        }

        timeval times[2];
        times[0].tv_sec = srcStat.st_atime;
        times[0].tv_usec = 0;
        times[1].tv_sec = srcStat.st_mtime;
        times[1].tv_usec = 0;
        utimes(dstNormalized.c_str(), times);
        return true;
#endif
    }

    static bool CopyDirectoryMetadata(const std::string& srcDirPathUtf8, const std::string& dstDirPathUtf8)
    {
#if defined(_WIN32)
        const std::wstring srcW = Utf8ToWide(srcDirPathUtf8);
        const std::wstring dstW = Utf8ToWide(dstDirPathUtf8);
        if (srcW.empty() || dstW.empty())
        {
            return false;
        }

        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(srcW.c_str(), GetFileExInfoStandard, &data))
        {
            return false;
        }

        const HANDLE handle = CreateFileW(dstW.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        const BOOL setTimeOk = SetFileTime(handle, &data.ftCreationTime, &data.ftLastAccessTime, &data.ftLastWriteTime);
        CloseHandle(handle);
        if (!setTimeOk)
        {
            return false;
        }

        const DWORD attrs = data.dwFileAttributes;
        return SetSettableFileAttributes(dstW, attrs);
#else
        const std::string srcNormalized = ToOutputNorm(srcDirPathUtf8);
        const std::string dstNormalized = ToOutputNorm(dstDirPathUtf8);
        if (ContainsNullByte(srcNormalized) || ContainsNullByte(dstNormalized))
        {
            return false;
        }

        struct stat srcStat;
        if (lstat(srcNormalized.c_str(), &srcStat) != 0 || !S_ISDIR(srcStat.st_mode))
        {
            return false;
        }

        timeval times[2];
        times[0].tv_sec = srcStat.st_atime;
        times[0].tv_usec = 0;
        times[1].tv_sec = srcStat.st_mtime;
        times[1].tv_usec = 0;
        if (utimes(dstNormalized.c_str(), times) != 0)
        {
            return false;
        }

        return chmod(dstNormalized.c_str(), srcStat.st_mode & 07777) == 0;
#endif
    }

#if !defined(_WIN32)
    static bool CopySymbolicLink(const std::string& srcPathUtf8, const std::string& dstPathUtf8, bool overwriteIfExists)
    {
        const std::string srcNormalized = ToOutputNorm(srcPathUtf8);
        const std::string dstNormalized = ToOutputNorm(dstPathUtf8);
        if (ContainsNullByte(srcNormalized) || ContainsNullByte(dstNormalized))
        {
            return false;
        }

        std::vector<char> buffer;
        buffer.resize(512);
        for (;;)
        {
            const ssize_t len = readlink(srcNormalized.c_str(), buffer.data(), buffer.size());
            if (len < 0)
            {
                return false;
            }
            if (static_cast<size_t>(len) < buffer.size())
            {
                buffer[static_cast<size_t>(len)] = '\0';
                break;
            }
            buffer.resize(buffer.size() * 2);
            if (buffer.size() > 1024 * 1024)
            {
                return false;
            }
        }

        if (GB_GetFileType(dstPathUtf8) != GB_FileType::NotExists)
        {
            if (!overwriteIfExists || !DeletePathAnyType(dstPathUtf8))
            {
                return false;
            }
        }

        return symlink(buffer.data(), dstNormalized.c_str()) == 0;
    }
#endif

    static bool CopyPathRecursive(const std::string& srcPathUtf8, const std::string& dstPathUtf8, bool overwriteIfExists);
    static bool CopyPathToFinalReplacing(const std::string& srcPathUtf8, const std::string& dstPathUtf8, GB_FileType srcType, bool overwriteIfExists);

    static bool CopyDirectoryRecursive(const std::string& srcDirPathUtf8, const std::string& dstDirPathUtf8, bool overwriteIfExists)
    {
        if (srcDirPathUtf8.empty() || dstDirPathUtf8.empty())
        {
            return false;
        }

        if (IsSubPathLexically(srcDirPathUtf8, dstDirPathUtf8) || IsSamePathLexically(srcDirPathUtf8, dstDirPathUtf8))
        {
            return false;
        }

        bool dstCreatedByThisCall = false;
        if (GB_GetFileType(dstDirPathUtf8) == GB_FileType::NotExists)
        {
#if defined(_WIN32)
            const std::wstring dstW = Utf8ToWide(dstDirPathUtf8);
            if (dstW.empty())
            {
                return false;
            }
            if (!CreateDirectoryW(dstW.c_str(), nullptr))
            {
                return false;
            }
#else
            const std::string dstNormalized = ToOutputNorm(dstDirPathUtf8);
            struct stat srcStat;
            const std::string srcNormalized = ToOutputNorm(srcDirPathUtf8);
            if (lstat(srcNormalized.c_str(), &srcStat) != 0)
            {
                return false;
            }

            // 先以“所有者可写可进入”的权限创建目标目录，避免源目录本身为只读权限时，
            // 目标目录刚创建出来就无法继续写入子项；全部复制完成后再恢复源目录权限与时间戳。
            const mode_t createMode = static_cast<mode_t>((srcStat.st_mode | S_IRWXU) & 07777);
            if (mkdir(dstNormalized.c_str(), createMode) != 0)
            {
                return false;
            }
#endif
            dstCreatedByThisCall = true;
        }
        else
        {
            if (!overwriteIfExists || !IsExistingDirectoryPath(dstDirPathUtf8))
            {
                return false;
            }
        }

        bool ok = true;
#if defined(_WIN32)
        const std::string srcDirWithSlash = EnsureTrailingSlash(srcDirPathUtf8);
        const std::string dstDirWithSlash = EnsureTrailingSlash(dstDirPathUtf8);
        const std::wstring patternW = Utf8ToWide(srcDirWithSlash + "*");
        if (patternW.empty())
        {
            ok = false;
        }
        else
        {
            WIN32_FIND_DATAW data;
            HANDLE find = FindFirstFileExBasicWithFallback(patternW, data);
            if (find == INVALID_HANDLE_VALUE)
            {
                const DWORD err = GetLastError();
                ok = (err == ERROR_FILE_NOT_FOUND);
            }
            else
            {
                do
                {
                    const wchar_t* name = data.cFileName;
                    if (IsDotOrDotDotW(name))
                    {
                        continue;
                    }

                    const bool isReparsePoint = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                    if (isReparsePoint)
                    {
                        // Windows 下不默认复制符号链接、联接点等重解析点，也不进入其目标目录。
                        // 这样可以避免把链接目标误复制进来，并避免目录联接造成循环遍历。
                        continue;
                    }

                    const std::string childNameUtf8 = WideToUtf8(std::wstring(name));
                    if (childNameUtf8.empty())
                    {
                        continue;
                    }
                    const std::string childSrc = srcDirWithSlash + childNameUtf8;
                    const std::string childDst = dstDirWithSlash + childNameUtf8;
                    if (!CopyPathRecursive(childSrc, childDst, overwriteIfExists))
                    {
                        ok = false;
                        break;
                    }
                } while (FindNextFileW(find, &data));

                const DWORD findErr = GetLastError();
                FindClose(find);
                if (ok && findErr != ERROR_NO_MORE_FILES)
                {
                    ok = false;
                }
            }
        }
#else
        const std::string srcDirWithSlash = EnsureTrailingSlash(srcDirPathUtf8);
        const std::string dstDirWithSlash = EnsureTrailingSlash(dstDirPathUtf8);
        DIR* dir = opendir(srcDirWithSlash.c_str());
        if (!dir)
        {
            ok = false;
        }
        else
        {
            for (;;)
            {
                errno = 0;
                struct dirent* entry = readdir(dir);
                if (!entry)
                {
                    if (errno != 0)
                    {
                        ok = false;
                    }
                    break;
                }

                const char* name = entry->d_name;
                if (IsDotOrDotDotA(name))
                {
                    continue;
                }

                const std::string childSrc = srcDirWithSlash + name;
                const std::string childDst = dstDirWithSlash + name;
                if (!CopyPathRecursive(childSrc, childDst, overwriteIfExists))
                {
                    ok = false;
                    break;
                }
            }

            if (closedir(dir) != 0)
            {
                ok = false;
            }
        }
#endif

        if (ok)
        {
            ok = CopyDirectoryMetadata(srcDirPathUtf8, dstDirPathUtf8);
        }

        if (!ok && dstCreatedByThisCall)
        {
            DeletePathAnyType(dstDirPathUtf8);
        }
        return ok;
    }

    static bool CopyPathRecursive(const std::string& srcPathUtf8, const std::string& dstPathUtf8, bool overwriteIfExists)
    {
        const GB_FileType srcType = GB_GetFileType(srcPathUtf8);
        if (srcType == GB_FileType::NotExists)
        {
            return false;
        }

        if (!IsDestinationParentDirectoryReady(dstPathUtf8))
        {
            return false;
        }

        const GB_FileType dstType = GB_GetFileType(dstPathUtf8);
        if (dstType != GB_FileType::NotExists)
        {
            if (!overwriteIfExists || !IsPathReplaceCompatible(srcType, dstPathUtf8))
            {
                return false;
            }

            if (srcType != GB_FileType::Directory)
            {
                return CopyPathToFinalReplacing(srcPathUtf8, dstPathUtf8, srcType, true);
            }
        }

        if (srcType == GB_FileType::RegularFile)
        {
            return CopyRegularFilePreserveMetadata(srcPathUtf8, dstPathUtf8, false);
        }
        if (srcType == GB_FileType::Directory)
        {
            return CopyDirectoryRecursive(srcPathUtf8, dstPathUtf8, overwriteIfExists);
        }
        if (srcType == GB_FileType::SymbolicLink)
        {
#if defined(_WIN32)
            return false;
#else
            return CopySymbolicLink(srcPathUtf8, dstPathUtf8, false);
#endif
        }

        return false;
    }

    static bool CopyPathToFinalReplacing(const std::string& srcPathUtf8, const std::string& dstPathUtf8, GB_FileType srcType, bool overwriteIfExists)
    {
        const GB_FileType dstType = GB_GetFileType(dstPathUtf8);
        if (dstType == GB_FileType::NotExists)
        {
            const std::string tempPathUtf8 = MakeMoveTempPath(dstPathUtf8);
            if (tempPathUtf8.empty())
            {
                return false;
            }

            if (!CopyPathRecursive(srcPathUtf8, tempPathUtf8, false))
            {
                DeletePathAnyType(tempPathUtf8);
                return false;
            }

            if (!SystemRenamePathNoFallback(tempPathUtf8, dstPathUtf8, false))
            {
                DeletePathAnyType(tempPathUtf8);
                return false;
            }

            return true;
        }

        if (!overwriteIfExists || !IsPathReplaceCompatible(srcType, dstPathUtf8))
        {
            return false;
        }

        const std::string tempPathUtf8 = MakeMoveTempPath(dstPathUtf8);
        if (tempPathUtf8.empty())
        {
            return false;
        }

        if (!CopyPathRecursive(srcPathUtf8, tempPathUtf8, false))
        {
            DeletePathAnyType(tempPathUtf8);
            return false;
        }

        const std::string backupPathUtf8 = MakeMoveTempPath(dstPathUtf8);
        if (backupPathUtf8.empty())
        {
            DeletePathAnyType(tempPathUtf8);
            return false;
        }

#if defined(_WIN32)
        const std::wstring dstW = Utf8ToWide(dstPathUtf8);
        if (dstW.empty() || !ClearOverwriteBlockingAttributesIfNeeded(dstW))
        {
            DeletePathAnyType(tempPathUtf8);
            return false;
        }
#endif

        if (!SystemRenamePathNoFallback(dstPathUtf8, backupPathUtf8, false))
        {
            DeletePathAnyType(tempPathUtf8);
            return false;
        }

        if (!SystemRenamePathNoFallback(tempPathUtf8, dstPathUtf8, false))
        {
            SystemRenamePathNoFallback(backupPathUtf8, dstPathUtf8, false);
            DeletePathAnyType(tempPathUtf8);
            return false;
        }

        if (!DeletePathAnyType(backupPathUtf8))
        {
            return false;
        }

        return true;
    }

    static bool MovePathByCopyThenDelete(const std::string& srcPathUtf8, const std::string& dstPathUtf8, GB_FileType srcType, bool overwriteIfExists)
    {
        if (!CopyPathToFinalReplacing(srcPathUtf8, dstPathUtf8, srcType, overwriteIfExists))
        {
            return false;
        }

        if (!DeletePathAnyType(srcPathUtf8))
        {
            return false;
        }

        return true;
    }

    static uint64_t SaturatingMultiplyUInt64(uint64_t leftValue, uint64_t rightValue)
    {
        if (leftValue == 0 || rightValue == 0)
        {
            return 0;
        }
        if (leftValue > std::numeric_limits<uint64_t>::max() / rightValue)
        {
            return std::numeric_limits<uint64_t>::max();
        }
        return leftValue * rightValue;
    }

    static bool MatchBytesAt(const GB_ByteBuffer& buffer, size_t offset, const unsigned char* bytes, size_t bytesCount)
    {
        if (bytes == nullptr || bytesCount == 0)
        {
            return false;
        }
        if (offset > buffer.size() || bytesCount > buffer.size() - offset)
        {
            return false;
        }
        return std::memcmp(buffer.data() + offset, bytes, bytesCount) == 0;
    }

    static bool MatchAsciiAt(const GB_ByteBuffer& buffer, size_t offset, const char* text)
    {
        if (text == nullptr)
        {
            return false;
        }

        const size_t len = std::strlen(text);
        return MatchBytesAt(buffer, offset, reinterpret_cast<const unsigned char*>(text), len);
    }

    static bool ContainsAsciiInFirstBytes(const GB_ByteBuffer& buffer, const std::string& needle, size_t maxBytes)
    {
        if (needle.empty() || buffer.empty())
        {
            return false;
        }

        const size_t scanSize = std::min(buffer.size(), maxBytes);
        if (scanSize < needle.size())
        {
            return false;
        }

        const auto begin = buffer.begin();
        const auto end = begin + static_cast<std::ptrdiff_t>(scanSize);
        return std::search(begin, end, needle.begin(), needle.end()) != end;
    }

    static std::string GuessIsoBmffExt(const GB_ByteBuffer& buffer)
    {
        // ISO Base Media File Format family:
        // size(4 bytes, big-endian) + 'ftyp'(4 bytes) + major_brand(4 bytes) ...
        if (buffer.size() < 12)
        {
            return "";
        }

        if (!MatchAsciiAt(buffer, 4, "ftyp"))
        {
            return "";
        }

        std::string majorBrand;
        majorBrand.resize(4);
        for (size_t i = 0; i < 4; i++)
        {
            majorBrand[i] = static_cast<char>(buffer[8 + i]);
        }

        if (majorBrand == "qt  ")
        {
            return ".mov";
        }

        // 3GPP brands
        if (majorBrand.size() >= 3 && majorBrand[0] == '3' && majorBrand[1] == 'g' && majorBrand[2] == 'p')
        {
            return ".3gp";
        }
        if (majorBrand.size() >= 3 && majorBrand[0] == '3' && majorBrand[1] == 'g' && majorBrand[2] == '2')
        {
            return ".3g2";
        }

        // HEIF/HEIC family (common brands)
        if (majorBrand == "heic" || majorBrand == "heix" || majorBrand == "hevc" || majorBrand == "hevx"
            || majorBrand == "mif1" || majorBrand == "msf1")
        {
            return ".heic";
        }

        // AVIF
        if (majorBrand == "avif" || majorBrand == "avis")
        {
            return ".avif";
        }

        return ".mp4";
    }

    static std::string GuessZipDerivedExt(const GB_ByteBuffer& buffer)
    {
        const unsigned char pk0304[] = { 0x50, 0x4B, 0x03, 0x04 };
        const unsigned char pk0506[] = { 0x50, 0x4B, 0x05, 0x06 };
        const unsigned char pk0708[] = { 0x50, 0x4B, 0x07, 0x08 };

        const bool isZip =
            MatchBytesAt(buffer, 0, pk0304, sizeof(pk0304)) ||
            MatchBytesAt(buffer, 0, pk0506, sizeof(pk0506)) ||
            MatchBytesAt(buffer, 0, pk0708, sizeof(pk0708));

        if (!isZip)
        {
            return "";
        }

        const size_t scanLimit = 64 * 1024;

        // OOXML (docx/xlsx/pptx): look for Content_Types and typical folder names
        if (ContainsAsciiInFirstBytes(buffer, "[Content_Types].xml", scanLimit))
        {
            if (ContainsAsciiInFirstBytes(buffer, "word/", scanLimit))
            {
                return ".docx";
            }
            if (ContainsAsciiInFirstBytes(buffer, "xl/", scanLimit))
            {
                return ".xlsx";
            }
            if (ContainsAsciiInFirstBytes(buffer, "ppt/", scanLimit))
            {
                return ".pptx";
            }
            return ".zip";
        }

        // Java / Android
        if (ContainsAsciiInFirstBytes(buffer, "META-INF/MANIFEST.MF", scanLimit))
        {
            return ".jar";
        }
        if (ContainsAsciiInFirstBytes(buffer, "AndroidManifest.xml", scanLimit) || ContainsAsciiInFirstBytes(buffer, "classes.dex", scanLimit))
        {
            return ".apk";
        }

        // Google Earth
        if (ContainsAsciiInFirstBytes(buffer, "doc.kml", scanLimit))
        {
            return ".kmz";
        }

        // EPUB
        if (ContainsAsciiInFirstBytes(buffer, "application/epub+zip", scanLimit))
        {
            return ".epub";
        }

        return ".zip";
    }
}

GB_FileType GB_GetFileType(const std::string& pathUtf8)
{
    if (!internal::IsValidNativePathString(pathUtf8))
    {
        return GB_FileType::NotExists;
    }

#if defined(_WIN32)
    const std::wstring pathW = internal::Utf8ToWide(pathUtf8);
    if (pathW.empty())
    {
        return GB_FileType::NotExists;
    }

    const DWORD attrs = GetFileAttributesW(pathW.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        return GB_FileType::NotExists;
    }

    return internal::GetWindowsFileTypeByAttributes(pathW, attrs);
#else
    const std::string normalized = internal::ToOutputNorm(pathUtf8);

    struct stat st;
    if (lstat(normalized.c_str(), &st) != 0)
    {
        return GB_FileType::NotExists;
    }

    return internal::GetPosixFileTypeFromMode(st.st_mode);
#endif
}

bool GB_IsPathExists(const std::string& pathUtf8)
{
    return GB_GetFileType(pathUtf8) != GB_FileType::NotExists;
}

bool GB_GetFileAttributes(const std::string& pathUtf8, GB_FileAttributes& outAttributes)
{
    outAttributes = GB_FileAttributes();
    if (!internal::IsValidNativePathString(pathUtf8))
    {
        return false;
    }

#if defined(_WIN32)
    const std::wstring pathW = internal::Utf8ToWide(pathUtf8);
    if (pathW.empty())
    {
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(pathW.c_str(), GetFileExInfoStandard, &data))
    {
        return false;
    }

    const DWORD attrs = data.dwFileAttributes;
    outAttributes.fileType = internal::GetWindowsFileTypeByAttributes(pathW, attrs);
    if (outAttributes.fileType == GB_FileType::RegularFile)
    {
        outAttributes.fileSizeByte = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | static_cast<uint64_t>(data.nFileSizeLow);
    }
    outAttributes.isReadOnly = (attrs & FILE_ATTRIBUTE_READONLY) != 0;
    outAttributes.isHidden = (attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
    outAttributes.isSystem = (attrs & FILE_ATTRIBUTE_SYSTEM) != 0;
    outAttributes.isArchive = (attrs & FILE_ATTRIBUTE_ARCHIVE) != 0;
    outAttributes.isTemporary = (attrs & FILE_ATTRIBUTE_TEMPORARY) != 0;
    outAttributes.createdTime = internal::FileTimeToDateTime(data.ftCreationTime);
    outAttributes.lastAccessTime = internal::FileTimeToDateTime(data.ftLastAccessTime);
    outAttributes.lastWriteTime = internal::FileTimeToDateTime(data.ftLastWriteTime);
    return outAttributes.fileType != GB_FileType::NotExists;
#else
    const std::string normalized = internal::ToOutputNorm(pathUtf8);

    struct stat st;
    if (lstat(normalized.c_str(), &st) != 0)
    {
        return false;
    }

    outAttributes.fileType = internal::GetPosixFileTypeFromMode(st.st_mode);
    if (outAttributes.fileType == GB_FileType::RegularFile && st.st_size > 0)
    {
        outAttributes.fileSizeByte = static_cast<uint64_t>(st.st_size);
    }
    outAttributes.isReadOnly = (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0;

    const std::string fileNameUtf8 = GB_GetFileName(normalized, true);
    outAttributes.isHidden = fileNameUtf8.size() > 1 && fileNameUtf8[0] == '.';
    outAttributes.isSystem = false;
    outAttributes.isArchive = false;
    outAttributes.isTemporary = false;
    outAttributes.createdTime = GB_DateTime::Invalid;
    outAttributes.lastAccessTime = internal::PosixTimeToDateTime(st.st_atime);
    outAttributes.lastWriteTime = internal::PosixTimeToDateTime(st.st_mtime);
    return true;
#endif
}

bool GB_SetFileAttributes(const std::string& pathUtf8, const GB_FileAttributeModifyOptions& modifyOptions)
{
    if (!internal::IsValidNativePathString(pathUtf8))
    {
        return false;
    }

#if defined(_WIN32)
    const std::wstring pathW = internal::Utf8ToWide(pathUtf8);
    if (pathW.empty())
    {
        return false;
    }

    DWORD attrs = GetFileAttributesW(pathW.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }

    const bool hasTimeChange = modifyOptions.changeCreatedTime || modifyOptions.changeLastAccessTime || modifyOptions.changeLastWriteTime;
    const bool hasAttributeChange = modifyOptions.changeReadOnly || modifyOptions.changeHidden || modifyOptions.changeSystem
        || modifyOptions.changeArchive || modifyOptions.changeTemporary;

    if (!hasTimeChange && !hasAttributeChange)
    {
        return true;
    }

    if (modifyOptions.changeReadOnly && !modifyOptions.readOnly && (attrs & FILE_ATTRIBUTE_READONLY) != 0)
    {
        const DWORD attrsWithoutReadOnly = attrs & ~FILE_ATTRIBUTE_READONLY;
        if (!internal::SetSettableFileAttributes(pathW, attrsWithoutReadOnly))
        {
            return false;
        }
        attrs = attrsWithoutReadOnly;
    }

    if (hasTimeChange)
    {
        FILETIME createdFileTime;
        FILETIME lastAccessFileTime;
        FILETIME lastWriteFileTime;
        const FILETIME* createdFileTimePtr = nullptr;
        const FILETIME* lastAccessFileTimePtr = nullptr;
        const FILETIME* lastWriteFileTimePtr = nullptr;

        if (modifyOptions.changeCreatedTime)
        {
            if (!internal::DateTimeToFileTime(modifyOptions.createdTime, createdFileTime))
            {
                return false;
            }
            createdFileTimePtr = &createdFileTime;
        }
        if (modifyOptions.changeLastAccessTime)
        {
            if (!internal::DateTimeToFileTime(modifyOptions.lastAccessTime, lastAccessFileTime))
            {
                return false;
            }
            lastAccessFileTimePtr = &lastAccessFileTime;
        }
        if (modifyOptions.changeLastWriteTime)
        {
            if (!internal::DateTimeToFileTime(modifyOptions.lastWriteTime, lastWriteFileTime))
            {
                return false;
            }
            lastWriteFileTimePtr = &lastWriteFileTime;
        }

        const HANDLE handle = CreateFileW(pathW.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        const BOOL ok = SetFileTime(handle, createdFileTimePtr, lastAccessFileTimePtr, lastWriteFileTimePtr);
        CloseHandle(handle);
        if (!ok)
        {
            return false;
        }
    }

    if (hasAttributeChange)
    {
        DWORD newAttrs = attrs;
        if (modifyOptions.changeReadOnly)
        {
            newAttrs = modifyOptions.readOnly ? (newAttrs | FILE_ATTRIBUTE_READONLY) : (newAttrs & ~FILE_ATTRIBUTE_READONLY);
        }
        if (modifyOptions.changeHidden)
        {
            newAttrs = modifyOptions.hidden ? (newAttrs | FILE_ATTRIBUTE_HIDDEN) : (newAttrs & ~FILE_ATTRIBUTE_HIDDEN);
        }
        if (modifyOptions.changeSystem)
        {
            newAttrs = modifyOptions.system ? (newAttrs | FILE_ATTRIBUTE_SYSTEM) : (newAttrs & ~FILE_ATTRIBUTE_SYSTEM);
        }
        if (modifyOptions.changeArchive)
        {
            newAttrs = modifyOptions.archive ? (newAttrs | FILE_ATTRIBUTE_ARCHIVE) : (newAttrs & ~FILE_ATTRIBUTE_ARCHIVE);
        }
        if (modifyOptions.changeTemporary)
        {
            newAttrs = modifyOptions.temporary ? (newAttrs | FILE_ATTRIBUTE_TEMPORARY) : (newAttrs & ~FILE_ATTRIBUTE_TEMPORARY);
        }

        if (newAttrs != attrs)
        {
            if (!internal::SetSettableFileAttributes(pathW, newAttrs))
            {
                return false;
            }
        }
    }

    return true;
#else
    if (modifyOptions.changeHidden || modifyOptions.changeSystem || modifyOptions.changeArchive
        || modifyOptions.changeTemporary || modifyOptions.changeCreatedTime)
    {
        return false;
    }

    const bool hasTimeChange = modifyOptions.changeLastAccessTime || modifyOptions.changeLastWriteTime;
    const bool hasAttributeChange = modifyOptions.changeReadOnly;
    if (!hasTimeChange && !hasAttributeChange)
    {
        return GB_GetFileType(pathUtf8) != GB_FileType::NotExists;
    }

    const std::string normalized = internal::ToOutputNorm(pathUtf8);

    struct stat st;
    if (lstat(normalized.c_str(), &st) != 0)
    {
        return false;
    }

    if (S_ISLNK(st.st_mode))
    {
        return false;
    }

    if (modifyOptions.changeReadOnly)
    {
        mode_t newMode = st.st_mode;
        if (modifyOptions.readOnly)
        {
            newMode &= static_cast<mode_t>(~(S_IWUSR | S_IWGRP | S_IWOTH));
        }
        else
        {
            newMode |= S_IWUSR;
        }

        if (newMode != st.st_mode)
        {
            if (chmod(normalized.c_str(), newMode) != 0)
            {
                return false;
            }
            st.st_mode = newMode;
        }
    }

    if (hasTimeChange)
    {
        timeval times[2];
        times[0].tv_sec = st.st_atime;
        times[0].tv_usec = 0;
        times[1].tv_sec = st.st_mtime;
        times[1].tv_usec = 0;

        if (modifyOptions.changeLastAccessTime)
        {
            if (!internal::DateTimeToTimeval(modifyOptions.lastAccessTime, times[0]))
            {
                return false;
            }
        }
        if (modifyOptions.changeLastWriteTime)
        {
            if (!internal::DateTimeToTimeval(modifyOptions.lastWriteTime, times[1]))
            {
                return false;
            }
        }

        if (utimes(normalized.c_str(), times) != 0)
        {
            return false;
        }
    }

    return true;
#endif
}

bool GB_CanReadFile(const std::string& filePathUtf8)
{
    if (!GB_IsFileExists(filePathUtf8))
    {
        return false;
    }

#if defined(_WIN32)
    return internal::TryOpenFileWithAccess(filePathUtf8, GENERIC_READ);
#else
    return internal::TryOpenFileWithFlags(filePathUtf8, O_RDONLY);
#endif
}

bool GB_CanWriteFile(const std::string& filePathUtf8)
{
    if (!GB_IsFileExists(filePathUtf8))
    {
        return false;
    }

#if defined(_WIN32)
    return internal::TryOpenFileWithAccess(filePathUtf8, GENERIC_WRITE);
#else
    return internal::TryOpenFileWithFlags(filePathUtf8, O_WRONLY);
#endif
}

bool GB_CanReadWriteFile(const std::string& filePathUtf8)
{
    if (!GB_IsFileExists(filePathUtf8))
    {
        return false;
    }

#if defined(_WIN32)
    return internal::TryOpenFileWithAccess(filePathUtf8, GENERIC_READ | GENERIC_WRITE);
#else
    return internal::TryOpenFileWithFlags(filePathUtf8, O_RDWR);
#endif
}

bool GB_RenamePath(const std::string& pathUtf8, const std::string& newNameUtf8, bool overwriteIfExists)
{
    if (!internal::IsValidNativePathString(pathUtf8) || !internal::IsValidNativePathString(newNameUtf8))
    {
        return false;
    }
    if (newNameUtf8 == "." || newNameUtf8 == ".." || internal::ContainsPathSlash(newNameUtf8))
    {
        return false;
    }

    const std::string trimmedPath = internal::StripTrailingSlashesButKeepRoot(pathUtf8);
    if (trimmedPath.empty() || internal::IsAbsoluteRootPath(trimmedPath))
    {
        return false;
    }

    const std::string dirPathUtf8 = GB_GetDirectoryPath(trimmedPath);
    const std::string dstPathUtf8 = dirPathUtf8.empty() ? newNameUtf8 : (dirPathUtf8 + newNameUtf8);
    return GB_MovePath(trimmedPath, dstPathUtf8, overwriteIfExists);
}

bool GB_MovePath(const std::string& srcPathUtf8, const std::string& dstPathUtf8, bool overwriteIfExists)
{
    if (!internal::IsValidNativePathString(srcPathUtf8) || !internal::IsValidNativePathString(dstPathUtf8))
    {
        return false;
    }

    const std::string srcTrimmed = internal::StripTrailingSlashesButKeepRoot(srcPathUtf8);
    const std::string dstTrimmed = internal::StripTrailingSlashesButKeepRoot(dstPathUtf8);
    if (srcTrimmed.empty() || dstTrimmed.empty())
    {
        return false;
    }
    if (internal::IsAbsoluteRootPath(srcTrimmed))
    {
        return false;
    }

    const GB_FileType srcType = GB_GetFileType(srcTrimmed);
    if (srcType == GB_FileType::NotExists)
    {
        return false;
    }
    if (internal::IsSamePathLexically(srcTrimmed, dstTrimmed))
    {
        return true;
    }

    bool isSameExistingPath = false;
    if (internal::TryCheckSameExistingPath(srcTrimmed, dstTrimmed, isSameExistingPath) && isSameExistingPath)
    {
        return true;
    }

    if (!internal::IsDestinationParentDirectoryReady(dstTrimmed))
    {
        return false;
    }

    if (srcType == GB_FileType::Directory && internal::IsSubPathLexically(srcTrimmed, dstTrimmed))
    {
        return false;
    }

    if (!overwriteIfExists && GB_GetFileType(dstTrimmed) != GB_FileType::NotExists)
    {
        return false;
    }

    if (!internal::IsPathReplaceCompatible(srcType, dstTrimmed))
    {
        return false;
    }

    const bool allowNativeCopyAcrossVolume =
#if defined(_WIN32)
        (srcType == GB_FileType::RegularFile);
#else
        false;
#endif

    if (internal::SystemRenamePathNoFallback(srcTrimmed, dstTrimmed, overwriteIfExists, allowNativeCopyAcrossVolume))
    {
        return true;
    }

    // 原生移动失败后，退化为“复制到目标路径 + 删除源路径”。
    // 这主要用于跨分区/跨文件系统场景，同时也能覆盖 Windows 目录跨卷移动不支持的问题。
    if (srcType == GB_FileType::Other)
    {
        return false;
    }

    return internal::MovePathByCopyThenDelete(srcTrimmed, dstTrimmed, srcType, overwriteIfExists);
}

std::string GB_GetCurrentDirectory()
{
#if defined(_WIN32)
    const DWORD required = GetCurrentDirectoryW(0, nullptr);
    if (required == 0)
    {
        return "";
    }

    std::wstring buffer;
    buffer.resize(static_cast<size_t>(required), L'\0');
    const DWORD written = GetCurrentDirectoryW(required, &buffer[0]);
    if (written == 0 || written >= required)
    {
        return "";
    }

    buffer.resize(static_cast<size_t>(written));
    return internal::NormalizeDirectoryPathUtf8(GB_WStringToUtf8(buffer));
#else
    std::vector<char> buffer;
    buffer.resize(512);

    for (;;)
    {
        if (getcwd(buffer.data(), buffer.size()) != nullptr)
        {
            return internal::NormalizeDirectoryPathUtf8(std::string(buffer.data()));
        }
        if (errno != ERANGE)
        {
            return "";
        }
        if (buffer.size() > 65536)
        {
            return "";
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

std::string GB_GetShortcutTargetPath(const std::string& shortcutPathUtf8)
{
    if (shortcutPathUtf8.empty())
    {
        return "";
    }

#if defined(_WIN32)
    const std::wstring shortcutPathW = internal::Utf8ToWide(shortcutPathUtf8);
    if (shortcutPathW.empty())
    {
        return "";
    }

    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE)
    {
        return "";
    }

    const bool needCoUninitialize = SUCCEEDED(initHr);
    IShellLinkW* shellLink = nullptr;
    IPersistFile* persistFile = nullptr;
    std::string result;

    do
    {
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&shellLink));
        if (FAILED(hr) || !shellLink)
        {
            break;
        }

        hr = shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistFile));
        if (FAILED(hr) || !persistFile)
        {
            break;
        }

        hr = persistFile->Load(shortcutPathW.c_str(), STGM_READ);
        if (FAILED(hr))
        {
            break;
        }

        std::vector<wchar_t> targetBuffer;
        targetBuffer.resize(32768, L'\0');
        WIN32_FIND_DATAW findData;
        std::memset(&findData, 0, sizeof(findData));

        hr = shellLink->GetPath(targetBuffer.data(), static_cast<int>(targetBuffer.size()), &findData, SLGP_RAWPATH);
        if (FAILED(hr) || targetBuffer[0] == L'\0')
        {
            std::fill(targetBuffer.begin(), targetBuffer.end(), L'\0');
            std::memset(&findData, 0, sizeof(findData));
            hr = shellLink->GetPath(targetBuffer.data(), static_cast<int>(targetBuffer.size()), &findData, 0);
        }
        if (FAILED(hr) || targetBuffer[0] == L'\0')
        {
            break;
        }

        std::wstring targetPathW(targetBuffer.data());
        std::wstring expandedTargetPathW;
        if (targetPathW.find(L'%') != std::wstring::npos && internal::ExpandEnvironmentString(targetPathW, expandedTargetPathW))
        {
            targetPathW.swap(expandedTargetPathW);
        }

        std::string targetPathUtf8 = GB_WStringToUtf8(targetPathW);
        internal::ReplaceBackslashWithSlash(targetPathUtf8);
        const bool targetIsDir = internal::EndsWithSlash(targetPathUtf8) || GB_IsDirectoryExists(targetPathUtf8);
        result = internal::BuildPathString(internal::ParseAndNormalizePathLexical(targetPathUtf8), targetIsDir);
    } while (false);

    if (persistFile)
    {
        persistFile->Release();
    }
    if (shellLink)
    {
        shellLink->Release();
    }
    if (needCoUninitialize)
    {
        CoUninitialize();
    }

    return result;
#else
    return "";
#endif
}

bool GB_GetDiskSpaceInfo(const std::string& pathUtf8, GB_DiskSpaceInfo& outSpaceInfo)
{
    outSpaceInfo = GB_DiskSpaceInfo();

    if (internal::ContainsNullByte(pathUtf8))
    {
        return false;
    }

    std::string queryPathUtf8 = pathUtf8;
    if (queryPathUtf8.empty())
    {
        queryPathUtf8 = GB_GetCurrentDirectory();
    }
    if (queryPathUtf8.empty())
    {
        return false;
    }

    const GB_FileType queryFileType = GB_GetFileType(queryPathUtf8);
    if (queryFileType != GB_FileType::NotExists && !GB_IsDirectoryExists(queryPathUtf8))
    {
        queryPathUtf8 = GB_GetDirectoryPath(queryPathUtf8);
        if (queryPathUtf8.empty())
        {
            queryPathUtf8 = ".";
        }
    }
    else if (queryFileType == GB_FileType::NotExists)
    {
        std::string candidatePathUtf8 = internal::StripTrailingSlashesButKeepRoot(queryPathUtf8);
        for (;;)
        {
            std::string parentPathUtf8 = GB_GetDirectoryPath(candidatePathUtf8);
            if (parentPathUtf8.empty())
            {
                candidatePathUtf8 = ".";
                break;
            }

            parentPathUtf8 = internal::StripTrailingSlashesButKeepRoot(parentPathUtf8);
            if (parentPathUtf8.empty() || parentPathUtf8 == candidatePathUtf8)
            {
                candidatePathUtf8 = parentPathUtf8.empty() ? "." : parentPathUtf8;
                break;
            }

            if (GB_GetFileType(parentPathUtf8) != GB_FileType::NotExists)
            {
                candidatePathUtf8 = parentPathUtf8;
                break;
            }
            candidatePathUtf8 = parentPathUtf8;
        }
        queryPathUtf8 = candidatePathUtf8;
    }

#if defined(_WIN32)
    const std::wstring pathW = internal::Utf8ToWide(queryPathUtf8);
    if (pathW.empty())
    {
        return false;
    }

    ULARGE_INTEGER availableSpace;
    ULARGE_INTEGER totalSpace;
    ULARGE_INTEGER freeSpace;
    if (!GetDiskFreeSpaceExW(pathW.c_str(), &availableSpace, &totalSpace, &freeSpace))
    {
        return false;
    }

    outSpaceInfo.availableSpaceByte = static_cast<uint64_t>(availableSpace.QuadPart);
    outSpaceInfo.freeSpaceByte = static_cast<uint64_t>(freeSpace.QuadPart);
    outSpaceInfo.totalSpaceByte = static_cast<uint64_t>(totalSpace.QuadPart);
    return true;
#else
    const std::string normalized = internal::ToOutputNorm(queryPathUtf8);

    struct statvfs spaceInfo;
    if (statvfs(normalized.c_str(), &spaceInfo) != 0)
    {
        return false;
    }

    const uint64_t fragmentSize = static_cast<uint64_t>(spaceInfo.f_frsize != 0 ? spaceInfo.f_frsize : spaceInfo.f_bsize);
    outSpaceInfo.availableSpaceByte = internal::SaturatingMultiplyUInt64(static_cast<uint64_t>(spaceInfo.f_bavail), fragmentSize);
    outSpaceInfo.freeSpaceByte = internal::SaturatingMultiplyUInt64(static_cast<uint64_t>(spaceInfo.f_bfree), fragmentSize);
    outSpaceInfo.totalSpaceByte = internal::SaturatingMultiplyUInt64(static_cast<uint64_t>(spaceInfo.f_blocks), fragmentSize);
    return true;
#endif
}

bool GB_IsFileExists(const std::string& filePathUtf8)
{
    return GB_GetFileType(filePathUtf8) == GB_FileType::RegularFile;
}

bool GB_IsDirectoryExists(const std::string& dirPathUtf8)
{
    bool exists = false;
    bool isDir = false;
    if (!internal::IsDirByStat(dirPathUtf8, exists, isDir))
    {
        return false;
    }
    return exists && isDir;
}

bool GB_CreateDirectory(const std::string& dirPathUtf8)
{
    if (!internal::IsValidNativePathString(dirPathUtf8))
    {
        return false;
    }
    return internal::MakeDirsRecursive(internal::EnsureTrailingSlash(dirPathUtf8));
}

bool GB_IsEmptyDirectory(const std::string& dirPathUtf8)
{
    bool exists = false;
    bool isDir = false;
    if (!internal::IsDirByStat(dirPathUtf8, exists, isDir) || !exists || !isDir)
    {
        return false;
    }

#if defined(_WIN32)
    const std::string dirWithSlash = internal::EnsureTrailingSlash(dirPathUtf8);
    if (dirWithSlash.empty())
    {
        return false;
    }

    WIN32_FIND_DATAW data;
    const std::wstring patternW = internal::Utf8ToWide(dirWithSlash + "*");
    if (patternW.empty())
    {
        return false;
    }

    HANDLE find = internal::FindFirstFileExBasicWithFallback(patternW, data);
    if (find == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND)
        {
            return true;
        }
        return false;
    }

    bool empty = true;
    do
    {
        const wchar_t* name = data.cFileName;
        if (!name)
        {
            continue;
        }
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
        {
            continue;
        }
        empty = false;
        break;
    } while (FindNextFileW(find, &data));

    const DWORD findErr = empty ? GetLastError() : ERROR_NO_MORE_FILES;
    FindClose(find);
    return empty && findErr == ERROR_NO_MORE_FILES;
#else
    const std::string dirWithSlash = internal::EnsureTrailingSlash(dirPathUtf8);
    if (dirWithSlash.empty())
    {
        return false;
    }

    DIR* dir = opendir(dirWithSlash.c_str());
    if (!dir)
    {
        return false;
    }

    bool empty = true;
    errno = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr)
    {
        const char* name = entry->d_name;
        if (internal::IsDotOrDotDotA(name))
        {
            continue;
        }
        empty = false;
        break;
    }

    const int readDirErrno = errno;
    const int closeDirResult = closedir(dir);
    return empty && readDirErrno == 0 && closeDirResult == 0;
#endif
}

bool GB_DeleteDirectory(const std::string& dirPathUtf8)
{
    if (!internal::IsValidNativePathString(dirPathUtf8))
    {
        return false;
    }

    const std::string trimmedPath = internal::StripTrailingSlashesButKeepRoot(dirPathUtf8);
    if (trimmedPath.empty())
    {
        return false;
    }

    // Refuse to delete absolute roots, "." and "..".
    if (internal::IsUnsafeDeleteTargetPath(trimmedPath))
    {
        return false;
    }

#if defined(_WIN32)
    const std::wstring pathW = internal::Utf8ToWide(trimmedPath);
    if (pathW.empty())
    {
        return false;
    }

    const DWORD attrs = GetFileAttributesW(pathW.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return false;
    }

    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        // 若删除对象本身就是目录符号链接/联接点，只删除重解析点本身，绝不递归进入其目标。
        return internal::RemoveEmptyDir(trimmedPath);
    }

    if (!internal::DeleteDirContents(trimmedPath))
    {
        return false;
    }
    return internal::RemoveEmptyDir(trimmedPath);
#else
    const std::string pathNormalized = internal::ToOutputNorm(trimmedPath);
    struct stat st;
    if (lstat(pathNormalized.c_str(), &st) != 0)
    {
        return false;
    }

    if (S_ISLNK(st.st_mode))
    {
        // Do not follow symlinked directories; delete the link itself.
        return unlink(pathNormalized.c_str()) == 0;
    }

    if (!S_ISDIR(st.st_mode))
    {
        return false;
    }

    if (!internal::DeleteDirContents(trimmedPath))
    {
        return false;
    }
    return rmdir(pathNormalized.c_str()) == 0;
#endif
}

bool GB_DeleteFile(const std::string& filePathUtf8)
{
    if (GB_GetFileType(filePathUtf8) != GB_FileType::RegularFile)
    {
        return false;
    }

#if defined(_WIN32)
    return internal::DeleteOneFile(filePathUtf8);
#else
    const std::string pathNormalized = internal::ToOutputNorm(filePathUtf8);
    return unlink(pathNormalized.c_str()) == 0;
#endif
}

bool GB_DeletePath(const std::string& pathUtf8)
{
    if (!internal::IsValidNativePathString(pathUtf8))
    {
        return false;
    }

    const std::string trimmedPath = internal::StripTrailingSlashesButKeepRoot(pathUtf8);
    if (trimmedPath.empty() || internal::IsUnsafeDeleteTargetPath(trimmedPath))
    {
        return false;
    }

    return internal::DeletePathAnyType(trimmedPath);
}

bool GB_CopyFile(const std::string& srcFilePathUtf8, const std::string& dstFilePathUtf8)
{
    if (GB_GetFileType(srcFilePathUtf8) != GB_FileType::RegularFile)
    {
        return false;
    }

    return GB_CopyPath(srcFilePathUtf8, dstFilePathUtf8, true);
}

bool GB_CopyPath(const std::string& srcPathUtf8, const std::string& dstPathUtf8, bool overwriteIfExists)
{
    if (!internal::IsValidNativePathString(srcPathUtf8) || !internal::IsValidNativePathString(dstPathUtf8))
    {
        return false;
    }

    const std::string srcTrimmed = internal::StripTrailingSlashesButKeepRoot(srcPathUtf8);
    const std::string dstTrimmed = internal::StripTrailingSlashesButKeepRoot(dstPathUtf8);
    if (srcTrimmed.empty() || dstTrimmed.empty())
    {
        return false;
    }
    if (internal::IsAbsoluteRootPath(srcTrimmed))
    {
        return false;
    }

    const GB_FileType srcType = GB_GetFileType(srcTrimmed);
    if (srcType == GB_FileType::NotExists || srcType == GB_FileType::Other)
    {
        return false;
    }
    if (internal::IsSamePathLexically(srcTrimmed, dstTrimmed))
    {
        return true;
    }

    bool isSameExistingPath = false;
    if (internal::TryCheckSameExistingPath(srcTrimmed, dstTrimmed, isSameExistingPath) && isSameExistingPath)
    {
        return true;
    }

    if (srcType == GB_FileType::Directory && internal::IsSubPathLexically(srcTrimmed, dstTrimmed))
    {
        return false;
    }

    return internal::CopyPathToFinalReplacing(srcTrimmed, dstTrimmed, srcType, overwriteIfExists);
}

std::vector<std::string> GB_GetFilesList(const std::string& dirPathUtf8, bool recursive)
{
    std::vector<std::string> out;
    internal::ListFilesRecursive(dirPathUtf8, recursive, out);
    return out;
}

std::string GB_GetFileName(const std::string& filePathUtf8, bool withExt)
{
    if (filePathUtf8.empty())
    {
        return "";
    }

    const std::string trimmedPath = internal::StripTrailingSlashesButKeepRoot(filePathUtf8);
    if (trimmedPath.empty() || internal::IsRootPathString(trimmedPath))
    {
        return "";
    }

    const size_t sepPos = trimmedPath.find_last_of('/');
    const std::string fileNameWithExt = (sepPos == std::string::npos) ? trimmedPath : trimmedPath.substr(sepPos + 1);
    if (internal::IsDotOrDotDotName(fileNameWithExt))
    {
        return "";
    }

    if (withExt)
    {
        return fileNameWithExt;
    }

    const size_t dotPos = fileNameWithExt.find_last_of('.');
    if (dotPos == std::string::npos || dotPos == 0)
    {
        return fileNameWithExt;
    }

    return fileNameWithExt.substr(0, dotPos);
}

std::string GB_GetFileExt(const std::string& filePathUtf8)
{
    if (filePathUtf8.empty())
    {
        return "";
    }

    const std::string trimmedPath = internal::StripTrailingSlashesButKeepRoot(filePathUtf8);
    if (trimmedPath.empty() || internal::IsRootPathString(trimmedPath))
    {
        return "";
    }

    const size_t sepPos = trimmedPath.find_last_of('/');
    const std::string fileNameWithExt = (sepPos == std::string::npos) ? trimmedPath : trimmedPath.substr(sepPos + 1);
    if (internal::IsDotOrDotDotName(fileNameWithExt))
    {
        return "";
    }

    const size_t dotPos = fileNameWithExt.find_last_of('.');
    if (dotPos == std::string::npos || dotPos == 0 || dotPos + 1 == fileNameWithExt.size())
    {
        return "";
    }

    return fileNameWithExt.substr(dotPos);
}

std::string GB_GetDirectoryPath(const std::string& filePathUtf8)
{
    if (filePathUtf8.empty() || internal::ContainsNullByte(filePathUtf8))
    {
        return "";
    }

    const std::string trimmedPath = internal::StripTrailingSlashesButKeepRoot(filePathUtf8);
    if (trimmedPath.empty() || internal::IsRootPathString(trimmedPath))
    {
        return "";
    }

    const size_t pos = trimmedPath.find_last_of('/');
    if (pos == std::string::npos)
    {
        return "";
    }

    const std::string dir = trimmedPath.substr(0, pos + 1);
    return internal::EnsureTrailingSlash(dir);
}

size_t GB_GetFileSizeByte(const std::string& filePathUtf8)
{
    unsigned long long size64 = 0;
    if (!internal::TryGetFileSize64(filePathUtf8, size64))
    {
        return 0;
    }

    const unsigned long long maxSize = static_cast<unsigned long long>(std::numeric_limits<size_t>::max());
    if (size64 > maxSize)
    {
        return std::numeric_limits<size_t>::max();
    }

    return static_cast<size_t>(size64);
}

double GB_GetFileSizeKB(const std::string& filePathUtf8)
{
    return static_cast<double>(GB_GetFileSizeByte(filePathUtf8)) / 1024.0;
}

double GB_GetFileSizeMB(const std::string& filePathUtf8)
{
    return GB_GetFileSizeKB(filePathUtf8) / 1024.0;
}

double GB_GetFileSizeGB(const std::string& filePathUtf8)
{
    return GB_GetFileSizeMB(filePathUtf8) / 1024.0;
}

std::string GB_GetExeDirectory()
{
#if defined(_WIN32)
    std::wstring buffer;
    buffer.resize(260);

    for (;;)
    {
        const DWORD written = GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
        if (written == 0)
        {
            return "";
        }

        if (written < buffer.size())
        {
            buffer.resize(static_cast<size_t>(written));
            break;
        }

        buffer.resize(buffer.size() * 2);
        if (buffer.size() > 32768)
        {
            return "";
        }
    }

    size_t sepPos = buffer.find_last_of(L"\\/");
    if (sepPos == std::wstring::npos)
    {
        return "";
    }
    const std::wstring dirW = buffer.substr(0, sepPos + 1);
    const std::string dirUtf8 = internal::WideToUtf8(dirW);
    return internal::NormalizeDirectoryPathUtf8(dirUtf8);
#else
    std::vector<char> buffer;
    buffer.resize(512);

    ssize_t len = -1;
    for (;;)
    {
        if (buffer.size() > 65536)
        {
            return "";
        }

        len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (len < 0)
        {
            return "";
        }

        if (static_cast<size_t>(len) < buffer.size() - 1)
        {
            buffer[static_cast<size_t>(len)] = '\0';
            break;
        }

        buffer.resize(buffer.size() * 2);
    }

    std::string path(buffer.data());
    internal::ReplaceBackslashWithSlash(path);
    const size_t sepPos = path.find_last_of('/');
    if (sepPos == std::string::npos)
    {
        return "";
    }
    const std::string dirUtf8 = path.substr(0, sepPos + 1);
    return internal::NormalizeDirectoryPathUtf8(dirUtf8);
#endif
}

bool GB_CreateFileRecursive(const std::string& filePathUtf8, bool overwriteIfExists)
{
    if (!internal::IsValidNativePathString(filePathUtf8))
    {
        return false;
    }

    if (internal::EndsWithSlash(filePathUtf8))
    {
        return false;
    }

    const std::string dirPathUtf8 = GB_GetDirectoryPath(filePathUtf8);
    if (!dirPathUtf8.empty())
    {
        if (!GB_CreateDirectory(dirPathUtf8))
        {
            return false;
        }
    }

    if (!overwriteIfExists)
    {
        if (GB_IsFileExists(filePathUtf8))
        {
            return GB_GetFileSizeByte(filePathUtf8) == 0;
        }
    }

#if defined(_WIN32)
    const std::wstring fileW = internal::Utf8ToWide(filePathUtf8);
    if (fileW.empty())
    {
        return false;
    }

    if (overwriteIfExists && !internal::ClearOverwriteBlockingAttributesIfNeeded(fileW))
    {
        return false;
    }

    const DWORD disposition = overwriteIfExists ? CREATE_ALWAYS : CREATE_NEW;
    const HANDLE handle = CreateFileW(fileW.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    CloseHandle(handle);
    return true;
#else
    int flags = O_WRONLY | O_CREAT;
    if (overwriteIfExists)
    {
        flags |= O_TRUNC;
    }
    else
    {
        flags |= O_EXCL;
    }

    const std::string filePathNormalized = internal::ToOutputNorm(filePathUtf8);
    if (internal::ContainsNullByte(filePathNormalized))
    {
        return false;
    }

    const int fd = open(filePathNormalized.c_str(), flags, 0644);
    if (fd < 0)
    {
        return false;
    }

    close(fd);
    return true;
#endif
}

std::string GB_GetRelativePath(const std::string& pathAUtf8, const std::string& pathBUtf8)
{
    if (pathAUtf8.empty())
    {
        return "";
    }

    const bool aIsDir = internal::EndsWithSlash(pathAUtf8) || GB_IsDirectoryExists(pathAUtf8);
    const std::string normalizedA = internal::BuildPathString(internal::ParseAndNormalizePathLexical(pathAUtf8), aIsDir);

    std::string baseDirUtf8;
    if (pathBUtf8.empty())
    {
        baseDirUtf8 = ".";
    }
    else
    {
        const bool bIsDir = internal::IsDirectoryHint(pathBUtf8) || GB_IsDirectoryExists(pathBUtf8);
        if (bIsDir)
        {
            baseDirUtf8 = pathBUtf8;
        }
        else
        {
            baseDirUtf8 = GB_GetDirectoryPath(pathBUtf8);
            if (baseDirUtf8.empty())
            {
                baseDirUtf8 = ".";
            }
        }
    }

    const internal::PathParts target = internal::ParseAndNormalizePathLexical(pathAUtf8);
    const internal::PathParts base = internal::ParseAndNormalizePathLexical(baseDirUtf8);

    if (!internal::EqualRoot(target, base))
    {
        return normalizedA;
    }

    size_t commonCount = 0;
    while (commonCount < target.segments.size() && commonCount < base.segments.size())
    {
        if (!internal::EqualSegment(target.segments[commonCount], base.segments[commonCount]))
        {
            break;
        }
        commonCount++;
    }

    std::vector<std::string> relSegments;
    relSegments.reserve((base.segments.size() - commonCount) + (target.segments.size() - commonCount));

    for (size_t i = commonCount; i < base.segments.size(); i++)
    {
        relSegments.push_back("..");
    }
    for (size_t i = commonCount; i < target.segments.size(); i++)
    {
        relSegments.push_back(target.segments[i]);
    }

    std::string rel = internal::JoinSegmentsWithSlash(relSegments);
    if (rel.empty())
    {
        rel = ".";
    }

    if (aIsDir)
    {
        rel = internal::EnsureTrailingSlash(rel);
    }

    return rel;
}

std::string GB_JoinPath(const std::string& leftPathUtf8, const std::string& rightPathUtf8)
{
    if (rightPathUtf8.empty())
    {
        const bool leftIsDir = internal::IsDirectoryHint(leftPathUtf8) || GB_IsDirectoryExists(leftPathUtf8);
        return internal::BuildPathString(internal::ParseAndNormalizePathLexical(leftPathUtf8), leftIsDir);
    }

    const bool rightIsDirHint = internal::IsDirectoryHint(rightPathUtf8);

    const internal::PathParts rightParsed = internal::ParseAndNormalizePathLexical(rightPathUtf8);
    if (rightParsed.isAbsolute)
    {
        const bool outIsDir = rightIsDirHint || GB_IsDirectoryExists(rightPathUtf8);
        return internal::BuildPathString(rightParsed, outIsDir);
    }

    std::string baseUtf8 = leftPathUtf8;
    if (!baseUtf8.empty())
    {
        bool leftExists = false;
        bool leftIsDir = false;
        if (internal::IsDirByStat(baseUtf8, leftExists, leftIsDir) && leftExists && !leftIsDir)
        {
            baseUtf8 = GB_GetDirectoryPath(baseUtf8);
        }
    }

    const std::string baseWithSlash = internal::EnsureTrailingSlash(baseUtf8);
    const std::string combined = baseWithSlash.empty() ? rightPathUtf8 : (baseWithSlash + rightPathUtf8);

    bool outIsDir = rightIsDirHint;
    if (!outIsDir)
    {
        outIsDir = GB_IsDirectoryExists(combined);
    }

    return internal::BuildPathString(internal::ParseAndNormalizePathLexical(combined), outIsDir);
}

std::string GB_GetTempDirectory()
{
#if defined(_WIN32)
    DWORD required = internal::GetTempPathBySystemPolicyW(0, nullptr);
    if (required == 0)
    {
        return "";
    }

    for (;;)
    {
        std::vector<wchar_t> buffer;
        buffer.resize(static_cast<size_t>(required) + 1, L'\0');

        const DWORD written = internal::GetTempPathBySystemPolicyW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (written == 0)
        {
            return "";
        }
        if (written < buffer.size())
        {
            const std::wstring pathW(buffer.data(), written);
            std::string pathUtf8 = GB_WStringToUtf8(pathW);
            return internal::NormalizeDirectoryPathUtf8(pathUtf8);
        }

        required = written;
        if (required > 32768)
        {
            return "";
        }
    }
#else
    std::string tmpDir = internal::GetEnvVarUtf8("TMPDIR");
    if (tmpDir.empty())
    {
        tmpDir = "/tmp";
    }
    return internal::NormalizeDirectoryPathUtf8(tmpDir);
#endif
}

std::string GB_GetHomeDirectory()
{
#if defined(_WIN32)
    std::string pathUtf8;
    if (internal::GetKnownFolderPathUtf8(FOLDERID_Profile, pathUtf8))
    {
        return pathUtf8;
    }

    // Fallback: %USERPROFILE% or %HOMEDRIVE%%HOMEPATH%
    const std::string userProfile = internal::GetEnvVarUtf8FromWide(L"USERPROFILE");
    if (!userProfile.empty())
    {
        return internal::NormalizeDirectoryPathUtf8(userProfile);
    }

    const std::string homeDrive = internal::GetEnvVarUtf8FromWide(L"HOMEDRIVE");
    const std::string homePath = internal::GetEnvVarUtf8FromWide(L"HOMEPATH");
    if (!homeDrive.empty() && !homePath.empty())
    {
        return internal::NormalizeDirectoryPathUtf8(homeDrive + homePath);
    }

    return "";
#else
    return internal::GetHomeDirectoryUtf8_NoThrow();
#endif
}

std::string GB_GetDesktopDirectory()
{
#if defined(_WIN32)
    std::string pathUtf8;
    if (internal::GetKnownFolderPathUtf8(FOLDERID_Desktop, pathUtf8))
    {
        return pathUtf8;
    }
    const std::string homeUtf8 = GB_GetHomeDirectory();
    if (homeUtf8.empty())
    {
        return "";
    }
    return internal::NormalizeDirectoryPathUtf8(homeUtf8 + "Desktop");
#else
    const std::string homeUtf8 = GB_GetHomeDirectory();
    if (homeUtf8.empty())
    {
        return "";
    }
    const std::string fromXdg = internal::GetXdgUserDirFromConfig("XDG_DESKTOP_DIR", homeUtf8);
    if (!fromXdg.empty())
    {
        return fromXdg;
    }
    return internal::NormalizeDirectoryPathUtf8(homeUtf8 + "Desktop");
#endif
}

std::string GB_GetDownloadsDirectory()
{
#if defined(_WIN32)
    std::string pathUtf8;
    if (internal::GetKnownFolderPathUtf8(FOLDERID_Downloads, pathUtf8))
    {
        return pathUtf8;
    }
    const std::string homeUtf8 = GB_GetHomeDirectory();
    if (homeUtf8.empty())
    {
        return "";
    }
    return internal::NormalizeDirectoryPathUtf8(homeUtf8 + "Downloads");
#else
    const std::string homeUtf8 = GB_GetHomeDirectory();
    if (homeUtf8.empty())
    {
        return "";
    }
    const std::string fromXdg = internal::GetXdgUserDirFromConfig("XDG_DOWNLOAD_DIR", homeUtf8);
    if (!fromXdg.empty())
    {
        return fromXdg;
    }
    return internal::NormalizeDirectoryPathUtf8(homeUtf8 + "Downloads");
#endif
}

std::string GB_GuessFileExt(const GB_ByteBuffer& fileBytes)
{
    if (fileBytes.empty())
    {
        return "";
    }

    const size_t size = fileBytes.size();

    // ---- CAD ----
    if (size >= 6 && internal::MatchAsciiAt(fileBytes, 0, "AC10"))
    {
        // DWG 版本号通常是 "AC10xx"（ASCII）。
        return ".dwg";
    }

    // ---- Executables / objects ----
    if (size >= 4 && fileBytes[0] == 0x7F && fileBytes[1] == 'E' && fileBytes[2] == 'L' && fileBytes[3] == 'F')
    {
        return ".elf";
    }
    if (size >= 2 && fileBytes[0] == 'M' && fileBytes[1] == 'Z')
    {
        return ".exe";
    }

    // ---- Documents ----
    if (size >= 5 && fileBytes[0] == '%' && fileBytes[1] == 'P' && fileBytes[2] == 'D' && fileBytes[3] == 'F' && fileBytes[4] == '-')
    {
        return ".pdf";
    }

    // ---- Images ----
    {
        const unsigned char pngSig[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
        if (internal::MatchBytesAt(fileBytes, 0, pngSig, sizeof(pngSig)))
        {
            return ".png";
        }
    }
    if (size >= 3 && fileBytes[0] == 0xFF && fileBytes[1] == 0xD8 && fileBytes[2] == 0xFF)
    {
        return ".jpg";
    }
    if (size >= 6 && (internal::MatchAsciiAt(fileBytes, 0, "GIF87a") || internal::MatchAsciiAt(fileBytes, 0, "GIF89a")))
    {
        return ".gif";
    }
    if (size >= 2 && fileBytes[0] == 'B' && fileBytes[1] == 'M')
    {
        return ".bmp";
    }
    if (size >= 4 && ((fileBytes[0] == 'I' && fileBytes[1] == 'I' && fileBytes[2] == 0x2A && fileBytes[3] == 0x00)
        || (fileBytes[0] == 'M' && fileBytes[1] == 'M' && fileBytes[2] == 0x00 && fileBytes[3] == 0x2A)))
    {
        return ".tif";
    }
    if (size >= 4 && fileBytes[0] == 0x00 && fileBytes[1] == 0x00 && fileBytes[2] == 0x01 && fileBytes[3] == 0x00)
    {
        return ".ico";
    }

    // ---- Containers / archives ----
    {
        const std::string zipDerived = internal::GuessZipDerivedExt(fileBytes);
        if (!zipDerived.empty())
        {
            return zipDerived;
        }
    }
    if (size >= 6 && fileBytes[0] == 0x37 && fileBytes[1] == 0x7A && fileBytes[2] == 0xBC && fileBytes[3] == 0xAF
        && fileBytes[4] == 0x27 && fileBytes[5] == 0x1C)
    {
        return ".7z";
    }
    if (size >= 7 && internal::MatchAsciiAt(fileBytes, 0, "Rar!\x1A\x07"))
    {
        return ".rar";
    }
    if (size >= 2 && fileBytes[0] == 0x1F && fileBytes[1] == 0x8B)
    {
        return ".gz";
    }
    if (size >= 3 && fileBytes[0] == 'B' && fileBytes[1] == 'Z' && fileBytes[2] == 'h')
    {
        return ".bz2";
    }
    if (size >= 6 && fileBytes[0] == 0xFD && fileBytes[1] == 0x37 && fileBytes[2] == 0x7A && fileBytes[3] == 0x58
        && fileBytes[4] == 0x5A && fileBytes[5] == 0x00)
    {
        return ".xz";
    }

    // ---- Media containers ----
    {
        const std::string isobmffExt = internal::GuessIsoBmffExt(fileBytes);
        if (!isobmffExt.empty())
        {
            return isobmffExt;
        }
    }
    if (size >= 4 && fileBytes[0] == 0x1A && fileBytes[1] == 0x45 && fileBytes[2] == 0xDF && fileBytes[3] == 0xA3)
    {
        // EBML (Matroska/WebM) documents start with 1A 45 DF A3.
        const bool isWebm = internal::ContainsAsciiInFirstBytes(fileBytes, "webm", 4096);
        return isWebm ? ".webm" : ".mkv";
    }
    if (size >= 12 && internal::MatchAsciiAt(fileBytes, 0, "RIFF"))
    {
        if (internal::MatchAsciiAt(fileBytes, 8, "WAVE"))
        {
            return ".wav";
        }
        if (internal::MatchAsciiAt(fileBytes, 8, "AVI "))
        {
            return ".avi";
        }
        if (internal::MatchAsciiAt(fileBytes, 8, "WEBP"))
        {
            return ".webp";
        }
        return ".riff";
    }
    if (size >= 4 && internal::MatchAsciiAt(fileBytes, 0, "fLaC"))
    {
        return ".flac";
    }
    if (size >= 4 && internal::MatchAsciiAt(fileBytes, 0, "OggS"))
    {
        return ".ogg";
    }
    if (size >= 3 && internal::MatchAsciiAt(fileBytes, 0, "ID3"))
    {
        return ".mp3";
    }
    if (size >= 2 && fileBytes[0] == 0xFF && (fileBytes[1] & 0xE0) == 0xE0)
    {
        // MPEG audio frame sync (heuristic).
        return ".mp3";
    }

    // ---- Databases ----
    {
        const unsigned char sqliteSig[] = { 'S','Q','L','i','t','e',' ','f','o','r','m','a','t',' ','3', 0x00 };
        if (size >= sizeof(sqliteSig) && internal::MatchBytesAt(fileBytes, 0, sqliteSig, sizeof(sqliteSig)))
        {
            return ".sqlite";
        }
    }

    // ---- Text-like (heuristic) ----
    // Try detect XML/HTML by leading whitespace + '<'
    {
        const size_t scanSize = std::min<size_t>(size, 64);
        size_t index = 0;
        while (index < scanSize)
        {
            const unsigned char ch = fileBytes[index];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
            {
                index++;
                continue;
            }
            if (ch == '<')
            {
                if (internal::ContainsAsciiInFirstBytes(fileBytes, "<?xml", 256))
                {
                    return ".xml";
                }
                if (internal::ContainsAsciiInFirstBytes(fileBytes, "<html", 256) || internal::ContainsAsciiInFirstBytes(fileBytes, "<!DOCTYPE html", 256))
                {
                    return ".html";
                }
                return ".xml";
            }
            break;
        }
    }

    return "";
}

