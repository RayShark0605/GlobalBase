#include "GB_Utf8String.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <clocale>
#include <cwchar>
#include <cerrno>
#include <stdexcept>
#include <locale>
#include <codecvt>
#include <climits>
#endif

using namespace std;

namespace internal
{
    // 从 s[pos] 解码一个 UTF-8 码点：
    // 成功：返回 true，写出 codePoint 与 nextPos（下一个字节位置）
    // 失败：返回 false，仅前进一个字节（nextPos = pos + 1），调用方可按“原始字节”处理
    static bool DecodeOne(const string& s, size_t pos, char32_t& codePoint, size_t& nextPos)
    {
        const size_t n = s.size();
        if (pos >= n)
        {
            return false;
        }

        unsigned char b0 = static_cast<unsigned char>(s[pos]);
        if (b0 < 0x80)
        {
            codePoint = b0;
            nextPos = pos + 1;
            return true;
        }

        int len = 0;
        char32_t cp = 0;

        if ((b0 & 0xE0) == 0xC0) { len = 2; cp = (b0 & 0x1F); }
        else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = (b0 & 0x0F); }
        else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = (b0 & 0x07); }
        else
        {
            nextPos = pos + 1; // 非法起始字节
            return false;
        }

        if (pos + len > n)
        {
            nextPos = pos + 1; // 截断
            return false;
        }

        for (int i = 1; i < len; ++i)
        {
            unsigned char bx = static_cast<unsigned char>(s[pos + i]);
            if ((bx & 0xC0) != 0x80)
            {
                nextPos = pos + 1; // 非 10xxxxxx
                return false;
            }
            cp = (cp << 6) | (bx & 0x3F);
        }

        // RFC 3629：最短编码、合法范围、排除代理项
        if ((len == 2 && cp < 0x80) ||
            (len == 3 && cp < 0x800) ||
            (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
        {
            nextPos = pos + 1;
            return false;
        }

        codePoint = cp;
        nextPos = pos + len;
        return true;
    }

    static bool DecodeSingleChar(const string& s, char32_t& codePoint)
    {
        size_t nextPos = 0;
        if (!DecodeOne(s, 0, codePoint, nextPos))
        {
            return false;
        }
        return nextPos == s.size(); // 必须恰好一个码点
    }

    static char32_t ToLowerAscii(char32_t cp)
    {
        // 仅 ASCII 大小写折叠
        if (cp >= U'A' && cp <= U'Z')
        {
            return cp + (U'a' - U'A');
        }
        return cp;
    }

    // 统一的“读一个码点”：如果 internal::DecodeOne 失败，就把该字节当作 U+FFFD 消费 1 字节
    static void DecodeOneOrReplacement(const string& s, size_t pos, char32_t& cp, size_t& nextPos)
    {
        if (!internal::DecodeOne(s, pos, cp, nextPos))
        {
            cp = 0xFFFDu;
            nextPos = pos + 1; // 失败时按 1 字节前进，保持可数性
        }
    }

    // 仅 ASCII 的大小写转换，避免受本地化影响
    static char ToLowerAsciiChar(char ch)
    {
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
    static char ToUpperAsciiChar(char ch)
    {
        return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
    }

    static bool IsValidUnicode(uint32_t cp)
    {
        // Unicode 标准平面范围：U+0000 ~ U+10FFFF，排除代理区
        return cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
    }

    // 说明：这里的“ANSI”指当前 LC_CTYPE locale 的多字节编码（如 zh_CN.GB18030）。
    // 若当前是 "C"/"POSIX"（7-bit ASCII），请先 setlocale 到合适的本地编码。
    static void EnsureLocaleInitialized()
    {
        const char* cur = std::setlocale(LC_CTYPE, nullptr);
        if (!cur || std::string(cur) == "C" || std::string(cur) == "POSIX")
        {
            std::setlocale(LC_CTYPE, ""); // 从环境继承
        }
    }
#ifndef _WIN32
    static std::wstring Utf8ToWString_Posix(const std::string& utf8Str)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> c8; // C++11 可用
        return c8.from_bytes(utf8Str);
    }

    static std::string WStringToUtf8_Posix(const std::wstring& ws)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> c8;
        return c8.to_bytes(ws);
    }
