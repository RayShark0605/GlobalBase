#ifndef GLOBALBASE_CRYPTO_H_H
#define GLOBALBASE_CRYPTO_H_H

#include "GlobalBasePort.h"
#include <string>
#include <cstddef>
#include <cstdint>

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

/**
 * @brief 计算 UTF-8 字符串的 CRC32（32-bit）校验值，并以 8 字节小写十六进制返回。
 *
 * @remarks
 * - 输入按字节序列参与计算，不做 Unicode 归一化。
 * - 本函数基于 zlib 的 crc32() 实现（CRC-32/ISO-HDLC 常用变体），并且 zlib 已在内部完成预处理/后处理。
 * - CRC32 不具备密码学安全性，仅建议用于快速校验/去重等非安全用途。
 *
 * @param utf8Text 输入 UTF-8 字符串（按字节序列处理）。
 * @return 8 字节小写十六进制字符串（例如 "cbf43926"）；若内部错误返回空串。
 */
GLOBALBASE_PORT std::string GB_Crc32Hash(const std::string& utf8Text);

enum class GB_Argon2Variant
{
    Argon2i = 0,
    Argon2d,
    Argon2id
};

namespace GB_Argon2
{
    /**
     * @brief Argon2 参数（与 RFC 9106 / OpenSSL EVP_KDF-ARGON2 对应）。
     *
     * @remarks
     * - memoryCostKiB 对应参数 m（以 KiB 为单位；OpenSSL 中为 memcost：1k blocks 数）。
     * - iterations    对应参数 t（迭代次数）。
     * - lanes         对应参数 p（并行度/lanes）。
     * - threads       为 OpenSSL 的线程提示参数（OSSL_KDF_PARAM_THREADS）。
     *   若 threads=0，则不向 OpenSSL 传递该参数（更稳妥，避免某些构建未启用内置线程池导致 derive 失败）。
     * - secret 与 associatedData 按“字节序列”传入（octet string），不做 UTF-8 合法性校验。
     */
    struct GB_Argon2Options
    {
        GB_Argon2Variant variant = GB_Argon2Variant::Argon2id;

        // RFC 9106 第二推荐（低内存）为：t=3, p=4, m=2^16(65536KiB=64MiB), salt=128-bit, tag=256-bit。
        uint32_t iterations = 3;
        uint32_t memoryCostKiB = 65536;
        uint32_t lanes = 4;

        // 0 表示不传递 threads 参数；否则会传递并尝试启用 OpenSSL 内置并行。
        uint32_t threads = 0;

        // Argon2 version: 0x10 或 0x13（默认 0x13，对应 v=19）。
        uint32_t version = 0x13;

        // 输出 tag 长度（字节）。RFC 9106 推荐至少 128-bit。
        size_t hashLength = 32;

        // 生成随机 salt 的长度（字节）。RFC 9106 推荐 128-bit。
        size_t saltLength = 16;

        // 可选 pepper（secret）与 associated data（ad）。
        std::string secret = "";
        std::string associatedData = "";
    };

    /**
     * @brief 对 UTF-8 字符串执行 Argon2 哈希，并返回标准编码字符串（argon2i/argon2d/argon2id）。
     *
     * @remarks
     * - 本函数基于 OpenSSL 3.2+ 提供的 EVP_KDF-ARGON2 实现；具体可用性与编译配置/Provider 有关。
     * - 输入按字节序列参与计算，不做 Unicode 归一化。
     * - salt 会在内部使用 OpenSSL RAND_bytes 生成随机字节（长度由 options.saltLength 指定）。
     * - 返回格式遵循常见 Argon2 编码：
     *   "$argon2id$v=19$m=65536,t=3,p=4$<salt_b64>$<hash_b64>"
     *   其中 Base64 使用标准字母表且省略 padding（与常见实现一致）。
     * - 若 OpenSSL 版本过低或算法不可用，将返回空串。
     *
     * @param utf8Text 输入 UTF-8 字符串（按字节序列处理）。
     * @param options Argon2 参数（默认使用 RFC 9106 第二推荐的低内存配置）。
     * @return std::string 失败返回空串；成功返回标准 Argon2 编码字符串。
     */
    GLOBALBASE_PORT std::string GB_Argon2Hash(const std::string& utf8Text, const GB_Argon2Options& options = GB_Argon2Options());

    /**
     * @brief 对 UTF-8 字符串执行 Argon2 哈希（使用指定 salt），并返回标准编码字符串。
     *
     * @remarks
     * - salt 按字节序列使用（octet string），不要求是可打印字符。
     * - 若希望与 GB_Argon2Hash 生成结果一致，可传入 saltLength=16 的随机 salt。
     *
     * @param utf8Text 输入 UTF-8 字符串（按字节序列处理）。
     * @param saltBytes salt 字节序列（通常建议 16 字节）。
     * @param options Argon2 参数。
     * @return std::string 失败返回空串；成功返回标准 Argon2 编码字符串。
     */
    GLOBALBASE_PORT std::string GB_Argon2HashWithSalt(const std::string& utf8Text, const std::string& saltBytes, const GB_Argon2Options& options = GB_Argon2Options());

    /**
     * @brief 校验 UTF-8 字符串是否匹配给定的 Argon2 编码哈希。
     *
     * @remarks
     * - 支持解析 "$argon2i/$argon2d/$argon2id" 变体与常见参数段（v/m/t/p）。
     * - Base64 解码使用标准字母表并允许省略 padding。
     * - 内部使用常数时间比较（CRYPTO_memcmp）避免时序泄露。
     *
     * @param utf8Text 输入 UTF-8 字符串（按字节序列处理）。
     * @param encodedHash Argon2 标准编码字符串。
     * @return true 匹配；false 不匹配或解析/计算失败。
     */
    GLOBALBASE_PORT bool GB_Argon2Verify(const std::string& utf8Text, const std::string& encodedHash);
} // namespace GB_Argon2

#endif
