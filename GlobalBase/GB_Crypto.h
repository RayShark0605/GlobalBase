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
     *   本库不会调用 OSSL_set_max_threads 修改全局线程池配置；若线程池未启用或上限不足，将自动降级为单线程。
     *   若 threads=0，则不向 OpenSSL 传递该参数。
     * - secret 与 associatedData 按“字节序列”传入（octet string），不做 UTF-8 合法性校验。
     */
    struct GB_Argon2Options
    {
        GB_Argon2Variant variant = GB_Argon2Variant::Argon2id;

        // RFC 9106 第二推荐（低内存）为：t=3, p=4, m=2^16(65536KiB=64MiB), salt=128-bit, tag=256-bit。
        uint32_t iterations = 3;
        uint32_t memoryCostKiB = 65536;
        uint32_t lanes = 4;

        // 0 表示不传递 threads 参数。
        // >0 表示向 OpenSSL 传递 OSSL_KDF_PARAM_THREADS 作为“并行度提示”。
        // 注意：OpenSSL 的线程池上限是 libctx 全局配置项（OSSL_set_max_threads）。本库不会修改该全局状态；
        // 若线程池未启用或上限不足，则会在内部将 threads 降级为 1。
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

namespace GB_AES
{
    enum class GB_AesMode
    {
        Ecb = 0,
        Cbc,
        Cfb128,
        Ofb,
        Ctr,
        Gcm
    };

    /**
     * @brief AES 加解密参数。
     *
     * @remarks
     * - utf8PlainText / outUtf8PlainText 均按“字节序列”处理，不做 Unicode 归一化与 UTF-8 合法性校验。
     * - ECB/CBC 可选 PKCS#7 padding（OpenSSL EVP 默认即为该 padding）；若关闭 padding，则输入长度必须为 16 的倍数。
     * - CFB/OFB/CTR 为流模式，忽略 pkcs7Padding（内部会强制关闭 EVP padding）。
     * - GCM 为 AEAD 模式，会额外产生 tag；解密时必须提供正确 tag，否则校验失败。
     */
    struct GB_AesOptions
    {
        GB_AesMode mode = GB_AesMode::Cbc;

        // AES key 长度（bit）：仅支持 128/192/256。
        size_t keyBits = 256;

        // 仅对 ECB/CBC 有效：是否启用 PKCS#7 padding。
        bool pkcs7Padding = true;

        // 对“自动生成/解析 iv”的辅助字段：
        // - 0 表示使用推荐默认值：ECB=0；GCM=12；其他模式=16。
        // - 非 GCM 模式下，IV 长度固定为 16（ECB 例外为 0）；若设置为其它值，内部会按固定值处理。
        size_t ivLength = 0;

        // GCM 的 tag 长度（字节），常用 16（128-bit）。
        size_t gcmTagLength = 16;

        // GCM 的 AAD（附加认证数据），可为空。
        std::string aadBytes = "";
    };

    /**
     * @brief 使用 AES 对 UTF-8 字符串加密，输出二进制密文（以及可选的 GCM tag）。
     *
     * @param utf8PlainText 输入明文（UTF-8，按字节序列处理）。
     * @param keyBytes AES key 字节序列；长度必须与 options.keyBits 匹配（16/24/32）。
     * @param ivBytes IV/nonce 字节序列：
     *        - ECB：忽略，可为空；
     *        - CBC/CFB/OFB/CTR：必须为 16 字节；
     *        - GCM：建议 12 字节（也可为其他长度，会通过 EVP_CTRL_GCM_SET_IVLEN 设置）。
     * @param options AES 参数。
     * @param outCipherBytes 输出密文（二进制）。
     * @param outGcmTagBytes 输出 GCM tag（二进制）；仅当 mode=Gcm 时有效（否则清空）。
     * @return true 成功；false 参数非法或 OpenSSL 失败。
     */
    GLOBALBASE_PORT bool GB_AesEncrypt(const std::string& utf8PlainText, const std::string& keyBytes, const std::string& ivBytes, const GB_AesOptions& options, std::string& outCipherBytes, std::string& outGcmTagBytes);

    /**
     * @brief 使用 AES 解密二进制密文为 UTF-8 字符串（按字节序列输出）。
     *
     * @param cipherBytes 输入密文（二进制）。
     * @param keyBytes AES key 字节序列；长度必须与 options.keyBits 匹配（16/24/32）。
     * @param ivBytes IV/nonce 字节序列（要求同 GB_AesEncrypt）。
     * @param options AES 参数。
     * @param gcmTagBytes GCM tag（二进制）；仅当 mode=Gcm 时需要提供。
     * @param outUtf8PlainText 输出明文（UTF-8，按字节序列处理）。
     * @return true 成功；false 解密失败（含参数非法、padding 错误、GCM tag 校验失败等）。
     */
    GLOBALBASE_PORT bool GB_AesDecrypt(const std::string& cipherBytes, const std::string& keyBytes, const std::string& ivBytes, const GB_AesOptions& options, const std::string& gcmTagBytes, std::string& outUtf8PlainText);

    /**
     * @brief AES 加密并输出 Base64 文本（仅编码密文字节；GCM tag 需另行保存/传输）。
     *
     * @param utf8PlainText 明文（UTF-8，按字节序列处理）。
     * @param keyBytes AES key 字节序列。
     * @param ivBytes IV/nonce 字节序列。
     * @param options AES 参数。
     * @param outGcmTagBytes 输出 GCM tag（二进制），仅 mode=Gcm 时有效。
     * @param urlSafe 是否使用 base64url 字母表。
     * @param noPadding 是否省略 '=' padding。
     * @return Base64 密文；失败返回空串。
     */
    GLOBALBASE_PORT std::string GB_AesEncryptToBase64(const std::string& utf8PlainText, const std::string& keyBytes, const std::string& ivBytes, const GB_AesOptions& options, std::string& outGcmTagBytes, bool urlSafe = false, bool noPadding = false);

    /**
     * @brief 从 Base64 文本解密为 UTF-8 字符串（Base64 仅包含密文；GCM tag 需另行提供）。
     *
     * @param base64CipherText Base64 密文文本。
     * @param keyBytes AES key 字节序列。
     * @param ivBytes IV/nonce 字节序列。
     * @param options AES 参数。
     * @param gcmTagBytes GCM tag（二进制），仅 mode=Gcm 时需要提供。
     * @param outUtf8PlainText 输出明文（UTF-8，按字节序列处理）。
     * @param urlSafe 是否使用 base64url 字母表。
     * @param noPadding 是否允许省略 '=' padding。
     * @return true 成功；false 解码或解密失败。
     */
    GLOBALBASE_PORT bool GB_AesDecryptFromBase64(const std::string& base64CipherText, const std::string& keyBytes, const std::string& ivBytes, const GB_AesOptions& options, const std::string& gcmTagBytes, std::string& outUtf8PlainText, bool urlSafe = false, bool noPadding = false);

    /**
     * @brief AES 加密并生成“打包”Base64：payload = iv || cipher || (tag)。
     *
     * @remarks
     * - 该接口会根据 options.mode/ivLength 自动生成随机 iv/nonce：
     *   - ECB：不生成；
     *   - GCM：默认 12 字节；
     *   - 其他模式：默认 16 字节。
     * - 返回的 Base64 仅包含二进制 payload，不包含算法名等元数据；
     *   解密时需使用一致的 options（模式/keyBits/padding/tagLength/ivLength）。
     *
     * @param utf8PlainText 明文（UTF-8，按字节序列处理）。
     * @param keyBytes AES key 字节序列。
     * @param options AES 参数。
     * @param urlSafe 是否使用 base64url 字母表。
     * @param noPadding 是否省略 '=' padding。
     * @return Base64(payload)；失败返回空串。
     */
    GLOBALBASE_PORT std::string GB_AesEncryptToBase64Packed(const std::string& utf8PlainText, const std::string& keyBytes, const GB_AesOptions& options = GB_AesOptions(), bool urlSafe = true, bool noPadding = true);

    /**
     * @brief 从“打包”Base64 解密：payload = iv || cipher || (tag)。
     *
     * @param base64Packed Base64(payload) 文本。
     * @param keyBytes AES key 字节序列。
     * @param options AES 参数（需与加密一致）。
     * @param outUtf8PlainText 输出明文（UTF-8，按字节序列处理）。
     * @param urlSafe 是否使用 base64url 字母表。
     * @param noPadding 是否允许省略 '=' padding。
     * @return true 成功；false 解析/解码/解密失败。
     */
    GLOBALBASE_PORT bool GB_AesDecryptFromBase64Packed(const std::string& base64Packed, const std::string& keyBytes, const GB_AesOptions& options, std::string& outUtf8PlainText, bool urlSafe = true, bool noPadding = true);

    /**
     * @brief 使用 PBKDF2-HMAC-SHA256 从密码派生 AES key 与 IV。
     *
     * @remarks
     * - PBKDF2 依据 PKCS#5 / RFC 2898，跨语言实现非常普遍；
     * - outKeyBytes/outIvBytes 为二进制字节序列；
     * - ECB 模式下 outIvBytes 为空串。
     *
     * @param passwordUtf8 密码（UTF-8，按字节序列处理）。
     * @param saltBytes salt（二进制字节序列，建议至少 8 字节）。
     * @param iterations 迭代次数（>=1）。
     * @param options AES 参数（决定 keyBits 与 ivLength 默认值）。
     * @param outKeyBytes 输出 key。
     * @param outIvBytes 输出 iv/nonce。
     * @return true 成功；false 失败。
     */
    GLOBALBASE_PORT bool GB_DeriveAesKeyAndIv_Pbkdf2HmacSha256(const std::string& passwordUtf8, const std::string& saltBytes, uint32_t iterations, const GB_AesOptions& options, std::string& outKeyBytes, std::string& outIvBytes);
}

namespace GB_RSA
{
    enum class GB_RsaPublicKeyFormat
    {
        Auto = 0,

        // PEM: "-----BEGIN PUBLIC KEY-----"（X.509 SubjectPublicKeyInfo）
        PemSubjectPublicKeyInfo,

        // PEM: "-----BEGIN RSA PUBLIC KEY-----"（PKCS#1 RSAPublicKey）
        PemPkcs1,

        // DER: X.509 SubjectPublicKeyInfo（二进制）
        DerSubjectPublicKeyInfo,

        // DER: PKCS#1 RSAPublicKey（二进制）
        DerPkcs1
    };

    enum class GB_RsaPrivateKeyFormat
    {
        Auto = 0,

        // PEM: "-----BEGIN PRIVATE KEY-----"（PKCS#8 PrivateKeyInfo，默认写出格式）
        PemPkcs8,

        // PEM: "-----BEGIN RSA PRIVATE KEY-----"（PKCS#1 RSAPrivateKey）
        PemPkcs1,

        // DER: PKCS#8 PrivateKeyInfo（二进制）
        DerPkcs8,

        // DER: PKCS#1 RSAPrivateKey（二进制）
        DerPkcs1
    };

    enum class GB_RsaPaddingMode
    {
        Pkcs1V15 = 0, // RSA_PKCS1_PADDING
        Oaep,         // RSA_PKCS1_OAEP_PADDING
        NoPadding     // RSA_NO_PADDING（不安全，仅兼容用途）
    };

    enum class GB_RsaHashMethod
    {
        Sha1 = 0,
        Sha256,
        Sha384,
        Sha512
    };

    /**
     * @brief RSA 密钥生成参数。
     *
     * @remarks
     * - keyBits 常用 2048/3072/4096；不建议使用 <=1024。
     * - publicExponent 通常使用 65537（0x10001）。
     * - 输出 key 的 std::string 为“字节序列”：
     *   - PEM 输出为可打印文本；
     *   - DER 输出为二进制（可能包含 '\0'）。
     */
    struct GB_RsaKeyGenOptions
    {
        size_t keyBits = 2048;
        uint32_t publicExponent = 65537;
        GB_RsaPublicKeyFormat publicKeyFormat = GB_RsaPublicKeyFormat::PemSubjectPublicKeyInfo;
        GB_RsaPrivateKeyFormat privateKeyFormat = GB_RsaPrivateKeyFormat::PemPkcs8;
    };

    /**
     * @brief RSA 加解密参数。
     *
     * @remarks
     * - RSA 对单块明文有长度上限：与 keyBits 与 padding 有关；
     *   本库会自动进行“分块 RSA”（将数据切片为多块分别加密，并把每块密文直接拼接输出）。
     * - 若数据较大，建议使用“混合加密”（RSA 只加密对称密钥，数据使用 AES-GCM），否则密文会急剧膨胀且性能较差。
     * - 当 padding=Oaep 时，可设置 oaepHash/mgf1Hash/oaepLabelBytes；其他 padding 会忽略这些字段。
     * - 可选启用 zlib 压缩：会在加密前对明文进行压缩（若压缩后更大，则自动回退为不压缩）。
     */
    struct GB_RsaCryptOptions
    {
        GB_RsaPaddingMode padding = GB_RsaPaddingMode::Oaep;

        // OAEP digest 与 MGF1 digest（仅 padding=Oaep 有效）。
        GB_RsaHashMethod oaepHash = GB_RsaHashMethod::Sha256;
        GB_RsaHashMethod mgf1Hash = GB_RsaHashMethod::Sha256;

        // OAEP label（octet string，可为空；仅 padding=Oaep 有效）。
        std::string oaepLabelBytes = "";

        // 是否在 RSA 之前做 zlib 压缩。
        bool zlibCompress = false;

        // zlib 压缩等级：0-9，-1 表示使用 zlib 默认（通常等价于 6）。
        int zlibCompressionLevel = -1;
    };

    /**
     * @brief 生成一对 RSA 公钥与私钥。
     *
     * @param options 密钥生成参数（密钥长度/指数/输出格式）。
     * @param outPublicKeyBytes 输出公钥（PEM 或 DER，取决于 options.publicKeyFormat）。
     * @param outPrivateKeyBytes 输出私钥（PEM 或 DER，取决于 options.privateKeyFormat）。
     * @return true 成功；false 失败。
     */
    GLOBALBASE_PORT bool GB_GenerateRsaKeyPair(const GB_RsaKeyGenOptions& options, std::string& outPublicKeyBytes, std::string& outPrivateKeyBytes);

    /**
     * @brief 使用 RSA 公钥加密 UTF-8 字符串，输出二进制密文（可能为多块拼接）。
     *
     * @param utf8PlainText 输入明文（UTF-8，按字节序列处理）。
     * @param publicKeyBytes 公钥（PEM 或 DER，取决于 publicKeyFormat）。
     * @param publicKeyFormat 公钥格式。
     * @param options RSA 参数（padding/OAEP digest/zlib 等）。
     * @param outCipherBytes 输出密文（二进制，按块拼接）。
     * @return true 成功；false 失败（含 key 解析失败、参数非法、OpenSSL 错误等）。
     */
    GLOBALBASE_PORT bool GB_RsaEncrypt(const std::string& utf8PlainText, const std::string& publicKeyBytes, GB_RsaPublicKeyFormat publicKeyFormat, const GB_RsaCryptOptions& options, std::string& outCipherBytes);

    /**
     * @brief 使用 RSA 私钥解密二进制密文为 UTF-8 字符串（按字节序列输出）。
     *
     * @param cipherBytes 输入密文（二进制，按 GB_RsaEncrypt 的块拼接格式）。
     * @param privateKeyBytes 私钥（PEM 或 DER，取决于 privateKeyFormat）。
     * @param privateKeyFormat 私钥格式。
     * @param options RSA 参数（需与加密保持一致：padding/OAEP digest/label/zlib）。
     * @param outUtf8PlainText 输出明文（UTF-8，按字节序列处理）。
     * @return true 成功；false 失败（含解密失败、padding 错误、zlib 解压失败等）。
     */
    GLOBALBASE_PORT bool GB_RsaDecrypt(const std::string& cipherBytes, const std::string& privateKeyBytes, GB_RsaPrivateKeyFormat privateKeyFormat, const GB_RsaCryptOptions& options, std::string& outUtf8PlainText);

    /**
     * @brief RSA 加密并输出 Base64 文本。
     *
     * @remarks
     * - 返回值为 Base64(cipherBytes)；cipherBytes 为 GB_RsaEncrypt 的二进制输出。
     * - 若 urlEscape=true，则会对 Base64 文本执行 URL-encoding（使用 libcurl 的 curl_easy_escape），便于直接拼入 URL/query。
     *
     * @param utf8PlainText 明文（UTF-8，按字节序列处理）。
     * @param publicKeyBytes 公钥（PEM 或 DER）。
     * @param publicKeyFormat 公钥格式。
     * @param options RSA 参数。
     * @param urlSafe 是否使用 base64url 字母表。
     * @param noPadding 是否省略 '=' padding。
     * @param urlEscape 是否进行 URL-encoding（仅对 Base64 文本）。
     * @return Base64 文本；失败返回空串。
     */
    GLOBALBASE_PORT std::string GB_RsaEncryptToBase64(const std::string& utf8PlainText, const std::string& publicKeyBytes, GB_RsaPublicKeyFormat publicKeyFormat, const GB_RsaCryptOptions& options = GB_RsaCryptOptions(), bool urlSafe = true, bool noPadding = true, bool urlEscape = false);

    /**
     * @brief 从 Base64 文本解密为 UTF-8 字符串。
     *
     * @remarks
     * - 若 urlEscaped=true，会先对输入进行 URL-decoding（使用 libcurl 的 curl_easy_unescape）。
     *
     * @param base64CipherText Base64 密文文本。
     * @param privateKeyBytes 私钥（PEM 或 DER）。
     * @param privateKeyFormat 私钥格式。
     * @param options RSA 参数（需与加密保持一致）。
     * @param outUtf8PlainText 输出明文（UTF-8，按字节序列处理）。
     * @param urlSafe 是否使用 base64url 字母表。
     * @param noPadding 是否允许省略 '=' padding。
     * @param urlEscaped 输入是否为 URL-encoding 过的 Base64 文本。
     * @return true 成功；false 失败。
     */
    GLOBALBASE_PORT bool GB_RsaDecryptFromBase64(const std::string& base64CipherText, const std::string& privateKeyBytes, GB_RsaPrivateKeyFormat privateKeyFormat, const GB_RsaCryptOptions& options, std::string& outUtf8PlainText, bool urlSafe = true, bool noPadding = true, bool urlEscaped = false);
}

namespace GB_ECC
{
    enum class GB_EccCurve
    {
        P256 = 0,     // prime256v1 / secp256r1
        P384,         // secp384r1
        P521,         // secp521r1
        Secp256k1     // secp256k1
    };

    enum class GB_EccPublicKeyFormat
    {
        Auto = 0,

        // PEM: "-----BEGIN PUBLIC KEY-----"（X.509 SubjectPublicKeyInfo）
        PemSubjectPublicKeyInfo,

        // DER: X.509 SubjectPublicKeyInfo（二进制）
        DerSubjectPublicKeyInfo
    };

    enum class GB_EccPrivateKeyFormat
    {
        Auto = 0,

        // PEM: "-----BEGIN PRIVATE KEY-----"（PKCS#8 PrivateKeyInfo，默认写出格式）
        PemPkcs8,

        // PEM: "-----BEGIN EC PRIVATE KEY-----"（SEC1 ECPrivateKey）
        PemSec1,

        // DER: PKCS#8 PrivateKeyInfo（二进制）
        DerPkcs8,

        // DER: SEC1 ECPrivateKey（二进制）
        DerSec1
    };

    /**
     * @brief ECC 密钥生成参数。
     *
     * @remarks
     * - 输出 key 的 std::string 为“字节序列”：
     *   - PEM 输出为可打印文本；
     *   - DER 输出为二进制（可能包含 '\0'）。
     * - 建议使用命名曲线（Named Curve）与 SubjectPublicKeyInfo/PKCS#8，跨语言兼容性最好。
     */
    struct GB_EccKeyGenOptions
    {
        GB_EccCurve curve = GB_EccCurve::P256;
        GB_EccPublicKeyFormat publicKeyFormat = GB_EccPublicKeyFormat::PemSubjectPublicKeyInfo;
        GB_EccPrivateKeyFormat privateKeyFormat = GB_EccPrivateKeyFormat::PemPkcs8;
    };

    /**
     * @brief ECC “混合加密”参数（ECDH + HKDF-SHA256 + AES-GCM）。
     *
     * @remarks
     * - 纯 ECC（EC 公钥）并不直接提供类似 RSA 的“直接加密任意长度明文”的能力；
     *   通常做法是：用 ECDH 派生共享密钥，再用对称算法（如 AES-GCM）加密数据。
     * - 下面的 GB_EccEncrypt/GB_EccDecrypt 采用每条消息生成一次“临时（ephemeral）EC 密钥对”，具备前向安全性。
     * - 输出/输入 cipherBytes 是自描述的二进制 payload，内部包含临时公钥与 GCM 所需参数。
     * - options.aadBytes 为可选 AAD（附加认证数据），加解密双方必须保持一致；它不会被打包进 payload。
     */
    struct GB_EccCryptOptions
    {
        // AES key 长度（bit）：仅支持 128/256。默认 256。
        size_t aesKeyBits = 256;

        // GCM nonce 长度（字节），建议 12。
        size_t nonceLength = 12;

        // GCM tag 长度（字节），常用 16。
        size_t gcmTagLength = 16;

        // 附加认证数据（AAD），可为空；不会被打包进 payload。
        std::string aadBytes = "";

        // HKDF info（上下文绑定），可为空；建议保持默认。
        std::string hkdfInfoBytes = "GB_ECC_HKDF_v1";
    };

    /**
     * @brief 生成一对 ECC 公钥与私钥。
     *
     * @param options 密钥生成参数（曲线/输出格式）。
     * @param outPublicKeyBytes 输出公钥（PEM 或 DER）。
     * @param outPrivateKeyBytes 输出私钥（PEM 或 DER）。
     * @return true 成功；false 失败。
     */
    GLOBALBASE_PORT bool GB_GenerateEccKeyPair(const GB_EccKeyGenOptions& options, std::string& outPublicKeyBytes, std::string& outPrivateKeyBytes);

    /**
     * @brief 使用接收方 ECC 公钥加密 UTF-8 字符串，输出二进制 payload。
     *
     * @remarks
     * payload 格式（二进制）：
     * - header:
     *   - "GBE1"(4) || version(1=1) || flags(1) || epkLen(2be) || nonceLen(1) || tagLen(1) || aesKeyBits(2be) || plainLen(4be)
     * - body:
     *   - epkDerSpki(epkLen) || nonce(nonceLen) || cipher(*) || tag(tagLen)
     *
     * 其中 epkDerSpki 为临时公钥（DER SubjectPublicKeyInfo）。
     *
     * @param utf8PlainText 明文（UTF-8，按字节序列处理）。
     * @param publicKeyBytes 接收方公钥（PEM 或 DER）。
     * @param publicKeyFormat 公钥格式。
     * @param options ECC 混合加密参数。
     * @param outCipherBytes 输出 payload（二进制）。
     * @return true 成功；false 失败。
     */
    GLOBALBASE_PORT bool GB_EccEncrypt(const std::string& utf8PlainText, const std::string& publicKeyBytes, GB_EccPublicKeyFormat publicKeyFormat, const GB_EccCryptOptions& options, std::string& outCipherBytes);

    /**
     * @brief 使用接收方 ECC 私钥解密二进制 payload 为 UTF-8 字符串。
     *
     * @param cipherBytes 输入 payload（二进制，来自 GB_EccEncrypt）。
     * @param privateKeyBytes 接收方私钥（PEM 或 DER）。
     * @param privateKeyFormat 私钥格式。
     * @param options ECC 混合加密参数（需与加密保持一致，尤其是 aadBytes/hkdfInfoBytes 等）。
     * @param outUtf8PlainText 输出明文（UTF-8，按字节序列处理）。
     * @return true 成功；false 失败（含解析失败、GCM tag 校验失败等）。
     */
    GLOBALBASE_PORT bool GB_EccDecrypt(const std::string& cipherBytes, const std::string& privateKeyBytes, GB_EccPrivateKeyFormat privateKeyFormat, const GB_EccCryptOptions& options, std::string& outUtf8PlainText);

    /**
     * @brief ECC 加密并输出 Base64 文本（Base64(payload)）。
     *
     * @param utf8PlainText 明文（UTF-8，按字节序列处理）。
     * @param publicKeyBytes 接收方公钥（PEM 或 DER）。
     * @param publicKeyFormat 公钥格式。
     * @param options ECC 混合加密参数。
     * @param urlSafe 是否使用 base64url 字母表。
     * @param noPadding 是否省略 '=' padding。
     * @return Base64(payload)；失败返回空串。
     */
    GLOBALBASE_PORT std::string GB_EccEncryptToBase64(const std::string& utf8PlainText, const std::string& publicKeyBytes, GB_EccPublicKeyFormat publicKeyFormat = GB_EccPublicKeyFormat::Auto, const GB_EccCryptOptions& options = GB_EccCryptOptions(), bool urlSafe = true, bool noPadding = true);

    /**
     * @brief 从 Base64 文本解密为 UTF-8 字符串。
     *
     * @param base64CipherText Base64(payload) 文本。
     * @param privateKeyBytes 接收方私钥（PEM 或 DER）。
     * @param privateKeyFormat 私钥格式。
     * @param options ECC 混合加密参数（需与加密保持一致）。
     * @param outUtf8PlainText 输出明文（UTF-8，按字节序列处理）。
     * @param urlSafe 是否使用 base64url 字母表。
     * @param noPadding 是否允许省略 '=' padding。
     * @return true 成功；false 失败。
     */
    GLOBALBASE_PORT bool GB_EccDecryptFromBase64(const std::string& base64CipherText, const std::string& privateKeyBytes, std::string& outUtf8PlainText, GB_EccPrivateKeyFormat privateKeyFormat = GB_EccPrivateKeyFormat::Auto, const GB_EccCryptOptions& options = GB_EccCryptOptions(), bool urlSafe = true, bool noPadding = true);
}


#endif
