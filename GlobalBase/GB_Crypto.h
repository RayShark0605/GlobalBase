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

/**
 * @brief 使用 AES-256-CBC（PKCS#7 填充）加密字节序列并输出 Base64(IV||Cipher)。
 *
 * - 模式与填充：采用 AES-256（32 字节密钥）+ CBC 分组模式，按 PKCS#7 规则填充至 16 字节块对齐。
 * - IV 处理（安全默认）：当 iv 为空时，从系统的安全随机源生成 16 字节随机 IV；加密后**将 IV 作为前缀**与密文拼接，再整体进行 Base64 编码输出。IV 不需要保密，但对于 CBC **必须不可预测**。
 * - 密钥与 IV 的“灵活规范化”（兼容选项）：当 flexibleKeyIv=true 时，密钥长度 >32 时按字节截断为前 32 字节；<32 时在尾部补 '\0' 至 32 字节；
 *   若传入了 iv 且长度 !=16，则 >16 时截断为前 16 字节，<16 时在尾部补 '\0' 至 16 字节。
 * - Base64 行为：`urlSafe` 与 `noPadding` 直接透传至内部 Base64 编码器；输出可选 URL-safe 字母表与去 '=' 结尾。
 * - 字符串语义：`text`、`key`、`iv` 与返回值皆为“原始字节序列”的容器；不进行任何字符集转换。
 *
 * @param text
 *     原始明文字节序列（std::string 仅作为字节容器）。
 * @param key
 *     密钥。当 flexibleKeyIv=true 时会被规范化为 32 字节；否则要求恰为 32 字节。
 * @param iv
 *     初始化向量原料。为空时自动生成随机 16 字节 IV 并前缀到输出；非空时按 flexibleKeyIv 规则处理或要求恰为 16 字节。
 * @param urlSafe
 *     Base64 是否使用 URL/文件名安全字母表（默认 false）。
 * @param noPadding
 *     Base64 是否采用“无填充”约定（默认 false）。
 * @param flexibleKeyIv
 *     是否启用“密钥/IV 长度灵活处理”（默认 true）：超长截断、过短补零。
 *
 * @return
 *     Base64 文本（编码自字节串：IV||Ciphertext）。若入参非法、随机源失败或内部错误，返回空字符串。
 */
GLOBALBASE_PORT std::string GB_Aes256Encrypt(const std::string& text, const std::string& key, const std::string& iv = std::string(), bool urlSafe = false, bool noPadding = false, bool flexibleKeyIv = true);

/**
 * @brief 从 Base64(IV||Cipher) 文本解码并使用 AES-256-CBC（PKCS#7 去填充）还原原文字节序列。
 *
 * - 输入契约：`encryptedText` 必须是由“16 字节 IV 前缀 + 密文”整体 Base64 编码得到；函数将先 Base64 解码，
 *   取前 16 字节为 IV，剩余字节为密文，再执行 CBC 解密与 PKCS#7 去填充。
 * - 严格解析：`strictMode`、`urlSafe`、`noPadding` 直接透传至内部 Base64 解码器，以控制字母表、空白、'=' 等校验策略。
 * - 密钥规范化：当 flexibleKey=true 时，密钥 >32 字节则截断，<32 字节则尾部补 '\0' 至 32；否则要求 key 恰为 32 字节。
 * - 字符串语义：所有字符串参数与返回值均视为“原始字节序列”，不做编码转换。
 * - 失败情形：Base64 非法；解码后长度 <16 或 (总长-16) 不是 16 的倍数；密钥长度不合规（在 flexibleKey=false 时）；
 *   CBC 解密或 PKCS#7 去填充失败；以上情况均返回空字符串。
 *
 * @param encryptedText
 *     Base64 文本，编码自字节串“IV||Ciphertext”。支持严格/宽松两种 Base64 解析策略（见 strictMode、urlSafe、noPadding）。
 * @param key
 *     密钥。当 flexibleKey=true 时会被规范化为 32 字节；否则要求恰为 32 字节。
 * @param strictMode
 *     Base64 严格模式开关（默认 false）。启用后将按所选字母表、长度与填充规则进行严格校验。
 * @param urlSafe
 *     Base64 是否采用 URL/文件名安全字母表（默认 false）。
 * @param noPadding
 *     Base64 是否采用“无填充”约定（默认 false）。
 * @param flexibleKey
 *     是否启用“密钥长度灵活处理”（默认 true）：超长截断、过短补零。
 *
 * @return
 *     还原出的原文字节序列；若输入不合法或解密失败，返回空字符串。
 */
