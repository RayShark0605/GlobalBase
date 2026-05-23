#ifndef GLOBALBASE_UTF8_STRING_H_H
#define GLOBALBASE_UTF8_STRING_H_H

#include "GlobalBasePort.h"
#include <cstdint>
#include <cstdarg>
#include <limits>
#include <string>
#include <vector>

// 构造 UTF-8 字符串
GLOBALBASE_PORT std::string GB_MakeUtf8String(const char* s);
GLOBALBASE_PORT std::string GB_MakeUtf8String(char32_t utf8Char);

// C++20
#if defined(__cpp_char8_t)
inline std::string GB_MakeUtf8String(const char8_t* s)
{
    if (!s)
    {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(s));
}
#endif

#define GB_STR(x) GB_MakeUtf8String(u8##x)
#define GB_CHAR(ch) U##ch
#define GB_CHAR2STR(ch) GB_MakeUtf8String(ch)

// UTF-8 转 ANSI 编码字符串
// 注意（POSIX）：
// - 本文件中的 GB_AnsiToUtf8 / GB_Utf8ToAnsi / GB_IsAnsi 等函数在 POSIX 下会依赖当前 LC_CTYPE locale 的多字节编码。
// - 为了在默认 "C/POSIX" locale 下尽量可用，库内部可能会在第一次调用这些函数时尝试执行 setlocale(LC_CTYPE, "")，
//   以从环境变量（LANG/LC_ALL/LC_CTYPE 等）继承本地化设置。
// - setlocale 会修改整个进程的全局 locale，且不是线程安全的；若希望完全避免该副作用，可在编译时定义 GB_DISABLE_POSIX_SETLOCALE_AUTO_INIT。
GLOBALBASE_PORT std::string GB_Utf8ToAnsi(const std::string& utf8Str);

// ANSI 编码字符串转 UTF-8
GLOBALBASE_PORT std::string GB_AnsiToUtf8(const std::string& ansiStr);

// 将指定编码的原始字节流严格转换为 UTF-8。
// - rawBytes：原始字节流（可包含 \0）。
// - encodingName：源编码名称；比较时不区分大小写，并忽略常见分隔符（如 '-'、'_'、空格）。
//   例如 "utf8"、"utf-8"、"utf-8-sig"、"utf8signature"、"GBK"、"GB18030"、"GB2312"、"Big5"、"Shift-JIS"、"EUC-JP"、
//   "EUC-KR"、"ISO-8859-6"、"ISO-8859-1"、"windows-1252"、"cp1252"、"utf16le"、"utf-16be"、"utf32"、"ansi"、"locale"、"system"、"oem" 等。
// - 对于 UTF-16/UTF-32：
//   - "utf16" / "utf32" 需要 BOM；
//   - "utf16le" / "utf16be" / "utf32le" / "utf32be" 在输入开头存在 BOM 时，会优先以 BOM 为准并跳过该 BOM。
// - 对于 "utf-8-sig" / "utf8bom"：若输入以 UTF-8 BOM 开头，会自动移除 BOM。
// - 返回值：转换后的 UTF-8 字节串。
// - 失败时抛出 std::runtime_error（例如编码名不支持、输入字节不符合指定编码、UTF-16/UTF-32 数据不完整或代理项非法等）。
GLOBALBASE_PORT std::string GB_BytesToUtf8(const std::string& rawBytes, const std::string& encodingName);

// 是否是“合法的 UTF-8 字节序列”
// 注意：返回 true 仅表示按 UTF-8 解码不会出错（字节序列是 well-formed）。
// 它并不能证明这段数据的“真实编码”一定是 UTF-8（例如：纯 ASCII 同时也是合法 UTF-8 / 多种 ANSI 代码页内容）。
GLOBALBASE_PORT bool GB_IsUtf8(const std::string& text);

// 是否是 ANSI 编码字符串
// 说明：这里的“ANSI”并不是一种固定编码。
// - Windows：指系统默认 ANSI 代码页（CP_ACP / GetACP）
// - Linux/macOS：指当前 LC_CTYPE locale 的多字节编码
GLOBALBASE_PORT bool GB_IsAnsi(const std::string& text);

// 尽可能判断字符串是否“看起来像 UTF-8”文本（启发式，不保证一定正确）
// - 纯 ASCII 无法区分 UTF-8 与 ANSI，因此会返回 false
GLOBALBASE_PORT bool GB_LooksLikeUtf8(const std::string& text);

// 尽可能判断字符串是否“看起来像 ANSI”文本（启发式，不保证一定正确）
// - 纯 ASCII 无法区分 UTF-8 与 ANSI，因此会返回 false
GLOBALBASE_PORT bool GB_LooksLikeAnsi(const std::string& text);

// std::wstring 转 UTF-8
GLOBALBASE_PORT std::string GB_WStringToUtf8(const std::wstring& wstring);

// UTF-8 转 std::wstring
GLOBALBASE_PORT std::wstring GB_Utf8ToWString(const std::string& utf8Str);

// UTF-8 转 char32_t 序列
GLOBALBASE_PORT std::vector<char32_t> GB_Utf8StringToChar32Vector(const std::string& utf8Str);

// 获取 UTF-8 字符串的长度（以 UTF-8 字符为单位）
GLOBALBASE_PORT size_t GB_GetUtf8Length(const std::string& utf8Str);

// 获取 UTF-8 字符串中指定索引的字符（UTF-8 字符偏移）
GLOBALBASE_PORT char32_t GB_GetUtf8Char(const std::string& utf8Str, int64_t index);

// 获取 UTF-8 字符串的子串（以 UTF-8 字符为单位）
GLOBALBASE_PORT std::string GB_Utf8Substr(const std::string& utf8Str, int64_t start, int64_t length = std::numeric_limits<int64_t>::max());