#endif // !_WIN32
}

string MakeUtf8String(const char* s)
{
    if (!s)
    {
        return {};
    }
	return string(s);
}

string MakeUtf8String(char32_t utf8Char)
{
    uint32_t u = static_cast<uint32_t>(utf8Char);
    string out;

    if (!internal::IsValidUnicode(u))
    {
        // 用 U+FFFD 作为替代
        u = 0xFFFD;
    }

    if (u <= 0x7F)
    {
        out.push_back(static_cast<char>(u));
    }
    else if (u <= 0x7FF)
    {
        out.push_back(static_cast<char>(0xC0 | (u >> 6)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    }
    else if (u <= 0xFFFF)
    {
        out.push_back(static_cast<char>(0xE0 | (u >> 12)));
        out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    }
    else // u <= 0x10FFFF
    {
        out.push_back(static_cast<char>(0xF0 | (u >> 18)));
        out.push_back(static_cast<char>(0x80 | ((u >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    }

    return out;
}

string Utf8ToAnsi(const string& utf8Str)
{
    if (utf8Str.empty())
    {
        return {};
    }
#if defined(_WIN32)
    // UTF-8 -> UTF-16
    const int wlen = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,            // 无效序列直接失败
        utf8Str.data(),
        static_cast<int>(utf8Str.size()),
        nullptr,
        0
    );
    if (wlen <= 0)
    {
        throw runtime_error("MultiByteToWideChar(CP_UTF8) failed (size).");
    }
    wstring ws(static_cast<size_t>(wlen), L'\0');
    const int wwritten = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8Str.data(),
        static_cast<int>(utf8Str.size()),
        &ws[0],
        wlen
    );
    if (wwritten <= 0)
    {
        throw runtime_error("MultiByteToWideChar(CP_UTF8) failed (convert).");
    }

    // UTF-16 -> ANSI(ACP)
    // 说明：CP_ACP 为系统 ANSI 代码页；不同机器可能不同，且会被用户修改。
    const int alen = ::WideCharToMultiByte(
        CP_ACP,
        WC_NO_BEST_FIT_CHARS,            // 避免近似匹配（可选）
        ws.data(),
        static_cast<int>(ws.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (alen <= 0)
    {
        throw runtime_error("WideCharToMultiByte(CP_ACP) failed (size).");
    }
    string ansi(static_cast<size_t>(alen), '\0');
    BOOL usedDefaultChar = FALSE;
    const int awritten = ::WideCharToMultiByte(
        CP_ACP,
        WC_NO_BEST_FIT_CHARS,
        ws.data(),
        static_cast<int>(ws.size()),
        &ansi[0],
        alen,
        nullptr,                          // 默认 '?'
        &usedDefaultChar                  // 如有无法表示的字符会置 TRUE
    );
    if (awritten <= 0)
    {
        throw runtime_error("WideCharToMultiByte(CP_ACP) failed (convert).");
    }
    // 这里不把 usedDefaultChar 当作错误抛出；如需“严格模式”可改为检测后抛异常。
    return ansi;

#else
    if (utf8Str.empty())
    {
        return {};
    }

    EnsureLocaleInitialized();

    // UTF-8 -> wstring
    std::wstring ws;
    try
    {
        ws = Utf8ToWString_Posix(utf8Str);
    }
    catch (const std::range_error&)
    {
        throw std::runtime_error("UTF-8 decoding failed.");
    }

    // wstring -> 本地多字节（依赖 LC_CTYPE）
    const wchar_t* src = ws.c_str();
    std::mbstate_t st = std::mbstate_t{};
    // 1) 预计算所需字节数（不含终止 '\0'）
    errno = 0;
    std::size_t need = std::wcsrtombs(nullptr, &src, 0, &st);
    if (need == static_cast<std::size_t>(-1))
    {
        throw std::runtime_error("Local multibyte encoding failed (wstring -> bytes).");
    }

    std::string out(need, '\0');
    src = ws.c_str();
    st = std::mbstate_t{};
    errno = 0;
    std::size_t written = std::wcsrtombs(&out[0], &src, need, &st);
    if (written == static_cast<std::size_t>(-1))
    {
        throw std::runtime_error("Local multibyte encoding failed (wstring -> bytes).");
    }
    // wcsrtombs 返回的字节数不含 '\0'，长度正好是 written/need
    // out 已经是正确大小
    return out;
#endif
}

string AnsiToUtf8(const string& ansiStr)
{
    if (ansiStr.empty())
    {
        return {};
    }
#if defined(_WIN32)
    // ANSI(ACP) -> UTF-16
    const int wlen = ::MultiByteToWideChar(
        CP_ACP,
        0,                                 // 不加 MB_ERR_INVALID_CHARS，防止某些旧代码页数据直接失败
        ansiStr.data(),
        static_cast<int>(ansiStr.size()),
        nullptr,
        0
    );
    if (wlen <= 0)
    {
        throw runtime_error("MultiByteToWideChar(CP_ACP) failed (size).");
    }
    wstring ws(static_cast<size_t>(wlen), L'\0');
    const int wwritten = ::MultiByteToWideChar(
        CP_ACP,
        0,
        ansiStr.data(),
        static_cast<int>(ansiStr.size()),
        &ws[0],
        wlen
    );
    if (wwritten <= 0)
    {
        throw runtime_error("MultiByteToWideChar(CP_ACP) failed (convert).");
    }

    // UTF-16 -> UTF-8
    const int u8len = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        ws.data(),
        static_cast<int>(ws.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (u8len <= 0)
    {
        throw runtime_error("WideCharToMultiByte(CP_UTF8) failed (size).");
    }
    string utf8(static_cast<size_t>(u8len), '\0');
    const int u8written = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        ws.data(),
        static_cast<int>(ws.size()),
        &utf8[0],
        u8len,
        nullptr,
        nullptr
    );
    if (u8written <= 0)
    {
        throw runtime_error("WideCharToMultiByte(CP_UTF8) failed (convert).");
    }
    return utf8;

#else
    if (ansiStr.empty())
    {
        return {};
    }

    EnsureLocaleInitialized();

    // 本地多字节 -> wstring（依赖 LC_CTYPE）
    const char* src = ansiStr.c_str();
    std::mbstate_t st = std::mbstate_t{};
    errno = 0;
    // 1) 计算需要的 wchar_t 数量（不含终止 L'\0'）
    std::size_t wlen = std::mbsrtowcs(nullptr, &src, 0, &st);
    if (wlen == static_cast<std::size_t>(-1))
    {
        throw std::runtime_error("Local multibyte decoding failed (bytes -> wstring).");
    }

    std::wstring ws(wlen, L'\0');
    src = ansiStr.c_str();
    st = std::mbstate_t{};
    errno = 0;
    std::size_t wwritten = std::mbsrtowcs(&ws[0], &src, wlen, &st);
    if (wwritten == static_cast<std::size_t>(-1))
    {
        throw std::runtime_error("Local multibyte decoding failed (bytes -> wstring).");
    }

    // wstring -> UTF-8
    try
    {
        return WStringToUtf8_Posix(ws);
    }
    catch (const std::range_error&)
    {
        throw std::runtime_error("UTF-8 encoding failed.");
    }
#endif
}

bool IsUtf8(const string& text)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        // 严格：一旦解码失败立即判 false（DecodeOne 已按 RFC 3629 检查最短编码/代理项等）
        if (!internal::DecodeOne(text, pos, cp, nextPos))
        {
            return false;
        }
        pos = nextPos;
    }
    return true;
}

string WStringToUtf8(const wstring& ws)
{
#if defined(_WIN32)
    if (ws.empty())
    {
        return {};
    }

    // 1) 计算所需字节数（不含 '\0'）
    const int sizeRequired = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS, // 非法代理项直接报错
        ws.data(),
        static_cast<int>(ws.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (sizeRequired <= 0)
    {
        throw runtime_error("WideCharToMultiByte failed (size).");
    }

    // 2) 实际转换
    string result(static_cast<size_t>(sizeRequired), '\0');
    int written = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        ws.data(),
        static_cast<int>(ws.size()),
        &result[0],
        sizeRequired,
        nullptr,
        nullptr
    );
    if (written <= 0)
    {
        throw runtime_error("WideCharToMultiByte failed (convert).");
    }
    return result;
#else
    if (ws.empty())
    {
        return {};
    }

    wstring_convert<codecvt_utf8<wchar_t>> cvt;
    try
    {
        return cvt.to_bytes(ws);
    }
    catch (const range_error&)
    {
        throw runtime_error("WStringToUtf8 conversion failed.");
    }
#endif
}

wstring Utf8ToWString(const string& utf8Str)
{
#if defined(_WIN32)
    if (utf8Str.empty())
    {
        return {};
    }

    // 1) 计算需要的 wchar_t 数量（不含 '\0'）
    const int sizeRequired = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,          // 非法 UTF-8 序列直接报错
        utf8Str.data(),
        static_cast<int>(utf8Str.size()),
        nullptr,
        0
    );
    if (sizeRequired <= 0)
    {
        throw runtime_error("MultiByteToWideChar failed (size).");
    }

    // 2) 实际转换
    wstring result(static_cast<size_t>(sizeRequired), L'\0');
    int written = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8Str.data(),
        static_cast<int>(utf8Str.size()),
        &result[0],
        sizeRequired
    );
    if (written <= 0)
    {
        throw runtime_error("MultiByteToWideChar failed (convert).");
    }
    return result;
#else
    if (utf8Str.empty())
    {
        return {};
    }

    wstring_convert<codecvt_utf8<wchar_t>> conv;
    try
    {
        return conv.from_bytes(utf8Str);
    }
    catch (const range_error&)
    {
        throw runtime_error("Utf8ToWString conversion failed.");
    }
#endif
}

// 获取 UTF-8 字符串的长度（以 UTF-8 字符/码点 为单位）
size_t GetUtf8Length(const string& utf8Str)
{
    size_t len = 0;
    size_t pos = 0;
    while (pos < utf8Str.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        internal::DecodeOne(utf8Str, pos, cp, nextPos); // 成功或失败都前进
        pos = nextPos;
        len++; // 非法字节按 1 个“字符”统计
    }
    return len;
}

char32_t GetUtf8Char(const string& utf8Str, int64_t index)
{
    // 不抛异常，不用可选类型：失败返回一个不可能出现的值 0x110000（> U+10FFFF）
    static constexpr char32_t kInvalidCodePoint = 0x110000;

    if (index < 0)
    {
        return kInvalidCodePoint;
    }

    size_t pos = 0;
    int64_t curIndex = 0;

    while (pos < utf8Str.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        bool ok = internal::DecodeOne(utf8Str, pos, cp, nextPos); // 失败也会 nextPos = pos + 1

        if (curIndex == index)
        {
            if (!ok)
            {
                return false;              // 该“字符”本身就是非法起始字节
            }
            return cp;
        }

        pos = nextPos;
        curIndex++;                        // 非法字节按“一个字符”计数，与你前面 API 约定一致
    }
    return kInvalidCodePoint;
}

string Utf8Substr(const string& utf8Str, int64_t start, int64_t length)
{
    if (start < 0 || length < 0)
    {
        return {}; // 不支持负索引；负长度视为空
    }

    // 快速返回：空串
    if (utf8Str.empty() || length == 0)
    {
        return {};
    }

    size_t pos = 0;
    int64_t charIndex = 0;

    // 1) 找到起始码点对应的字节偏移
    size_t startByte = string::npos;
    while (pos < utf8Str.size() && charIndex < start)
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        internal::DecodeOne(utf8Str, pos, cp, nextPos);
        pos = nextPos;
        charIndex++;
    }
    if (charIndex < start)
    {
        return {}; // 起始 >= 总长度
    }
    startByte = pos;

    // 2) 继续前进 length 个码点，得到结束字节偏移
    int64_t remain = length;
    while (pos < utf8Str.size() && remain > 0)
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        internal::DecodeOne(utf8Str, pos, cp, nextPos);
        pos = nextPos;
        remain--;
    }
    const size_t endByte = pos; // 若提前结束，endByte==size()
    return utf8Str.substr(startByte, endByte - startByte);
}

