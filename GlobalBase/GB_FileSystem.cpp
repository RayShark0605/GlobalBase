#include "GB_FileSystem.h"
#include "GB_Utf8String.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <sys/stat.h>

#if defined(_WIN32)
#   define NOMINMAX
#   include <windows.h>
#   include <io.h>
#else
#   include <dirent.h>
#   include <unistd.h>
#   include <fcntl.h>
#   include <sys/types.h>
#   include <utime.h>
#endif

using namespace std;

namespace internal
{
    static inline bool IsSlash(char ch)
    {
        return ch == '/' || ch == '\\';
    }

    // 将任意路径分隔符转为指定分隔符
    static string ReplaceSlashes(const string& p, char toSlash)
    {
        string s = p;
        for (size_t i = 0; i < s.size(); i++)
        {
            if (IsSlash(s[i]))
            {
                s[i] = toSlash;
            }
        }
        return s;
    }

    // 统一内部输出：使用 forward slash '/'
    static string ToOutputNorm(const string& p)
    {
        return ReplaceSlashes(p, '/');
    }

    // 供 Windows WinAPI 调用：使用反斜杠
    static string ToWindowsNative(const string& p)
    {
        // 输入可能是混合分隔符，统一成反斜杠。
        return ReplaceSlashes(p, '\\');
    }

    // 去掉末尾多余的分隔符（但保留根，如 "C:/", "/"）
    static string StripTrailingSlashes(const string& in)
    {
        if (in.empty())
        {
            return in;
        }
        string s = ToOutputNorm(in);
        // 检测是否是根
#if defined(_WIN32)
        // 可能是 "C:/", "C:"（相对少见）, 或 UNC 根 "//server/share/"
        if (s.size() <= 3 && ((s.size() >= 2 && s[1] == ':') || s == "/" || s.find("//") == 0))
        {
            // 最多保留到 "C:/"
            if (s.size() == 2 && s[1] == ':')
            {
                return s + '/';
            }
            return s;
        }
#else
        if (s == "/")
        {
            return s;
        }
#endif
        while (!s.empty() && s.back() == '/')
        {
            s.pop_back();
        }
        return s;
    }

    static string EnsureTrailingSlash(const string& in)
    {
        string s = ToOutputNorm(in);
        if (s.empty())
        {
            return "/";
        }
        if (s.back() != '/')
        {
            s.push_back('/');
        }
        return s;
    }

    // 分离目录与文件名（输出用 forward slash）
    static void SplitDirBase(const string& pathUtf8, string& dirOut, string& baseOut)
    {
        string norm = ToOutputNorm(pathUtf8);
        // 特判纯目录形态（末尾带 /）
        bool endsWithSlash = !norm.empty() && norm.back() == '/';
        string s = endsWithSlash ? StripTrailingSlashes(norm) : norm;

        const size_t pos = s.find_last_of('/');
        if (pos == string::npos)
        {
            dirOut.clear();
            baseOut = s;
        }
        else
        {
            dirOut = s.substr(0, pos);
            baseOut = s.substr(pos + 1);
        }
    }

#if defined(_WIN32)
    // ---------- UTF-8 <-> UTF-16 helpers (Windows) ----------
    // 参考：MultiByteToWideChar / WideCharToMultiByte 官方文档  :contentReference[oaicite:0]{index=0}
    inline wstring Utf8ToWide(const string& s)
    {
        if (s.empty())
        {
            return wstring();
        }
        const string native = ToWindowsNative(s);
        int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, native.c_str(),
            static_cast<int>(native.size()), nullptr, 0);
        if (wlen <= 0)
        {
            return wstring();
        }
        wstring ws(static_cast<size_t>(wlen), L'\0');
        int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, native.c_str(),
            static_cast<int>(native.size()), &ws[0], wlen);
        if (written <= 0)
        {
            return wstring();
        }
        return ws;
    }

    inline string WideToUtf8(const wstring& ws)
    {
        if (ws.empty())
        {
            return string();
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
            static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0)
        {
            return string();
        }
        string out(static_cast<size_t>(len), '\0');
        int written = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
            static_cast<int>(ws.size()), &out[0], len, nullptr, nullptr);
        if (written <= 0)
        {
            return string();
        }
        // 统一输出正斜杠
        return ToOutputNorm(out);
    }
