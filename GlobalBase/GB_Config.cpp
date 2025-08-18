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

#else
    static string GetHomeDir()
    {
        const char* home = getenv("HOME");
        if (home && *home)
        {
            return MakeUtf8String(home);
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

#endif

}

string GetGbConfigPath()
{
#if defined(_WIN32)
    // 提示性的“注册表路径”描述字符串
    return GB_STR("计算机\\HKEY_CURRENT_USER\\Software\\GlobalBase");
#else
    return GetLinuxConfigFile();
#endif
}

bool IsExistsGbConfig(const string& keyUtf8)
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

bool GetGbConfig(const string& keyUtf8, string& valueUtf8)
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

bool SetGbConfig(const string& keyUtf8, const string& valueUtf8)
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

bool DeleteGbConfig(const string& keyUtf8)
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

unordered_map<string, string> GetAllGbConfig()
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