string Utf8ToLower(const string& utf8Str)
{
    string out;
    out.reserve(utf8Str.size()); // 最终长度不会超过原串

    size_t pos = 0;
    while (pos < utf8Str.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        bool ok = internal::DecodeOne(utf8Str, pos, cp, nextPos);

        // 对于 ASCII（单字节、且 < 0x80），做大小写转换
        if (ok && nextPos == pos + 1)
        {
            unsigned char b0 = static_cast<unsigned char>(utf8Str[pos]);
            if (b0 < 0x80)
            {
                out.push_back(internal::ToLowerAsciiChar(static_cast<char>(b0)));
                pos = nextPos;
                continue;
            }
        }

        // 非 ASCII 或解码失败：原样拷贝这段字节
        out.append(utf8Str.data() + pos, utf8Str.data() + nextPos);
        pos = nextPos;
    }
    return out;
}

string Utf8ToUpper(const string& utf8Str)
{
    string out;
    out.reserve(utf8Str.size());

    size_t pos = 0;
    while (pos < utf8Str.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        bool ok = internal::DecodeOne(utf8Str, pos, cp, nextPos);

        if (ok && nextPos == pos + 1)
        {
            unsigned char b0 = static_cast<unsigned char>(utf8Str[pos]);
            if (b0 < 0x80)
            {
                out.push_back(internal::ToUpperAsciiChar(static_cast<char>(b0)));
                pos = nextPos;
                continue;
            }
        }

        out.append(utf8Str.data() + pos, utf8Str.data() + nextPos);
        pos = nextPos;
    }
    return out;
}