GLOBALBASE_PORT std::string GB_Aes256Decrypt(const std::string& encryptedText, const std::string& key, bool strictMode = false, bool urlSafe = false, bool noPadding = false, bool flexibleKey = true);

/**
 * @brief 生成一对 RSA 密钥，并以 Base64(PKCS#1/DER) 的裸文本返回。
 *
 * - 密钥格式（非常重要）：
 *   1) 私钥按 PKCS#1 RSAPrivateKey 的 ASN.1 结构：version(=0)、n、e、d、p、q、dp、dq、qi，
 *      再按 DER 编码；公钥按 PKCS#1 RSAPublicKey：SEQUENCE{ n, e }，再按 DER 编码。
 *   2) 本函数返回的是 **DER 的 Base64 主体**（不带“-----BEGIN …----- / -----END …-----”头尾，
 *      也不插入换行），便于与现有接口保持一致。
 *
 * - 生成策略：
 *   1) keySize 表示 n 的目标比特数；内部按 keySize/2 生成等长素数 p、q，再构造 n=p*q。
 *   2) 当 keySize<1024 时自动提升到 1024；建议实际使用 ≥2048。
 *   3) 公钥指数 e 固定为 65537（0x10001），再计算 d 使得 e·d ≡ 1 (mod φ(n))。
 *
 * - 返回值：成功返回 true 且
 *      publicKey = Base64(DER(RSAPublicKey{n,e})),
 *      privateKey = Base64(DER(RSAPrivateKey{version,n,e,d,p,q,dp,dq,qi}))；
 *   失败返回 false，输出参数不修改或为空。
 *
 * - 兼容性提示：
 *   返回值不是 X.509 SubjectPublicKeyInfo/SPKI，也不是 PKCS#8；如需在外部库中使用，
 *   请确认对方能接受 **PKCS#1 裸 DER 的 Base64 主体**。
 */
GLOBALBASE_PORT bool GB_RsaGenerateKeyPair(std::string& publicKey, std::string& privateKey, int keySize = 2048);

/**
 * @brief 使用 PKCS#1 v1.5（RSAES-PKCS1-v1_5）对明文进行分块加密，并整体 Base64 输出。
 *
 * - 密钥格式：参数 encryptionKey 必须是 Base64( DER(RSAPublicKey{ n, e }) ) 的 **主体文本**。
 *   函数会先 Base64 解码，再解析 DER 取出 n、e。
 *
 * - 分块与填充：
 *   设 k = 模长（字节数）。单块最大明文长度 mLen = k - 11；对每个明文分块 M 生成
 *   编码块 EM = 0x00 || 0x02 || PS || 0x00 || M，其中 PS 为长度 ≥8 的**全非零**随机字节串。
 *   然后计算 C = EM^e mod n，并将 C 以**定长 k 字节**拼接。
 *
 * - 输出：将所有密文块按原顺序拼接为字节串，再做 Base64（标准字母表、含‘=’，不 URL-safe）输出。
 *
 * - 失败返回空字符串：包含但不限于 Base64/DER 非法、公钥解析失败、k<11、或内部错误。
 *
 * - 安全特性：由于 v1.5 填充包含随机 PS，同一明文多次加密会得到不同密文；但 v1.5 方案在新设计中
 *   已被认为弱于 OAEP，除非为协议兼容，否则建议优先使用 OAEP。
 */
GLOBALBASE_PORT std::string GB_RsaEncrypt(const std::string& plainText, const std::string& encryptionKey);

/**
 * @brief 使用 PKCS#1 v1.5（RSAES-PKCS1-v1_5）解密 Base64(拼接的 k 字节密文块)，并返回原文字节序列。
 *
 * - 密钥格式：参数 decryptionKey 必须是 Base64( DER(RSAPrivateKey{ version=0,n,e,d,p,q,dp,dq,qi }) )
 *   的**主体文本**。函数会先 Base64 解码，再解析 DER 取出 n、d。
 *
 * - 输入密文：encryptedText 是整体 Base64 文本；解码后必须能被 k 整除（每块 k 字节）。
 *
 * - 解密与去填充：
 *   对每个密文块 C 执行 m = C^d mod n，并将 m 左填充至 k 字节作为 EM；
 *   检查 EM 是否形如 0x00 || 0x02 || PS(全非零, 长度≥8) || 0x00 || M，
 *   找到分隔 0x00 后将其后的 M 追加到最终明文。
 *
 * - 输出：返回拼接后的原文字节序列（按原样字节，不做编码转换）。
 *
 * - 失败返回空字符串：例如 Base64/DER 非法、密文长度非法、去填充失败等。
 */
GLOBALBASE_PORT std::string GB_RsaDecrypt(const std::string& encryptedText, const std::string& decryptionKey);


#endif