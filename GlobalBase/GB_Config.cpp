#include "GB_Config.h"
#include "GB_Utf8String.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cerrno>
#include <sys/stat.h>
#include <functional>
#if defined(_WIN32)
#  include <Shlwapi.h>
#  pragma comment(lib, "Advapi32.lib")
#  pragma comment(lib, "Shlwapi.lib")
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <fcntl.h>
#  include <limits.h>
#endif

#ifdef max
#undef max
#endif

#ifdef min
#undef min
#endif

using namespace std;

namespace internal
{
#if defined(_WIN32)

    static wstring Utf8ToWide(const string& s)
    {
        if (s.empty())
        {
            return wstring();
        }
        const int len = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
            static_cast<int>(s.size()), nullptr, 0);
        if (len <= 0)
        {
            return wstring();
        }
        wstring ws(static_cast<size_t>(len), L'\0');
        const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
            static_cast<int>(s.size()), &ws[0], len);
        if (written <= 0)
        {
            return wstring();
        }
        return ws;
    }

    static string WideToUtf8(const wstring& ws)
    {
        if (ws.empty())
        {
            return string();
        }
        const int len = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
            nullptr, 0, nullptr, nullptr);
        if (len <= 0)
        {
            return string();
        }
        string s(static_cast<size_t>(len), '\0');
        const int written = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
            &s[0], len, nullptr, nullptr);
        if (written <= 0)
        {
            return string();
        }
        return s;
    }

    static const wchar_t* kBaseKeyPathW = L"Software\\GlobalBase";

    static bool OpenBaseKey(HKEY& hKey, REGSAM samDesired, bool create)
    {
        hKey = nullptr;
        if (create)
        {
            DWORD disp = 0;
            const LONG lr = ::RegCreateKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\GlobalBase",
                0, nullptr, 0,
                samDesired,          // 关键：由调用者指定需要的权限
                nullptr, &hKey, &disp
            );
            return (lr == ERROR_SUCCESS);
        }
        else
        {
            const LONG lr = ::RegOpenKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\GlobalBase",
                0,
                samDesired,          // 关键：由调用者指定需要的权限
                &hKey
            );
            return (lr == ERROR_SUCCESS);
        }
    }

    // 读
    static bool WinReadValue(const string& nameUtf8, string& valueUtf8)
    {
        HKEY hKey = nullptr;
        if (!OpenBaseKey(hKey, KEY_QUERY_VALUE, false))
        {
            return false;
        }
        const wstring valueNameW = Utf8ToWide(nameUtf8);
        DWORD type = 0;
        DWORD cb = 0;
        // 先探测大小
        LONG lr = ::RegGetValueW(hKey, nullptr, valueNameW.c_str(), RRF_RT_REG_SZ, &type, nullptr, &cb);
        if (lr != ERROR_SUCCESS || cb == 0)
        {
            ::RegCloseKey(hKey);
            return false;
        }
        vector<wchar_t> buffer(cb / sizeof(wchar_t) + 1, L'\0');
        lr = ::RegGetValueW(hKey, nullptr, valueNameW.c_str(), RRF_RT_REG_SZ, &type,
            buffer.data(), &cb);
        ::RegCloseKey(hKey);
        if (lr != ERROR_SUCCESS)
        {
            return false;
        }
        // REG_SZ 可能未包含显式 '\0'，我们已经多留了一个
        valueUtf8 = WideToUtf8(wstring(buffer.data()));
        return true;
    }

    // 写
    static bool WinWriteValue(const string& nameUtf8, const string& valueUtf8)
    {
        HKEY hKey = nullptr;
        if (!OpenBaseKey(hKey, KEY_SET_VALUE | KEY_QUERY_VALUE, /*create=*/true))
        {
            return false;
        }
        const wstring valueNameW = Utf8ToWide(nameUtf8);
        const wstring valueW = Utf8ToWide(valueUtf8);
        const DWORD cbData = static_cast<DWORD>((valueW.size() + 1) * sizeof(wchar_t)); // 含 '\0'
        const LONG lr = ::RegSetValueExW(hKey, valueNameW.c_str(), 0, REG_SZ,
            reinterpret_cast<const BYTE*>(valueW.c_str()), cbData);
        ::RegCloseKey(hKey);
        return (lr == ERROR_SUCCESS);
    }

    static bool WinDeleteValue(const string& nameUtf8)
    {
        HKEY hKey = nullptr;
        // 关键修改：必须带 KEY_SET_VALUE
        if (!OpenBaseKey(hKey, KEY_SET_VALUE, /*create=*/false))
        {
            return false;
        }

        const wstring valueNameW = Utf8ToWide(nameUtf8);
        const LONG lr = ::RegDeleteValueW(hKey,
            valueNameW.empty() ? nullptr : valueNameW.c_str()); // 空名表示删除默认值
        ::RegCloseKey(hKey);
        return (lr == ERROR_SUCCESS);
    }

    static unordered_map<string, string> WinEnumAll()
    {
        unordered_map<string, string> m;
        HKEY hKey = nullptr;
        if (!OpenBaseKey(hKey, KEY_READ | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, false))
        {
            return m;
        }
        DWORD cValues = 0, maxName = 0, maxData = 0, type = 0;
        LONG lr = ::RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &cValues, &maxName, &maxData, nullptr, nullptr);
        if (lr != ERROR_SUCCESS)
        {
            ::RegCloseKey(hKey);
            return m;
        }
        vector<wchar_t> nameBuf(maxName + 1, L'\0');
        vector<wchar_t> dataBuf((maxData / sizeof(wchar_t)) + 2, L'\0');

        for (DWORD i = 0; i < cValues; i++)
        {
            DWORD nameLen = maxName + 1;
            DWORD cbData = static_cast<DWORD>(dataBuf.size() * sizeof(wchar_t));
            fill(nameBuf.begin(), nameBuf.end(), L'\0');
            fill(dataBuf.begin(), dataBuf.end(), L'\0');

            lr = ::RegEnumValueW(hKey, i, nameBuf.data(), &nameLen, nullptr, &type,
                reinterpret_cast<BYTE*>(dataBuf.data()), &cbData);
            if (lr != ERROR_SUCCESS)
            {
                continue;
            }
            if (type == REG_SZ)
            {
                const string key = WideToUtf8(wstring(nameBuf.data()));
                const string val = WideToUtf8(wstring(dataBuf.data()));
                m[key] = val;
            }
        }
        ::RegCloseKey(hKey);
        return m;
    }

    static wstring NormalizeSlashesW(const wstring& s)
    {
        wstring t = s;
        for (wchar_t& ch : t)
        {
            if (ch == L'/')
            {
                ch = L'\\';
            }
        }
        // 去除尾部反斜杠（保留根键本身的空子键）
        while (!t.empty() && t.back() == L'\\')
        {
            t.pop_back();
        }
        return t;
    }

    static bool StartsWithNoCaseW(const wstring& text, const wstring& prefix)
    {
        if (prefix.size() > text.size())
        {
            return false;
        }
        for (size_t i = 0; i < prefix.size(); i++)
        {
            wchar_t a = text[i];
            wchar_t b = prefix[i];
            if (a >= L'A' && a <= L'Z')
            {
                a = static_cast<wchar_t>(a - L'A' + L'a');
            }
            if (b >= L'A' && b <= L'Z')
            {
                b = static_cast<wchar_t>(b - L'A' + L'a');
            }
            if (a != b)
            {
                return false;
            }
        }
        return true;
    }

    static bool ParseWindowsRegPath(const string& regPathUtf8, HKEY& rootKey, wstring& subKeyW)
    {
        rootKey = HKEY_CURRENT_USER; // 默认 HKCU
        subKeyW.clear();

        // 修剪与 UTF-8 → UTF-16
        const string trimmed = GB_Utf8Trim(regPathUtf8);
        if (trimmed.empty())
        {
            return false;
        }
        wstring pathW = internal::Utf8ToWide(trimmed);
        pathW = NormalizeSlashesW(pathW);

        // 可选前缀："计算机\" 或 "Computer\"
        if (StartsWithNoCaseW(pathW, L"计算机\\"))
        {
            pathW = pathW.substr(3 + 1); // 3个汉字 + '\'
        }
        else if (StartsWithNoCaseW(pathW, L"Computer\\"))
        {
            pathW = pathW.substr(8 + 1);
        }

        // 提取根键 token
        const size_t pos = pathW.find(L'\\');
        const wstring token = (pos == wstring::npos) ? pathW : pathW.substr(0, pos);
        const wstring remain = (pos == wstring::npos) ? L"" : pathW.substr(pos + 1);

        auto tokEq = [&](const wchar_t* s) -> bool
            {
                return StartsWithNoCaseW(token, s) && token.size() == wcslen(s);
            };

        if (tokEq(L"HKEY_CURRENT_USER") || tokEq(L"HKCU"))
        {
            rootKey = HKEY_CURRENT_USER;
            subKeyW = remain;
        }
        else if (tokEq(L"HKEY_LOCAL_MACHINE") || tokEq(L"HKLM"))
        {
            rootKey = HKEY_LOCAL_MACHINE;
            subKeyW = remain;
        }
        else if (tokEq(L"HKEY_CLASSES_ROOT") || tokEq(L"HKCR"))
        {
            rootKey = HKEY_CLASSES_ROOT;
            subKeyW = remain;
        }
        else if (tokEq(L"HKEY_USERS") || tokEq(L"HKU"))
        {
            rootKey = HKEY_USERS;
            subKeyW = remain;
        }
        else if (tokEq(L"HKEY_CURRENT_CONFIG") || tokEq(L"HKCC"))
        {
            rootKey = HKEY_CURRENT_CONFIG;
            subKeyW = remain;
        }
        else
        {
            // 未提供根键：整个 path 认为是 HKCU 下的子键
            rootKey = HKEY_CURRENT_USER;
            subKeyW = pathW;
        }

        // 去掉子键的前导反斜杠
        while (!subKeyW.empty() && subKeyW.front() == L'\\')
        {
            subKeyW.erase(subKeyW.begin());
        }

        return true;
    }

    static bool OpenKeyAnyView(HKEY root, const wstring& subKeyW, REGSAM baseSam, HKEY& outKey)
    {
        outKey = nullptr;

#if defined(KEY_WOW64_64KEY)
        // 先试 64 位视图
        if (::RegOpenKeyExW(root, subKeyW.c_str(), 0, baseSam | KEY_WOW64_64KEY, &outKey) == ERROR_SUCCESS)
        {
            return true;
        }
        // 再试 32 位视图
        if (::RegOpenKeyExW(root, subKeyW.c_str(), 0, baseSam | KEY_WOW64_32KEY, &outKey) == ERROR_SUCCESS)
        {
            return true;
        }
#endif
        // 最后试“当前视图”
        if (::RegOpenKeyExW(root, subKeyW.c_str(), 0, baseSam, &outKey) == ERROR_SUCCESS)
        {
            return true;
        }
        return false;
    }

    static string GetLeafNameFromPathUtf8(const string& pathUtf8, char slash1 = '\\', char slash2 = '/')
    {
        const string trimmed = GB_Utf8Trim(pathUtf8);
        if (trimmed.empty())
        {
            return string();
        }
        size_t p1 = trimmed.find_last_of(slash1);
        size_t p2 = trimmed.find_last_of(slash2);
        size_t pos = string::npos;
        if (p1 == string::npos) pos = p2;
        else if (p2 == string::npos) pos = p1;
        else pos = max(p1, p2);

        if (pos == string::npos) return trimmed;
        if (pos + 1 >= trimmed.size()) return string();
        return trimmed.substr(pos + 1);
    }