vector<string> Utf8Split(const string& textUtf8, char32_t delimiter)
{
	vector<string> parts;

	size_t tokenStart = 0;
	size_t pos = 0;
	while (pos < textUtf8.size())
	{
		char32_t cp = 0;
		size_t nextPos = pos;
		bool ok = internal::DecodeOne(textUtf8, pos, cp, nextPos);
        if (!ok)
        {
            // 非法字节：按原样跳过一个字节（注意：不能把 delimiter 与字节直接比较）
            pos++;
            continue;
        }

        if (cp == delimiter)
        {
            parts.emplace_back(textUtf8.substr(tokenStart, pos - tokenStart));
            tokenStart = nextPos;
        }
        pos = nextPos;
	}

    parts.emplace_back(textUtf8.substr(tokenStart));
    return parts;
}

int64_t Utf8Find(const string& text, const string& needle, bool caseSensitive)
{
    // 1) 预解码模式串到码点数组（并可选 ASCII 折叠）
    vector<char32_t> pat;
    {
        size_t pos = 0;
        while (pos < needle.size())
        {
            char32_t cp = 0;
            size_t nextPos = pos;
            internal::DecodeOneOrReplacement(needle, pos, cp, nextPos);
            if (!caseSensitive)
            {
                cp = internal::ToLowerAscii(cp);
            }
            pat.push_back(cp);
            pos = nextPos;
        }
    }

    const size_t m = pat.size();
    if (m == 0)
    {
        return 0; // 与 string::find("") 一致
    }

    // 2) 计算 KMP 的前缀函数（LPS）
    vector<size_t> lps(m, 0);
    {
        size_t len = 0;
        for (size_t i = 1; i < m; )
        {
            if (pat[i] == pat[len])
            {
                lps[i++] = ++len;
            }
            else if (len != 0)
            {
                len = lps[len - 1];
            }
            else
            {
                lps[i++] = 0;
            }
        }
    }

    // 3) 流式解码 text 并进行 KMP 匹配（无需整串展开为码点向量）
    size_t j = 0;                // 已匹配 pat[0..j-1]
    size_t textBytePos = 0;      // 字节位置
    int64_t textCharIndex = 0;   // 已读码点数量（也就是当前码点索引）

    while (textBytePos < text.size())
    {
        char32_t cp = 0;
        size_t nextPos = textBytePos;
        internal::DecodeOneOrReplacement(text, textBytePos, cp, nextPos);
        if (!caseSensitive)
        {
            cp = internal::ToLowerAscii(cp);
        }

        while (j > 0 && cp != pat[j])
        {
            j = lps[j - 1];
        }
        if (cp == pat[j])
        {
            j++;
            if (j == m)
            {
                // 命中：起始“码点偏移” = 当前码点索引 - m + 1
                return textCharIndex - static_cast<int64_t>(m) + 1;
            }
        }

        textBytePos = nextPos;
        textCharIndex++;
    }

    return -1;
}