// 将 UTF-8 字符串转换为小写（仅 ASCII）
GLOBALBASE_PORT std::string GB_Utf8ToLower(const std::string& utf8Str);

// 将 UTF-8 字符串转换为大写（仅 ASCII）
GLOBALBASE_PORT std::string GB_Utf8ToUpper(const std::string& utf8Str);

// 按“单个 Unicode 码点”分割
GLOBALBASE_PORT std::vector<std::string> GB_Utf8Split(const std::string& textUtf8, char32_t delimiter, bool removeEmptySections = true);

// 比较两个 UTF-8 字符串是否相等。
// - 当 caseSensitive=true：按字节精确比较。
// - 当 caseSensitive=false：仅对 ASCII 字母做大小写不敏感比较（A-Z 与 a-z 视为相同），
//   其他非 ASCII 字节仍按原值精确比较（不做 Unicode 大小写折叠/规范化）。
// 说明：本函数不校验 UTF-8 合法性，也不做任何编码转换；复杂度 O(N)，不分配额外内存。
GLOBALBASE_PORT bool GB_Utf8Equals(const std::string& text1Utf8, const std::string& text2Utf8, bool caseSensitive = true);

// 按“自然排序 / logical compare”比较两个 UTF-8 字符串。
// - Windows：
//   - 对纯 ASCII 输入，直接走库内快速路径；
//   - 对合法 UTF-8 的非 ASCII 输入，调用 StrCmpLogicalW，尽量贴近 Windows 资源管理器的文件名排序；
//   - 若输入不是合法 UTF-8，或任一输入包含嵌入式 '\0'，则退化为库内兼容实现。
//   - 注意：StrCmpLogicalW 的排序结果带有平台/版本依赖性，不适合作为需要长期稳定性的“规范排序”规则。
// - 非 Windows：始终使用库内兼容实现（数字按数值比较、ASCII 大小写不敏感）。
// 返回值：< 0 表示 text1Utf8 < text2Utf8；0 表示等价；> 0 表示 text1Utf8 > text2Utf8。
GLOBALBASE_PORT int GB_Utf8CompareLogical(const std::string& text1Utf8, const std::string& text2Utf8);

// 检查 UTF-8 字符串是否以指定的 UTF-8 字符串开头（可选大小写敏感）
GLOBALBASE_PORT bool GB_Utf8StartsWith(const std::string& textUtf8, const std::string& targetUtf8, bool caseSensitive = true);

// 检查 UTF-8 字符串是否以指定的 UTF-8 字符串结尾（可选大小写敏感）
GLOBALBASE_PORT bool GB_Utf8EndsWith(const std::string& textUtf8, const std::string& targetUtf8, bool caseSensitive = true);

// 查找子串：返回第一个匹配的起始位置（UTF-8 字符偏移），未找到返回 -1
// - startPos：查找起始位置（UTF-8 字符偏移，按码点计，不是字节偏移）
GLOBALBASE_PORT int64_t GB_Utf8Find(const std::string& text, const std::string& needle, bool caseSensitive = true, int64_t startPos = 0);

// 查找子串：返回最后一个匹配的起始位置（UTF-8 字符偏移），未找到返回 -1
GLOBALBASE_PORT int64_t GB_Utf8FindLast(const std::string& text, const std::string& needle, bool caseSensitive = true);

// 删除 UTF-8 字符串两端的指定字符（默认空白字符、Tab、\r和\n）
GLOBALBASE_PORT std::string GB_Utf8Trim(const std::string& utf8Str, const std::string& trimChars = " \t\r\n");
GLOBALBASE_PORT std::string GB_Utf8TrimLeft(const std::string& utf8Str, const std::string& trimChars = " \t\r\n");
GLOBALBASE_PORT std::string GB_Utf8TrimRight(const std::string& utf8Str, const std::string& trimChars = " \t\r\n");

// 规范化 UTF-8 字符串中的空白字符
// - 移除两端空白；
// - 将内部连续空白折叠为单个 ASCII 空格（' '）。
GLOBALBASE_PORT std::string GB_Utf8Simplified(const std::string& utf8Str);

// 替换 UTF-8 字符串中的子串（可选大小写敏感）
GLOBALBASE_PORT std::string GB_Utf8Replace(const std::string& utf8Str, const std::string& oldValue, const std::string& newValue, bool caseSensitive = true);

// printf 风格格式化生成 UTF-8 字符串
// 注意：
// - format 必须是 UTF-8 字节序列；
// - %s 对应参数也假定为 UTF-8 字节序列；
// - 本函数不做编码转换，只按 C 运行库规则格式化并拼接字节。
// 发生格式化错误时会抛出 std::runtime_error。
GLOBALBASE_PORT std::string GB_Utf8VFormat(const char* format, va_list args);

// printf 风格格式化生成 UTF-8 字符串（可变参数版本）。
// 注意：
// - format 必须是 UTF-8 字节序列；
// - %s 对应参数也假定为 UTF-8 字节序列；
// - 本函数不做编码转换，只按 C 运行库规则格式化并拼接字节。
// 发生格式化错误时会抛出 std::runtime_error。
GLOBALBASE_PORT std::string GB_Utf8Format(const char* format, ...);

// 获取当前控制台编码
GLOBALBASE_PORT void GB_GetConsoleEncodingString(std::string& encodingString);
GLOBALBASE_PORT void GB_GetConsoleEncodingCode(unsigned int& codePageId); // 未知编码则返回UINT_MAX

// 设置控制台编码
GLOBALBASE_PORT bool GB_SetConsoleEncoding(unsigned int codePageId);

// 设置控制台编码为 UTF-8
GLOBALBASE_PORT bool GB_SetConsoleEncodingToUtf8();

#endif