#ifndef GLOBALBASE_UTF8_STRING_H_H
#define GLOBALBASE_UTF8_STRING_H_H

#include "GB_Utility.h"

// 构造 UTF-8 字符串
GLOBALBASE_PORT std::string MakeUtf8String(const char* s);

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

// std::wstring 转 UTF-8
GLOBALBASE_PORT std::string WStringToUtf8(const std::wstring& ws);

// UTF-8 转 std::wstring
GLOBALBASE_PORT std::wstring Utf8ToWString(const std::string& utf8Str);

// 按“单个 Unicode 码点”分割
GLOBALBASE_PORT std::vector<std::string> Utf8Split(const std::string& textUtf8, char32_t delimiter);





#endif