#endif

    inline bool IsDirByStat(const string& pathUtf8, bool& exists, bool& isDir)
    {
#if defined(_WIN32)
        const wstring w = Utf8ToWide(pathUtf8);
        if (w.empty())
        {
            exists = false;
            isDir = false;
            return false;
        }
        // GetFileAttributes(W)：文件或目录属性（INVALID_FILE_ATTRIBUTES 表示失败/不存在）
        // 官方：GetFileAttributesA/W 文档。
        DWORD attr = GetFileAttributesW(w.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
        {
            exists = false;
            isDir = false;
            return true;
        }
        exists = true;
        isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return true;
#else
        struct stat st;
        string native = ReplaceSlashes(pathUtf8, '/');
        if (stat(native.c_str(), &st) == 0)
        {
            exists = true;
            isDir = S_ISDIR(st.st_mode);
            return true;
        }
        exists = false;
        isDir = false;
        return true;
#endif
    }

    // 递归创建目录（逐级）
    inline bool MakeDirsRecursive(const string& dirUtf8)
    {
        if (dirUtf8.empty())
        {
            return false;
        }
        // 统一正斜杠，逐级创建
        string norm = EnsureTrailingSlash(dirUtf8);
        // 从根开始切分
        size_t start = 0;
#if defined(_WIN32)
        // 处理 "C:/..." 或 "//server/share/..."
        if (norm.size() >= 3 && norm[1] == ':' && norm[2] == '/')
        {
            start = 3; // 保留 "C:/"
        }
        else if (norm.size() >= 2 && norm[0] == '/' && norm[1] == '/')
        {
            // UNC 前缀，先定位到第三个 '/'
            size_t p = norm.find('/', 2);
            if (p != string::npos)
            {
                p = norm.find('/', p + 1);
                start = (p == string::npos) ? norm.size() : p + 1;
            }
        }
        else if (norm.size() >= 1 && norm[0] == '/')
        {
            start = 1;
        }
#else
        if (norm.size() >= 1 && norm[0] == '/')
        {
            start = 1;
        }
#endif
        for (size_t i = start; i < norm.size(); i++)
        {
            if (norm[i] == '/')
            {
                const string sub = norm.substr(0, i);
                if (sub.empty())
                {
                    continue;
                }
#if defined(_WIN32)
                wstring wsub = Utf8ToWide(sub);
                if (wsub.empty())
                {
                    return false;
                }
                DWORD attr = GetFileAttributesW(wsub.c_str());
                if (attr == INVALID_FILE_ATTRIBUTES)
                {
                    // CreateDirectoryW 仅创建最后一级；要逐级建。 :contentReference[oaicite:2]{index=2}
                    if (!CreateDirectoryW(wsub.c_str(), nullptr))
                    {
                        // 如果竞态导致已存在，继续；否则失败
                        DWORD ec = GetLastError();
                        if (ec != ERROR_ALREADY_EXISTS)
                        {
                            return false;
                        }
                    }
                }
                else if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    return false; // 同名文件阻塞
                }
#else
                if (mkdir(sub.c_str(), 0755) != 0)
                {
                    if (errno == EEXIST)
                    {
                        // 存在则继续，但需确认是目录
                        struct stat st;
                        if (stat(sub.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
                        {
                            return false;
                        }
                    }
                    else
                    {
                        // 非根时 ENOENT 说明上级没建；但我们是逐级建，一般不会触发
                        return false;
                    }
                }
#endif
            }
        }
        return true;
    }

    // 删除单个文件
    inline bool DeleteOneFile(const string& fileUtf8)
    {
#if defined(_WIN32)
        wstring w = Utf8ToWide(fileUtf8);
        if (w.empty())
        {
            return false;
        }
        if (!DeleteFileW(w.c_str()))
        {
            return false;
        }
        return true;
#else
        string native = ReplaceSlashes(fileUtf8, '/');
        return ::unlink(native.c_str()) == 0;
#endif
    }

    // 删除空目录
    inline bool RemoveEmptyDir(const string& dirUtf8)
    {
#if defined(_WIN32)
        wstring w = Utf8ToWide(dirUtf8);
        if (w.empty())
        {
            return false;
        }
        return ::RemoveDirectoryW(w.c_str()) != 0;
#else
        string native = ReplaceSlashes(dirUtf8, '/');
        return ::rmdir(native.c_str()) == 0;
#endif
    }

    // 递归列举文件（仅文件），结果用 forward slash，文件不带末尾 '/'
    inline void ListFilesRecursive(const string& dirUtf8, bool recursive, vector<string>& out)
    {
#if defined(_WIN32)
        // Windows: FindFirstFileExW + FindNextFileW  :contentReference[oaicite:3]{index=3}
        string pattern = EnsureTrailingSlash(dirUtf8) + "*";
        wstring wpat = Utf8ToWide(pattern);
        if (wpat.empty())
        {
            return;
        }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(wpat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            const wchar_t* name = fd.cFileName;
            if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0')))
            {
                continue; // skip . and ..
            }
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            string item = EnsureTrailingSlash(dirUtf8) + WideToUtf8(name);
            item = ToOutputNorm(item);
            if (isDir)
            {
                if (recursive)
                {
                    ListFilesRecursive(EnsureTrailingSlash(item), true, out);
                }
            }
            else
            {
                out.push_back(item);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
#else
        string nativeDir = EnsureTrailingSlash(dirUtf8);
        DIR* d = ::opendir(nativeDir.c_str()); // opendir/readdir 约定  :contentReference[oaicite:4]{index=4}
        if (!d)
        {
            return;
        }
        while (dirent* ent = ::readdir(d))
        {
            const char* name = ent->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            {
                continue;
            }
            string item = nativeDir + name;
            struct stat st;
            if (stat(item.c_str(), &st) != 0)
            {
                continue;
            }
            if (S_ISDIR(st.st_mode))
            {
                if (recursive)
                {
                    ListFilesRecursive(EnsureTrailingSlash(item), true, out);
                }
            }
            else if (S_ISREG(st.st_mode))
            {
                out.push_back(ToOutputNorm(item));
            }
        }
        ::closedir(d);
#endif
    }

    // 递归删除目录内容（目录本身不删），仅内部使用
    inline bool DeleteDirContents(const string& dirUtf8)
    {
#if defined(_WIN32)
        string pattern = EnsureTrailingSlash(dirUtf8) + "*";
        wstring wpat = Utf8ToWide(pattern);
        if (wpat.empty())
        {
            return false;
        }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(wpat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        bool ok = true;
        do
        {
            const wchar_t* name = fd.cFileName;
            if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0')))
            {
                continue;
            }
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            string child = EnsureTrailingSlash(dirUtf8) + WideToUtf8(name);
            child = ToOutputNorm(child);
            if (isDir)
            {
                if (!DeleteDirContents(EnsureTrailingSlash(child)))
                {
                    ok = false;
                    break;
                }
                if (!RemoveEmptyDir(child))
                {
                    ok = false;
                    break;
                }
            }
            else
            {
                if (!DeleteOneFile(child))
                {
                    ok = false;
                    break;
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return ok;
#else
        string nativeDir = EnsureTrailingSlash(dirUtf8);
        DIR* d = ::opendir(nativeDir.c_str());
        if (!d)
        {
            return false;
        }
        bool ok = true;
        while (dirent* ent = ::readdir(d))
        {
            const char* name = ent->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            {
                continue;
            }
            string child = nativeDir + name;
            struct stat st;
            if (lstat(child.c_str(), &st) != 0)
            {
                ok = false;
                break;
            }
            if (S_ISDIR(st.st_mode))
            {
                if (!DeleteDirContents(EnsureTrailingSlash(child)))
                {
                    ok = false;
                    break;
                }
                if (::rmdir(child.c_str()) != 0)
                {
                    ok = false;
                    break;
                }
            }
            else
            {
                if (::unlink(child.c_str()) != 0)
                {
                    ok = false;
                    break;
                }
            }
        }
        ::closedir(d);
        return ok;
#endif
    }

    // 平台无关：获取文件大小（字节），仅常规文件。成功返回 true 并写入 sizeOut。
    static bool TryGetFileSize64(const string& filePathUtf8, uint64_t& sizeOut)
    {
#if defined(_WIN32)
        // 先排除不存在或目录
        const wstring w = Utf8ToWide(filePathUtf8);
        if (w.empty())
        {
            return false;
        }

        const DWORD attr = GetFileAttributesW(w.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
        {
            return false;
        }
        if (attr & FILE_ATTRIBUTE_DIRECTORY)
        {
            return false; // 目录不在本 API 的定义范围内
        }

        // 以最小权限打开，不阻塞他人（共享读/写/删）
        const HANDLE h = CreateFileW(
            w.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        LARGE_INTEGER li = {};
        const BOOL ok = GetFileSizeEx(h, &li); // 64 位大小
        ::CloseHandle(h);

        if (!ok || li.QuadPart < 0)
        {
            return false;
        }
        sizeOut = static_cast<uint64_t>(li.QuadPart);
        return true;
#else
        string native = ReplaceSlashes(filePathUtf8, '/');
        struct stat st;
        if (stat(native.c_str(), &st) != 0)
        {
            return false;
        }
        if (!S_ISREG(st.st_mode))
        {
            return false; // 仅常规文件
        }
        // st_size 为 off_t，POSIX 规定表示文件字节数；符号链接时为其路径长度。我们已限制为常规文件。 :contentReference[oaicite:3]{index=3}
        if (st.st_size < 0)
        {
            return false;
        }
        sizeOut = static_cast<uint64_t>(st.st_size);
        return true;
    }
#endif
    }

    static void NormalizeToUnixDir(string& path)
    {
        for (size_t i = 0; i < path.size(); i++)
        {
            if (path[i] == '\\')
            {
                path[i] = '/';
            }
        }
        // 去掉所有尾部斜杠，最后补一个，保证“有且仅有一个”
        while (!path.empty() && path.back() == '/')
        {
            path.pop_back();
        }
        path.push_back('/');
    }

    static void ReplaceBackslashWithSlash(string& s)
    {
        for (size_t i = 0; i < s.size(); i++)
        {
            if (s[i] == '\\')
            {
                s[i] = '/';
            }
        }
    }

    struct ParsedPath
    {
        string root;                  // Windows: "c:", "//server/share"; Linux: "/"; relative: ""
        vector<string> segments; // 不包含 root 的各段
        bool isAbsolute = false;
    };

    static inline bool IsAsciiAlpha(char ch)
    {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    }

    static inline char AsciiToLower(char ch)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return ch;
    }

    static string AsciiToLowerString(const string& s)
    {
        string out = s;
        for (size_t i = 0; i < out.size(); i++)
        {
            out[i] = AsciiToLower(out[i]);
        }
        return out;
    }

    static bool EqualRoot(const string& a, const string& b)
    {
#if defined(_WIN32)
        return AsciiToLowerString(a) == AsciiToLowerString(b);
#else
        return a == b;
#endif
    }

    static bool EqualSegment(const string& a, const string& b)
    {
#if defined(_WIN32)
        // Windows 通常大小写不敏感；这里做 ASCII 级别的宽松比较（非 ASCII 字符保持原样）。
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); i++)
        {
            if (AsciiToLower(a[i]) != AsciiToLower(b[i]))
            {
                return false;
            }
        }
        return true;
#else
        return a == b;
#endif
    }

    static void SplitPathSegments(const string& path, size_t startIndex, vector<string>& segmentsOut)
    {
        segmentsOut.clear();
        size_t i = startIndex;
        while (i < path.size())
        {
            // 跳过连续 '/'
            while (i < path.size() && path[i] == '/')
            {
                i++;
            }
            if (i >= path.size())
            {
                break;
            }
            const size_t j = path.find('/', i);
            if (j == string::npos)
            {
                segmentsOut.push_back(path.substr(i));
                break;
            }
            segmentsOut.push_back(path.substr(i, j - i));
            i = j + 1;
        }
    }

    static void NormalizeSegmentsLexical(vector<string>& segments, bool isAbsolute)
    {
        vector<string> out;
        out.reserve(segments.size());
        for (size_t i = 0; i < segments.size(); i++)
        {
            const string& seg = segments[i];
            if (seg.empty() || seg == ".")
            {
                continue;
            }
            if (seg == "..")
            {
                if (!out.empty() && out.back() != "..")
                {
                    out.pop_back();
                }
                else
                {
                    // 绝对路径不能越过根；相对路径则保留 ".."
                    if (!isAbsolute)
                    {
                        out.push_back("..");
                    }
                }
                continue;
            }
            out.push_back(seg);
        }
        segments.swap(out);
    }

    static ParsedPath ParseAndNormalizePathLexical(const string& rawPathUtf8)
    {
        ParsedPath out;
        string path = ToOutputNorm(rawPathUtf8);

#if defined(_WIN32)
        // 处理 Windows 扩展前缀："\\\\?\\" -> "//?/"、"\\\\.\\" -> "//./"（在 ToOutputNorm 后）
        if (path.rfind("//?/", 0) == 0)
        {
            // "\\\\?\\UNC\\server\\share\\..." -> "//?/UNC/server/share/..."
            if (path.rfind("//?/UNC/", 0) == 0)
            {
                path = "//" + path.substr(8); // len("//?/UNC/") == 8
            }
            else
            {
                path = path.substr(4); // len("//?/") == 4
            }
        }
        else if (path.rfind("//./", 0) == 0)
        {
            path = path.substr(4);
        }

        size_t startIndex = 0;
        if (path.size() >= 2 && IsAsciiAlpha(path[0]) && path[1] == ':')
        {
            // Drive root (e.g. C:/ or C:)
            out.root = string(1, AsciiToLower(path[0])) + ":";
            out.isAbsolute = true;
            startIndex = 2;
            if (startIndex < path.size() && path[startIndex] == '/')
            {
                startIndex++;
            }
        }
        else if (path.rfind("//", 0) == 0)
        {
            // UNC root: //server/share
            size_t serverEnd = path.find('/', 2);
            if (serverEnd == string::npos)
            {
                out.root = AsciiToLowerString(path);
                out.isAbsolute = true;
                return out;
            }
            size_t shareEnd = path.find('/', serverEnd + 1);
            if (shareEnd == string::npos)
            {
                out.root = AsciiToLowerString(path);
                out.isAbsolute = true;
                return out;
            }
            out.root = AsciiToLowerString(path.substr(0, shareEnd));
            out.isAbsolute = true;
            startIndex = shareEnd + 1;
        }
        else if (!path.empty() && path[0] == '/')
        {
            out.root = "/";
            out.isAbsolute = true;
            startIndex = 1;
        }
        else
        {
            out.root.clear();
            out.isAbsolute = false;
            startIndex = 0;
        }

        SplitPathSegments(path, startIndex, out.segments);
        NormalizeSegmentsLexical(out.segments, out.isAbsolute);
        return out;
#else
        size_t startIndex = 0;
        if (!path.empty() && path[0] == '/')
        {
            out.root = "/";
            out.isAbsolute = true;
            startIndex = 1;
        }
        SplitPathSegments(path, startIndex, out.segments);
        NormalizeSegmentsLexical(out.segments, out.isAbsolute);
        return out;
#endif
    }

    static string JoinSegmentsWithSlash(const vector<string>& segments)
    {
        if (segments.empty())
        {
            return string();
        }
        string out;
        // 预估容量：每段 + 分隔符
        size_t total = 0;
        for (size_t i = 0; i < segments.size(); i++)
        {
            total += segments[i].size() + 1;
        }
        out.reserve(total);

        for (size_t i = 0; i < segments.size(); i++)
        {
            if (i > 0)
            {
                out.push_back('/');
            }
            out.append(segments[i]);
        }
        return out;
    }

    static bool EndsWithDotOrDotDot(const string& pathUtf8)
    {
        if (pathUtf8.empty())
        {
            return false;
        }

        string s = pathUtf8;
        ReplaceBackslashWithSlash(s);

        // 去尾斜杠后判断最后一个片段
        while (!s.empty() && s.back() == '/')
        {
            s.pop_back();
        }
        if (s.empty())
        {
            return false;
        }

        const size_t pos = s.find_last_of('/');
        const string last = (pos == string::npos) ? s : s.substr(pos + 1);
        return last == "." || last == "..";
    }

    struct LexicalPath
    {
        string root;
        bool isAbsolute = false;
        bool isDrive = false;
        bool isUnc = false;
        vector<string> segments;
    };

    static LexicalPath ParseLexicalPath(const string& rawPathUtf8)
    {
        LexicalPath path;
        string s = rawPathUtf8;
        ReplaceBackslashWithSlash(s);

        size_t index = 0;

        // Drive: "C:" or "C:/..." or "C:..."  -> treat as rooted at drive.
        if (s.size() >= 2 && IsAsciiAlpha(s[0]) && s[1] == ':')
        {
            path.isAbsolute = true;
            path.isDrive = true;
            path.root = s.substr(0, 2);
            index = 2;
            if (index < s.size() && s[index] == '/')
            {
                index++;
            }
        }
        // UNC: //server/share/...
        else if (s.size() >= 2 && s[0] == '/' && s[1] == '/')
        {
            path.isAbsolute = true;
            path.isUnc = true;

            // root = //server/share
            const size_t serverEnd = s.find('/', 2);
            if (serverEnd == string::npos)
            {
                path.root = s;
                index = s.size();
            }
            else
            {
                const size_t shareEnd = s.find('/', serverEnd + 1);
                if (shareEnd == string::npos)
                {
                    path.root = s;
                    index = s.size();
                }
                else
                {
                    path.root = s.substr(0, shareEnd);
                    index = shareEnd + 1;
                }
            }
        }
        // Unix absolute: /...
        else if (!s.empty() && s[0] == '/')
        {
            path.isAbsolute = true;
            path.root = "/";
            index = 1;
        }

        // Parse segments (skip empty segments caused by duplicate slashes)
        string current;
        for (size_t i = index; i <= s.size(); i++)
        {
            const char ch = (i < s.size()) ? s[i] : '/';
            if (ch == '/')
            {
                if (!current.empty())
                {
                    path.segments.push_back(current);
                    current.clear();
                }
            }
            else
            {
                current.push_back(ch);
            }
        }
        return path;
    }

    static void NormalizeDotSegments(LexicalPath& path)
    {
        vector<string> out;
        out.reserve(path.segments.size());

        for (size_t i = 0; i < path.segments.size(); i++)
        {
            const string& seg = path.segments[i];
            if (seg.empty() || seg == ".")
            {
                continue;
            }
            if (seg == "..")
            {
                if (!out.empty() && out.back() != "..")
                {
                    out.pop_back();
                }
                else
                {
                    if (!path.isAbsolute)
                    {
                        out.push_back("..");
                    }
                }
                continue;
            }
            out.push_back(seg);
        }

        path.segments.swap(out);
    }

    static string BuildPathString(const LexicalPath& path, bool forceDir)
    {
        string out;

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
            // 若是根目录（"/", "C:/", "//server/share/"），保留末尾 '/'
            const bool isRootUnix = out == "/";
            const bool isRootDrive = (out.size() == 3 && IsAsciiAlpha(out[0]) && out[1] == ':' && out[2] == '/');
            const bool isRootUnc = (path.isUnc && out == path.root + "/");

            if (!isRootUnix && !isRootDrive && !isRootUnc)
            {
                while (out.size() > 1 && out.back() == '/')
                {
                    out.pop_back();
                }
            }
        }

        return out;
    }
}

bool GB_IsFileExists(const string& filePathUtf8)
{
    bool exists = false;
    bool isDir = false;
    if (!internal::IsDirByStat(filePathUtf8, exists, isDir))
    {
        return false;
    }
    return exists && !isDir;
}

bool GB_IsDirectoryExists(const string& dirPathUtf8)
{
    bool exists = false;
    bool isDir = false;
    if (!internal::IsDirByStat(dirPathUtf8, exists, isDir))
    {
        return false;
    }
    return exists && isDir;
}

bool GB_CreateDirectory(const string& dirPathUtf8)
{
    // 递归创建，逐级建；Windows 的 CreateDirectory* 仅建末级，这里做完整 walk
    return internal::MakeDirsRecursive(internal::EnsureTrailingSlash(dirPathUtf8));
}

bool GB_IsEmptyDirectory(const string& dirPathUtf8)
{
    if (!GB_IsDirectoryExists(dirPathUtf8))
    {
        return false;
    }
#if defined(_WIN32)
    string pattern = internal::EnsureTrailingSlash(dirPathUtf8) + "*";
    wstring wpat = internal::Utf8ToWide(pattern);
    if (wpat.empty())
    {
        return false;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(wpat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    bool empty = true;
    do
    {
        const wchar_t* name = fd.cFileName;
        if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0')))
        {
            continue;
        }
        empty = false;
        break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
#else
    string native = internal::EnsureTrailingSlash(dirPathUtf8);
    DIR* d = ::opendir(native.c_str());
    if (!d)
    {
        return false;
    }
    bool empty = true;
    while (dirent* ent = ::readdir(d))
    {
        const char* name = ent->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
        {
            continue;
        }
        empty = false;
        break;
    }
    ::closedir(d);
    return empty;
#endif
}

bool GB_DeleteDirectory(const string& dirPathUtf8)
{
    if (!GB_IsDirectoryExists(dirPathUtf8))
    {
        return false;
    }
    const string dir = internal::EnsureTrailingSlash(dirPathUtf8);
    if (!internal::DeleteDirContents(dir))
    {
        return false;
    }
    return internal::RemoveEmptyDir(dir);
}

bool GB_DeleteFile(const string& filePathUtf8)
{
    if (!GB_IsFileExists(filePathUtf8))
    {
        return false;
    }
    return internal::DeleteOneFile(filePathUtf8);
}

bool GB_CopyFile(const string& srcFilePathUtf8, const string& dstFilePathUtf8)
{
#if defined(_WIN32)
    // 使用 CopyFileW（会覆盖由第三参数决定；这里选择覆盖 false -> 允许覆盖）
    wstring wsrc = internal::Utf8ToWide(srcFilePathUtf8);
    wstring wdst = internal::Utf8ToWide(dstFilePathUtf8);
    if (wsrc.empty() || wdst.empty())
    {
        return false;
    }
    // 官方：CopyFile/FindFirstFile 系列文档（与 GetFileAttributesW 同系列）
    return ::CopyFileW(wsrc.c_str(), wdst.c_str(), FALSE) != 0;
#else
    // 纯 C++11 实现：ifstream/ofstream 流式复制（覆盖）。
    string src = internal::ReplaceSlashes(srcFilePathUtf8, '/');
    string dst = internal::ReplaceSlashes(dstFilePathUtf8, '/');

    FILE* in = ::fopen(src.c_str(), "rb");
    if (!in)
    {
        return false;
    }
    FILE* out = ::fopen(dst.c_str(), "wb");
    if (!out)
    {
        ::fclose(in);
        return false;
    }
    const size_t bufSize = 1 << 20; // 1 MB
    vector<char> buf(bufSize);
    bool ok = true;
    while (ok)
    {
        size_t n = ::fread(buf.data(), 1, buf.size(), in);
        if (n > 0)
        {
            if (::fwrite(buf.data(), 1, n, out) != n)
            {
                ok = false;
                break;
            }
        }
        if (n < buf.size())
        {
            if (::ferror(in))
            {
                ok = false;
            }
            break; // EOF
        }
    }
    ::fclose(in);
    if (::fclose(out) != 0)
    {
        ok = false;
    }
    return ok;
#endif
}

vector<string> GB_GetFilesList(const string& dirPathUtf8, bool recursive)
{
    vector<string> out;
    if (!GB_IsDirectoryExists(dirPathUtf8))
    {
        return out;
    }
    internal::ListFilesRecursive(internal::EnsureTrailingSlash(dirPathUtf8), recursive, out);
    return out;
}

string GB_GetFileName(const string& rawFilePathUtf8, bool withExt)
{
    string filePathUtf8 = Utf8Replace(rawFilePathUtf8, GB_STR("\\\\"), GB_STR("\\"));
    filePathUtf8 = Utf8Replace(filePathUtf8, GB_STR("\\"), GB_STR("/"));
    const int64_t pos1 = Utf8FindLast(filePathUtf8, GB_STR("/"));
    if (pos1 < 0)
    {
        return filePathUtf8;
	}
    const string fileNameWithExt = Utf8Substr(filePathUtf8, pos1 + 1);
    if (withExt)
    {
        return fileNameWithExt;
	}

    const int64_t pos2 = Utf8FindLast(fileNameWithExt, GB_STR("."));
    if (pos2 < 0)
    {
        return fileNameWithExt;
    }
	return Utf8Substr(fileNameWithExt, 0, pos2);
}

string GB_GetFileExt(const string& filePathUtf8)
{
    const int64_t pos = Utf8FindLast(filePathUtf8, GB_STR("."));
    if (pos < 0)
    {
        return "";
    }

	return Utf8Substr(filePathUtf8, pos);
}

string GB_GetDirectoryPath(const string& rawFilePathUtf8)
{
    string filePathUtf8 = Utf8Replace(rawFilePathUtf8, GB_STR("\\\\"), GB_STR("\\"));
    filePathUtf8 = Utf8Replace(filePathUtf8, GB_STR("\\"), GB_STR("/"));
    const int64_t pos = Utf8FindLast(filePathUtf8, GB_STR("/"));
    if (pos < 0)
    {
        return "";
    }

	return Utf8Substr(filePathUtf8, 0, pos + 1);
}

size_t GB_GetFileSizeByte(const string& filePathUtf8)
{
    uint64_t size64 = 0;
    if (!internal::TryGetFileSize64(filePathUtf8, size64))
    {
        return 0;
    }
    constexpr static uint64_t maxSizeT = static_cast<uint64_t>(numeric_limits<size_t>::max());
    if (size64 > maxSizeT)
    {
        return static_cast<size_t>(maxSizeT); // 截断以适配 32 位构建
    }
    return static_cast<size_t>(size64);
}

double GB_GetFileSizeKB(const string& filePathUtf8)
{
    uint64_t size64 = 0;
    if (!internal::TryGetFileSize64(filePathUtf8, size64))
    {
        return 0.0;
    }
    constexpr static double oneKB = 1024.0; // KiB
    return static_cast<double>(size64) / oneKB;
}

double GB_GetFileSizeMB(const string& filePathUtf8)
{
    uint64_t size64 = 0;
    if (!internal::TryGetFileSize64(filePathUtf8, size64))
    {
        return 0.0;
    }
    constexpr static double oneMB = 1024.0 * 1024.0; // MiB
    return static_cast<double>(size64) / oneMB;
}

double GB_GetFileSizeGB(const string& filePathUtf8)
{
    uint64_t size64 = 0;
    if (!internal::TryGetFileSize64(filePathUtf8, size64))
    {
        return 0.0;
    }
    constexpr static double oneGB = 1024.0 * 1024.0 * 1024.0; // GiB
    return static_cast<double>(size64) / oneGB;
}

string GB_GetExeDirectory()
{
#if defined(_WIN32)
    // 处理可能超出 MAX_PATH 的路径：循环扩容
    vector<wchar_t> buf(512);
    for (;;)
    {
        DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0)
        {
            return string(); // 失败
        }
        if (len < buf.size() - 1)
        {
            buf[len] = L'\0';
            break; // 成功
        }
        // len == nSize（被截断），扩大缓冲区
        buf.resize(buf.size() * 2);
    }

    wstring fullWs(buf.data());
    // 去掉文件名，得到目录
    size_t pos = fullWs.find_last_of(L"\\/");
    if (pos == wstring::npos)
    {
        return string();
    }
    wstring dirWs = fullWs.substr(0, pos);
    string dirUtf8 = internal::WideToUtf8(dirWs);
    internal::NormalizeToUnixDir(dirUtf8);
    return dirUtf8;

#else
    // Linux: readlink("/proc/self/exe")，注意：返回长度不含NUL，需要手动补
    vector<char> buf(256);
    for (;;)
    {
        ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        if (n < 0)
        {
            return string(); // 失败
        }
        if (static_cast<size_t>(n) < buf.size() - 1)
        {
            buf[static_cast<size_t>(n)] = '\0';
            break; // 成功
        }
        // 缓冲区被截断，扩容重试
        buf.resize(buf.size() * 2);
    }

    string full(buf.data());
    // 某些发行版在可执行文件被删除时，路径末尾可能带" (deleted)"标记，这里去掉
    const string deletedTag = " (deleted)";
    if (full.size() > deletedTag.size() && full.compare(full.size() - deletedTag.size(), deletedTag.size(), deletedTag) == 0)
    {
        full.erase(full.size() - deletedTag.size());
    }

    // 去掉文件名，得到目录
    size_t pos = full.find_last_of('/');
    if (pos == string::npos)
    {
        return string();
    }
    string dir = full.substr(0, pos);
    NormalizeToUnixDir(dir);
    return dir;
#endif
}

bool GB_CreateFileRecursive(const string& filePathUtf8, bool overwriteIfExists)
{
    if (filePathUtf8.empty())
    {
        return false;
    }

    // 统一分隔符
    string normPath = filePathUtf8;
    for (size_t i = 0; i < normPath.size(); i++)
    {
        if (normPath[i] == '\\')
        {
            normPath[i] = '/';
        }
    }

    // 末尾是分隔符 => 视为目录，拒绝
    {
        const char lastCh = normPath.back();
        if (lastCh == '/' || lastCh == '\\')
        {
            return false;
        }
    }

    // 先确保父目录存在
    const string dirPathUtf8 = GB_GetDirectoryPath(normPath);
    if (!dirPathUtf8.empty())
    {
        if (!GB_CreateDirectory(dirPathUtf8))
        {
            return false;
        }
    }

    // 存在性与非覆盖策略
    if (!overwriteIfExists)
    {
        if (GB_IsFileExists(normPath))
        {
            const size_t sizeByte = GB_GetFileSizeByte(normPath);
            return sizeByte == 0; // 已存在且已为空 ⇒ OK；否则拒绝
        }
    }

#if defined(_WIN32)
    // UTF-8 -> UTF-16
    auto Utf8ToWide = [](const string& utf8) -> wstring
        {
            if (utf8.empty())
            {
                return wstring();
            }
            const int need = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                static_cast<int>(utf8.size()),
                nullptr, 0);
            if (need <= 0)
            {
                return wstring();
            }
            wstring ws(static_cast<size_t>(need), L'\0');
            const int wrote = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                static_cast<int>(utf8.size()),
                &ws[0], need);
            if (wrote != need)
            {
                return wstring();
            }
            return ws;
        };

    const wstring wPath = Utf8ToWide(normPath);
    if (wPath.empty())
    {
        return false;
    }

    const DWORD creationDisposition = overwriteIfExists ? CREATE_ALWAYS : CREATE_NEW; // 见备注
    HANDLE h = ::CreateFileW(
        wPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,        // 允许其他进程读
        nullptr,
        creationDisposition,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    ::CloseHandle(h);
    return true;

#else
    // Linux/Unix：确保只在“不存在时创建”或“截断创建”
    int flags = O_WRONLY | O_CREAT | (overwriteIfExists ? O_TRUNC : O_EXCL);
    const int fd = ::open(normPath.c_str(), flags, 0644); // 最终权限受 umask 影响
    if (fd < 0)
    {
        return false;
    }
    (void)::close(fd);
    return true;
#endif
}

string GB_GetRelativePath(const string& pathAUtf8, const string& pathBUtf8)
{
    if (pathAUtf8.empty())
    {
        return "";
    }

    // 1) 判定 A 是否目录：优先相信用户输入形态（末尾分隔符），其次才查文件系统。
    bool pathAIsDir = false;
    if (!pathAUtf8.empty() && (pathAUtf8.back() == '/' || pathAUtf8.back() == '\\'))
    {
        pathAIsDir = true;
    }
    else
    {
        pathAIsDir = GB_IsDirectoryExists(pathAUtf8);
    }

    // 2) 基准路径：若 B 是文件，则以其父目录作为 base；若 B 是目录，则以 B 本身作为 base。
    bool pathBIsDir = false;
    if (!pathBUtf8.empty() && (pathBUtf8.back() == '/' || pathBUtf8.back() == '\\'))
    {
        pathBIsDir = true;
    }
    else
    {
        pathBIsDir = GB_IsDirectoryExists(pathBUtf8);
    }

    string baseDirUtf8 = "";
    if (pathBUtf8.empty())
    {
        baseDirUtf8 = ".";
    }
    else if (pathBIsDir)
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

    // 3) 纯字符串（lexical）解析与标准化
    const internal::ParsedPath target = internal::ParseAndNormalizePathLexical(pathAUtf8);
    const internal::ParsedPath base = internal::ParseAndNormalizePathLexical(baseDirUtf8);

    // Windows：不同盘符/不同 UNC share 无法构造相对路径；此处回退为“规范化后的 A”。
    // 参考：Win32 PathRelativePathToW 要求两者有共同前缀，否则失败。
    if (!internal::EqualRoot(target.root, base.root) || target.isAbsolute != base.isAbsolute)
    {
        string out = internal::ToOutputNorm(pathAUtf8);
        if (pathAIsDir)
        {
            out = internal::EnsureTrailingSlash(out);
        }
        return out;
    }

    // 4) 计算公共前缀
    size_t common = 0;
    const size_t minCount = min(target.segments.size(), base.segments.size());
    for (; common < minCount; common++)
    {
        if (!internal::EqualSegment(target.segments[common], base.segments[common]))
        {
            break;
        }
    }

    // 5) base -> common：补 ".."；common -> target：补余下片段
    vector<string> relSegments;
    relSegments.reserve((base.segments.size() - common) + (target.segments.size() - common));

    for (size_t i = common; i < base.segments.size(); i++)
    {
        relSegments.push_back("..");
    }
    for (size_t i = common; i < target.segments.size(); i++)
    {
        relSegments.push_back(target.segments[i]);
    }

    string relPath = internal::JoinSegmentsWithSlash(relSegments);
    if (relPath.empty())
    {
        relPath = ".";
    }

    // 6) 若 A 为目录，保证结果末尾带 '/'
    if (pathAIsDir)
    {
        relPath = internal::EnsureTrailingSlash(relPath);
    }

    return relPath;
}

string GB_JoinPath(const string& leftPathUtf8, const string& rightPathUtf8)
{
    bool outputIsDir = false;
    if (!rightPathUtf8.empty())
    {
        outputIsDir = internal::IsSlash(rightPathUtf8.back()) || internal::EndsWithDotOrDotDot(rightPathUtf8);
    }
    else
    {
        if (!leftPathUtf8.empty() && internal::IsSlash(leftPathUtf8.back()))
        {
            outputIsDir = true;
        }
        else if (GB_IsDirectoryExists(leftPathUtf8))
        {
            outputIsDir = true;
        }
    }

    // 统一分隔符
    string leftNorm = leftPathUtf8;
    string rightNorm = rightPathUtf8;
    internal::ReplaceBackslashWithSlash(leftNorm);
    internal::ReplaceBackslashWithSlash(rightNorm);

    // right 为空：直接规范化 left
    if (rightNorm.empty())
    {
        internal::LexicalPath path = internal::ParseLexicalPath(leftNorm);
        internal::NormalizeDotSegments(path);

        // 若文件系统中存在且为目录，也认为是目录
        if (!outputIsDir)
        {
            const string tmp = internal::BuildPathString(path, false);
            bool exists = false;
            bool isDir = false;
            if (internal::IsDirByStat(tmp, exists, isDir) && exists && isDir)
            {
                outputIsDir = true;
            }
        }
        return internal::BuildPathString(path, outputIsDir);
    }

    // left + right：尽量把 left 当作目录（除非明确是文件）
    string base = leftNorm;
    if (!base.empty())
    {
        bool leftExists = false;
        bool leftIsDir = false;
        if (internal::IsDirByStat(base, leftExists, leftIsDir) && leftExists && !leftIsDir)
        {
            base = GB_GetDirectoryPath(base);
        }

        // 作为目录拼接时，确保 base 末尾有 '/'
        if (!base.empty() && base.back() != '/')
        {
            base.push_back('/');
        }
    }

    const string combined = base + rightNorm;
    internal::LexicalPath combinedParsed = internal::ParseLexicalPath(combined);
    internal::NormalizeDotSegments(combinedParsed);

    if (!outputIsDir)
    {
        const string tmp = internal::BuildPathString(combinedParsed, false);
        bool exists = false;
        bool isDir = false;
        if (internal::IsDirByStat(tmp, exists, isDir) && exists && isDir)
        {
            outputIsDir = true;
        }
    }
    return internal::BuildPathString(combinedParsed, outputIsDir);
}