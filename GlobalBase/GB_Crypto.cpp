#include "GB_Crypto.h"

#include <cstdint>
#include <cstring>
#include <stdio.h>

#include "openssl/evp.h"
#include "openssl/opensslv.h"

namespace
{
    static const char* GetBase64Alphabet(bool urlSafe)
    {
        // RFC 4648: base64 与 base64url 的字母表差异仅在 62/63 字符。
        return urlSafe
            ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
            : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    }

    static bool IsBase64Whitespace(char ch)
    {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    }

    static bool BuildBase64DecodeTable(bool urlSafe, int8_t outTable[256])
    {
        if (!outTable)
        {
            return false;
        }

        std::memset(outTable, -1, 256 * sizeof(outTable[0]));

        const char* alphabet = GetBase64Alphabet(urlSafe);
        for (int i = 0; i < 64; i++)
        {
            outTable[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
        }

        // 允许 padding。
        outTable[static_cast<unsigned char>('=')] = -2;
        return true;
    }

    static bool NormalizeBase64Input(const std::string& input, bool noPadding, std::string& outNormalized)
    {
        outNormalized.clear();
        outNormalized.reserve(input.size());

        for (size_t i = 0; i < input.size(); i++)
        {
            const char ch = input[i];
            if (IsBase64Whitespace(ch))
            {
                continue;
            }
            outNormalized.push_back(ch);
        }

        if (outNormalized.empty())
        {
            return true;
        }

        // RFC 4648：Base64 编码长度应为 4 的倍数；若省略 padding，则可能出现余数 2 或 3。
        const size_t mod = outNormalized.size() % 4;
        if (mod == 0)
        {
            return true;
        }

        if (!noPadding)
        {
            return false;
        }

        if (mod == 1)
        {
            // 余数为 1 不可能是合法的 Base64（无论是否省略 padding）。
            return false;
        }

        // mod == 2 或 3，按需要补齐 '='。
        const size_t padCount = (4 - mod) % 4;
        outNormalized.append(padCount, '=');
        return true;
    }

    static char ToHexCharLower(unsigned char value)
    {
        if (value < 10U)
        {
            return static_cast<char>('0' + value);
        }
        return static_cast<char>('a' + (value - 10U));
    }

    static std::string BytesToLowerHexString(const unsigned char* data, size_t size)
    {
        if (data == nullptr || size == 0)
        {
            return std::string();
        }

        std::string hexString;
        hexString.resize(size * 2);

        for (size_t i = 0; i < size; i++)
        {
            const unsigned char byteValue = data[i];
            hexString[i * 2 + 0] = ToHexCharLower(static_cast<unsigned char>((byteValue >> 4) & 0x0F));
            hexString[i * 2 + 1] = ToHexCharLower(static_cast<unsigned char>(byteValue & 0x0F));
        }

        return hexString;
    }

    static const char* GetShaAlgorithmName(GB_ShaMethod method)
    {
        switch (method)
        {
        case GB_ShaMethod::Sha256:
            return "SHA256";
        case GB_ShaMethod::Sha512:
            return "SHA512";
        case GB_ShaMethod::Sha3_256:
            return "SHA3-256";
        case GB_ShaMethod::Sha3_512:
            return "SHA3-512";
        default:
            return "SHA256";
        }
    }

    static std::string HashUtf8StringToLowerHex(const std::string& utf8Text, const char* algorithmName)
    {
        if (algorithmName == nullptr || algorithmName[0] == '\0')
        {
            return std::string();
        }

        EVP_MD_CTX* digestContext = ::EVP_MD_CTX_new();
        if (digestContext == nullptr)
        {
            return std::string();
        }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EVP_MD* fetchedMd = ::EVP_MD_fetch(nullptr, algorithmName, nullptr);
        const EVP_MD* digestType = fetchedMd;
        if (fetchedMd == nullptr || digestType == nullptr)
        {
            ::EVP_MD_CTX_free(digestContext);
            return std::string();
        }
#else
        const EVP_MD* digestType = ::EVP_get_digestbyname(algorithmName);
        if (digestType == nullptr)
        {
            if (std::strcmp(algorithmName, "SHA256") == 0 || std::strcmp(algorithmName, "SHA-256") == 0)
            {
                digestType = ::EVP_sha256();
            }
            else if (std::strcmp(algorithmName, "SHA512") == 0 || std::strcmp(algorithmName, "SHA-512") == 0)
            {
                digestType = ::EVP_sha512();
            }
#if defined(EVP_sha3_256)
            else if (std::strcmp(algorithmName, "SHA3-256") == 0)
            {
                digestType = ::EVP_sha3_256();
            }
#endif
#if defined(EVP_sha3_512)
            else if (std::strcmp(algorithmName, "SHA3-512") == 0)
            {
                digestType = ::EVP_sha3_512();
            }
#endif
        }

        if (digestType == nullptr)
        {
            ::EVP_MD_CTX_free(digestContext);
            return std::string();
        }
#endif

        const int initOk = ::EVP_DigestInit_ex(digestContext, digestType, nullptr);
        if (initOk != 1)
        {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
            ::EVP_MD_free(fetchedMd);
#endif
            ::EVP_MD_CTX_free(digestContext);
            return std::string();
        }

        if (!utf8Text.empty())
        {
            const int updateOk = ::EVP_DigestUpdate(digestContext, utf8Text.data(), utf8Text.size());
            if (updateOk != 1)
            {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
                ::EVP_MD_free(fetchedMd);
#endif
                ::EVP_MD_CTX_free(digestContext);
                return std::string();
            }
        }

        unsigned char digestBytes[EVP_MAX_MD_SIZE];
        unsigned int digestSize = 0U;

        const int finalOk = ::EVP_DigestFinal_ex(digestContext, digestBytes, &digestSize);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        ::EVP_MD_free(fetchedMd);
#endif
        ::EVP_MD_CTX_free(digestContext);

        if (finalOk != 1 || digestSize == 0U)
        {
            return std::string();
        }

        return BytesToLowerHexString(digestBytes, static_cast<size_t>(digestSize));
    }
}

std::string GB_Base64Encode(const std::string& utf8Text, bool urlSafe, bool noPadding)
{
    if (utf8Text.empty())
    {
        return std::string();
    }

    const unsigned char* data = reinterpret_cast<const unsigned char*>(utf8Text.data());
    const size_t dataSize = utf8Text.size();
    const char* alphabet = GetBase64Alphabet(urlSafe);

    const size_t reservedSize = ((dataSize + 2) / 3) * 4;
    std::string result;
    result.reserve(reservedSize);

    for (size_t i = 0; i < dataSize; i += 3)
    {
        const size_t remain = dataSize - i;
        const unsigned int b0 = static_cast<unsigned int>(data[i]);
        const unsigned int b1 = (remain > 1) ? static_cast<unsigned int>(data[i + 1]) : 0U;
        const unsigned int b2 = (remain > 2) ? static_cast<unsigned int>(data[i + 2]) : 0U;

        const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;

        result.push_back(alphabet[(triple >> 18) & 0x3F]);
        result.push_back(alphabet[(triple >> 12) & 0x3F]);
        result.push_back((remain > 1) ? alphabet[(triple >> 6) & 0x3F] : '=');
        result.push_back((remain > 2) ? alphabet[triple & 0x3F] : '=');
    }

    if (noPadding)
    {
        while (!result.empty() && result.back() == '=')
        {
            result.pop_back();
        }
    }

    return result;
}

bool GB_Base64Decode(const std::string& base64Text, std::string& outUtf8Text, bool urlSafe, bool noPadding)
{
    outUtf8Text.clear();

    std::string normalized;
    if (!NormalizeBase64Input(base64Text, noPadding, normalized))
    {
        return false;
    }

    if (normalized.empty())
    {
        return true;
    }

    int8_t decodeTable[256];
    if (!BuildBase64DecodeTable(urlSafe, decodeTable))
    {
        return false;
    }

    const size_t inputSize = normalized.size();
    if (inputSize % 4 != 0)
    {
        return false;
    }

    outUtf8Text.reserve((inputSize / 4) * 3);

    for (size_t i = 0; i < inputSize; i += 4)
    {
        const char c0 = normalized[i + 0];
        const char c1 = normalized[i + 1];
        const char c2 = normalized[i + 2];
        const char c3 = normalized[i + 3];

        const int v0 = decodeTable[static_cast<unsigned char>(c0)];
        const int v1 = decodeTable[static_cast<unsigned char>(c1)];
        const int v2 = decodeTable[static_cast<unsigned char>(c2)];
        const int v3 = decodeTable[static_cast<unsigned char>(c3)];

        // 前两位不能是 padding，也不能是非法字符。
        if (v0 < 0 || v1 < 0)
        {
            return false;
        }

        const bool pad2 = (v2 == -2);
        const bool pad3 = (v3 == -2);

        if (pad2)
        {
            // 形如 "XX=="，输出 1 字节。必须是最后一个块。
            if (!pad3)
            {
                return false;
            }
            if (i + 4 != inputSize)
            {
                return false;
            }

            // Canonical 校验：当仅输出 1 字节时，v1 的低 4 bit 应为 0。
            if ((v1 & 0x0F) != 0)
            {
                return false;
            }

            const unsigned char byte0 = static_cast<unsigned char>((v0 << 2) | (v1 >> 4));
            outUtf8Text.push_back(static_cast<char>(byte0));
            break;
        }

        if (v2 < 0)
        {
            return false;
        }

        if (pad3)
        {
            // 形如 "XXX="，输出 2 字节。必须是最后一个块。
            if (i + 4 != inputSize)
            {
                return false;
            }

            // Canonical 校验：当仅输出 2 字节时，v2 的低 2 bit 应为 0。
            if ((v2 & 0x03) != 0)
            {
                return false;
            }

            const unsigned char byte0 = static_cast<unsigned char>((v0 << 2) | (v1 >> 4));
            const unsigned char byte1 = static_cast<unsigned char>(((v1 & 0x0F) << 4) | (v2 >> 2));
            outUtf8Text.push_back(static_cast<char>(byte0));
            outUtf8Text.push_back(static_cast<char>(byte1));
            break;
        }

        if (v3 < 0)
        {
            return false;
        }

        // 无 padding，输出 3 字节。
        const unsigned char byte0 = static_cast<unsigned char>((v0 << 2) | (v1 >> 4));
        const unsigned char byte1 = static_cast<unsigned char>(((v1 & 0x0F) << 4) | (v2 >> 2));
        const unsigned char byte2 = static_cast<unsigned char>(((v2 & 0x03) << 6) | v3);
        outUtf8Text.push_back(static_cast<char>(byte0));
        outUtf8Text.push_back(static_cast<char>(byte1));
        outUtf8Text.push_back(static_cast<char>(byte2));
    }

    return true;
}

std::string GB_Md5Hash(const std::string& utf8Text)
{
    EVP_MD_CTX* digestContext = ::EVP_MD_CTX_new();
    if (digestContext == nullptr)
    {
        return std::string();
    }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_MD* md5Type = ::EVP_MD_fetch(nullptr, "MD5", nullptr);
    if (md5Type == nullptr)
    {
        ::EVP_MD_CTX_free(digestContext);
        return std::string();
    }
    const EVP_MD* digestType = md5Type;
#else
    const EVP_MD* digestType = ::EVP_md5();
#endif

    const int initOk = ::EVP_DigestInit_ex(digestContext, digestType, nullptr);
    if (initOk != 1)
    {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        ::EVP_MD_free(md5Type);
#endif
        ::EVP_MD_CTX_free(digestContext);
        return std::string();
    }

    if (!utf8Text.empty())
    {
        const int updateOk = ::EVP_DigestUpdate(digestContext, utf8Text.data(), utf8Text.size());
        if (updateOk != 1)
        {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
            ::EVP_MD_free(md5Type);
#endif
            ::EVP_MD_CTX_free(digestContext);
            return std::string();
        }
    }

    unsigned char digestBytes[EVP_MAX_MD_SIZE];
    unsigned int digestSize = 0U;

    const int finalOk = ::EVP_DigestFinal_ex(digestContext, digestBytes, &digestSize);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    ::EVP_MD_free(md5Type);
#endif
    ::EVP_MD_CTX_free(digestContext);

    if (finalOk != 1 || digestSize == 0U)
    {
        return std::string();
    }

    return BytesToLowerHexString(digestBytes, static_cast<size_t>(digestSize));
}

std::string GB_ShaHash(const std::string& utf8Text, GB_ShaMethod method)
{
    const char* algorithmName = GetShaAlgorithmName(method);
    return HashUtf8StringToLowerHex(utf8Text, algorithmName);
}

