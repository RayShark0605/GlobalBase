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
    static bool TryGetFileSize64(const std::string& filePathUtf8, uint64_t& sizeOut)
    {
#if defined(_WIN32)
        // 先排除不存在或目录
        const std::wstring w = Utf8ToWide(filePathUtf8);
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
        std::string native = ReplaceSlashes(filePathUtf8, '/');
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

GLOBALBASE_PORT size_t GB_GetFileSizeByte(const std::string& filePathUtf8)
{
    uint64_t size64 = 0;
    if (!internal::TryGetFileSize64(filePathUtf8, size64))
    {
        return 0;
    }
    const uint64_t maxSizeT = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
    if (size64 > maxSizeT)
    {
        return static_cast<size_t>(maxSizeT); // 截断以适配 32 位构建
    }
    return static_cast<size_t>(size64);
}

GLOBALBASE_PORT double GB_GetFileSizeKB(const std::string& filePathUtf8)
{
    uint64_t size64 = 0;
    if (!internal::TryGetFileSize64(filePathUtf8, size64))
    {
        return 0.0;
    }
    constexpr static double oneKB = 1024.0; // KiB
    return static_cast<double>(size64) / oneKB;
}

GLOBALBASE_PORT double GB_GetFileSizeMB(const std::string& filePathUtf8)
{
    uint64_t size64 = 0;
    if (!internal::TryGetFileSize64(filePathUtf8, size64))
    {
        return 0.0;
    }
    constexpr static double oneMB = 1024.0 * 1024.0; // MiB
    return static_cast<double>(size64) / oneMB;
}

GLOBALBASE_PORT double GB_GetFileSizeGB(const std::string& filePathUtf8)
{
    uint64_t size64 = 0;
    if (!internal::TryGetFileSize64(filePathUtf8, size64))
    {
        return 0.0;
    }
    constexpr static double oneGB = 1024.0 * 1024.0 * 1024.0; // GiB
    return static_cast<double>(size64) / oneGB;
}