#else
    static string GetHomeDir()
    {
        const char* home = getenv("HOME");
        if (home && *home)
        {
            return GB_MakeUtf8String(home);
        }
        return ""; // 极少见：无 HOME
    }

    static string GetLinuxConfigFile()
    {
        const char* xdg = getenv("XDG_CONFIG_HOME"); // XDG Base Directory
        string base;
        if (xdg && *xdg)
        {
            base = string(xdg);
        }
        else
        {
            const string home = GetHomeDir();
            if (home.empty())
            {
                return string("./GlobalBase/config.kv");
            }
            base = home + "/.config";
        }
        return base + "/GlobalBase/config.kv";
    }

    static bool MkdirsRecursively(const string& dir)
    {
        if (dir.empty())
        {
            return false;
        }
        // 逐级创建
        size_t pos = 1; // 跳过可能的开头 '/'
        while (true)
        {
            pos = dir.find('/', pos);
            const string sub = dir.substr(0, pos);
            if (!sub.empty())
            {
                struct stat st;
                if (stat(sub.c_str(), &st) != 0)
                {
                    if (mkdir(sub.c_str(), 0700) != 0 && errno != EEXIST)
                    {
                        return false;
                    }
                }
                else if (!S_ISDIR(st.st_mode))
                {
                    return false;
                }
            }
            if (pos == string::npos)
            {
                break;
            }
            pos++;
        }
        return true;
    }

    // URL 百分号编码（UTF-8 字节级）
    static bool IsUnreserved(unsigned char c)
    {
        return (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
    }
    static string UrlEncode(const string& s)
    {
        static const char* hex = "0123456789ABCDEF";
        string out;
        out.reserve(s.size() * 3);
        for (size_t i = 0; i < s.size(); i++)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (IsUnreserved(c))
            {
                out.push_back(static_cast<char>(c));
            }
            else if (c == ' ') // 避免可见性问题，空格也编码
            {
                out.push_back('%'); out.push_back('2'); out.push_back('0');
            }
            else
            {
                out.push_back('%');
                out.push_back(hex[(c >> 4) & 0xF]);
                out.push_back(hex[c & 0xF]);
            }
        }
        return out;
    }
    static bool UrlDecode(const string& s, string& out)
    {
        out.clear();
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++)
        {
            const char ch = s[i];
            if (ch == '%')
            {
                if (i + 2 >= s.size()) return false;
                auto hexval = [](char c) -> int
                    {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        return -1;
                    };
                const int hi = hexval(s[i + 1]);
                const int lo = hexval(s[i + 2]);
                if (hi < 0 || lo < 0) return false;
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            }
            else
            {
                out.push_back(ch);
            }
        }
        return true;
    }

    static string Dirname(const string& path)
    {
        const size_t pos = path.find_last_of('/');
        if (pos == string::npos) return string(".");
        if (pos == 0) return string("/");
        return path.substr(0, pos);
    }

    static bool AtomicWriteFile(const string& path, const string& content)
    {
        const string dir = Dirname(path);
        if (!MkdirsRecursively(dir))
        {
            return false;
        }

        // 临时文件名：同目录，确保 rename 原子
        ostringstream oss;
        oss << path << ".tmp." << ::getpid() << "." << ::time(nullptr);
        const string tmp = oss.str();

        // O_CREAT|O_EXCL 防止覆盖其他进程的临时文件
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
        {
            return false;
        }

        const char* data = content.data();
        size_t left = content.size();
        while (left > 0)
        {
            const ssize_t n = ::write(fd, data, left);
            if (n < 0)
            {
                const int e = errno;
                ::close(fd);
                ::unlink(tmp.c_str());
                (void)e;
                return false;
            }
            data += n;
            left -= static_cast<size_t>(n);
        }

        // 持久化数据
        if (::fsync(fd) != 0)
        {
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        if (::close(fd) != 0)
        {
            ::unlink(tmp.c_str());
            return false;
        }

        // 原子替换
        if (::rename(tmp.c_str(), path.c_str()) != 0)
        {
            ::unlink(tmp.c_str());
            return false;
        }

        // 刷新目录项（更强的崩溃一致性习惯做法）
        int dfd = ::open(dir.c_str(), O_DIRECTORY | O_RDONLY);
        if (dfd >= 0)
        {
            (void)::fsync(dfd);
            ::close(dfd);
        }
        return true;
    }

    static bool LoadAllKv(const string& filePath,
        unordered_map<string, string>& m)
    {
        m.clear();
        ifstream ifs(filePath.c_str(), ios::in | ios::binary);
        if (!ifs.good())
        {
            return true; // 不存在视为空
        }
        string line;
        while (getline(ifs, line))
        {
            // 去除行末 CR
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t eq = line.find('=');
            if (eq == string::npos) continue;

            const string kEnc = line.substr(0, eq);
            const string vEnc = line.substr(eq + 1);
            string k, v;
            if (!UrlDecode(kEnc, k)) continue;
            if (!UrlDecode(vEnc, v)) continue;
            m[k] = v;
        }
        return true;
    }

    static bool StoreAllKv(const string& filePath,
        const unordered_map<string, string>& m)
    {
        ostringstream oss;
        for (auto it = m.begin(); it != m.end(); it++)
        {
            const string line = UrlEncode(it->first) + "=" + UrlEncode(it->second) + "\n";
            oss << line;
        }
        const string content = oss.str();
        return AtomicWriteFile(filePath, content);
    }

    static string NormalizePosixDir(const string& pathUtf8)
    {
        string s = GB_Utf8Trim(pathUtf8);
        if (s.empty())
        {
            return s;
        }
        // 兼容性：把反斜杠替换为正斜杠
        for (size_t i = 0; i < s.size(); i++)
        {
            if (s[i] == '\\')
            {
                s[i] = '/';
            }
        }
        // 去除尾部斜杠（除根“/”外）
        while (s.size() > 1 && s.back() == '/')
        {
            s.pop_back();
        }
        return s;
    }

#endif
}

