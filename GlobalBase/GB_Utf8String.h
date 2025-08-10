#ifndef GLOBALBASE_UTF8_STRING_H_H
#define GLOBALBASE_UTF8_STRING_H_H

#include "GB_Utility.h"

// 构造 UTF-8 字符串
GLOBALBASE_PORT std::string MakeUtf8String(const char* s);
GLOBALBASE_PORT std::string MakeUtf8String(char32_t utf8Char);

// C++20
#if defined(__cpp_char8_t)
inline std::string MakeUtf8String(const char8_t* s)
{
    if (!s)
    {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(s));
}
#endif

#define GB_STR(x) MakeUtf8String(u8##x)
#define GB_CHAR(ch) U##ch
#define GB_CHAR2STR(ch) MakeUtf8String(ch)

// UTF-8 转 ANSI 编码字符串
GLOBALBASE_PORT std::string Utf8ToAnsi(const std::string& utf8Str);

// ANSI 编码字符串转 UTF-8
GLOBALBASE_PORT std::string AnsiToUtf8(const std::string& ansiStr);

// 是否是 UTF-8 编码字符串
GLOBALBASE_PORT bool IsUtf8(const std::string& text);

// std::wstring 转 UTF-8
GLOBALBASE_PORT std::string WStringToUtf8(const std::wstring& wstring);

// UTF-8 转 std::wstring
GLOBALBASE_PORT std::wstring Utf8ToWString(const std::string& utf8Str);

// 获取 UTF-8 字符串的长度（以 UTF-8 字符为单位）
GLOBALBASE_PORT size_t GetUtf8Length(const std::string& utf8Str);

// 获取 UTF-8 字符串中指定索引的字符（UTF-8 字符偏移）
GLOBALBASE_PORT char32_t GetUtf8Char(const std::string& utf8Str, int64_t index);

// 获取 UTF-8 字符串的子串（以 UTF-8 字符为单位）
GLOBALBASE_PORT std::string Utf8Substr(const std::string& utf8Str, int64_t start, int64_t length = std::numeric_limits<int64_t>::max());

// 将 UTF-8 字符串转换为小写（仅 ASCII）
GLOBALBASE_PORT std::string Utf8ToLower(const std::string& utf8Str);

// 将 UTF-8 字符串转换为大写（仅 ASCII）
GLOBALBASE_PORT std::string Utf8ToUpper(const std::string& utf8Str);

// 按“单个 Unicode 码点”分割
GLOBALBASE_PORT std::vector<std::string> Utf8Split(const std::string& textUtf8, char32_t delimiter);

// 查找子串：返回第一个匹配的起始位置（UTF-8 字符偏移），未找到返回 -1
GLOBALBASE_PORT int64_t Utf8Find(const std::string& text, const std::string& needle, bool caseSensitive = true);



#endif