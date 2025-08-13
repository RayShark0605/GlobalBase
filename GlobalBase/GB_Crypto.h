#ifndef GLOBALBASE_CRYPTO_H_H
#define GLOBALBASE_CRYPTO_H_H

#include <string>
#include "GlobalBasePort.h"

/**
 * @brief 将任意字节序列按 RFC 4648 进行 Base64 编码。
 *
 * - 字母表：默认使用标准 Base64 字母表 "A–Z a–z 0–9 + /"；当 urlSafe=true 时改用 URL/文件名安全字母表 "A–Z a–z 0–9 - _"。
 * - 填充：默认按 RFC 规则在末尾补 '='；当 noPadding=true 时省略尾部 '='。
 * - 换行：不插入任何换行（MIME 的 76 列换行属于上层协议约束，本实现不启用）。
 *
 * @param bytes
 *     任意二进制数据（以 std::string 的字节视为原始八位字节序列，不做字符编码转换）。
 * @param urlSafe
 *     是否使用 URL/文件名安全字母表（默认 false）。为 true 时用 '-'、'_' 取代 '+'、'/'，其余规则相同。
 * @param noPadding
 *     是否省略尾部 '='（默认 false）。为 true 时不输出任何 '='；为 false 时按照剩余 8/16 位分别补 "==" / "="。
 * @return
 *     编码后的 Base64 文本；空输入返回空字符串。
 */
GLOBALBASE_PORT std::string GB_Base64Encode(const std::string& bytes, bool urlSafe = false, bool noPadding = false);

/**
 * @brief 按 RFC 4648 规则解码 Base64 文本为原始字节序列。
 *
 * - 兼容性（非严格模式）：当 strictMode=false 时，解码器会忽略 ASCII 空白（空格、\t、\r、\n 等），
 *   并在长度 %4==2/3 时自动补齐缺失的 '='；同时**兼容标准与 URL-safe 两套字母表**。
 * - 严格校验（严格模式）：当 strictMode=true 时，
 *   1) 仅接受由 urlSafe 指定的那套字母表；2) 拒绝任何空白字符；
 *   3) 校验末组“零填充位”（只剩 1 个输出字节时要求第二个 sextet 低 4 位为 0；只剩 2 个输出字节时要求第三个 sextet 低 2 位为 0）；
 *   4) 当 noPadding=false 时输入长度必须为 4 的倍数；当 noPadding=true 时禁止出现任何 '='。
 * - URL 安全：urlSafe=true 表示使用并（在严格模式下）只接受 '-'、'_' 变体；非严格模式下两套字母表皆可。
 * - 填充：若 noPadding=false 则允许出现并依规则处理 '='；若 noPadding=true，严格模式下禁止 '='，非严格模式下仍可根据长度自动补齐后再解码。
 *
 * @param base64Info
 *     Base64 文本；根据 strictMode 的不同可包含或不包含空白/填充字符。
 * @param strictMode
 *     严格模式开关（默认 false）。启用后执行字母表/空白/长度/零填充位等严格校验；关闭则更宽松、更具兼容性。
 * @param urlSafe
 *     是否采用 URL/文件名安全字母表（默认 false）。严格模式下仅接受所选字母表；非严格模式下两套字母表均可被识别。
 * @param noPadding
 *     是否采用“无填充”约定（默认 false）。严格模式下：noPadding=true 禁止 '='；noPadding=false 要求长度为 4 的倍数。
 *     非严格模式下：允许缺失 '=' 并自动补齐（长度 %4==2/3 时补 "=" / "=="; %4==1 视为非法）。
 * @return
 *     解码后的原始字节序列；若输入不合法则返回空字符串。
 */
GLOBALBASE_PORT std::string GB_Base64Decode(const std::string& base64Info, bool strictMode = false, bool urlSafe = false, bool noPadding = false);

// 计算字符串的 MD5 哈希值
GLOBALBASE_PORT std::string GB_GetMd5(const std::string& input);

// SHA-256 / SHA-512（一次性输入，返回小写十六进制）
GLOBALBASE_PORT std::string GB_GetSha256(const std::string& input);
GLOBALBASE_PORT std::string GB_GetSha512(const std::string& input);














#endif