string GB_GetGbConfigPath()
{
#if defined(_WIN32)
    // 提示性的“注册表路径”描述字符串
    return GB_STR("计算机\\HKEY_CURRENT_USER\\Software\\GlobalBase");
#else
    return GetLinuxConfigFile();
#endif
}

bool GB_IsExistsGbConfig(const string& keyUtf8)
{
    if (keyUtf8.empty())
    {
        return false;
    }

#if defined(_WIN32)
    string dummy;
    return internal::WinReadValue(keyUtf8, dummy);
#else
    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> m;
    if (!internal::LoadAllKv(filePath, m))
    {
        return false;
    }
    return (m.find(keyUtf8) != m.end());
#endif
}

bool GB_GetGbConfig(const string& keyUtf8, string& valueUtf8)
{
    if (keyUtf8.empty())
    {
        return false;
    }

#if defined(_WIN32)
    return internal::WinReadValue(keyUtf8, valueUtf8);
#else
    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> m;
    if (!internal::LoadAllKv(filePath, m))
    {
        return false;
    }
    auto it = m.find(keyUtf8);
    if (it == m.end())
    {
        return false;
    }
    valueUtf8 = it->second;
    return true;
#endif

}

bool GB_SetGbConfig(const string& keyUtf8, const string& valueUtf8)
{
    if (keyUtf8.empty())
    {
        return false;
    }
#if defined(_WIN32)
    return internal::WinWriteValue(keyUtf8, valueUtf8);
#else
    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> m;
    if (!internal::LoadAllKv(filePath, m))
    {
        return false;
    }
    m[keyUtf8] = valueUtf8;
    return internal::StoreAllKv(filePath, m);
#endif
}

bool GB_DeleteGbConfig(const string& keyUtf8)
{
    if (keyUtf8.empty())
    {
        return false;
    }
#if defined(_WIN32)
    return internal::WinDeleteValue(keyUtf8);
#else
    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> m;
    if (!internal::LoadAllKv(filePath, m))
    {
        return false;
    }
    const size_t erased = m.erase(keyUtf8);
    if (erased == 0)
    {
        return false;
    }
    return internal::StoreAllKv(filePath, m);
#endif
}

unordered_map<string, string> GB_GetAllGbConfig()
{
#if defined(_WIN32)
    return internal::WinEnumAll();
#else
    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> m;
    internal::LoadAllKv(filePath, m);
    return m;
#endif
}

bool GB_IsExistsConfigPath(const string& configPathUtf8)
{
#if defined(_WIN32)
    if (configPathUtf8.empty())
    {
        return false;
    }

    HKEY root = nullptr;
    wstring subKeyW;
    if (!internal::ParseWindowsRegPath(configPathUtf8, root, subKeyW))
    {
        return false;
    }

    // 根键本身（无子键路径）视为存在
    if (subKeyW.empty())
    {
        return true;
    }

    HKEY hKey = nullptr;
    const bool ok = internal::OpenKeyAnyView(root, subKeyW, KEY_READ, hKey);
    if (ok && hKey)
    {
        ::RegCloseKey(hKey);
    }
    return ok;
#else
    const string path = internal::NormalizePosixDir(configPathUtf8);
    if (path.empty())
    {
        return false;
    }

    struct stat st;
    if (stat(path.c_str(), &st) != 0)
    {
        return false;
    }
    return S_ISDIR(st.st_mode) != 0;
#endif
}

