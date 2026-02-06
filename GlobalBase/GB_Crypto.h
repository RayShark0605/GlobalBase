#ifndef GLOBALBASE_CRYPTO_H_H
#define GLOBALBASE_CRYPTO_H_H

#include "GlobalBasePort.h"
#include <string>

/**
 * @brief 将 UTF-8 字符串进行 Base64 编码。
 *
 * @remarks
 * - 当 urlSafe=false 时使用标准 Base64 字母表：A-Z a-z 0-9 + /
 * - 当 urlSafe=true  时使用 URL/文件名安全字母表（base64url）：A-Z a-z 0-9 - _
 * - 当 noPadding=true 时会省略尾部 '='（padding）。
 *
 * 以上变体遵循 RFC 4648 的约定。
 *
 * @param utf8Text 输入 UTF-8 字符串（按字节序列处理）。
 * @param urlSafe 是否使用 URL/文件名安全字母表（base64url）。
 * @param noPadding 是否省略尾部 '='。
 * @return Base64 编码后的字符串。
 */
GLOBALBASE_PORT std::string GB_Base64Encode(const std::string& utf8Text, bool urlSafe = false, bool noPadding = false);

/**
 * @brief 将 Base64 字符串解码为 UTF-8 字符串。
 *
 * @remarks
 * - 输入允许包含空白字符（空格/\t/\r/\n），会被忽略。
 * - 当 noPadding=true 时允许输入缺失尾部 '='（会按 RFC 4648 规则自动补齐）。
 * - 当输入包含 padding 时，会进行 canonical 校验（末尾未使用 bit 必须为 0），避免出现多种 Base64 文本映射到同一字节序列的情况。
 * - 本函数按字节序列解码，不会强制校验结果是否为合法 UTF-8（如需校验可调用 GB_IsUtf8）。
 *
 * @param base64Text Base64 字符串。
 * @param outUtf8Text 输出解码后的 UTF-8 字符串。
 * @param urlSafe 是否使用 URL/文件名安全字母表（base64url）。
 * @param noPadding 是否允许输入省略尾部 '='。
 * @return 成功返回 true；输入非法或解码失败返回 false。
 */
GLOBALBASE_PORT bool GB_Base64Decode(const std::string& base64Text, std::string& outUtf8Text, bool urlSafe = false, bool noPadding = false);

/**
 * @brief 计算 UTF-8 字符串的 MD5（128-bit）哈希，并以 32 字节小写十六进制返回。
 *
 * @remarks
 * - 输入按字节序列参与哈希，不做 Unicode 归一化。
 * - OpenSSL 3.x 下优先使用 EVP_MD_fetch("MD5") 以适配 provider；OpenSSL 1.1.x 及更早使用 EVP_md5()。
 * - MD5 已被证明不具备抗碰撞能力，不适合作为安全签名或密码学用途；仅建议用于非安全场景的校验/去重等。
 *
 * @param utf8Text 输入 UTF-8 字符串（按字节序列处理）。
 * @return 32 字节小写十六进制字符串；若内部错误返回空串（MD5 结果不会为空）。
 */
GLOBALBASE_PORT std::string GB_Md5Hash(const std::string& utf8Text);

enum class GB_ShaMethod
{
    Sha256 = 0,
    Sha512,
    Sha3_256,
    Sha3_512
};

/**
 * @brief 计算 UTF-8 字符串的 SHA 哈希，并以小写十六进制返回。
 *
 * @remarks
 * - 输入按字节序列参与哈希，不做 Unicode 归一化。
 * - 输出为小写十六进制：256-bit 算法返回 64 字节；512-bit 算法返回 128 字节。
 * - SHA3 系列要求 OpenSSL 1.1.1+ 或 OpenSSL 3.x；若当前 OpenSSL 未提供该算法（例如 provider 未启用），将返回空串。
 *
 * @param utf8Text 输入 UTF-8 字符串（按字节序列处理）。
 * @param method SHA 方法，默认 SHA-256。
 * @return 小写十六进制字符串；若内部错误或算法不可用返回空串。
 */
GLOBALBASE_PORT std::string GB_ShaHash(const std::string& utf8Text, GB_ShaMethod method = GB_ShaMethod::Sha256);
















#endif