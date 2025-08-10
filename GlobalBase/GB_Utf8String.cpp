#include "GB_Utf8String.h"
#if defined(_WIN32)
#include <windows.h>
#else
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

}

string MakeUtf8String(const char* s)
{
    if (!s)
    {
        return {};
    }
	return string(s);
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

    // 非 Windows：通常 wchar_t 为 4 字节（UTF-32）
    wstring_convert<codecvt_utf8<wchar_t>> conv;
    try
    {
        return conv.to_bytes(ws);
    }
    catch (const range_error&)
    {
        throw runtime_error("UTF-32 to UTF-8 conversion failed.");
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
        throw std::runtime_error("MultiByteToWideChar failed (convert).");
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
        throw runtime_error("UTF-8 to UTF-32 conversion failed.");
    }
#endif
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



