#include "GB_FileSystem.h"
#include "GB_Utf8String.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sys/stat.h>

#if defined(_WIN32)
#    define NOMINMAX
#    include <windows.h>
#    include <shlobj.h>
#    include <KnownFolders.h>
#else
#    include <dirent.h>
#    include <fcntl.h>
#    include <pwd.h>
#    include <sys/types.h>
#    include <unistd.h>
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


    static unsigned char ToLowerAscii(unsigned char ch)
    {
        if (ch >= static_cast<unsigned char>('A') && ch <= static_cast<unsigned char>('Z'))
        {
            return static_cast<unsigned char>(ch - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
        }
        return ch;
    }

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

        // Drive root
        if (s.size() >= 2 && IsAsciiAlpha(s[0]) && s[1] == ':')
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
    static std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty())
        {
            return L"";
        }

        const std::string native = ToWindowsNative(utf8);
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
        return out;
    }

    static std::string WideToUtf8(const std::wstring& wide)
    {
        if (wide.empty())
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

        const DWORD attrs = GetFileAttributesW(pathW.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            return false;
        }
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return false;
        }

        const HANDLE handle = CreateFileW(pathW.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        LARGE_INTEGER size = {};
        const BOOL ok = GetFileSizeEx(handle, &size);
        CloseHandle(handle);
        if (!ok)
        {
            return false;
        }

        if (size.QuadPart < 0)
        {
            return false;
        }

        outSize = static_cast<unsigned long long>(size.QuadPart);
        return true;
#else
        const std::string normalized = ToOutputNorm(filePathUtf8);

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

    static void ClearReadOnlyAttributeIfNeeded(const std::wstring& pathW)
    {
        if (pathW.empty())
        {
            return;
        }

        const DWORD attrs = GetFileAttributesW(pathW.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            return;
        }
        if ((attrs & FILE_ATTRIBUTE_READONLY) == 0)
        {
            return;
        }

        SetFileAttributesW(pathW.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
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
        HANDLE find = FindFirstFileW(patternW.c_str(), &data);
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
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
            {
                continue;
            }

            const std::string child = dirWithSlash + WideToUtf8(std::wstring(name));
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

        FindClose(find);
        return true;
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

        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr)
        {
            const char* name = entry->d_name;
            if (!name)
            {
                continue;
            }
            if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
            {
                continue;
            }

            const std::string fullPath = dirWithSlash + name;
            struct stat st;
            if (lstat(fullPath.c_str(), &st) != 0)
            {
                continue;
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

        closedir(dir);
        return true;
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
        HANDLE find = FindFirstFileExW(patternW.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
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
            if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
            {
                continue;
            }

            const std::string item = dirWithSlash + WideToUtf8(std::wstring(name));
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
            if (!name)
            {
                continue;
            }
            if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
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
#endif

    static bool MatchBytesAt(const GB_ByteBuffer& buffer, size_t offset, const unsigned char* bytes, size_t bytesCount)
    {
        if (bytes == nullptr || bytesCount == 0)
        {
            return false;
        }
        if (buffer.size() < offset + bytesCount)
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

bool GB_IsFileExists(const std::string& filePathUtf8)
{
    bool exists = false;
    bool isDir = false;
    if (!internal::IsDirByStat(filePathUtf8, exists, isDir))
    {
        return false;
    }
    return exists && !isDir;
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
    if (dirPathUtf8.empty())
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

    HANDLE find = FindFirstFileW(patternW.c_str(), &data);
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

    FindClose(find);
    return empty;
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
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr)
    {
        const char* name = entry->d_name;
        if (!name)
        {
            continue;
        }
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
        {
            continue;
        }
        empty = false;
        break;
    }

    closedir(dir);
    return empty;
#endif
}

bool GB_DeleteDirectory(const std::string& dirPathUtf8)
{
    if (dirPathUtf8.empty())
    {
        return false;
    }

    const std::string trimmedPath = internal::StripTrailingSlashesButKeepRoot(dirPathUtf8);
    if (trimmedPath.empty())
    {
        return false;
    }

    // Refuse to delete absolute roots: "/", "C:/", "//server/share/".
    if (internal::IsAbsoluteRootPath(trimmedPath))
    {
        return false;
    }

#if defined(_WIN32)
    bool exists = false;
    bool isDir = false;
    if (!internal::IsDirByStat(trimmedPath, exists, isDir) || !exists || !isDir)
    {
        return false;
    }

    if (!internal::DeleteDirContents(trimmedPath))
    {
        return false;
    }
    return internal::RemoveEmptyDir(trimmedPath);
#else
    struct stat st;
    if (lstat(trimmedPath.c_str(), &st) != 0)
    {
        return false;
    }

    if (S_ISLNK(st.st_mode))
    {
        // Do not follow symlinked directories; delete the link itself.
        const std::string pathNormalized = internal::ToOutputNorm(trimmedPath);
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
    return rmdir(trimmedPath.c_str()) == 0;
#endif
}

bool GB_DeleteFile(const std::string& filePathUtf8)
{
    if (!GB_IsFileExists(filePathUtf8))
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

bool GB_CopyFile(const std::string& srcFilePathUtf8, const std::string& dstFilePathUtf8)
{
    if (!GB_IsFileExists(srcFilePathUtf8))
    {
        return false;
    }

    if (dstFilePathUtf8.empty())
    {
        return false;
    }

#if defined(_WIN32)
    const std::wstring srcW = internal::Utf8ToWide(srcFilePathUtf8);
    const std::wstring dstW = internal::Utf8ToWide(dstFilePathUtf8);
    if (srcW.empty() || dstW.empty())
    {
        return false;
    }

    return CopyFileW(srcW.c_str(), dstW.c_str(), FALSE) != 0;
#else
    const std::string srcNormalized = internal::ToOutputNorm(srcFilePathUtf8);

    std::FILE* src = std::fopen(srcNormalized.c_str(), "rb");
    if (!src)
    {
        return false;
    }

    const std::string dstNormalized = internal::ToOutputNorm(dstFilePathUtf8);

    std::FILE* dst = std::fopen(dstNormalized.c_str(), "wb");
    if (!dst)
    {
        std::fclose(src);
        return false;
    }

    std::vector<char> buffer;
    buffer.resize(1024 * 1024);

    bool ok = true;
    for (;;)
    {
        const size_t bytesRead = std::fread(buffer.data(), 1, buffer.size(), src);
        if (bytesRead > 0)
        {
            const size_t bytesWritten = std::fwrite(buffer.data(), 1, bytesRead, dst);
            if (bytesWritten != bytesRead)
            {
                ok = false;
                break;
            }
        }

        if (bytesRead < buffer.size())
        {
            if (std::ferror(src))
            {
                ok = false;
            }
            break;
        }
    }

    std::fclose(src);
    std::fclose(dst);
    return ok;
#endif
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
    if (trimmedPath.empty())
    {
        return "";
    }

    const size_t sepPos = trimmedPath.find_last_of('/');
    const std::string fileNameWithExt = (sepPos == std::string::npos) ? trimmedPath : trimmedPath.substr(sepPos + 1);

    if (withExt)
    {
        return fileNameWithExt;
    }

    const size_t dotPos = fileNameWithExt.find_last_of('.');
    if (dotPos == std::string::npos)
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
    if (trimmedPath.empty())
    {
        return "";
    }

    const size_t sepPos = trimmedPath.find_last_of('/');
    const size_t dotPos = trimmedPath.find_last_of('.');
    if (dotPos == std::string::npos)
    {
        return "";
    }
    if (sepPos != std::string::npos && dotPos <= sepPos)
    {
        return "";
    }

    return trimmedPath.substr(dotPos);
}

std::string GB_GetDirectoryPath(const std::string& filePathUtf8)
{
    if (filePathUtf8.empty())
    {
        return "";
    }

    const size_t pos = filePathUtf8.find_last_of("/\\");
    if (pos == std::string::npos)
    {
        return "";
    }

    std::string dir = filePathUtf8.substr(0, pos + 1);
    internal::ReplaceBackslashWithSlash(dir);
    return dir;
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
    if (filePathUtf8.empty())
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
    const DWORD required = GetTempPathW(0, nullptr);
    if (required == 0)
    {
        return "";
    }

    std::vector<wchar_t> buffer;
    buffer.resize(static_cast<size_t>(required) + 1, L'\0');
    const DWORD written = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (written == 0 || written >= buffer.size())
    {
        return "";
    }

    const std::wstring pathW(buffer.data(), written);
    std::string pathUtf8 = GB_WStringToUtf8(pathW);
    return internal::NormalizeDirectoryPathUtf8(pathUtf8);
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