bool GB_CreateConfigPath(const string& configPathUtf8, bool recursive)
{
#if defined(_WIN32)
    if (configPathUtf8.empty())
    {
        return false;
    }

    HKEY root = nullptr;
    wstring subKeyW;
    if (!internal::ParseWindowsRegPath(configPathUtf8, root, subKeyW))
    {
        return false;
    }

    if (subKeyW.empty())
    {
        // 只有根键，无需创建
        return true;
    }

    if (recursive)
    {
        // 逐级创建
        HKEY cur = root;
        HKEY created = nullptr;
        bool needClose = false;

        size_t start = 0;
        while (start <= subKeyW.size())
        {
            size_t sep = subKeyW.find(L'\\', start);
            const wstring part = (sep == wstring::npos) ? subKeyW.substr(start) : subKeyW.substr(start, sep - start);

            if (!part.empty())
            {
                HKEY hNew = nullptr;
                DWORD disp = 0;

#if defined(KEY_WOW64_64KEY)
                // 尽可能在 64 位视图下创建，不行再退回当前视图（兼容 32 位系统）
                LONG lr = ::RegCreateKeyExW(cur, part.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hNew, &disp);
                if (lr != ERROR_SUCCESS)
                {
                    lr = ::RegCreateKeyExW(cur, part.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &hNew, &disp);
                }
#else
                LONG lr = ::RegCreateKeyExW(cur, part.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &hNew, &disp);
#endif
                if (lr != ERROR_SUCCESS)
                {
                    if (needClose && cur && cur != root)
                    {
                        ::RegCloseKey(cur);
                    }
                    return false;
                }

                if (needClose && cur && cur != root)
                {
                    ::RegCloseKey(cur);
                }
                cur = hNew;
                needClose = true;
            }

            if (sep == wstring::npos)
            {
                break;
            }
            start = sep + 1;
        }

        if (needClose && cur && cur != root)
        {
            ::RegCloseKey(cur);
        }
        return true;
    }
    else
    {
        // 非递归：仅创建最后一级，父键必须已存在
        const size_t pos = subKeyW.find_last_of(L'\\');
        const wstring parent = (pos == wstring::npos) ? L"" : subKeyW.substr(0, pos);
        const wstring leaf = (pos == wstring::npos) ? subKeyW : subKeyW.substr(pos + 1);

        HKEY hParent = nullptr;
        if (!parent.empty())
        {
            if (!internal::OpenKeyAnyView(root, parent, KEY_CREATE_SUB_KEY | KEY_READ, hParent))
            {
                return false;
            }
        }
        else
        {
            // 父即根
            hParent = root;
        }

        HKEY hLeaf = nullptr;
        DWORD disp = 0;

#if defined(KEY_WOW64_64KEY)
        LONG lr = ::RegCreateKeyExW(hParent, leaf.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hLeaf, &disp);
        if (lr != ERROR_SUCCESS)
        {
            lr = ::RegCreateKeyExW(hParent, leaf.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &hLeaf, &disp);
        }
#else
        LONG lr = ::RegCreateKeyExW(hParent, leaf.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &hLeaf, &disp);
#endif
        if (!parent.empty() && hParent)
        {
            ::RegCloseKey(hParent);
        }

        if (lr != ERROR_SUCCESS)
        {
            return false;
        }

        if (hLeaf)
        {
            ::RegCloseKey(hLeaf);
        }
        return true;
    }
#else
    const string path = internal::NormalizePosixDir(configPathUtf8);
    if (path.empty())
    {
        return false;
    }
    
    if (recursive)
    {
        return internal::MkdirsRecursively(path);
    }
    else
    {
        // 仅创建最后一级，父目录必须存在
        auto dirname = [](const string& p) -> string
            {
                const size_t pos = p.find_last_of('/');
                if (pos == string::npos)
                {
                    return string(".");
                }
                if (pos == 0)
                {
                    return string("/");
                }
                return p.substr(0, pos);
            };
    
        const string parent = dirname(path);
        struct stat st;
        if (stat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        {
            return false;
        }
        if (mkdir(path.c_str(), 0700) == 0)
        {
            return true;
        }
        // 已存在视为成功（且必须是目录）
        if (errno == EEXIST && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        {
            return true;
        }
        return false;
    }
#endif
}

bool GB_IsExistsConfigValue(const string& configPathUtf8, const string& keyNameUtf8)
{
    GB_ConfigItem item;
    if (!GB_GetConfigItem(configPathUtf8, item, false))
    {
		return false;
    }
    for (const GB_ConfigValue& value : item.values)
    {
        if (value.nameUtf8 == keyNameUtf8)
        {
            return true;
        }
    }
    return false;
}

bool GB_IsExistsChildConfig(const string& configPathUtf8, const string& childConfigNameUtf8)
{
    GB_ConfigItem item;
    if (!GB_GetConfigItem(configPathUtf8, item, false))
    {
        return false;
    }
    for (const GB_ConfigItem& childItem : item.childenItems)
    {
        if (childItem.nameUtf8 == childConfigNameUtf8)
        {
            return true;
        }
    }
    return false;
}

bool GB_AddChildConfig(const string& configPathUtf8, const string& childConfigNameUtf8)
{
    const string trimmedPath = GB_Utf8Trim(configPathUtf8);
    const string trimmedChild = GB_Utf8Trim(childConfigNameUtf8);
    if (trimmedPath.empty() || trimmedChild.empty())
    {
        return false;
    }
    // 子项名不允许包含路径分隔符
    if (trimmedChild.find('\\') != string::npos || trimmedChild.find('/') != string::npos)
    {
        return false;
    }

#if defined(_WIN32)
    HKEY root = nullptr;
    wstring subKeyW;
    if (!internal::ParseWindowsRegPath(trimmedPath, root, subKeyW))
    {
        return false;
    }

    // 打开父键
    HKEY hParent = nullptr;
    bool opened = false;
    if (subKeyW.empty())
    {
        hParent = root;
        opened = (hParent != nullptr);
    }
    else
    {
        opened = internal::OpenKeyAnyView(root, subKeyW, KEY_CREATE_SUB_KEY | KEY_READ, hParent);
    }
    if (!opened || !hParent)
    {
        return false;
    }

    // 创建子键
    const wstring childW = GB_Utf8ToWString(trimmedChild);
    HKEY hChild = nullptr;
    DWORD disp = 0;
#if defined(KEY_WOW64_64KEY)
    LONG lr = ::RegCreateKeyExW(hParent, childW.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hChild, &disp);
    if (lr != ERROR_SUCCESS)
    {
        lr = ::RegCreateKeyExW(hParent, childW.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &hChild, &disp);
    }
#else
    LONG lr = ::RegCreateKeyExW(hParent, childW.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &hChild, &disp);
#endif

    if (!subKeyW.empty() && hParent)
    {
        ::RegCloseKey(hParent);
    }
    if (lr != ERROR_SUCCESS)
    {
        return false;
    }
    if (hChild)
    {
        ::RegCloseKey(hChild);
    }
    return true;

#else
    // —— POSIX：写入占位键以“显式存在”该子项 —— //
    const string normalize = [&]()
        {
            string s = trimmedPath;
            for (char& ch : s) { if (ch == '\\') ch = '/'; }
            // 去两端 '/'
            while (!s.empty() && s.front() == '/') s.erase(s.begin());
            while (!s.empty() && s.back() == '/') s.pop_back();
            return s;
        }();

    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> m;
    if (!internal::LoadAllKv(filePath, m))
    {
        return false;
    }

    // 父/子 前缀
    const string childPrefix = normalize.empty() ? trimmedChild : (normalize + "/" + trimmedChild);
    const string childPrefixSlash = childPrefix + "/";

    // 已存在（任意一个以 childPrefix/ 开头的键即认为子项存在）
    for (const auto& kv : m)
    {
        if (GB_Utf8StartsWith(kv.first, childPrefixSlash, true))
        {
            return false;
        }
    }

    // 写入一个占位键，使该子项在树上可见；占位名采用不太可能冲突的保留名
    static const string kPlaceholderName = ".__placeholder__";
    m[childPrefixSlash + kPlaceholderName] = "";

    return internal::StoreAllKv(filePath, m);
#endif
}

bool GB_DeleteChildConfig(const string& configPathUtf8, const string& childConfigNameUtf8)
{
    const string trimmedPath = GB_Utf8Trim(configPathUtf8);
    const string trimmedChild = GB_Utf8Trim(childConfigNameUtf8);
    if (trimmedPath.empty() || trimmedChild.empty())
    {
        return false;
    }
    if (trimmedChild.find('\\') != string::npos || trimmedChild.find('/') != string::npos)
    {
        return false;
    }

#if defined(_WIN32)
    HKEY root = nullptr;
    wstring subKeyW;
    if (!internal::ParseWindowsRegPath(trimmedPath, root, subKeyW))
    {
        return false;
    }

    HKEY hParent = nullptr;
    bool opened = false;
    if (subKeyW.empty())
    {
        hParent = root;
        opened = (hParent != nullptr);
    }
    else
    {
        opened = internal::OpenKeyAnyView(root, subKeyW, KEY_READ | KEY_WRITE, hParent);
    }
    if (!opened || !hParent)
    {
        return false;
    }

    const wstring childW = GB_Utf8ToWString(trimmedChild);
    // 递归删除子键
    LONG lr = ::RegDeleteTreeW(hParent, childW.c_str());

    if (!subKeyW.empty() && hParent)
    {
        ::RegCloseKey(hParent);
    }
    return lr == ERROR_SUCCESS;

#else
    const string normalize = [&]()
        {
            string s = trimmedPath;
            for (char& ch : s) { if (ch == '\\') ch = '/'; }
            while (!s.empty() && s.front() == '/') s.erase(s.begin());
            while (!s.empty() && s.back() == '/') s.pop_back();
            return s;
        }();

    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> m;
    if (!internal::LoadAllKv(filePath, m))
    {
        return false;
    }

    const string childPrefix = normalize.empty() ? trimmedChild : (normalize + "/" + trimmedChild);
    const string childPrefixSlash = childPrefix + "/";

    bool erasedAny = false;
    for (auto it = m.begin(); it != m.end(); )
    {
        if (GB_Utf8StartsWith(it->first, childPrefixSlash, true))
        {
            it = m.erase(it);
            erasedAny = true;
            continue;
        }
        ++it;
    }
    if (!erasedAny)
    {
        return false; // 不存在该子项
    }
    return internal::StoreAllKv(filePath, m);
#endif
}

bool GB_RenameChildConfig(const string& configPathUtf8, const string& childConfigNameUtf8, const string& newNameUtf8)
{
    const string trimmedPath = GB_Utf8Trim(configPathUtf8);
    const string oldName = GB_Utf8Trim(childConfigNameUtf8);
    const string newName = GB_Utf8Trim(newNameUtf8);

    if (trimmedPath.empty() || oldName.empty() || newName.empty())
    {
        return false;
    }
    if (oldName == newName)
    {
        return true; // 视为已满足
    }
    if (oldName.find('\\') != string::npos || oldName.find('/') != string::npos) return false;
    if (newName.find('\\') != string::npos || newName.find('/') != string::npos) return false;

#if defined(_WIN32)
    HKEY root = nullptr;
    wstring subKeyW;
    if (!internal::ParseWindowsRegPath(trimmedPath, root, subKeyW))
    {
        return false;
    }

    HKEY hParent = nullptr;
    bool opened = false;
    if (subKeyW.empty())
    {
        hParent = root;
        opened = (hParent != nullptr);
    }
    else
    {
        opened = internal::OpenKeyAnyView(root, subKeyW, KEY_READ | KEY_WRITE, hParent);
    }
    if (!opened || !hParent)
    {
        return false;
    }

    const wstring oldW = GB_Utf8ToWString(oldName);
    const wstring newW = GB_Utf8ToWString(newName);

    // 1) 优先尝试 RegRenameKey（Win7+）
    using PFN_RegRenameKey = LONG(WINAPI*)(HKEY, LPCWSTR, LPCWSTR);
    HMODULE adv = ::GetModuleHandleW(L"Advapi32.dll");
    PFN_RegRenameKey pRename = adv ? reinterpret_cast<PFN_RegRenameKey>(::GetProcAddress(adv, "RegRenameKey")) : nullptr;

    if (pRename)
    {
        LONG lr = pRename(hParent, oldW.c_str(), newW.c_str());
        if (!subKeyW.empty() && hParent)
        {
            ::RegCloseKey(hParent);
        }
        return lr == ERROR_SUCCESS;
    }

    // 2) 兼容路径：复制整棵子树到新名，再删除旧名
    function<bool(HKEY, const wstring&, const wstring&)> copyTree;
    copyTree = [&](HKEY parent, const wstring& srcName, const wstring& dstName) -> bool
        {
            HKEY hSrc = nullptr;
            if (::RegOpenKeyExW(parent, srcName.c_str(), 0, KEY_READ | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &hSrc) != ERROR_SUCCESS)
            {
                return false;
            }

            HKEY hDst = nullptr;
            DWORD disp = 0;
            if (::RegCreateKeyExW(parent, dstName.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &hDst, &disp) != ERROR_SUCCESS)
            {
                ::RegCloseKey(hSrc);
                return false;
            }

            // 复制值
            DWORD subKeyCount = 0, maxSubKeyLen = 0;
            DWORD valueCount = 0, maxValueNameLen = 0, maxValueLen = 0;
            if (::RegQueryInfoKeyW(hSrc, nullptr, nullptr, nullptr, &subKeyCount, &maxSubKeyLen, nullptr,
                &valueCount, &maxValueNameLen, &maxValueLen, nullptr, nullptr) != ERROR_SUCCESS)
            {
                ::RegCloseKey(hSrc);
                ::RegCloseKey(hDst);
                return false;
            }

            vector<wchar_t> valueNameBuf(static_cast<size_t>(maxValueNameLen) + 1, L'\0');
            vector<BYTE> valueDataBuf(static_cast<size_t>(maxValueLen) + 2 * sizeof(wchar_t), 0);

            for (DWORD i = 0; i < valueCount; i++)
            {
                DWORD nameLen = static_cast<DWORD>(valueNameBuf.size());
                DWORD dataLen = static_cast<DWORD>(valueDataBuf.size());
                DWORD type = 0;
                fill(valueNameBuf.begin(), valueNameBuf.end(), L'\0');
                fill(valueDataBuf.begin(), valueDataBuf.end(), 0);

                if (::RegEnumValueW(hSrc, i, valueNameBuf.data(), &nameLen, nullptr, &type,
                    reinterpret_cast<BYTE*>(valueDataBuf.data()), &dataLen) == ERROR_SUCCESS)
                {
                    ::RegSetValueExW(hDst, valueNameBuf.data(), 0, type,
                        reinterpret_cast<BYTE*>(valueDataBuf.data()), dataLen);
                }
            }

            // 递归复制子键
            vector<wchar_t> subKeyNameBuf(static_cast<size_t>(maxSubKeyLen) + 1, L'\0');
            for (DWORD i = 0; i < subKeyCount; i++)
            {
                DWORD nameLen = static_cast<DWORD>(subKeyNameBuf.size());
                fill(subKeyNameBuf.begin(), subKeyNameBuf.end(), L'\0');

                if (::RegEnumKeyExW(hSrc, i, subKeyNameBuf.data(), &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
                {
                    const wstring child = subKeyNameBuf.data();
                    const wstring srcChildPath = srcName + L"\\" + child;
                    const wstring dstChildPath = dstName + L"\\" + child;

                    if (!copyTree(parent, srcChildPath, dstChildPath))
                    {
                        ::RegCloseKey(hSrc);
                        ::RegCloseKey(hDst);
                        return false;
                    }
                }
            }
            ::RegCloseKey(hSrc);
            ::RegCloseKey(hDst);
            return true;
        };

    // 使用：
    const bool copied = copyTree(hParent, oldW, newW);
    bool ok = false;
    if (copied)
    {
        ok = (::RegDeleteTreeW(hParent, oldW.c_str()) == ERROR_SUCCESS);
    }

    if (!subKeyW.empty() && hParent)
    {
        ::RegCloseKey(hParent);
    }
    return ok;

#else
    const string normalize = [&]()
        {
            string s = trimmedPath;
            for (char& ch : s) { if (ch == '\\') ch = '/'; }
            while (!s.empty() && s.front() == '/') s.erase(s.begin());
            while (!s.empty() && s.back() == '/') s.pop_back();
            return s;
        }();

    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> m;
    if (!internal::LoadAllKv(filePath, m))
    {
        return false;
    }

    const string oldPrefix = normalize.empty() ? oldName : (normalize + "/" + oldName);
    const string oldPrefixSlash = oldPrefix + "/";

    const string newPrefix = normalize.empty() ? newName : (normalize + "/" + newName);
    const string newPrefixSlash = newPrefix + "/";

    // 目的前缀已存在则失败，避免不小心覆盖
    for (const auto& kv : m)
    {
        if (GB_Utf8StartsWith(kv.first, newPrefixSlash, true))
        {
            return false;
        }
    }

    bool touched = false;
    unordered_map<string, string> out;
    out.reserve(m.size());

    for (const auto& kv : m)
    {
        const string& key = kv.first;
        const string& val = kv.second;

        if (GB_Utf8StartsWith(key, oldPrefixSlash, true))
        {
            // oldPrefix/.... -> newPrefix/....
            const string suffix = key.substr(oldPrefixSlash.size());
            out[newPrefixSlash + suffix] = val;
            touched = true;
        }
        else
        {
            out[key] = val;
        }
    }

    if (!touched)
    {
        return false; // 没有该子项
    }
    return internal::StoreAllKv(filePath, out);
#endif
}

bool GB_GetConfigValue(const string& configPathUtf8, const string& keyNameUtf8, GB_ConfigValue& configValue)
{
    GB_ConfigItem item;
    if (!GB_GetConfigItem(configPathUtf8, item, false))
    {
        return false;
    }
    for (const GB_ConfigValue& value : item.values)
    {
        if (value.nameUtf8 == keyNameUtf8)
        {
            configValue = value;
            return true;
        }
    }
    return false;
}

bool GB_SetConfigValue(const string& configPathUtf8, const string& keyNameUtf8, const GB_ConfigValue& configValue)
{
    const string trimmedPath = GB_Utf8Trim(configPathUtf8);
    const string trimmedKey = GB_Utf8Trim(keyNameUtf8);
    if (trimmedPath.empty() || trimmedKey.empty())
    {
        return false;
    }

#if defined(_WIN32)
    HKEY root = nullptr;
    wstring subKeyW;
    if (!internal::ParseWindowsRegPath(trimmedPath, root, subKeyW))
    {
        return false;
    }

    // 打开（或创建）目标子键
    HKEY hKey = nullptr;
    bool needClose = false;
    if (subKeyW.empty())
    {
        // 只有根键
        hKey = root;
    }
    else
    {
        if (!internal::OpenKeyAnyView(root, subKeyW, KEY_SET_VALUE | KEY_QUERY_VALUE, hKey))
        {
            // 尝试递归创建后再打开
            (void)GB_CreateConfigPath(trimmedPath, true);
            if (!internal::OpenKeyAnyView(root, subKeyW, KEY_SET_VALUE | KEY_QUERY_VALUE, hKey))
            {
                return false;
            }
        }
        needClose = true;
    }

    const wstring valueNameW = GB_Utf8ToWString(trimmedKey);

    DWORD type = REG_NONE;
    const BYTE* data = nullptr;
    DWORD cbData = 0;
    vector<BYTE> buf;                  // 持有实际数据
    vector<wchar_t> wstrMultiBuffer;   // 专供 REG_MULTI_SZ

    switch (configValue.valueType)
    {
    case GB_ConfigValueType::GbConfigValueType_String:
    case GB_ConfigValueType::GbConfigValueType_ExpandString:
    {
        const wstring ws = GB_Utf8ToWString(configValue.valueUtf8);
        const size_t bytes = (ws.size() + 1) * sizeof(wchar_t); // 包含终止\0
        buf.resize(bytes);
        memcpy(buf.data(), ws.c_str(), bytes);
        type = (configValue.valueType == GB_ConfigValueType::GbConfigValueType_ExpandString) ? REG_EXPAND_SZ : REG_SZ;
        data = buf.data();
        cbData = static_cast<DWORD>(buf.size());
        break;
    } // 这些类型写入时需包含结尾的NUL字节

    case GB_ConfigValueType::GbConfigValueType_MultiString:
    {
        // REG_MULTI_SZ：多个以\0结尾的字符串，整体以额外的\0终止（双\0）
        for (size_t i = 0; i < configValue.multiStringValuesUtf8.size(); i++)
        {
            const wstring ws = GB_Utf8ToWString(configValue.multiStringValuesUtf8[i]);
            wstrMultiBuffer.insert(wstrMultiBuffer.end(), ws.begin(), ws.end());
            wstrMultiBuffer.push_back(L'\0');
        }
        wstrMultiBuffer.push_back(L'\0'); // 额外终止\0，形成双\0
        const size_t bytes = wstrMultiBuffer.size() * sizeof(wchar_t);
        buf.resize(bytes);
        memcpy(buf.data(), wstrMultiBuffer.data(), bytes);
        type = REG_MULTI_SZ;
        data = buf.data();
        cbData = static_cast<DWORD>(buf.size());
        break;
    } // REG_MULTI_SZ 的格式要求见文档：以双NUL结尾

    case GB_ConfigValueType::GbConfigValueType_DWord:
    {
        const DWORD v = static_cast<DWORD>(configValue.dwordValue);
        type = REG_DWORD;
        data = reinterpret_cast<const BYTE*>(&v);
        cbData = sizeof(DWORD);
        break;
    }

    case GB_ConfigValueType::GbConfigValueType_QWord:
    {
        const ULONGLONG v = static_cast<ULONGLONG>(configValue.qwordValue);
        type = REG_QWORD;
        data = reinterpret_cast<const BYTE*>(&v);
        cbData = sizeof(ULONGLONG);
        break;
    } // QWORD = 64 位整数类型

    case GB_ConfigValueType::GbConfigValueType_Binary:
    {
        buf.assign(configValue.binaryValue.begin(), configValue.binaryValue.end());
        type = REG_BINARY;
        data = buf.empty() ? reinterpret_cast<const BYTE*>("") : buf.data();
        cbData = static_cast<DWORD>(buf.size());
        break;
    }

    default:
    {
        // 未知类型：按普通字符串兜底
        const wstring ws = GB_Utf8ToWString(configValue.valueUtf8);
        const size_t bytes = (ws.size() + 1) * sizeof(wchar_t);
        buf.resize(bytes);
        memcpy(buf.data(), ws.c_str(), bytes);
        type = REG_SZ;
        data = buf.data();
        cbData = static_cast<DWORD>(buf.size());
        break;
    }
    }

    const LONG lr = ::RegSetValueExW(hKey, valueNameW.empty() ? nullptr : valueNameW.c_str(), 0, type, data, cbData);
    if (needClose && hKey && hKey != root)
    {
        ::RegCloseKey(hKey);
    }
    return (lr == ERROR_SUCCESS);

#else
    // —— POSIX：写入到 ~/.config/globalbase/config.kv 的“逻辑路径/键名” —— //
    auto Normalize = [](string s) -> string
        {
            s = GB_Utf8Trim(s);
            for (size_t i = 0; i < s.size(); i++)
            {
                if (s[i] == '\\')
                {
                    s[i] = '/';
                }
            }
            while (!s.empty() && s.front() == '/')
            {
                s.erase(s.begin());
            }
            while (!s.empty() && s.back() == '/')
            {
                s.pop_back();
            }
            return s;
        };

    // value 名不允许带分层分隔符，避免误造更深层路径
    if (trimmedKey.find('/') != string::npos || trimmedKey.find('\\') != string::npos)
    {
        return false;
    }

    const string prefix = Normalize(trimmedPath);
    const string fullKey = prefix.empty() ? trimmedKey : (prefix + "/" + trimmedKey);

    string toStore;
    switch (configValue.valueType)
    {
    case GbConfigValueType::GbConfigValueType_String:
    case GbConfigValueType::GbConfigValueType_ExpandString:
        toStore = configValue.valueUtf8;
        break;

    case GbConfigValueType::GbConfigValueType_DWord:
        toStore = to_string(static_cast<unsigned long long>(configValue.dwordValue));
        break;

    case GbConfigValueType::GbConfigValueType_QWord:
        toStore = to_string(static_cast<unsigned long long>(configValue.qwordValue));
        break;

    case GbConfigValueType::GbConfigValueType_MultiString:
    {
        // 简单可读的序列化：用换行连接
        for (size_t i = 0; i < configValue.multiStringValuesUtf8.size(); i++)
        {
            if (i > 0) toStore.push_back('\n');
            toStore += configValue.multiStringValuesUtf8[i];
        }
        break;
    }

    case GbConfigValueType::GbConfigValueType_Binary:
    {
        static const char* hex = "0123456789ABCDEF";
        string hexStr;
        hexStr.reserve(configValue.binaryValue.size() * 2);
        for (size_t i = 0; i < configValue.binaryValue.size(); i++)
        {
            const uint8_t b = configValue.binaryValue[i];
            hexStr.push_back(hex[(b >> 4) & 0xF]);
            hexStr.push_back(hex[b & 0xF]);
        }
        toStore = hexStr;
        break;
    }

    default:
        toStore = configValue.valueUtf8;
        break;
    }

    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> kv;
    if (!internal::LoadAllKv(filePath, kv))
    {
        return false;
    }
    kv[fullKey] = toStore;
    return internal::StoreAllKv(filePath, kv);
#endif
}

bool GB_DeleteConfigValue(const string& configPathUtf8, const string& keyNameUtf8)
{
    const string trimmedPath = GB_Utf8Trim(configPathUtf8);
    const string trimmedKey = GB_Utf8Trim(keyNameUtf8);
    if (trimmedPath.empty() || trimmedKey.empty())
    {
        return false;
    }

#if defined(_WIN32)
    HKEY root = nullptr;
    wstring subKeyW;
    if (!internal::ParseWindowsRegPath(trimmedPath, root, subKeyW))
    {
        return false;
    }

    HKEY hKey = nullptr;
    bool needClose = false;

    if (subKeyW.empty())
    {
        hKey = root;
    }
    else
    {
        if (!internal::OpenKeyAnyView(root, subKeyW, KEY_SET_VALUE | KEY_QUERY_VALUE, hKey))
        {
            return false;
        }
        needClose = true;
    }

    const wstring valueNameW = GB_Utf8ToWString(trimmedKey);
    const LONG lr = ::RegDeleteValueW(hKey, valueNameW.empty() ? nullptr : valueNameW.c_str());

    if (needClose && hKey)
    {
        ::RegCloseKey(hKey);
    }
    return lr == ERROR_SUCCESS;

#else
    // 不允许 key 名里出现路径分隔符
    if (trimmedKey.find('\\') != string::npos || trimmedKey.find('/') != string::npos)
    {
        return false;
    }

    const string normalize = [&]()
        {
            string s = trimmedPath;
            for (char& ch : s) { if (ch == '\\') ch = '/'; }
            while (!s.empty() && s.front() == '/') s.erase(s.begin());
            while (!s.empty() && s.back() == '/') s.pop_back();
            return s;
        }();

    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> m;
    if (!internal::LoadAllKv(filePath, m))
    {
        return false;
    }

    const string fullKey = normalize.empty() ? trimmedKey : (normalize + "/" + trimmedKey);

    const size_t erased = m.erase(fullKey);
    if (erased == 0)
    {
        return false;
    }
    return internal::StoreAllKv(filePath, m);
#endif
}

bool GB_ClearConfigValue(const string& configPathUtf8)
{
    const string trimmedPath = GB_Utf8Trim(configPathUtf8);
    if (trimmedPath.empty())
    {
        return false;
    }

#if defined(_WIN32)
    HKEY root = nullptr;
    wstring subKeyW;
    if (!internal::ParseWindowsRegPath(trimmedPath, root, subKeyW))
    {
        return false;
    }

    HKEY hKey = nullptr;
    bool needClose = false;

    if (subKeyW.empty())
    {
        hKey = root;
    }
    else
    {
        if (!internal::OpenKeyAnyView(root, subKeyW, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_READ, hKey))
        {
            return false;
        }
        needClose = true;
    }

    // 查询最大值名长度以便分配缓冲
    DWORD valueCount = 0;
    DWORD maxValueNameLen = 0;
    const LONG qi = ::RegQueryInfoKeyW(
        hKey, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        &valueCount, &maxValueNameLen,
        nullptr, nullptr, nullptr);

    if (qi != ERROR_SUCCESS)
    {
        if (needClose && hKey) { ::RegCloseKey(hKey); }
        return false;
    }

    vector<wchar_t> nameBuf(maxValueNameLen + 1, L'\0');
    bool ok = true;

    // 始终用索引0枚举并删除，避免删除后索引位移
    while (true)
    {
        DWORD nameLen = maxValueNameLen + 1;
        LONG er = ::RegEnumValueW(hKey, 0, nameBuf.data(), &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (er == ERROR_NO_MORE_ITEMS)
        {
            break;
        }
        if (er != ERROR_SUCCESS)
        {
            ok = false;
            break;
        }
        const LONG dr = ::RegDeleteValueW(hKey, nameLen == 0 ? nullptr : nameBuf.data()); // 默认值名长度为0
        if (dr != ERROR_SUCCESS)
        {
            ok = false;
            break;
        }
    }

    if (needClose && hKey)
    {
        ::RegCloseKey(hKey);
    }
    return ok;

#else
    const string normalize = [&]()
        {
            string s = trimmedPath;
            for (char& ch : s) { if (ch == '\\') ch = '/'; }
            while (!s.empty() && s.front() == '/') s.erase(s.begin());
            while (!s.empty() && s.back() == '/') s.pop_back();
            return s;
        }();

    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> m;
    if (!internal::LoadAllKv(filePath, m))
    {
        return false;
    }

    // 仅删除“本层”的键值：prefix/KeyName（后缀不再含‘/’）
    const string prefix = normalize;
    const string prefixSlash = prefix.empty() ? "" : (prefix + "/");

    vector<string> keysToErase;
    keysToErase.reserve(64);

    for (const auto& kv : m)
    {
        const string& full = kv.first;
        if (!prefix.empty())
        {
            if (!GB_Utf8StartsWith(full, prefixSlash, true))
            {
                continue;
            }
            const string suffix = full.substr(prefixSlash.size());
            if (suffix.find('/') == string::npos)
            {
                keysToErase.push_back(full);
            }
        }
        else
        {
            // 根层：没有‘/’的视为根层键值
            if (full.find('/') == string::npos)
            {
                keysToErase.push_back(full);
            }
        }
    }

    for (const string& k : keysToErase)
    {
        m.erase(k);
    }

    // 清空本层即算成功（即使本来就是空的）
    return internal::StoreAllKv(filePath, m);
#endif
}

bool GB_RenameConfigValue(const string& configPathUtf8, const string& keyNameUtf8, const string& newNameUtf8)
{
    const string trimmedPath = GB_Utf8Trim(configPathUtf8);
    const string oldName = GB_Utf8Trim(keyNameUtf8);
    const string newName = GB_Utf8Trim(newNameUtf8);

    if (trimmedPath.empty() || oldName.empty() || newName.empty())
    {
        return false;
    }
    if (oldName == newName)
    {
        return true;
    }
    if (oldName.find('\\') != string::npos || oldName.find('/') != string::npos) return false;
    if (newName.find('\\') != string::npos || newName.find('/') != string::npos) return false;

#if defined(_WIN32)
    HKEY root = nullptr;
    wstring subKeyW;
    if (!internal::ParseWindowsRegPath(trimmedPath, root, subKeyW))
    {
        return false;
    }

    HKEY hKey = nullptr;
    bool needClose = false;

    if (subKeyW.empty())
    {
        hKey = root;
    }
    else
    {
        if (!internal::OpenKeyAnyView(root, subKeyW, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_READ, hKey))
        {
            return false;
        }
        needClose = true;
    }

    const wstring oldW = GB_Utf8ToWString(oldName);
    const wstring newW = GB_Utf8ToWString(newName);

    // 若目标名已存在，则失败以免覆盖
    DWORD typeNew = 0;
    DWORD cbNew = 0;
    LONG hasNew = ::RegQueryValueExW(hKey, newW.c_str(), nullptr, &typeNew, nullptr, &cbNew);
    if (hasNew == ERROR_SUCCESS)
    {
        if (needClose && hKey) { ::RegCloseKey(hKey); }
        return false;
    }

    // 读取旧值（类型 + 数据）
    DWORD type = 0;
    DWORD cbData = 0;
    LONG q1 = ::RegQueryValueExW(hKey, oldW.c_str(), nullptr, &type, nullptr, &cbData);
    if (q1 != ERROR_SUCCESS)
    {
        if (needClose && hKey) { ::RegCloseKey(hKey); }
        return false;
    }

    vector<BYTE> buf(cbData ? cbData : 1);
    DWORD cb = cbData;
    LONG q2 = ::RegQueryValueExW(hKey, oldW.c_str(), nullptr, &type, buf.data(), &cb);
    if (q2 != ERROR_SUCCESS)
    {
        if (needClose && hKey) { ::RegCloseKey(hKey); }
        return false;
    }

    // 写入新名同值
    LONG s = ::RegSetValueExW(hKey, newW.c_str(), 0, type, buf.empty() ? nullptr : buf.data(), cb);
    if (s != ERROR_SUCCESS)
    {
        if (needClose && hKey) { ::RegCloseKey(hKey); }
        return false;
    }

    // 删除旧名
    const LONG d = ::RegDeleteValueW(hKey, oldW.c_str());

    if (needClose && hKey)
    {
        ::RegCloseKey(hKey);
    }

    return d == ERROR_SUCCESS;

#else
    const string normalize = [&]()
        {
            string s = trimmedPath;
            for (char& ch : s) { if (ch == '\\') ch = '/'; }
            while (!s.empty() && s.front() == '/') s.erase(s.begin());
            while (!s.empty() && s.back() == '/') s.pop_back();
            return s;
        }();

    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> m;
    if (!internal::LoadAllKv(filePath, m))
    {
        return false;
    }

    const string oldKey = normalize.empty() ? oldName : (normalize + "/" + oldName);
    const string newKey = normalize.empty() ? newName : (normalize + "/" + newName);

    auto itOld = m.find(oldKey);
    if (itOld == m.end())
    {
        return false;
    }
    if (m.find(newKey) != m.end())
    {
        return false; // 目标已存在，避免覆盖
    }

    m[newKey] = itOld->second;
    m.erase(itOld);

    return internal::StoreAllKv(filePath, m);
#endif
}

bool GB_GetConfigItem(const string& configPathUtf8, GB_ConfigItem& configItem, bool recursive)
{
    configItem = GB_ConfigItem{};
    const string trimmed = GB_Utf8Trim(configPathUtf8);
    if (trimmed.empty())
    {
        return false;
    }

#if defined(_WIN32)
    // —— Windows：注册表递归 ——
    HKEY root = nullptr;
    wstring subKeyW;
    if (!internal::ParseWindowsRegPath(trimmed, root, subKeyW)) // 解析“计算机\HKCU\.../...”等写法
    {
        return false;
    }

    // 取显示名：叶子名（兼容 / 与 \）
    configItem.nameUtf8 = internal::GetLeafNameFromPathUtf8(trimmed, '\\', '/');

    // 小工具：把 Windows REG_* 类型映射到 GbConfigValueType
    auto MapRegType = [](DWORD t) -> GB_ConfigValueType
        {
            switch (t)
            {
            case REG_SZ:          return GB_ConfigValueType::GbConfigValueType_String;
            case REG_EXPAND_SZ:   return GB_ConfigValueType::GbConfigValueType_ExpandString;
            case REG_MULTI_SZ:    return GB_ConfigValueType::GbConfigValueType_MultiString;
            case REG_DWORD:       return GB_ConfigValueType::GbConfigValueType_DWord;
            case REG_QWORD:       return GB_ConfigValueType::GbConfigValueType_QWord;
            case REG_BINARY:      return GB_ConfigValueType::GbConfigValueType_Binary;
            default:              return GB_ConfigValueType::GbConfigValueType_Unknown;
            }
        };

    // 递归函数：给定 root/subKeyW，填充 outItem
    function<bool(HKEY, const wstring&, GB_ConfigItem&)> Recurse;
    Recurse = [&](HKEY rootKey, const wstring& keyPathW, GB_ConfigItem& outItem) -> bool
        {
            // 打开当前键（尽量 64 位视图，不行回退）
            HKEY hKey = nullptr;
            bool opened = false;
            if (keyPathW.empty())
            {
                // 根键本身：直接用 rootKey 句柄
                hKey = rootKey;
                opened = (hKey != nullptr);
            }
            else
            {
                opened = internal::OpenKeyAnyView(rootKey, keyPathW, KEY_READ | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, hKey);
            }
            if (!opened || !hKey)
            {
                return false;
            }

            // 询问信息：值数量/最大名/最大数据/子键数量/最大子键名
            DWORD subKeyCount = 0, maxSubKeyLen = 0;
            DWORD valueCount = 0, maxValueNameLen = 0, maxValueLen = 0;
            LONG lrInfo = ::RegQueryInfoKeyW(
                hKey,
                nullptr, nullptr, nullptr,
                &subKeyCount, &maxSubKeyLen, nullptr,
                &valueCount, &maxValueNameLen, &maxValueLen,
                nullptr, nullptr
            );
            if (lrInfo != ERROR_SUCCESS)
            {
                if (!keyPathW.empty() && hKey) ::RegCloseKey(hKey);
                return false;
            }

            vector<wchar_t> valueNameBuf(static_cast<size_t>(maxValueNameLen) + 1, L'\0');
            vector<BYTE>    valueDataBuf(static_cast<size_t>(maxValueLen) + 2 * sizeof(wchar_t), 0); // 给 REG_SZ/REG_MULTI_SZ 预留额外 NUL
            vector<wchar_t> subKeyNameBuf(static_cast<size_t>(maxSubKeyLen) + 1, L'\0');

            // —— 枚举当前键的所有值（含可能的“默认值”，其名称可为空字符串） ——
            for (DWORD i = 0; i < valueCount; i++)
            {
                DWORD nameLen = static_cast<DWORD>(valueNameBuf.size());
                DWORD dataLen = static_cast<DWORD>(valueDataBuf.size());
                DWORD type = 0;
                fill(valueNameBuf.begin(), valueNameBuf.end(), L'\0');
                fill(valueDataBuf.begin(), valueDataBuf.end(), 0);

                LONG lr = ::RegEnumValueW(
                    hKey, i,
                    valueNameBuf.data(), &nameLen,
                    nullptr,
                    &type,
                    reinterpret_cast<BYTE*>(valueDataBuf.data()), &dataLen
                );
                if (lr != ERROR_SUCCESS)
                {
                    // 名/数据缓冲可能不够，保守地扩容重试一次
                    if (lr == ERROR_MORE_DATA)
                    {
                        // 再问一次最新的最大值
                        DWORD _sk = 0, _msl = 0, _vc = 0, _mvnl = 0, _mvl = 0;
                        if (::RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, &_sk, &_msl, nullptr, &_vc, &_mvnl, &_mvl, nullptr, nullptr) == ERROR_SUCCESS)
                        {
                            valueNameBuf.assign(static_cast<size_t>(_mvnl) + 1, L'\0');
                            valueDataBuf.assign(static_cast<size_t>(_mvl) + 2 * sizeof(wchar_t), 0);
                            nameLen = static_cast<DWORD>(valueNameBuf.size());
                            dataLen = static_cast<DWORD>(valueDataBuf.size());
                            lr = ::RegEnumValueW(hKey, i, valueNameBuf.data(), &nameLen, nullptr, &type,
                                reinterpret_cast<BYTE*>(valueDataBuf.data()), &dataLen);
                        }
                    }
                }
                if (lr != ERROR_SUCCESS)
                {
                    continue;
                }

                GB_ConfigValue one;
                one.nameUtf8 = internal::WideToUtf8(wstring(valueNameBuf.data()));
                one.valueType = MapRegType(type);

                switch (type)
                {
                case REG_SZ:
                case REG_EXPAND_SZ:
                {
                    const wstring ws(reinterpret_cast<wchar_t*>(valueDataBuf.data()), dataLen / sizeof(wchar_t));
                    one.valueUtf8 = internal::WideToUtf8(wstring(ws.c_str()));
                    
                    break;
                }
                case REG_MULTI_SZ:
                {
                    // 以双 NUL 结尾，逐段切分
                    const wchar_t* p = reinterpret_cast<const wchar_t*>(valueDataBuf.data());
                    const size_t n = dataLen / sizeof(wchar_t);
                    size_t pos = 0;
                    while (pos < n && p[pos] != L'\0')
                    {
                        const wchar_t* s = &p[pos];
                        const size_t len = wcslen(s);
                        one.multiStringValuesUtf8.emplace_back(internal::WideToUtf8(wstring(s)));
                        pos += len + 1;
                    }
                    break;
                }
                case REG_DWORD:
                    if (dataLen >= sizeof(DWORD))
                    {
                        DWORD v = 0;
                        memcpy(&v, valueDataBuf.data(), sizeof(DWORD));
                        one.dwordValue = static_cast<uint32_t>(v);
                    }
                    break;
                case REG_QWORD:
                    if (dataLen >= sizeof(ULONGLONG))
                    {
                        ULONGLONG v = 0;
                        memcpy(&v, valueDataBuf.data(), sizeof(ULONGLONG));
                        one.qwordValue = static_cast<uint64_t>(v);
                    }
                    break;
                case REG_BINARY:
                default:
                    one.binaryValue.assign(valueDataBuf.begin(), valueDataBuf.begin() + dataLen);
                    break;
                }

                outItem.values.emplace_back(move(one));
            }

            // —— 枚举子键并递归 ——
            for (DWORD i = 0; i < subKeyCount; i++)
            {
                DWORD subLen = static_cast<DWORD>(subKeyNameBuf.size());
                fill(subKeyNameBuf.begin(), subKeyNameBuf.end(), L'\0');

                if (::RegEnumKeyExW(hKey, i, subKeyNameBuf.data(), &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                {
                    continue;
                }

                GB_ConfigItem child;
                child.nameUtf8 = internal::WideToUtf8(wstring(subKeyNameBuf.data()));
                if (!recursive)
                {
                    // 非递归：仅列出直接子键名，不下钻，不读取其键值
                    outItem.childenItems.emplace_back(move(child));
                    continue;
                }

                const wstring childPathW = keyPathW.empty() ? wstring(subKeyNameBuf.data()) : (keyPathW + L'\\' + wstring(subKeyNameBuf.data()));

                if (Recurse(rootKey, childPathW, child))
                {
                    outItem.childenItems.emplace_back(move(child));
                }
            }

            if (!keyPathW.empty() && hKey)
            {
                ::RegCloseKey(hKey);
            }
            return true;
        };

    return Recurse(root, subKeyW, configItem);

#else
    // —— POSIX：把 config.kv 的“键名”按‘/’分层当作“路径树” ——
    // 1) 全量载入 kv
    const string filePath = internal::GetLinuxConfigFile();
    unordered_map<string, string> kv;
    if (!internal::LoadAllKv(filePath, kv))
    {
        return false;
    }

    // 2) 归一化前缀：把 '\' → '/'，去掉首尾 '/'
    auto Normalize = [](string s) -> string
        {
            s = GB_Utf8Trim(s);
            for (size_t i = 0; i < s.size(); i++)
            {
                if (s[i] == '\\')
                {
                    s[i] = '/';
                }
            }
            while (!s.empty() && s.front() == '/')
            {
                s.erase(s.begin());
            }
            while (!s.empty() && s.back() == '/')
            {
                s.pop_back();
            }
            return s;
        };

    const string prefix = Normalize(trimmed);
    configItem.nameUtf8 = internal::GetLeafNameFromPathUtf8(prefix.empty() ? string("/") : prefix, '/', '/');

    // 递归构造：从给定 prefix 出发，把 kv 中以 prefix 为前缀的键分成“直接值”和“子项首段”
    function<void(const string&, GbConfigItem&)> BuildTree;
    BuildTree = [&](const string& curPrefix, GbConfigItem& outItem)
        {
            const string head = curPrefix;
            const string need = head.empty() ? string() : (head + "/");

            // 收集：childName -> true
            unordered_map<string, bool> childNames;

            for (const auto& it : kv)
            {
                const string& fullKey = it.first;
                if (!head.empty())
                {
                    if (fullKey.size() < need.size()) continue;
                    if (fullKey.compare(0, need.size(), need) != 0) continue;
                }
                // 相对部分
                const string rel = head.empty() ? fullKey : fullKey.substr(need.size());
                const size_t slashPos = rel.find('/');
                if (slashPos == string::npos)
                {
                    // 直接在本层的“值”
                    GbConfigValue one;
                    one.nameUtf8 = rel;                 // 例如 prefix="User"、key="User/Name" → name="Name"
                    one.valueType = GbConfigValueType::GbConfigValueType_String;
                    one.valueUtf8 = it.second;
                    outItem.values.emplace_back(move(one));
                }
                else
                {
                    // 子项首段
                    const string child = rel.substr(0, slashPos);
                    if (!child.empty())
                    {
                        childNames[child] = true;
                    }
                }
            }

            // 递归孩子
            for (const auto& kvChild : childNames)
            {
                GbConfigItem childItem;
                childItem.nameUtf8 = kvChild.first;
                if (recursive)
                {
                    const string childPrefix = head.empty() ? kvChild.first : (head + "/" + kvChild.first);
                    BuildTree(childPrefix, childItem);
                }
                outItem.childenItems.emplace_back(move(childItem));
            }
        };

    BuildTree(prefix, configItem);
    return true;
#endif
}

