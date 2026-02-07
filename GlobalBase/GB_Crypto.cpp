#include "GB_Crypto.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <openssl/x509.h>

#include <openssl/ec.h>
#include <openssl/hmac.h>

#if OPENSSL_VERSION_NUMBER >= 0x10101000L
#include <openssl/kdf.h>
#endif

#include <curl/curl.h>

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/provider.h>
#endif

#if OPENSSL_VERSION_NUMBER >= 0x30200000L
#include <openssl/core_names.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/thread.h>
#endif
#include <zlib.h>

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#endif

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

namespace
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    // 注意：OSSL_PROVIDER_unload 在进程退出时的析构顺序中可能引发崩溃（例如 libcrypto 已先行清理内部锁）。
    // 因此这里选择“加载一次，进程生命周期内保持加载”，避免静态析构阶段调用 unload。
    static OSSL_PROVIDER*& GetDefaultProviderRaw()
    {
        static OSSL_PROVIDER* provider = nullptr;
        return provider;
    }

    static OSSL_PROVIDER*& GetLegacyProviderRaw()
    {
        static OSSL_PROVIDER* provider = nullptr;
        return provider;
    }

    static void LoadOpenSslProviders()
    {
        // "default" 通常已加载，但显式加载可提升跨平台一致性。
        if (GetDefaultProviderRaw() == nullptr)
        {
            GetDefaultProviderRaw() = ::OSSL_PROVIDER_load(nullptr, "default");
        }

        // "legacy" 用于部分传统算法（例如某些发行版/配置下的 MD5/MD4 等）。
        // 若加载失败也不作为错误处理。
        if (GetLegacyProviderRaw() == nullptr)
        {
            GetLegacyProviderRaw() = ::OSSL_PROVIDER_load(nullptr, "legacy");
        }
    }

    static void EnsureOpenSslProvidersLoaded()
    {
        static std::once_flag onceFlag;
        std::call_once(onceFlag, LoadOpenSslProviders);
    }

    struct EvpMdDeleter
    {
        void operator()(EVP_MD* md) const
        {
            if (md != nullptr)
            {
                ::EVP_MD_free(md);
            }
        }
    };
#endif

    static EVP_MD_CTX* CreateEvpMdCtx()
    {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
        return ::EVP_MD_CTX_create();
#else
        return ::EVP_MD_CTX_new();
#endif
    }

    struct EvpMdCtxDeleter
    {
        void operator()(EVP_MD_CTX* ctx) const
        {
            if (ctx != nullptr)
            {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
                ::EVP_MD_CTX_destroy(ctx);
#else
                ::EVP_MD_CTX_free(ctx);
#endif
            }
        }
    };

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

    static const std::array<int8_t, 256>& GetBase64DecodeTable(bool urlSafe)
    {
        if (urlSafe)
        {
            static const std::array<int8_t, 256> table = []() -> std::array<int8_t, 256>
                {
                    std::array<int8_t, 256> tempTable;
                    if (!BuildBase64DecodeTable(true, tempTable.data()))
                    {
                        tempTable.fill(-1);
                        tempTable[static_cast<unsigned char>('=')] = -2;
                    }
                    return tempTable;
                }();
            return table;
        }

        static const std::array<int8_t, 256> table = []() -> std::array<int8_t, 256>
            {
                std::array<int8_t, 256> tempTable;
                if (!BuildBase64DecodeTable(false, tempTable.data()))
                {
                    tempTable.fill(-1);
                    tempTable[static_cast<unsigned char>('=')] = -2;
                }
                return tempTable;
            }();
        return table;
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

    static bool SafeAddSizeT(size_t a, size_t b, size_t& out)
    {
        if (a > (std::numeric_limits<size_t>::max)() - b)
        {
            return false;
        }

        out = a + b;
        return true;
    }

    static bool SafeMulSizeT(size_t a, size_t b, size_t& out)
    {
        if (a == 0 || b == 0)
        {
            out = 0;
            return true;
        }

        if (a > (std::numeric_limits<size_t>::max)() / b)
        {
            return false;
        }

        out = a * b;
        return true;
    }

    static bool GenerateRandomBytes(size_t bytesCount, std::string& outBytes)
    {
        outBytes.clear();

        if (bytesCount == 0)
        {
            return true;
        }

        if (bytesCount > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        outBytes.resize(bytesCount);
        if (::RAND_bytes(reinterpret_cast<unsigned char*>(&outBytes[0]), static_cast<int>(bytesCount)) != 1)
        {
            outBytes.clear();
            return false;
        }

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

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter> digestContext(CreateEvpMdCtx());
        if (digestContext.get() == nullptr)
        {
            return std::string();
        }

        const EVP_MD* digestType = nullptr;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        std::unique_ptr<EVP_MD, EvpMdDeleter> fetchedMd(::EVP_MD_fetch(nullptr, algorithmName, nullptr));
        digestType = fetchedMd.get();
#else

#if OPENSSL_VERSION_NUMBER < 0x10100000L
        static std::once_flag addAllDigestsOnceFlag;
        std::call_once(addAllDigestsOnceFlag, []() {
            ::OpenSSL_add_all_digests();
            });
#endif
        digestType = ::EVP_get_digestbyname(algorithmName);
#  if OPENSSL_VERSION_NUMBER >= 0x10101000L
        if (digestType == nullptr)
        {
            if (::strcmp(algorithmName, "SHA3-256") == 0)
            {
                digestType = ::EVP_sha3_256();
            }
            else if (::strcmp(algorithmName, "SHA3-512") == 0)
            {
                digestType = ::EVP_sha3_512();
            }
        }
#  endif
#endif

        if (digestType == nullptr)
        {
            return std::string();
        }

        if (::EVP_DigestInit_ex(digestContext.get(), digestType, nullptr) != 1)
        {
            return std::string();
        }

        if (!utf8Text.empty())
        {
            if (::EVP_DigestUpdate(digestContext.get(), utf8Text.data(), utf8Text.size()) != 1)
            {
                return std::string();
            }
        }

        unsigned char digestBytes[EVP_MAX_MD_SIZE];
        ::memset(digestBytes, 0, sizeof(digestBytes));

        unsigned int digestSize = 0;
        if (::EVP_DigestFinal_ex(digestContext.get(), digestBytes, &digestSize) != 1)
        {
            return std::string();
        }

        return BytesToLowerHexString(digestBytes, static_cast<size_t>(digestSize));
    }

    static std::string Uint32ToLowerHex8(uint32_t value)
    {
        std::string result;
        result.resize(8);

        for (size_t i = 0; i < 8; i++)
        {
            const uint32_t shiftBits = static_cast<uint32_t>((7 - i) * 4);
            const unsigned char nibble = static_cast<unsigned char>((value >> shiftBits) & 0x0F);
            result[i] = ToHexCharLower(nibble);
        }

        return result;
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

    // 计算输出大小：fullSize = ceil(dataSize / 3) * 4
    size_t dataSizePlus2 = 0;
    if (!SafeAddSizeT(dataSize, 2, dataSizePlus2))
    {
        return std::string();
    }

    const size_t groups = dataSizePlus2 / 3;
    size_t fullSize = 0;
    if (!SafeMulSizeT(groups, 4, fullSize))
    {
        return std::string();
    }

    size_t outputSize = fullSize;
    if (noPadding)
    {
        const size_t mod = dataSize % 3;
        if (mod == 1)
        {
            outputSize -= 2;
        }
        else if (mod == 2)
        {
            outputSize -= 1;
        }
    }

    std::string result;
    result.resize(outputSize);

    size_t outIndex = 0;

    for (size_t i = 0; i < dataSize; i += 3)
    {
        const size_t remain = dataSize - i;

        const unsigned int b0 = static_cast<unsigned int>(data[i]);
        const unsigned int b1 = (remain > 1) ? static_cast<unsigned int>(data[i + 1]) : 0U;
        const unsigned int b2 = (remain > 2) ? static_cast<unsigned int>(data[i + 2]) : 0U;

        const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;

        const char c0 = alphabet[(triple >> 18) & 0x3F];
        const char c1 = alphabet[(triple >> 12) & 0x3F];
        const char c2 = (remain > 1) ? alphabet[(triple >> 6) & 0x3F] : '=';
        const char c3 = (remain > 2) ? alphabet[triple & 0x3F] : '=';

        if (!noPadding || remain >= 3)
        {
            result[outIndex++] = c0;
            result[outIndex++] = c1;
            result[outIndex++] = c2;
            result[outIndex++] = c3;
        }
        else if (remain == 2)
        {
            result[outIndex++] = c0;
            result[outIndex++] = c1;
            result[outIndex++] = c2;
        }
        else
        {
            // remain == 1
            result[outIndex++] = c0;
            result[outIndex++] = c1;
        }
    }

    if (outIndex != result.size())
    {
        result.resize(outIndex);
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
    const std::array<int8_t, 256>& decodeTable = GetBase64DecodeTable(urlSafe);

    const size_t inputSize = normalized.size();
    if (inputSize % 4 != 0)
    {
        return false;
    }

    // 预估最大输出长度：inputSize/4*3
    const size_t groups = inputSize / 4;
    size_t reserveSize = 0;
    if (SafeMulSizeT(groups, 3, reserveSize))
    {
        outUtf8Text.reserve(reserveSize);
    }

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
    return HashUtf8StringToLowerHex(utf8Text, "MD5");
}

std::string GB_ShaHash(const std::string& utf8Text, GB_ShaMethod method)
{
    const char* algorithmName = GetShaAlgorithmName(method);
    return HashUtf8StringToLowerHex(utf8Text, algorithmName);
}

std::string GB_Crc32Hash(const std::string& utf8Text)
{
    // 若 buf 为 Z_NULL，crc32() 返回所需的初始 crc 值。
    uLong crc = ::crc32(0L, Z_NULL, 0);

    const unsigned char* data = reinterpret_cast<const unsigned char*>(utf8Text.data());
    size_t remaining = utf8Text.size();

    // crc32() 的长度参数是 uInt（通常是 32-bit），这里分块处理以避免极端情况下溢出。
    while (remaining > 0)
    {
        const size_t chunkSize = std::min(remaining, static_cast<size_t>(std::numeric_limits<uInt>::max()));
        crc = ::crc32(crc, data, static_cast<uInt>(chunkSize));

        data += chunkSize;
        remaining -= chunkSize;
    }

    return Uint32ToLowerHex8(static_cast<uint32_t>(crc));
}

static void SecureCleanseBuffer(void* data, size_t size)
{
    if (data != nullptr && size > 0)
    {
        ::OPENSSL_cleanse(data, size);
    }
}

class StringCleansingGuard
{
public:
    explicit StringCleansingGuard(std::string& bytes) : m_bytes(bytes) {}

    ~StringCleansingGuard()
    {
        if (!m_bytes.empty())
        {
            SecureCleanseBuffer(&m_bytes[0], m_bytes.size());
        }
    }

private:
    std::string& m_bytes;
};

namespace GB_Argon2
{
#if OPENSSL_VERSION_NUMBER >= 0x30200000L

    static const char* GetArgon2KdfName(GB_Argon2Variant variant)
    {
        switch (variant)
        {
        case GB_Argon2Variant::Argon2i:
            return "ARGON2I";
        case GB_Argon2Variant::Argon2d:
            return "ARGON2D";
        case GB_Argon2Variant::Argon2id:
            return "ARGON2ID";
        default:
            return "ARGON2ID";
        }
    }

    static const char* GetArgon2EncodedName(GB_Argon2Variant variant)
    {
        switch (variant)
        {
        case GB_Argon2Variant::Argon2i:
            return "argon2i";
        case GB_Argon2Variant::Argon2d:
            return "argon2d";
        case GB_Argon2Variant::Argon2id:
            return "argon2id";
        default:
            return "argon2id";
        }
    }

    static int GetArgon2VersionNumber(uint32_t version)
    {
        // Argon2 version: 0x10 -> 16，0x13 -> 19。
        if (version == 0x10)
        {
            return 16;
        }
        if (version == 0x13)
        {
            return 19;
        }
        // 其他版本 OpenSSL 可能不支持，这里按 19 回退。
        return 19;
    }

    static uint32_t ParseArgon2VersionNumber(int versionNumber)
    {
        if (versionNumber == 16)
        {
            return 0x10;
        }
        if (versionNumber == 19)
        {
            return 0x13;
        }
        return 0;
    }

    struct EvpKdfDeleter
    {
        void operator()(EVP_KDF* kdf) const
        {
            if (kdf != nullptr)
            {
                ::EVP_KDF_free(kdf);
            }
        }
    };

    struct EvpKdfCtxDeleter
    {
        void operator()(EVP_KDF_CTX* ctx) const
        {
            if (ctx != nullptr)
            {
                ::EVP_KDF_CTX_free(ctx);
            }
        }
    };

    static void NormalizeArgon2Options(GB_Argon2::GB_Argon2Options& options)
    {
        if (options.iterations == 0)
        {
            options.iterations = 1;
        }

        if (options.lanes == 0)
        {
            options.lanes = 1;
        }

        // Argon2 要求 m >= 8 * p（单位 KiB）。注意 lanes 可能较大，避免 32bit 乘法溢出。
        const uint64_t minMemoryCost64 = static_cast<uint64_t>(options.lanes) * 8ULL;
        const uint32_t minMemoryCost = (minMemoryCost64 > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
            ? std::numeric_limits<uint32_t>::max()
            : static_cast<uint32_t>(minMemoryCost64);

        if (options.memoryCostKiB < minMemoryCost)
        {
            options.memoryCostKiB = minMemoryCost;
        }

        if (options.hashLength == 0)
        {
            options.hashLength = 32;
        }

        if (options.saltLength == 0)
        {
            options.saltLength = 16;
        }

        if (options.version != 0x10 && options.version != 0x13)
        {
            options.version = 0x13;
        }

        if (options.threads > 0 && options.threads > options.lanes)
        {
            options.threads = options.lanes;
        }
    }

    static void* GetOctetStringPointerForOpenSsl(std::string& bytes)
    {
        // OpenSSL 的 OSSL_PARAM_construct_octet_string 需要非 const 的 void*。
        // 对 std::string 来说，&bytes[0] 在非空时是稳定且可写的。
        if (bytes.empty())
        {
            static char emptyBuffer[1] = { 0 };
            return emptyBuffer;
        }

        return &bytes[0];
    }

    static bool DeriveArgon2BytesOnce(const std::string& passwordBytes, const std::string& saltBytes, const GB_Argon2::GB_Argon2Options& options, bool includeThreadsParam, std::string& outDerivedBytes)
    {
        outDerivedBytes.clear();

        if (saltBytes.empty())
        {
            return false;
        }

        GB_Argon2::GB_Argon2Options normalizedOptions = options;
        NormalizeArgon2Options(normalizedOptions);

        if (normalizedOptions.hashLength == 0)
        {
            return false;
        }

        if (normalizedOptions.hashLength > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        EnsureOpenSslProvidersLoaded();

        const char* kdfName = GetArgon2KdfName(normalizedOptions.variant);

        std::unique_ptr<EVP_KDF, EvpKdfDeleter> kdf(::EVP_KDF_fetch(nullptr, kdfName, nullptr));
        if (kdf.get() == nullptr)
        {
            return false;
        }

        std::unique_ptr<EVP_KDF_CTX, EvpKdfCtxDeleter> kdfContext(::EVP_KDF_CTX_new(kdf.get()));
        if (kdfContext.get() == nullptr)
        {
            return false;
        }

        const bool useThreadsParam = includeThreadsParam && (normalizedOptions.threads > 0);
        uint32_t threads = useThreadsParam ? normalizedOptions.threads : 0;

        // OpenSSL 的线程池是按 libctx 全局配置的（OSSL_set_max_threads）。
        // 本库不主动修改全局状态：若线程池未启用或上限不足，则将 threads 降级为 1（等价于不并行）。
        if (threads > 1)
        {
            const uint64_t maxThreads64 = ::OSSL_get_max_threads(nullptr);
            if (maxThreads64 == 0)
            {
                threads = 1;
            }
            else if (maxThreads64 < static_cast<uint64_t>(threads))
            {
                const uint64_t clamped = std::min<uint64_t>(
                    maxThreads64,
                    static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
                );
                threads = static_cast<uint32_t>(clamped);
                if (threads < 1)
                {
                    threads = 1;
                }
            }
        }

        // OSSL_PARAM_construct_octet_string 需要可写的 void*，这里拷贝一份来避免 const_cast。
        std::string passwordCopy = passwordBytes;
        std::string saltCopy = saltBytes;
        std::string secretCopy = normalizedOptions.secret;
        std::string adCopy = normalizedOptions.associatedData;

        StringCleansingGuard passwordGuard(passwordCopy);
        StringCleansingGuard secretGuard(secretCopy);
        StringCleansingGuard adGuard(adCopy);

        outDerivedBytes.resize(normalizedOptions.hashLength);

        size_t derivedLength = normalizedOptions.hashLength;
        uint32_t earlyClean = 1;

        OSSL_PARAM params[16];
        size_t paramCount = 0;
        constexpr size_t paramsCapacity = sizeof(params) / sizeof(params[0]);

        auto AddParam = [&](const OSSL_PARAM& param) -> bool
            {
                if (paramCount + 1 >= paramsCapacity)
                {
                    return false;
                }

                params[paramCount++] = param;
                return true;
            };

        if (!AddParam(::OSSL_PARAM_construct_uint32(
            OSSL_KDF_PARAM_ARGON2_LANES,
            &normalizedOptions.lanes
        )))
        {
            return false;
        }

        if (!AddParam(::OSSL_PARAM_construct_uint32(
            OSSL_KDF_PARAM_ARGON2_MEMCOST,
            &normalizedOptions.memoryCostKiB
        )))
        {
            return false;
        }

        if (!AddParam(::OSSL_PARAM_construct_uint32(
            OSSL_KDF_PARAM_ITER,
            &normalizedOptions.iterations
        )))
        {
            return false;
        }

        if (useThreadsParam)
        {
            if (!AddParam(::OSSL_PARAM_construct_uint32(
                OSSL_KDF_PARAM_THREADS,
                &threads
            )))
            {
                return false;
            }
        }

        if (!AddParam(::OSSL_PARAM_construct_size_t(
            OSSL_KDF_PARAM_SIZE,
            &derivedLength
        )))
        {
            return false;
        }

        if (!AddParam(::OSSL_PARAM_construct_uint32(
            OSSL_KDF_PARAM_ARGON2_VERSION,
            &normalizedOptions.version
        )))
        {
            return false;
        }

#if defined(OSSL_KDF_PARAM_EARLY_CLEAN)
        if (!AddParam(::OSSL_PARAM_construct_uint32(
            OSSL_KDF_PARAM_EARLY_CLEAN,
            &earlyClean
        )))
        {
            return false;
        }
#else
        (void)earlyClean;
#endif

        if (!AddParam(::OSSL_PARAM_construct_octet_string(
            OSSL_KDF_PARAM_SALT,
            GetOctetStringPointerForOpenSsl(saltCopy),
            saltCopy.size()
        )))
        {
            return false;
        }

        if (!secretCopy.empty())
        {
            if (!AddParam(::OSSL_PARAM_construct_octet_string(
                OSSL_KDF_PARAM_SECRET,
                GetOctetStringPointerForOpenSsl(secretCopy),
                secretCopy.size()
            )))
            {
                return false;
            }
        }

        if (!adCopy.empty())
        {
            if (!AddParam(::OSSL_PARAM_construct_octet_string(
                OSSL_KDF_PARAM_ARGON2_AD,
                GetOctetStringPointerForOpenSsl(adCopy),
                adCopy.size()
            )))
            {
                return false;
            }
        }

        if (!AddParam(::OSSL_PARAM_construct_octet_string(
            OSSL_KDF_PARAM_PASSWORD,
            GetOctetStringPointerForOpenSsl(passwordCopy),
            passwordCopy.size()
        )))
        {
            return false;
        }

        params[paramCount++] = ::OSSL_PARAM_construct_end();

        const int deriveOk = ::EVP_KDF_derive(
            kdfContext.get(),
            reinterpret_cast<unsigned char*>(&outDerivedBytes[0]),
            outDerivedBytes.size(),
            params
        );

        if (deriveOk != 1)
        {
            if (!outDerivedBytes.empty())
            {
                SecureCleanseBuffer(&outDerivedBytes[0], outDerivedBytes.size());
            }
            outDerivedBytes.clear();
            return false;
        }

        return true;
    }

    static bool DeriveArgon2Bytes(const std::string& passwordBytes, const std::string& saltBytes, const GB_Argon2::GB_Argon2Options& options, std::string& outDerivedBytes)
    {
        outDerivedBytes.clear();

        // 先尝试带 threads 参数；若失败（例如 OpenSSL 未启用内置线程支持），再回退到不带 threads 参数。
        if (options.threads != 0)
        {
            if (DeriveArgon2BytesOnce(passwordBytes, saltBytes, options, true, outDerivedBytes))
            {
                return true;
            }

            return DeriveArgon2BytesOnce(passwordBytes, saltBytes, options, false, outDerivedBytes);
        }

        return DeriveArgon2BytesOnce(passwordBytes, saltBytes, options, false, outDerivedBytes);
    }

    static void SplitStringByChar(const std::string& text, char delimiter, std::vector<std::string>& outParts, bool keepEmpty)
    {
        outParts.clear();

        size_t start = 0;
        while (start <= text.size())
        {
            const size_t pos = text.find(delimiter, start);
            const size_t end = (pos == std::string::npos) ? text.size() : pos;
            const size_t length = end - start;

            if (length > 0 || keepEmpty)
            {
                outParts.push_back(text.substr(start, length));
            }

            if (pos == std::string::npos)
            {
                break;
            }

            start = pos + 1;
        }
    }

    static bool TryParseUint32(const std::string& text, uint32_t& outValue)
    {
        outValue = 0;
        if (text.empty())
        {
            return false;
        }

        uint64_t value = 0;
        for (size_t i = 0; i < text.size(); i++)
        {
            const char ch = text[i];
            if (ch < '0' || ch > '9')
            {
                return false;
            }
            value = value * 10ULL + static_cast<uint64_t>(ch - '0');
            if (value > 0xFFFFFFFFULL)
            {
                return false;
            }
        }

        outValue = static_cast<uint32_t>(value);
        return true;
    }

    static bool ParseArgon2EncodedHash(const std::string& encodedHash, GB_Argon2Variant& outVariant, GB_Argon2Options& outOptions, std::string& outSaltBytes, std::string& outHashBytes)
    {
        outSaltBytes.clear();
        outHashBytes.clear();
        outOptions = GB_Argon2Options();

        std::vector<std::string> parts;
        SplitStringByChar(encodedHash, '$', parts, true);

        // 期望："" "argon2id" "v=19" "m=...,t=...,p=..." "saltb64" "hashb64"
        if (parts.size() < 5)
        {
            return false;
        }
        if (!parts.empty() && !parts[0].empty())
        {
            return false;
        }

        const std::string& variantText = parts[1];
        if (variantText == "argon2i")
        {
            outVariant = GB_Argon2Variant::Argon2i;
        }
        else if (variantText == "argon2d")
        {
            outVariant = GB_Argon2Variant::Argon2d;
        }
        else if (variantText == "argon2id")
        {
            outVariant = GB_Argon2Variant::Argon2id;
        }
        else
        {
            return false;
        }

        size_t index = 2;
        uint32_t parsedVersion = 0x13;

        if (index < parts.size() && parts[index].size() >= 2 && parts[index][0] == 'v' && parts[index][1] == '=')
        {
            const std::string versionNumberText = parts[index].substr(2);
            uint32_t versionNumber = 0;
            if (!TryParseUint32(versionNumberText, versionNumber))
            {
                return false;
            }
            parsedVersion = ParseArgon2VersionNumber(static_cast<int>(versionNumber));
            if (parsedVersion == 0)
            {
                return false;
            }
            index++;
        }

        if (index >= parts.size())
        {
            return false;
        }

        // 参数段：m=...,t=...,p=...
        uint32_t parsedMemcost = 0;
        uint32_t parsedIterations = 0;
        uint32_t parsedLanes = 0;

        std::vector<std::string> kvParts;
        SplitStringByChar(parts[index], ',', kvParts, false);

        for (size_t i = 0; i < kvParts.size(); i++)
        {
            const std::string& kv = kvParts[i];
            const size_t equalPos = kv.find('=');
            if (equalPos == std::string::npos)
            {
                return false;
            }
            const std::string key = kv.substr(0, equalPos);
            const std::string valueText = kv.substr(equalPos + 1);
            uint32_t value = 0;
            if (!TryParseUint32(valueText, value))
            {
                return false;
            }

            if (key == "m")
            {
                parsedMemcost = value;
            }
            else if (key == "t")
            {
                parsedIterations = value;
            }
            else if (key == "p")
            {
                parsedLanes = value;
            }
            else
            {
                // 其他参数忽略。
            }
        }

        if (parsedMemcost == 0 || parsedIterations == 0 || parsedLanes == 0)
        {
            return false;
        }

        // Argon2 规范要求 m >= 8 * p（单位 KiB），否则该哈希参数本身就是非法的。
        if (static_cast<uint64_t>(parsedMemcost) < static_cast<uint64_t>(parsedLanes) * 8ULL)
        {
            return false;
        }

        index++;
        if (index + 1 >= parts.size())
        {
            return false;
        }

        const std::string& saltBase64 = parts[index + 0];
        const std::string& hashBase64 = parts[index + 1];

        if (!GB_Base64Decode(saltBase64, outSaltBytes, false, true))
        {
            return false;
        }
        if (!GB_Base64Decode(hashBase64, outHashBytes, false, true))
        {
            return false;
        }

        if (outSaltBytes.empty() || outHashBytes.empty())
        {
            return false;
        }

        outOptions.variant = outVariant;
        outOptions.version = parsedVersion;
        outOptions.memoryCostKiB = parsedMemcost;
        outOptions.iterations = parsedIterations;
        outOptions.lanes = parsedLanes;
        outOptions.hashLength = outHashBytes.size();
        outOptions.saltLength = outSaltBytes.size();
        outOptions.threads = 0;
        outOptions.secret.clear();
        outOptions.associatedData.clear();

        return true;
    }
#endif

    std::string GB_Argon2Hash(const std::string& utf8Text, const GB_Argon2Options& options)
    {
#if OPENSSL_VERSION_NUMBER < 0x30200000L
        (void)utf8Text;
        (void)options;
        return std::string();
#else
        GB_Argon2Options normalizedOptions = options;
        NormalizeArgon2Options(normalizedOptions);

        std::string saltBytes;
        if (!GenerateRandomBytes(normalizedOptions.saltLength, saltBytes))
        {
            return std::string();
        }

        normalizedOptions.saltLength = saltBytes.size();
        return GB_Argon2HashWithSalt(utf8Text, saltBytes, normalizedOptions);
#endif
    }

    std::string GB_Argon2HashWithSalt(const std::string& utf8Text, const std::string& saltBytes, const GB_Argon2Options& options)
    {
#if OPENSSL_VERSION_NUMBER < 0x30200000L
        (void)utf8Text;
        (void)saltBytes;
        (void)options;
        return std::string();
#else
        if (saltBytes.empty())
        {
            return std::string();
        }

        GB_Argon2Options normalizedOptions = options;
        NormalizeArgon2Options(normalizedOptions);
        normalizedOptions.saltLength = saltBytes.size();

        std::string derivedBytes;
        StringCleansingGuard derivedGuard(derivedBytes);
        if (!DeriveArgon2Bytes(utf8Text, saltBytes, normalizedOptions, derivedBytes))
        {
            return std::string();
        }

        const std::string saltBase64 = GB_Base64Encode(saltBytes, false, true);
        const std::string hashBase64 = GB_Base64Encode(derivedBytes, false, true);
        if (saltBase64.empty() || hashBase64.empty())
        {
            return std::string();
        }

        const int versionNumber = GetArgon2VersionNumber(normalizedOptions.version);

        std::string result;
        result.reserve(128 + saltBase64.size() + hashBase64.size());
        result += "$";
        result += GetArgon2EncodedName(normalizedOptions.variant);
        result += "$v=";
        result += std::to_string(versionNumber);
        result += "$m=";
        result += std::to_string(normalizedOptions.memoryCostKiB);
        result += ",t=";
        result += std::to_string(normalizedOptions.iterations);
        result += ",p=";
        result += std::to_string(normalizedOptions.lanes);
        result += "$";
        result += saltBase64;
        result += "$";
        result += hashBase64;
        return result;
#endif
    }

    bool GB_Argon2Verify(const std::string& utf8Text, const std::string& encodedHash)
    {
#if OPENSSL_VERSION_NUMBER < 0x30200000L
        (void)utf8Text;
        (void)encodedHash;
        return false;
#else
        GB_Argon2Variant variant = GB_Argon2Variant::Argon2id;
        GB_Argon2Options options;
        std::string saltBytes;
        std::string expectedHashBytes;

        if (!ParseArgon2EncodedHash(encodedHash, variant, options, saltBytes, expectedHashBytes))
        {
            return false;
        }

        options.variant = variant;
        options.hashLength = expectedHashBytes.size();
        options.saltLength = saltBytes.size();
        options.threads = 0;
        options.secret.clear();
        options.associatedData.clear();

        std::string derivedBytes;
        StringCleansingGuard derivedGuard(derivedBytes);
        if (!DeriveArgon2Bytes(utf8Text, saltBytes, options, derivedBytes))
        {
            return false;
        }

        if (derivedBytes.size() != expectedHashBytes.size())
        {
            return false;
        }

        return ::CRYPTO_memcmp(derivedBytes.data(), expectedHashBytes.data(), derivedBytes.size()) == 0;
#endif
    }
} // namespace GB_Argon2

namespace GB_AES
{
    static void NormalizeAesOptionsForCipher(GB_AesOptions& options)
    {
        // 只对 ECB/CBC 允许 padding；其他模式强制关闭 padding。
        if (options.mode != GB_AesMode::Ecb && options.mode != GB_AesMode::Cbc)
        {
            options.pkcs7Padding = false;
        }

        if (options.mode == GB_AesMode::Gcm)
        {
            if (options.gcmTagLength == 0)
            {
                options.gcmTagLength = 16;
            }
        }
    }

    static size_t GetRecommendedIvLength(GB_AesMode mode)
    {
        switch (mode)
        {
        case GB_AesMode::Ecb:
            return 0;
        case GB_AesMode::Gcm:
            // NIST SP 800-38D 常用推荐：96-bit nonce（12 字节）。
            return 12;
        default:
            // CBC/CFB/OFB/CTR：AES block size 为 16 字节。
            return 16;
        }
    }

    static size_t GetIvLengthForPacked(GB_AesMode mode, size_t requestedIvLength)
    {
        if (mode == GB_AesMode::Ecb)
        {
            return 0;
        }

        if (mode == GB_AesMode::Gcm)
        {
            // GCM 允许非 12 字节 IV，但 12 字节是最常见/推荐的长度。
            if (requestedIvLength == 0)
            {
                return GetRecommendedIvLength(GB_AesMode::Gcm);
            }
            return requestedIvLength;
        }

        // 其它 AES block 模式：IV 长度必须为 16 字节。
        (void)requestedIvLength;
        return 16;
    }

    static bool IsValidAesKeyBits(size_t keyBits)
    {
        return keyBits == 128 || keyBits == 192 || keyBits == 256;
    }

    static size_t GetAesKeyBytesCount(size_t keyBits)
    {
        if (keyBits == 128)
        {
            return 16;
        }
        if (keyBits == 192)
        {
            return 24;
        }
        if (keyBits == 256)
        {
            return 32;
        }
        return 0;
    }

    static bool ValidateAesParams(const std::string& keyBytes, const std::string& ivBytes, const GB_AesOptions& options, size_t& outIvLength)
    {
        outIvLength = options.ivLength;
        if (outIvLength == 0)
        {
            outIvLength = GetRecommendedIvLength(options.mode);
        }

        if (!IsValidAesKeyBits(options.keyBits))
        {
            return false;
        }

        const size_t expectedKeyBytes = GetAesKeyBytesCount(options.keyBits);
        if (expectedKeyBytes == 0 || keyBytes.size() != expectedKeyBytes)
        {
            return false;
        }

        if (options.mode == GB_AesMode::Ecb)
        {
            // ECB 不使用 IV。
            return true;
        }

        if (options.mode == GB_AesMode::Gcm)
        {
            // GCM 允许任意 IV 长度（需要额外设置 IVLEN）。但不能为空。
            if (ivBytes.empty())
            {
                return false;
            }
            return true;
        }

        // CBC/CFB/OFB/CTR：OpenSSL EVP 的 AES 期望 IV=16 字节。
        if (ivBytes.size() != 16)
        {
            return false;
        }

        return true;
    }

    static const EVP_CIPHER* GetAesCipher(GB_AesMode mode, size_t keyBits)
    {
        if (!IsValidAesKeyBits(keyBits))
        {
            return nullptr;
        }

        switch (mode)
        {
        case GB_AesMode::Ecb:
            if (keyBits == 128) return ::EVP_aes_128_ecb();
            if (keyBits == 192) return ::EVP_aes_192_ecb();
            return ::EVP_aes_256_ecb();

        case GB_AesMode::Cbc:
            if (keyBits == 128) return ::EVP_aes_128_cbc();
            if (keyBits == 192) return ::EVP_aes_192_cbc();
            return ::EVP_aes_256_cbc();

        case GB_AesMode::Cfb128:
            if (keyBits == 128) return ::EVP_aes_128_cfb128();
            if (keyBits == 192) return ::EVP_aes_192_cfb128();
            return ::EVP_aes_256_cfb128();

        case GB_AesMode::Ofb:
            if (keyBits == 128) return ::EVP_aes_128_ofb();
            if (keyBits == 192) return ::EVP_aes_192_ofb();
            return ::EVP_aes_256_ofb();

        case GB_AesMode::Ctr:
            if (keyBits == 128) return ::EVP_aes_128_ctr();
            if (keyBits == 192) return ::EVP_aes_192_ctr();
            return ::EVP_aes_256_ctr();

        case GB_AesMode::Gcm:
            if (keyBits == 128) return ::EVP_aes_128_gcm();
            if (keyBits == 192) return ::EVP_aes_192_gcm();
            return ::EVP_aes_256_gcm();

        default:
            return nullptr;
        }
    }

    struct EvpCipherCtxDeleter
    {
        void operator()(EVP_CIPHER_CTX* ctx) const
        {
            if (ctx != nullptr)
            {
                ::EVP_CIPHER_CTX_free(ctx);
            }
        }
    };

    static bool EvpEncryptUpdateInChunks(EVP_CIPHER_CTX* ctx, unsigned char* outBytes, size_t outCapacity, size_t& outWritten, const unsigned char* inBytes, size_t inSize)
    {
        outWritten = 0;

        if (ctx == nullptr)
        {
            return false;
        }

        if (inSize == 0)
        {
            return true;
        }

        constexpr static size_t maxChunkSize = static_cast<size_t>((std::numeric_limits<int>::max)());

        while (inSize > 0)
        {
            const size_t chunkSize = (inSize > maxChunkSize) ? maxChunkSize : inSize;
            int chunkOutLen = 0;

            if (::EVP_EncryptUpdate(ctx, outBytes ? (outBytes + outWritten) : nullptr, &chunkOutLen, inBytes, static_cast<int>(chunkSize)) != 1)
            {
                return false;
            }

            if (outBytes != nullptr)
            {
                if (chunkOutLen < 0)
                {
                    return false;
                }

                size_t newWritten = 0;
                if (!SafeAddSizeT(outWritten, static_cast<size_t>(chunkOutLen), newWritten))
                {
                    return false;
                }
                if (newWritten > outCapacity)
                {
                    return false;
                }
                outWritten = newWritten;
            }

            inBytes += chunkSize;
            inSize -= chunkSize;
        }

        return true;
    }

    static bool EvpDecryptUpdateInChunks(EVP_CIPHER_CTX* ctx, unsigned char* outBytes, size_t outCapacity, size_t& outWritten, const unsigned char* inBytes, size_t inSize)
    {
        outWritten = 0;

        if (ctx == nullptr)
        {
            return false;
        }

        if (inSize == 0)
        {
            return true;
        }

        const size_t maxChunkSize = static_cast<size_t>((std::numeric_limits<int>::max)());

        while (inSize > 0)
        {
            const size_t chunkSize = (inSize > maxChunkSize) ? maxChunkSize : inSize;
            int chunkOutLen = 0;

            if (::EVP_DecryptUpdate(ctx, outBytes ? (outBytes + outWritten) : nullptr, &chunkOutLen, inBytes, static_cast<int>(chunkSize)) != 1)
            {
                return false;
            }

            if (outBytes != nullptr)
            {
                if (chunkOutLen < 0)
                {
                    return false;
                }

                size_t newWritten = 0;
                if (!SafeAddSizeT(outWritten, static_cast<size_t>(chunkOutLen), newWritten))
                {
                    return false;
                }
                if (newWritten > outCapacity)
                {
                    return false;
                }
                outWritten = newWritten;
            }

            inBytes += chunkSize;
            inSize -= chunkSize;
        }

        return true;
    }

    bool GB_AesEncrypt(const std::string& utf8PlainText, const std::string& keyBytes, const std::string& ivBytes, const GB_AesOptions& options, std::string& outCipherBytes, std::string& outGcmTagBytes)
    {
        outCipherBytes.clear();
        outGcmTagBytes.clear();

        GB_AesOptions normalizedOptions = options;
        NormalizeAesOptionsForCipher(normalizedOptions);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        size_t ignoredIvLength = 0;
        if (!ValidateAesParams(keyBytes, ivBytes, normalizedOptions, ignoredIvLength))
        {
            return false;
        }

        const EVP_CIPHER* cipher = GetAesCipher(normalizedOptions.mode, normalizedOptions.keyBits);
        if (cipher == nullptr)
        {
            return false;
        }

        std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter> ctx(::EVP_CIPHER_CTX_new());
        if (ctx.get() == nullptr)
        {
            return false;
        }

        const unsigned char* keyPtr = reinterpret_cast<const unsigned char*>(keyBytes.data());
        const unsigned char* ivPtr = ivBytes.empty() ? nullptr : reinterpret_cast<const unsigned char*>(ivBytes.data());

        if (normalizedOptions.mode == GB_AesMode::Gcm)
        {
            // 1) 初始化 cipher（不设置 key/iv）
            if (::EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr) != 1)
            {
                return false;
            }

            // 2) 设置 IV 长度（默认 12；非 12 时显式设置）
            if (!ivBytes.empty() && ivBytes.size() != GetRecommendedIvLength(GB_AesMode::Gcm))
            {
                if (::EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(ivBytes.size()), nullptr) != 1)
                {
                    return false;
                }
            }

            // 3) 设置 key/iv
            if (::EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, keyPtr, ivPtr) != 1)
            {
                return false;
            }

            // 4) AAD（可选）
            if (!normalizedOptions.aadBytes.empty())
            {
                size_t ignoredAadWritten = 0;
                if (!EvpEncryptUpdateInChunks(
                    ctx.get(),
                    nullptr,
                    0,
                    ignoredAadWritten,
                    reinterpret_cast<const unsigned char*>(normalizedOptions.aadBytes.data()),
                    normalizedOptions.aadBytes.size()))
                {
                    return false;
                }
            }

            // 5) Encrypt
            const int blockSize = ::EVP_CIPHER_block_size(cipher);
            const size_t blockSizeBytes = static_cast<size_t>(std::max(blockSize, 0));
            size_t maxOutSize = 0;
            if (!SafeAddSizeT(utf8PlainText.size(), blockSizeBytes, maxOutSize))
            {
                return false;
            }
            if (maxOutSize == 0)
            {
                maxOutSize = 1;
            }

            outCipherBytes.resize(maxOutSize);

            size_t outLen1 = 0;
            if (!EvpEncryptUpdateInChunks(
                ctx.get(),
                reinterpret_cast<unsigned char*>(&outCipherBytes[0]),
                maxOutSize,
                outLen1,
                utf8PlainText.empty() ? nullptr : reinterpret_cast<const unsigned char*>(utf8PlainText.data()),
                utf8PlainText.size()))
            {
                outCipherBytes.clear();
                return false;
            }

            int outLen2 = 0;
            if (::EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(&outCipherBytes[0]) + outLen1, &outLen2) != 1)
            {
                outCipherBytes.clear();
                return false;
            }

            if (outLen2 < 0)
            {
                outCipherBytes.clear();
                return false;
            }

            size_t finalSize = 0;
            if (!SafeAddSizeT(outLen1, static_cast<size_t>(outLen2), finalSize))
            {
                outCipherBytes.clear();
                return false;
            }
            outCipherBytes.resize(finalSize);

            // 6) Get TAG
            if (normalizedOptions.gcmTagLength == 0 || normalizedOptions.gcmTagLength > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                outCipherBytes.clear();
                return false;
            }

            outGcmTagBytes.resize(normalizedOptions.gcmTagLength);

            if (::EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(normalizedOptions.gcmTagLength), &outGcmTagBytes[0]) != 1)
            {
                outCipherBytes.clear();
                outGcmTagBytes.clear();
                return false;
            }

            return true;
        }

        // 非 GCM：ECB/CBC/CFB/OFB/CTR
        if (::EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, keyPtr, ivPtr) != 1)
        {
            return false;
        }

        // padding：仅 ECB/CBC 允许
        if (::EVP_CIPHER_CTX_set_padding(ctx.get(), normalizedOptions.pkcs7Padding ? 1 : 0) != 1)
        {
            return false;
        }

        const int blockSize = ::EVP_CIPHER_block_size(cipher);
        const size_t blockSizeBytes = static_cast<size_t>(std::max(blockSize, 0));
        size_t maxOutSize = 0;
        if (!SafeAddSizeT(utf8PlainText.size(), blockSizeBytes, maxOutSize))
        {
            return false;
        }
        if (maxOutSize == 0)
        {
            maxOutSize = 1;
        }

        outCipherBytes.resize(maxOutSize);

        size_t outLen1 = 0;
        if (!EvpEncryptUpdateInChunks(
            ctx.get(),
            reinterpret_cast<unsigned char*>(&outCipherBytes[0]),
            maxOutSize,
            outLen1,
            utf8PlainText.empty() ? nullptr : reinterpret_cast<const unsigned char*>(utf8PlainText.data()),
            utf8PlainText.size()))
        {
            outCipherBytes.clear();
            return false;
        }

        int outLen2 = 0;
        if (::EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(&outCipherBytes[0]) + outLen1, &outLen2) != 1)
        {
            outCipherBytes.clear();
            return false;
        }

        if (outLen2 < 0)
        {
            outCipherBytes.clear();
            return false;
        }

        size_t finalSize = 0;
        if (!SafeAddSizeT(outLen1, static_cast<size_t>(outLen2), finalSize))
        {
            outCipherBytes.clear();
            return false;
        }
        outCipherBytes.resize(finalSize);
        return true;
    }

    bool GB_AesDecrypt(const std::string& cipherBytes, const std::string& keyBytes, const std::string& ivBytes, const GB_AesOptions& options, const std::string& gcmTagBytes, std::string& outUtf8PlainText)
    {
        outUtf8PlainText.clear();

        GB_AesOptions normalizedOptions = options;
        NormalizeAesOptionsForCipher(normalizedOptions);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        size_t ignoredIvLength = 0;
        if (!ValidateAesParams(keyBytes, ivBytes, normalizedOptions, ignoredIvLength))
        {
            return false;
        }

        const EVP_CIPHER* cipher = GetAesCipher(normalizedOptions.mode, normalizedOptions.keyBits);
        if (cipher == nullptr)
        {
            return false;
        }

        std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter> ctx(::EVP_CIPHER_CTX_new());
        if (ctx.get() == nullptr)
        {
            return false;
        }

        const unsigned char* keyPtr = reinterpret_cast<const unsigned char*>(keyBytes.data());
        const unsigned char* ivPtr = ivBytes.empty() ? nullptr : reinterpret_cast<const unsigned char*>(ivBytes.data());

        if (normalizedOptions.mode == GB_AesMode::Gcm)
        {
            if (normalizedOptions.gcmTagLength == 0)
            {
                return false;
            }

            if (gcmTagBytes.size() != normalizedOptions.gcmTagLength)
            {
                return false;
            }

            if (normalizedOptions.gcmTagLength > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return false;
            }

            // 1) Init cipher
            if (::EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr) != 1)
            {
                return false;
            }

            // 2) Set IV length if non-default
            if (!ivBytes.empty() && ivBytes.size() != GetRecommendedIvLength(GB_AesMode::Gcm))
            {
                if (::EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(ivBytes.size()), nullptr) != 1)
                {
                    return false;
                }
            }

            // 3) Set key/iv
            if (::EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, keyPtr, ivPtr) != 1)
            {
                return false;
            }

            // 4) AAD
            if (!normalizedOptions.aadBytes.empty())
            {
                size_t ignoredAadWritten = 0;
                if (!EvpDecryptUpdateInChunks(
                    ctx.get(),
                    nullptr,
                    0,
                    ignoredAadWritten,
                    reinterpret_cast<const unsigned char*>(normalizedOptions.aadBytes.data()),
                    normalizedOptions.aadBytes.size()))
                {
                    return false;
                }
            }

            // 5) Decrypt (Update)
            const int blockSize = ::EVP_CIPHER_block_size(cipher);
            const size_t blockSizeBytes = static_cast<size_t>(std::max(blockSize, 0));
            size_t maxOutSize = 0;
            if (!SafeAddSizeT(cipherBytes.size(), blockSizeBytes, maxOutSize))
            {
                return false;
            }
            if (maxOutSize == 0)
            {
                maxOutSize = 1;
            }

            outUtf8PlainText.resize(maxOutSize);

            size_t outLen1 = 0;
            if (!EvpDecryptUpdateInChunks(
                ctx.get(),
                reinterpret_cast<unsigned char*>(&outUtf8PlainText[0]),
                maxOutSize,
                outLen1,
                cipherBytes.empty() ? nullptr : reinterpret_cast<const unsigned char*>(cipherBytes.data()),
                cipherBytes.size()))
            {
                outUtf8PlainText.clear();
                return false;
            }

            // 6) Set expected TAG (before Final)
            if (::EVP_CIPHER_CTX_ctrl(
                ctx.get(),
                EVP_CTRL_GCM_SET_TAG,
                static_cast<int>(gcmTagBytes.size()),
                const_cast<char*>(gcmTagBytes.data())) != 1)
            {
                outUtf8PlainText.clear();
                return false;
            }

            // 7) Final: will verify tag
            int outLen2 = 0;
            const int finalOk = ::EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(&outUtf8PlainText[0]) + outLen1, &outLen2);
            if (finalOk != 1)
            {
                outUtf8PlainText.clear();
                return false;
            }

            if (outLen2 < 0)
            {
                outUtf8PlainText.clear();
                return false;
            }

            size_t finalSize = 0;
            if (!SafeAddSizeT(outLen1, static_cast<size_t>(outLen2), finalSize))
            {
                outUtf8PlainText.clear();
                return false;
            }
            outUtf8PlainText.resize(finalSize);
            return true;
        }

        // 非 GCM
        if (::EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, keyPtr, ivPtr) != 1)
        {
            return false;
        }

        if (::EVP_CIPHER_CTX_set_padding(ctx.get(), normalizedOptions.pkcs7Padding ? 1 : 0) != 1)
        {
            return false;
        }

        const int blockSize = ::EVP_CIPHER_block_size(cipher);
        const size_t blockSizeBytes = static_cast<size_t>(std::max(blockSize, 0));
        size_t maxOutSize = 0;
        if (!SafeAddSizeT(cipherBytes.size(), blockSizeBytes, maxOutSize))
        {
            return false;
        }
        if (maxOutSize == 0)
        {
            maxOutSize = 1;
        }

        outUtf8PlainText.resize(maxOutSize);

        size_t outLen1 = 0;
        if (!EvpDecryptUpdateInChunks(
            ctx.get(),
            reinterpret_cast<unsigned char*>(&outUtf8PlainText[0]),
            maxOutSize,
            outLen1,
            cipherBytes.empty() ? nullptr : reinterpret_cast<const unsigned char*>(cipherBytes.data()),
            cipherBytes.size()))
        {
            outUtf8PlainText.clear();
            return false;
        }

        int outLen2 = 0;
        if (::EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(&outUtf8PlainText[0]) + outLen1, &outLen2) != 1)
        {
            outUtf8PlainText.clear();
            return false;
        }

        if (outLen2 < 0)
        {
            outUtf8PlainText.clear();
            return false;
        }

        size_t finalSize = 0;
        if (!SafeAddSizeT(outLen1, static_cast<size_t>(outLen2), finalSize))
        {
            outUtf8PlainText.clear();
            return false;
        }
        outUtf8PlainText.resize(finalSize);
        return true;
    }

    std::string GB_AesEncryptToBase64(const std::string& utf8PlainText, const std::string& keyBytes, const std::string& ivBytes, const GB_AesOptions& options, std::string& outGcmTagBytes, bool urlSafe, bool noPadding)
    {
        outGcmTagBytes.clear();

        std::string cipherBytes;
        if (!GB_AesEncrypt(utf8PlainText, keyBytes, ivBytes, options, cipherBytes, outGcmTagBytes))
        {
            return std::string();
        }

        return GB_Base64Encode(cipherBytes, urlSafe, noPadding);
    }

    bool GB_AesDecryptFromBase64(const std::string& base64CipherText, const std::string& keyBytes, const std::string& ivBytes, const GB_AesOptions& options, const std::string& gcmTagBytes, std::string& outUtf8PlainText, bool urlSafe, bool noPadding)
    {
        outUtf8PlainText.clear();

        std::string cipherBytes;
        if (!GB_Base64Decode(base64CipherText, cipherBytes, urlSafe, noPadding))
        {
            return false;
        }

        return GB_AesDecrypt(cipherBytes, keyBytes, ivBytes, options, gcmTagBytes, outUtf8PlainText);
    }

    std::string GB_AesEncryptToBase64Packed(const std::string& utf8PlainText, const std::string& keyBytes, const GB_AesOptions& options, bool urlSafe, bool noPadding)
    {
        GB_AesOptions normalizedOptions = options;
        NormalizeAesOptionsForCipher(normalizedOptions);

        const size_t keyBytesCount = GetAesKeyBytesCount(normalizedOptions.keyBits);
        if (keyBytesCount == 0 || keyBytes.size() != keyBytesCount)
        {
            return std::string();
        }

        const size_t ivLength = GetIvLengthForPacked(normalizedOptions.mode, normalizedOptions.ivLength);

        std::string ivBytes;
        if (!GenerateRandomBytes(ivLength, ivBytes))
        {
            return std::string();
        }

        std::string cipherBytes;
        std::string tagBytes;

        if (!GB_AesEncrypt(utf8PlainText, keyBytes, ivBytes, normalizedOptions, cipherBytes, tagBytes))
        {
            return std::string();
        }

        std::string payload;
        payload.reserve(ivBytes.size() + cipherBytes.size() + tagBytes.size());
        payload.append(ivBytes);
        payload.append(cipherBytes);
        payload.append(tagBytes);

        return GB_Base64Encode(payload, urlSafe, noPadding);
    }

    bool GB_AesDecryptFromBase64Packed(const std::string& base64Packed, const std::string& keyBytes, const GB_AesOptions& options, std::string& outUtf8PlainText, bool urlSafe, bool noPadding)
    {
        outUtf8PlainText.clear();

        GB_AesOptions normalizedOptions = options;
        NormalizeAesOptionsForCipher(normalizedOptions);

        const size_t keyBytesCount = GetAesKeyBytesCount(normalizedOptions.keyBits);
        if (keyBytesCount == 0 || keyBytes.size() != keyBytesCount)
        {
            return false;
        }

        std::string payload;
        if (!GB_Base64Decode(base64Packed, payload, urlSafe, noPadding))
        {
            return false;
        }

        const size_t ivLength = GetIvLengthForPacked(normalizedOptions.mode, normalizedOptions.ivLength);

        const size_t tagLength = (normalizedOptions.mode == GB_AesMode::Gcm) ? normalizedOptions.gcmTagLength : 0;

        if (payload.size() < ivLength + tagLength)
        {
            return false;
        }

        const size_t cipherOffset = ivLength;
        const size_t cipherLength = payload.size() - ivLength - tagLength;

        const std::string ivBytes = (ivLength > 0) ? payload.substr(0, ivLength) : std::string();
        const std::string cipherBytes = (cipherLength > 0) ? payload.substr(cipherOffset, cipherLength) : std::string();
        const std::string tagBytes = (tagLength > 0) ? payload.substr(cipherOffset + cipherLength, tagLength) : std::string();

        return GB_AesDecrypt(cipherBytes, keyBytes, ivBytes, normalizedOptions, tagBytes, outUtf8PlainText);
    }

    bool GB_DeriveAesKeyAndIv_Pbkdf2HmacSha256(const std::string& passwordUtf8, const std::string& saltBytes, uint32_t iterations, const GB_AesOptions& options, std::string& outKeyBytes, std::string& outIvBytes)
    {
        outKeyBytes.clear();
        outIvBytes.clear();

        if (iterations < 1)
        {
            return false;
        }

        if (saltBytes.empty())
        {
            return false;
        }

        GB_AesOptions normalizedOptions = options;
        NormalizeAesOptionsForCipher(normalizedOptions);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        const size_t keyBytesCount = GetAesKeyBytesCount(normalizedOptions.keyBits);
        if (keyBytesCount == 0)
        {
            return false;
        }

        const size_t ivLength = GetIvLengthForPacked(normalizedOptions.mode, normalizedOptions.ivLength);

        const size_t totalLength = keyBytesCount + ivLength;
        if (totalLength == 0 || totalLength > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        std::string derivedBytes;
        derivedBytes.resize(totalLength);
        StringCleansingGuard derivedGuard(derivedBytes);

        const int ok = ::PKCS5_PBKDF2_HMAC(
            passwordUtf8.empty() ? "" : passwordUtf8.data(),
            static_cast<int>(passwordUtf8.size()),
            reinterpret_cast<const unsigned char*>(saltBytes.data()),
            static_cast<int>(saltBytes.size()),
            static_cast<int>(iterations),
            ::EVP_sha256(),
            static_cast<int>(derivedBytes.size()),
            reinterpret_cast<unsigned char*>(&derivedBytes[0])
        );

        if (ok != 1)
        {
            return false;
        }

        outKeyBytes = derivedBytes.substr(0, keyBytesCount);
        outIvBytes = (ivLength > 0) ? derivedBytes.substr(keyBytesCount, ivLength) : std::string();

        return true;
    }
}

namespace GB_RSA
{
    namespace
    {
        struct BioDeleter
        {
            void operator()(BIO* bio) const
            {
                if (bio != nullptr)
                {
                    ::BIO_free(bio);
                }
            }
        };

        struct EvpPkeyDeleter
        {
            void operator()(EVP_PKEY* pkey) const
            {
                if (pkey != nullptr)
                {
                    ::EVP_PKEY_free(pkey);
                }
            }
        };

        struct EvpPkeyCtxDeleter
        {
            void operator()(EVP_PKEY_CTX* ctx) const
            {
                if (ctx != nullptr)
                {
                    ::EVP_PKEY_CTX_free(ctx);
                }
            }
        };

        struct RsaDeleter
        {
            void operator()(RSA* rsa) const
            {
                if (rsa != nullptr)
                {
                    ::RSA_free(rsa);
                }
            }
        };

        struct BnDeleter
        {
            void operator()(BIGNUM* bn) const
            {
                if (bn != nullptr)
                {
                    ::BN_free(bn);
                }
            }
        };

        struct CurlDeleter
        {
            void operator()(CURL* handle) const
            {
                if (handle != nullptr)
                {
                    ::curl_easy_cleanup(handle);
                }
            }
        };

        static bool EnsureCurlGlobalInit()
        {
            static std::once_flag initFlag;
            static bool initOk = false;

            std::call_once(initFlag, []() {
                initOk = (::curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
                });

            return initOk;
        }

        static std::string CurlEscapeString(const std::string& input)
        {
            if (!EnsureCurlGlobalInit())
            {
                return std::string();
            }

            std::unique_ptr<CURL, CurlDeleter> handle(::curl_easy_init());
            if (handle.get() == nullptr)
            {
                return std::string();
            }

            if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return std::string();
            }

            char* escaped = ::curl_easy_escape(handle.get(), input.data(), static_cast<int>(input.size()));
            if (escaped == nullptr)
            {
                return std::string();
            }

            std::string result(escaped);
            ::curl_free(escaped);
            return result;
        }

        static bool CurlUnescapeString(const std::string& input, std::string& outResult)
        {
            outResult.clear();

            if (!EnsureCurlGlobalInit())
            {
                return false;
            }

            std::unique_ptr<CURL, CurlDeleter> handle(::curl_easy_init());
            if (handle.get() == nullptr)
            {
                return false;
            }

            if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return false;
            }

            int outLength = 0;
            char* unescaped = ::curl_easy_unescape(handle.get(), input.data(), static_cast<int>(input.size()), &outLength);
            if (unescaped == nullptr)
            {
                return false;
            }

            if (outLength < 0)
            {
                ::curl_free(unescaped);
                return false;
            }

            outResult.assign(unescaped, static_cast<size_t>(outLength));
            ::curl_free(unescaped);
            return true;
        }

        static bool BioToBytes(BIO* bio, std::string& outBytes)
        {
            outBytes.clear();

            if (bio == nullptr)
            {
                return false;
            }

            char* dataPtr = nullptr;
            const long dataLen = ::BIO_get_mem_data(bio, &dataPtr);
            if (dataLen < 0 || dataPtr == nullptr)
            {
                return false;
            }

            if (static_cast<unsigned long long>(dataLen) > static_cast<unsigned long long>((std::numeric_limits<size_t>::max)()))
            {
                return false;
            }

            outBytes.assign(dataPtr, static_cast<size_t>(dataLen));
            return true;
        }

        static std::unique_ptr<BIO, BioDeleter> CreateReadOnlyBioFromBytes(const std::string& bytes)
        {
            if (bytes.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return std::unique_ptr<BIO, BioDeleter>();
            }

            // BIO_new_mem_buf 不会拷贝数据，因此必须保证 bytes 的生命周期覆盖 BIO 使用期。
            // 本函数内部立刻进行解析，不会跨越该作用域。
            return std::unique_ptr<BIO, BioDeleter>(::BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size())));
        }

        static const EVP_MD* GetEvpMd(GB_RsaHashMethod method)
        {
            switch (method)
            {
            case GB_RsaHashMethod::Sha1:
                return ::EVP_sha1();
            case GB_RsaHashMethod::Sha256:
                return ::EVP_sha256();
            case GB_RsaHashMethod::Sha384:
                return ::EVP_sha384();
            case GB_RsaHashMethod::Sha512:
                return ::EVP_sha512();
            default:
                return ::EVP_sha256();
            }
        }

        static bool TryZlibCompress(const std::string& inputBytes, int compressionLevel, std::string& outCompressedBytes)
        {
            outCompressedBytes.clear();

            if (inputBytes.size() > static_cast<size_t>((std::numeric_limits<uLong>::max)()))
            {
                return false;
            }

            const uLong srcLen = static_cast<uLong>(inputBytes.size());
            const uLong bound = ::compressBound(srcLen);
            if (bound == 0)
            {
                return false;
            }

            if (bound > static_cast<uLong>((std::numeric_limits<uLongf>::max)()))
            {
                return false;
            }

            if (compressionLevel < -1 || compressionLevel > 9)
            {
                return false;
            }

            outCompressedBytes.resize(static_cast<size_t>(bound));

            uLongf destLen = static_cast<uLongf>(bound);
            const int zRet = ::compress2(
                reinterpret_cast<Bytef*>(&outCompressedBytes[0]),
                &destLen,
                reinterpret_cast<const Bytef*>(inputBytes.empty() ? "" : inputBytes.data()),
                srcLen,
                compressionLevel);

            if (zRet != Z_OK)
            {
                outCompressedBytes.clear();
                return false;
            }

            if (destLen > static_cast<uLongf>((std::numeric_limits<size_t>::max)()))
            {
                outCompressedBytes.clear();
                return false;
            }

            outCompressedBytes.resize(static_cast<size_t>(destLen));
            return true;
        }

        static bool TryZlibDecompress(const std::string& compressedBytes, uint32_t expectedOutputSize, std::string& outBytes)
        {
            outBytes.clear();

            constexpr uint32_t maxDecompressedSize = 256U * 1024U * 1024U;
            if (expectedOutputSize > maxDecompressedSize)
            {
                return false;
            }

            if (compressedBytes.empty() && expectedOutputSize == 0)
            {
                // zlib 对空数据的压缩结果并非空串；但若遇到边界输入，这里直接视为成功。
                return true;
            }

            z_stream stream;
            std::memset(&stream, 0, sizeof(stream));

            if (::inflateInit(&stream) != Z_OK)
            {
                return false;
            }

            const unsigned char* inputPtr = compressedBytes.empty() ? nullptr : reinterpret_cast<const unsigned char*>(compressedBytes.data());
            size_t remaining = compressedBytes.size();

            if (expectedOutputSize > 0)
            {
                // 仅用于性能优化，避免被异常/恶意输入触发巨额 reserve（潜在 DoS）。
                const size_t reserveSize = std::min<size_t>(static_cast<size_t>(expectedOutputSize), 32U * 1024U * 1024U);
                outBytes.reserve(reserveSize);
            }
            std::array<unsigned char, 16 * 1024> buffer;
            int zRet = Z_OK;
            while (true)
            {
                if (stream.avail_in == 0 && remaining > 0)
                {
                    const size_t chunkSize = std::min(remaining, static_cast<size_t>((std::numeric_limits<uInt>::max)()));
                    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(inputPtr));
                    stream.avail_in = static_cast<uInt>(chunkSize);
                    inputPtr += chunkSize;
                    remaining -= chunkSize;
                }

                stream.next_out = reinterpret_cast<Bytef*>(&buffer[0]);
                stream.avail_out = static_cast<uInt>(buffer.size());

                zRet = ::inflate(&stream, Z_NO_FLUSH);

                if (zRet != Z_OK && zRet != Z_STREAM_END)
                {
                    ::inflateEnd(&stream);
                    outBytes.clear();
                    return false;
                }

                const size_t produced = buffer.size() - static_cast<size_t>(stream.avail_out);
                if (produced > 0)
                {
                    const size_t oldSize = outBytes.size();
                    size_t newSize = 0;
                    if (!SafeAddSizeT(oldSize, produced, newSize))
                    {
                        ::inflateEnd(&stream);
                        outBytes.clear();
                        return false;
                    }
                    outBytes.resize(newSize);
                    std::memcpy(&outBytes[oldSize], &buffer[0], produced);
                }

                if (zRet == Z_STREAM_END)
                {
                    // 严格要求输入正好用尽：不允许在 deflate 流结束后仍残留额外字节（可能是拼接/污染数据）。
                    if (remaining != 0 || stream.avail_in != 0)
                    {
                        ::inflateEnd(&stream);
                        outBytes.clear();
                        return false;
                    }

                    break;
                }

                if (remaining == 0 && stream.avail_in == 0)
                {
                    // 输入耗尽但未结束，说明数据不完整。
                    ::inflateEnd(&stream);
                    outBytes.clear();
                    return false;
                }
            }

            ::inflateEnd(&stream);

            if (outBytes.size() != static_cast<size_t>(expectedOutputSize))
            {
                outBytes.clear();
                return false;
            }

            return true;
        }

        static void WriteUint32Be(uint32_t value, char outBytes[4])
        {
            outBytes[0] = static_cast<char>((value >> 24) & 0xFF);
            outBytes[1] = static_cast<char>((value >> 16) & 0xFF);
            outBytes[2] = static_cast<char>((value >> 8) & 0xFF);
            outBytes[3] = static_cast<char>((value) & 0xFF);
        }

        static uint32_t ReadUint32Be(const unsigned char* bytes)
        {
            return
                (static_cast<uint32_t>(bytes[0]) << 24) |
                (static_cast<uint32_t>(bytes[1]) << 16) |
                (static_cast<uint32_t>(bytes[2]) << 8) |
                (static_cast<uint32_t>(bytes[3]));
        }

        static bool IsLikelyPem(const std::string& keyBytes)
        {
            return keyBytes.find("-----BEGIN") != std::string::npos;
        }

        static std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> LoadPublicKeyFromBytes(const std::string& publicKeyBytes, GB_RsaPublicKeyFormat publicKeyFormat)
        {
            if (publicKeyBytes.empty())
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

            const bool isPem = IsLikelyPem(publicKeyBytes);

            auto tryPemSpki = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(publicKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                EVP_PKEY* pkey = ::PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
                };

            auto tryPemPkcs1 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(publicKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<RSA, RsaDeleter> rsa(::PEM_read_bio_RSAPublicKey(bio.get(), nullptr, nullptr, nullptr));
                if (rsa.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey(::EVP_PKEY_new());
                if (pkey.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                if (::EVP_PKEY_assign_RSA(pkey.get(), rsa.get()) != 1)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                rsa.release();
                return pkey;
                };

            auto tryDerSpki = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(publicKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                EVP_PKEY* pkey = ::d2i_PUBKEY_bio(bio.get(), nullptr);
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
                };

            auto tryDerPkcs1 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(publicKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<RSA, RsaDeleter> rsa(::d2i_RSAPublicKey_bio(bio.get(), nullptr));
                if (rsa.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey(::EVP_PKEY_new());
                if (pkey.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                if (::EVP_PKEY_assign_RSA(pkey.get(), rsa.get()) != 1)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                rsa.release();
                return pkey;
                };

            if (publicKeyFormat == GB_RsaPublicKeyFormat::PemSubjectPublicKeyInfo)
            {
                return tryPemSpki();
            }
            if (publicKeyFormat == GB_RsaPublicKeyFormat::PemPkcs1)
            {
                return tryPemPkcs1();
            }
            if (publicKeyFormat == GB_RsaPublicKeyFormat::DerSubjectPublicKeyInfo)
            {
                return tryDerSpki();
            }
            if (publicKeyFormat == GB_RsaPublicKeyFormat::DerPkcs1)
            {
                return tryDerPkcs1();
            }

            // Auto：优先按文本特征选择 PEM，再退回 DER。
            if (isPem)
            {
                std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey = tryPemSpki();
                if (pkey.get() != nullptr)
                {
                    return pkey;
                }
                pkey = tryPemPkcs1();
                if (pkey.get() != nullptr)
                {
                    return pkey;
                }
            }

            std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey = tryDerSpki();
            if (pkey.get() != nullptr)
            {
                return pkey;
            }
            return tryDerPkcs1();
        }

        static std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> LoadPrivateKeyFromBytes(const std::string& privateKeyBytes, GB_RsaPrivateKeyFormat privateKeyFormat)
        {
            if (privateKeyBytes.empty())
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

            const bool isPem = IsLikelyPem(privateKeyBytes);

            auto tryPemPkcs8 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(privateKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                EVP_PKEY* pkey = ::PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
                };

            auto tryPemPkcs1 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(privateKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<RSA, RsaDeleter> rsa(::PEM_read_bio_RSAPrivateKey(bio.get(), nullptr, nullptr, nullptr));
                if (rsa.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey(::EVP_PKEY_new());
                if (pkey.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                if (::EVP_PKEY_assign_RSA(pkey.get(), rsa.get()) != 1)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                rsa.release();
                return pkey;
                };

            auto tryDerPkcs8 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(privateKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                EVP_PKEY* pkey = ::d2i_PrivateKey_bio(bio.get(), nullptr);
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
                };

            auto tryDerPkcs1 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(privateKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<RSA, RsaDeleter> rsa(::d2i_RSAPrivateKey_bio(bio.get(), nullptr));
                if (rsa.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey(::EVP_PKEY_new());
                if (pkey.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                if (::EVP_PKEY_assign_RSA(pkey.get(), rsa.get()) != 1)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                rsa.release();
                return pkey;
                };

            if (privateKeyFormat == GB_RsaPrivateKeyFormat::PemPkcs8)
            {
                return tryPemPkcs8();
            }
            if (privateKeyFormat == GB_RsaPrivateKeyFormat::PemPkcs1)
            {
                return tryPemPkcs1();
            }
            if (privateKeyFormat == GB_RsaPrivateKeyFormat::DerPkcs8)
            {
                return tryDerPkcs8();
            }
            if (privateKeyFormat == GB_RsaPrivateKeyFormat::DerPkcs1)
            {
                return tryDerPkcs1();
            }

            if (isPem)
            {
                std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey = tryPemPkcs8();
                if (pkey.get() != nullptr)
                {
                    return pkey;
                }
                pkey = tryPemPkcs1();
                if (pkey.get() != nullptr)
                {
                    return pkey;
                }
            }

            std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey = tryDerPkcs8();
            if (pkey.get() != nullptr)
            {
                return pkey;
            }
            return tryDerPkcs1();
        }

        static bool ConfigureRsaEncryptDecryptContext(EVP_PKEY_CTX* ctx, const GB_RsaCryptOptions& options)
        {
            if (ctx == nullptr)
            {
                return false;
            }

            int padding = RSA_PKCS1_OAEP_PADDING;
            switch (options.padding)
            {
            case GB_RsaPaddingMode::Pkcs1V15:
                padding = RSA_PKCS1_PADDING;
                break;
            case GB_RsaPaddingMode::Oaep:
                padding = RSA_PKCS1_OAEP_PADDING;
                break;
            case GB_RsaPaddingMode::NoPadding:
                padding = RSA_NO_PADDING;
                break;
            default:
                padding = RSA_PKCS1_OAEP_PADDING;
                break;
            }

            if (::EVP_PKEY_CTX_set_rsa_padding(ctx, padding) <= 0)
            {
                return false;
            }

            if (options.padding == GB_RsaPaddingMode::Oaep)
            {
                const EVP_MD* oaepMd = GetEvpMd(options.oaepHash);
                const EVP_MD* mgf1Md = GetEvpMd(options.mgf1Hash);
                if (oaepMd == nullptr || mgf1Md == nullptr)
                {
                    return false;
                }

                if (::EVP_PKEY_CTX_set_rsa_oaep_md(ctx, oaepMd) <= 0)
                {
                    return false;
                }
                if (::EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, mgf1Md) <= 0)
                {
                    return false;
                }

                if (!options.oaepLabelBytes.empty())
                {
                    if (options.oaepLabelBytes.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
                    {
                        return false;
                    }

                    unsigned char* label = reinterpret_cast<unsigned char*>(::OPENSSL_malloc(options.oaepLabelBytes.size()));
                    if (label == nullptr)
                    {
                        return false;
                    }

                    std::memcpy(label, options.oaepLabelBytes.data(), options.oaepLabelBytes.size());

                    // set0：ctx 接管内存。
                    if (::EVP_PKEY_CTX_set0_rsa_oaep_label(ctx, label, static_cast<int>(options.oaepLabelBytes.size())) <= 0)
                    {
                        ::OPENSSL_free(label);
                        return false;
                    }
                }
            }

            return true;
        }

        static bool ComputeMaxPlaintextBlockSize(EVP_PKEY* rsaKey, const GB_RsaCryptOptions& options, size_t& outMaxPlainBlockSize)
        {
            outMaxPlainBlockSize = 0;

            if (rsaKey == nullptr)
            {
                return false;
            }

            const int keyBytesInt = ::EVP_PKEY_size(rsaKey);
            if (keyBytesInt <= 0)
            {
                return false;
            }
            const size_t keyBytes = static_cast<size_t>(keyBytesInt);

            if (options.padding == GB_RsaPaddingMode::Pkcs1V15)
            {
                if (keyBytes <= 11)
                {
                    return false;
                }
                outMaxPlainBlockSize = keyBytes - 11;
                return true;
            }

            if (options.padding == GB_RsaPaddingMode::Oaep)
            {
                const EVP_MD* oaepMd = GetEvpMd(options.oaepHash);
                if (oaepMd == nullptr)
                {
                    return false;
                }
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
                const int hLenInt = ::EVP_MD_get_size(oaepMd);
#else
                const int hLenInt = ::EVP_MD_size(oaepMd);
#endif
                if (hLenInt <= 0)
                {
                    return false;
                }
                const size_t hLen = static_cast<size_t>(hLenInt);

                size_t needed = 0;
                if (!SafeMulSizeT(hLen, 2, needed))
                {
                    return false;
                }
                if (!SafeAddSizeT(needed, 2, needed))
                {
                    return false;
                }
                if (keyBytes <= needed)
                {
                    return false;
                }
                outMaxPlainBlockSize = keyBytes - needed;
                return true;
            }

            if (options.padding == GB_RsaPaddingMode::NoPadding)
            {
                // 为避免歧义，这里要求每块输入长度必须 == keyBytes。
                outMaxPlainBlockSize = keyBytes;
                return true;
            }

            return false;
        }

        static bool BuildRsaPayload(const std::string& utf8PlainText, const GB_RsaCryptOptions& options, std::string& outPayloadBytes)
        {
            outPayloadBytes.clear();

            if (utf8PlainText.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
            {
                return false;
            }

            const uint32_t originalSize = static_cast<uint32_t>(utf8PlainText.size());
            bool useCompressed = false;
            std::string bodyBytes = utf8PlainText;

            if (options.zlibCompress)
            {
                std::string compressedBytes;
                if (!TryZlibCompress(utf8PlainText, options.zlibCompressionLevel, compressedBytes))
                {
                    return false;
                }

                if (compressedBytes.size() < utf8PlainText.size())
                {
                    useCompressed = true;
                    bodyBytes.swap(compressedBytes);
                }
            }

            // payload = "GBR1" || flags(1) || originalSize(4be) || body
            const size_t headerSize = 4 + 1 + 4;
            size_t totalSize = 0;
            if (!SafeAddSizeT(headerSize, bodyBytes.size(), totalSize))
            {
                return false;
            }

            outPayloadBytes.reserve(totalSize);
            outPayloadBytes.append("GBR1", 4);
            outPayloadBytes.push_back(useCompressed ? static_cast<char>(0x01) : static_cast<char>(0x00));

            char sizeBytes[4];
            WriteUint32Be(originalSize, sizeBytes);
            outPayloadBytes.append(sizeBytes, 4);

            if (!bodyBytes.empty())
            {
                outPayloadBytes.append(bodyBytes);
            }

            return true;
        }

        static bool ParseRsaPayload(const std::string& payloadBytes, const GB_RsaCryptOptions& options, std::string& outUtf8PlainText)
        {
            (void)options;
            outUtf8PlainText.clear();

            const size_t headerSize = 4 + 1 + 4;
            if (payloadBytes.size() < headerSize)
            {
                return false;
            }

            if (payloadBytes.compare(0, 4, "GBR1") != 0)
            {
                return false;
            }

            const unsigned char flags = static_cast<unsigned char>(payloadBytes[4]);
            const bool isCompressed = (flags & 0x01U) != 0;

            const unsigned char* sizePtr = reinterpret_cast<const unsigned char*>(payloadBytes.data() + 5);
            const uint32_t originalSize = ReadUint32Be(sizePtr);

            const std::string bodyBytes = payloadBytes.substr(headerSize);

            if (!isCompressed)
            {
                if (bodyBytes.size() != static_cast<size_t>(originalSize))
                {
                    return false;
                }
                outUtf8PlainText = bodyBytes;
                return true;
            }

            // isCompressed
            if (originalSize == 0)
            {
                // 压缩数据解压后应为空。
                std::string decompressed;
                if (!TryZlibDecompress(bodyBytes, 0, decompressed))
                {
                    return false;
                }
                outUtf8PlainText.clear();
                return true;
            }

            std::string decompressed;
            if (!TryZlibDecompress(bodyBytes, originalSize, decompressed))
            {
                return false;
            }

            outUtf8PlainText.swap(decompressed);
            return true;
        }

    } // anonymous namespace

    bool GB_GenerateRsaKeyPair(const GB_RsaKeyGenOptions& options, std::string& outPublicKeyBytes, std::string& outPrivateKeyBytes)
    {
        outPublicKeyBytes.clear();
        outPrivateKeyBytes.clear();

        if (options.keyBits < 1024)
        {
            return false;
        }

        if ((options.publicExponent & 1U) == 0U || options.publicExponent < 3)
        {
            return false;
        }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter> ctx(::EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
        if (ctx.get() == nullptr)
        {
            return false;
        }

        if (::EVP_PKEY_keygen_init(ctx.get()) != 1)
        {
            return false;
        }

        if (::EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), static_cast<int>(options.keyBits)) <= 0)
        {
            return false;
        }

        std::unique_ptr<BIGNUM, BnDeleter> exponent(::BN_new());
        if (exponent.get() == nullptr)
        {
            return false;
        }

        if (::BN_set_word(exponent.get(), static_cast<BN_ULONG>(options.publicExponent)) != 1)
        {
            return false;
        }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        if (::EVP_PKEY_CTX_set1_rsa_keygen_pubexp(ctx.get(), exponent.get()) <= 0)
        {
            return false;
        }
#else
        if (::EVP_PKEY_CTX_set_rsa_keygen_pubexp(ctx.get(), exponent.get()) <= 0)
        {
            return false;
        }

        // OpenSSL 1.1.x 的 set_rsa_keygen_pubexp 可能接管 BIGNUM 的生命周期；避免 double free。
        exponent.release();
#endif

        EVP_PKEY* rawKey = nullptr;
        if (::EVP_PKEY_keygen(ctx.get(), &rawKey) != 1)
        {
            return false;
        }

        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> keyPair(rawKey);
        if (keyPair.get() == nullptr)
        {
            return false;
        }

        // --- Export public key ---
        {
            std::unique_ptr<BIO, BioDeleter> bio(::BIO_new(::BIO_s_mem()));
            if (bio.get() == nullptr)
            {
                return false;
            }

            bool writeOk = false;
            switch (options.publicKeyFormat)
            {
            case GB_RsaPublicKeyFormat::PemSubjectPublicKeyInfo:
                writeOk = (::PEM_write_bio_PUBKEY(bio.get(), keyPair.get()) == 1);
                break;
            case GB_RsaPublicKeyFormat::PemPkcs1:
            {
                RSA* rsa = ::EVP_PKEY_get1_RSA(keyPair.get());
                std::unique_ptr<RSA, RsaDeleter> rsaPtr(rsa);
                writeOk = (rsaPtr.get() != nullptr) && (::PEM_write_bio_RSAPublicKey(bio.get(), rsaPtr.get()) == 1);
                break;
            }
            case GB_RsaPublicKeyFormat::DerSubjectPublicKeyInfo:
                writeOk = (::i2d_PUBKEY_bio(bio.get(), keyPair.get()) == 1);
                break;
            case GB_RsaPublicKeyFormat::DerPkcs1:
            {
                RSA* rsa = ::EVP_PKEY_get1_RSA(keyPair.get());
                std::unique_ptr<RSA, RsaDeleter> rsaPtr(rsa);
                writeOk = (rsaPtr.get() != nullptr) && (::i2d_RSAPublicKey_bio(bio.get(), rsaPtr.get()) == 1);
                break;
            }
            default:
                writeOk = (::PEM_write_bio_PUBKEY(bio.get(), keyPair.get()) == 1);
                break;
            }

            if (!writeOk)
            {
                return false;
            }

            if (!BioToBytes(bio.get(), outPublicKeyBytes))
            {
                return false;
            }
        }

        // --- Export private key ---
        {
            std::unique_ptr<BIO, BioDeleter> bio(::BIO_new(::BIO_s_mem()));
            if (bio.get() == nullptr)
            {
                outPublicKeyBytes.clear();
                return false;
            }

            bool writeOk = false;
            switch (options.privateKeyFormat)
            {
            case GB_RsaPrivateKeyFormat::PemPkcs8:
                writeOk = (::PEM_write_bio_PKCS8PrivateKey(bio.get(), keyPair.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
                break;
            case GB_RsaPrivateKeyFormat::PemPkcs1:
            {
                RSA* rsa = ::EVP_PKEY_get1_RSA(keyPair.get());
                std::unique_ptr<RSA, RsaDeleter> rsaPtr(rsa);
                writeOk = (rsaPtr.get() != nullptr) && (::PEM_write_bio_RSAPrivateKey(bio.get(), rsaPtr.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
                break;
            }
            case GB_RsaPrivateKeyFormat::DerPkcs8:
                writeOk = (::i2d_PKCS8PrivateKey_bio(bio.get(), keyPair.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
                break;
            case GB_RsaPrivateKeyFormat::DerPkcs1:
            {
                RSA* rsa = ::EVP_PKEY_get1_RSA(keyPair.get());
                std::unique_ptr<RSA, RsaDeleter> rsaPtr(rsa);
                writeOk = (rsaPtr.get() != nullptr) && (::i2d_RSAPrivateKey_bio(bio.get(), rsaPtr.get()) == 1);
                break;
            }
            default:
                writeOk = (::PEM_write_bio_PKCS8PrivateKey(bio.get(), keyPair.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
                break;
            }

            if (!writeOk)
            {
                outPublicKeyBytes.clear();
                return false;
            }

            if (!BioToBytes(bio.get(), outPrivateKeyBytes))
            {
                outPublicKeyBytes.clear();
                outPrivateKeyBytes.clear();
                return false;
            }
        }

        return true;
    }

    bool GB_RsaEncrypt(const std::string& utf8PlainText, const std::string& publicKeyBytes, GB_RsaPublicKeyFormat publicKeyFormat, const GB_RsaCryptOptions& options, std::string& outCipherBytes)
    {
        outCipherBytes.clear();

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> publicKey = LoadPublicKeyFromBytes(publicKeyBytes, publicKeyFormat);
        if (publicKey.get() == nullptr)
        {
            return false;
        }

        const int keyBytesInt = ::EVP_PKEY_size(publicKey.get());
        if (keyBytesInt <= 0)
        {
            return false;
        }
        const size_t keyBytes = static_cast<size_t>(keyBytesInt);

        std::string payloadBytes;
        if (!BuildRsaPayload(utf8PlainText, options, payloadBytes))
        {
            return false;
        }

        StringCleansingGuard payloadGuard(payloadBytes);

        size_t maxPlainBlockSize = 0;
        if (!ComputeMaxPlaintextBlockSize(publicKey.get(), options, maxPlainBlockSize))
        {
            return false;
        }
        if (maxPlainBlockSize == 0)
        {
            return false;
        }

        if (!payloadBytes.empty() && (payloadBytes.size() % maxPlainBlockSize != 0) && options.padding == GB_RsaPaddingMode::NoPadding)
        {
            // NoPadding 需要每块输入长度严格等于 keyBytes，本库此处不做自动补齐。
            return false;
        }

        // 分块 RSA：密文每块固定为 keyBytes。
        size_t blocks = 0;
        if (!payloadBytes.empty())
        {
            blocks = (payloadBytes.size() + maxPlainBlockSize - 1) / maxPlainBlockSize;
        }

        size_t reserveBytes = 0;
        if (!SafeMulSizeT(blocks, keyBytes, reserveBytes))
        {
            return false;
        }
        outCipherBytes.reserve(reserveBytes);

        std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter> ctx(::EVP_PKEY_CTX_new(publicKey.get(), nullptr));
        if (ctx.get() == nullptr)
        {
            return false;
        }

        if (::EVP_PKEY_encrypt_init(ctx.get()) != 1)
        {
            return false;
        }

        if (!ConfigureRsaEncryptDecryptContext(ctx.get(), options))
        {
            return false;
        }

        const unsigned char* inputPtr = payloadBytes.empty() ? nullptr : reinterpret_cast<const unsigned char*>(payloadBytes.data());
        size_t remaining = payloadBytes.size();

        while (remaining > 0)
        {
            const size_t chunkSize = std::min(remaining, maxPlainBlockSize);

            const size_t oldSize = outCipherBytes.size();
            size_t newSize = 0;
            if (!SafeAddSizeT(oldSize, keyBytes, newSize))
            {
                outCipherBytes.clear();
                return false;
            }
            outCipherBytes.resize(newSize);

            size_t outLen = keyBytes;
            if (::EVP_PKEY_encrypt(ctx.get(), reinterpret_cast<unsigned char*>(&outCipherBytes[oldSize]), &outLen, inputPtr, chunkSize) != 1)
            {
                outCipherBytes.clear();
                return false;
            }

            // RSA 输出应固定等于 keyBytes。
            if (outLen != keyBytes)
            {
                outCipherBytes.clear();
                return false;
            }

            inputPtr += chunkSize;
            remaining -= chunkSize;
        }

        return true;
    }

    bool GB_RsaDecrypt(const std::string& cipherBytes, const std::string& privateKeyBytes, GB_RsaPrivateKeyFormat privateKeyFormat, const GB_RsaCryptOptions& options, std::string& outUtf8PlainText)
    {
        outUtf8PlainText.clear();

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> privateKey = LoadPrivateKeyFromBytes(privateKeyBytes, privateKeyFormat);
        if (privateKey.get() == nullptr)
        {
            return false;
        }

        const int keyBytesInt = ::EVP_PKEY_size(privateKey.get());
        if (keyBytesInt <= 0)
        {
            return false;
        }
        const size_t keyBytes = static_cast<size_t>(keyBytesInt);

        if (!cipherBytes.empty() && (cipherBytes.size() % keyBytes != 0))
        {
            return false;
        }

        std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter> ctx(::EVP_PKEY_CTX_new(privateKey.get(), nullptr));
        if (ctx.get() == nullptr)
        {
            return false;
        }

        if (::EVP_PKEY_decrypt_init(ctx.get()) != 1)
        {
            return false;
        }

        if (!ConfigureRsaEncryptDecryptContext(ctx.get(), options))
        {
            return false;
        }

        const unsigned char* inputPtr = cipherBytes.empty() ? nullptr : reinterpret_cast<const unsigned char*>(cipherBytes.data());
        size_t remaining = cipherBytes.size();

        std::string payloadBytes;
        StringCleansingGuard payloadGuard(payloadBytes);
        payloadBytes.reserve(cipherBytes.size());

        while (remaining > 0)
        {
            const size_t oldSize = payloadBytes.size();
            size_t newSize = 0;
            if (!SafeAddSizeT(oldSize, keyBytes, newSize))
            {
                return false;
            }
            payloadBytes.resize(newSize);

            size_t outLen = keyBytes;
            if (::EVP_PKEY_decrypt(ctx.get(), reinterpret_cast<unsigned char*>(&payloadBytes[oldSize]), &outLen, inputPtr, keyBytes) != 1)
            {
                return false;
            }

            if (outLen > keyBytes)
            {
                return false;
            }
            payloadBytes.resize(oldSize + outLen);

            inputPtr += keyBytes;
            remaining -= keyBytes;
        }

        return ParseRsaPayload(payloadBytes, options, outUtf8PlainText);
    }

    std::string GB_RsaEncryptToBase64(const std::string& utf8PlainText, const std::string& publicKeyBytes, GB_RsaPublicKeyFormat publicKeyFormat, const GB_RsaCryptOptions& options, bool urlSafe, bool noPadding, bool urlEscape)
    {
        std::string cipherBytes;
        if (!GB_RsaEncrypt(utf8PlainText, publicKeyBytes, publicKeyFormat, options, cipherBytes))
        {
            return std::string();
        }

        std::string base64Text = GB_Base64Encode(cipherBytes, urlSafe, noPadding);
        if (base64Text.empty() && !cipherBytes.empty())
        {
            return std::string();
        }

        if (urlEscape)
        {
            std::string escaped = CurlEscapeString(base64Text);
            if (escaped.empty() && !base64Text.empty())
            {
                return std::string();
            }
            return escaped;
        }

        return base64Text;
    }

    bool GB_RsaDecryptFromBase64(const std::string& base64CipherText, const std::string& privateKeyBytes, GB_RsaPrivateKeyFormat privateKeyFormat, const GB_RsaCryptOptions& options, std::string& outUtf8PlainText, bool urlSafe, bool noPadding, bool urlEscaped)
    {
        outUtf8PlainText.clear();

        std::string base64Text = base64CipherText;
        if (urlEscaped)
        {
            if (!CurlUnescapeString(base64CipherText, base64Text))
            {
                return false;
            }
        }

        std::string cipherBytes;
        if (!GB_Base64Decode(base64Text, cipherBytes, urlSafe, noPadding))
        {
            return false;
        }

        return GB_RsaDecrypt(cipherBytes, privateKeyBytes, privateKeyFormat, options, outUtf8PlainText);
    }
}

namespace GB_ECC
{
    namespace
    {
        struct BioDeleter
        {
            void operator()(BIO* bio) const
            {
                if (bio != nullptr)
                {
                    ::BIO_free(bio);
                }
            }
        };

        struct EvpPkeyDeleter
        {
            void operator()(EVP_PKEY* pkey) const
            {
                if (pkey != nullptr)
                {
                    ::EVP_PKEY_free(pkey);
                }
            }
        };

        struct EvpPkeyCtxDeleter
        {
            void operator()(EVP_PKEY_CTX* ctx) const
            {
                if (ctx != nullptr)
                {
                    ::EVP_PKEY_CTX_free(ctx);
                }
            }
        };

        struct EcKeyDeleter
        {
            void operator()(EC_KEY* ecKey) const
            {
                if (ecKey != nullptr)
                {
                    ::EC_KEY_free(ecKey);
                }
            }
        };

        static bool BioToBytes(BIO* bio, std::string& outBytes)
        {
            outBytes.clear();

            if (bio == nullptr)
            {
                return false;
            }

            char* dataPtr = nullptr;
            const long dataLen = ::BIO_get_mem_data(bio, &dataPtr);
            if (dataLen < 0 || dataPtr == nullptr)
            {
                return false;
            }

            outBytes.assign(dataPtr, static_cast<size_t>(dataLen));
            return true;
        }

        static std::unique_ptr<BIO, BioDeleter> CreateReadOnlyBioFromBytes(const std::string& bytes)
        {
            if (bytes.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return std::unique_ptr<BIO, BioDeleter>();
            }

            // BIO_new_mem_buf 不会拷贝数据，因此必须保证 bytes 的生命周期覆盖 BIO 使用期。
            // 本函数内部立刻进行解析，不会跨越该作用域。
            return std::unique_ptr<BIO, BioDeleter>(::BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size())));
        }

        static bool IsLikelyPem(const std::string& keyBytes)
        {
            return keyBytes.find("-----BEGIN") != std::string::npos;
        }

        static void WriteUint16Be(uint16_t value, char outBytes[2])
        {
            outBytes[0] = static_cast<char>((value >> 8) & 0xFF);
            outBytes[1] = static_cast<char>((value) & 0xFF);
        }

        static void WriteUint32Be(uint32_t value, char outBytes[4])
        {
            outBytes[0] = static_cast<char>((value >> 24) & 0xFF);
            outBytes[1] = static_cast<char>((value >> 16) & 0xFF);
            outBytes[2] = static_cast<char>((value >> 8) & 0xFF);
            outBytes[3] = static_cast<char>((value) & 0xFF);
        }

        static uint16_t ReadUint16Be(const unsigned char* bytes)
        {
            return static_cast<uint16_t>(
                (static_cast<uint16_t>(bytes[0]) << 8) |
                (static_cast<uint16_t>(bytes[1])));
        }

        static uint32_t ReadUint32Be(const unsigned char* bytes)
        {
            return
                (static_cast<uint32_t>(bytes[0]) << 24) |
                (static_cast<uint32_t>(bytes[1]) << 16) |
                (static_cast<uint32_t>(bytes[2]) << 8) |
                (static_cast<uint32_t>(bytes[3]));
        }

        static int GetCurveNidFromEvpPkey(EVP_PKEY* key)
        {
            if (key == nullptr)
            {
                return 0;
            }

            if (::EVP_PKEY_base_id(key) != EVP_PKEY_EC)
            {
                return 0;
            }

            std::unique_ptr<EC_KEY, EcKeyDeleter> ecKey(::EVP_PKEY_get1_EC_KEY(key));
            if (ecKey.get() == nullptr)
            {
                return 0;
            }

            const EC_GROUP* group = ::EC_KEY_get0_group(ecKey.get());
            if (group == nullptr)
            {
                return 0;
            }

            return ::EC_GROUP_get_curve_name(group);
        }

        static int GetCurveNidFromCurve(GB_EccCurve curve)
        {
            switch (curve)
            {
            case GB_EccCurve::P256:
                return NID_X9_62_prime256v1;
            case GB_EccCurve::P384:
                return NID_secp384r1;
            case GB_EccCurve::P521:
                return NID_secp521r1;
            case GB_EccCurve::Secp256k1:
                return NID_secp256k1;
            default:
                return NID_X9_62_prime256v1;
            }
        }

        static std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> GenerateEcKeyPairByCurveNid(int curveNid)
        {
            if (curveNid == 0)
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
            EnsureOpenSslProvidersLoaded();
#endif

            std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter> ctx(::EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
            if (ctx.get() == nullptr)
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

            if (::EVP_PKEY_keygen_init(ctx.get()) != 1)
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

            if (::EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), curveNid) <= 0)
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
            // 使用命名曲线表示，跨语言兼容性更好。
            if (::EVP_PKEY_CTX_set_ec_param_enc(ctx.get(), OPENSSL_EC_NAMED_CURVE) <= 0)
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }
#endif

            EVP_PKEY* rawKey = nullptr;
            if (::EVP_PKEY_keygen(ctx.get(), &rawKey) != 1)
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

            return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(rawKey);
        }

        static bool ExportPublicKeyToDerSpki(EVP_PKEY* key, std::string& outDerBytes)
        {
            outDerBytes.clear();

            if (key == nullptr)
            {
                return false;
            }

            std::unique_ptr<BIO, BioDeleter> bio(::BIO_new(::BIO_s_mem()));
            if (bio.get() == nullptr)
            {
                return false;
            }

            if (::i2d_PUBKEY_bio(bio.get(), key) != 1)
            {
                return false;
            }

            return BioToBytes(bio.get(), outDerBytes);
        }

        static std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> LoadPublicKeyFromBytes(const std::string& publicKeyBytes, GB_EccPublicKeyFormat publicKeyFormat)
        {
            if (publicKeyBytes.empty())
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

            const bool isPem = IsLikelyPem(publicKeyBytes);

            auto tryPemSpki = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(publicKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                EVP_PKEY* pkey = ::PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
                };

            auto tryDerSpki = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(publicKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                EVP_PKEY* pkey = ::d2i_PUBKEY_bio(bio.get(), nullptr);
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
                };

            std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey;

            if (publicKeyFormat == GB_EccPublicKeyFormat::PemSubjectPublicKeyInfo)
            {
                pkey = tryPemSpki();
            }
            else if (publicKeyFormat == GB_EccPublicKeyFormat::DerSubjectPublicKeyInfo)
            {
                pkey = tryDerSpki();
            }
            else
            {
                if (isPem)
                {
                    pkey = tryPemSpki();
                    if (pkey.get() == nullptr)
                    {
                        pkey = tryDerSpki();
                    }
                }
                else
                {
                    pkey = tryDerSpki();
                    if (pkey.get() == nullptr)
                    {
                        pkey = tryPemSpki();
                    }
                }
            }

            if (pkey.get() == nullptr)
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

            if (::EVP_PKEY_base_id(pkey.get()) != EVP_PKEY_EC)
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

            return pkey;
        }

        static std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> LoadPrivateKeyFromBytes(const std::string& privateKeyBytes, GB_EccPrivateKeyFormat privateKeyFormat)
        {
            if (privateKeyBytes.empty())
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

            const bool isPem = IsLikelyPem(privateKeyBytes);

            auto tryPemPkcs8 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(privateKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                EVP_PKEY* pkey = ::PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
                };

            auto tryPemSec1 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(privateKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<EC_KEY, EcKeyDeleter> ecKey(::PEM_read_bio_ECPrivateKey(bio.get(), nullptr, nullptr, nullptr));
                if (ecKey.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey(::EVP_PKEY_new());
                if (pkey.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                if (::EVP_PKEY_assign_EC_KEY(pkey.get(), ecKey.get()) != 1)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                ecKey.release();
                return pkey;
                };

            auto tryDerPkcs8 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(privateKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                EVP_PKEY* pkey = ::d2i_PrivateKey_bio(bio.get(), nullptr);
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
                };

            auto tryDerSec1 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
                std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(privateKeyBytes);
                if (bio.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<EC_KEY, EcKeyDeleter> ecKey(::d2i_ECPrivateKey_bio(bio.get(), nullptr));
                if (ecKey.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey(::EVP_PKEY_new());
                if (pkey.get() == nullptr)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                if (::EVP_PKEY_assign_EC_KEY(pkey.get(), ecKey.get()) != 1)
                {
                    return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
                }
                ecKey.release();
                return pkey;
                };

            std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey;

            if (privateKeyFormat == GB_EccPrivateKeyFormat::PemPkcs8)
            {
                pkey = tryPemPkcs8();
            }
            else if (privateKeyFormat == GB_EccPrivateKeyFormat::PemSec1)
            {
                pkey = tryPemSec1();
            }
            else if (privateKeyFormat == GB_EccPrivateKeyFormat::DerPkcs8)
            {
                pkey = tryDerPkcs8();
            }
            else if (privateKeyFormat == GB_EccPrivateKeyFormat::DerSec1)
            {
                pkey = tryDerSec1();
            }
            else
            {
                if (isPem)
                {
                    pkey = tryPemPkcs8();
                    if (pkey.get() == nullptr)
                    {
                        pkey = tryPemSec1();
                    }
                }
                else
                {
                    pkey = tryDerPkcs8();
                    if (pkey.get() == nullptr)
                    {
                        pkey = tryDerSec1();
                    }
                }
            }

            if (pkey.get() == nullptr)
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

            if (::EVP_PKEY_base_id(pkey.get()) != EVP_PKEY_EC)
            {
                return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
            }

            return pkey;
        }

        static bool DeriveSharedSecretEcdh(EVP_PKEY* privateKey, EVP_PKEY* peerPublicKey, std::string& outSharedSecret)
        {
            outSharedSecret.clear();

            if (privateKey == nullptr || peerPublicKey == nullptr)
            {
                return false;
            }

            std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter> ctx(::EVP_PKEY_CTX_new(privateKey, nullptr));
            if (ctx.get() == nullptr)
            {
                return false;
            }

            if (::EVP_PKEY_derive_init(ctx.get()) != 1)
            {
                return false;
            }

            if (::EVP_PKEY_derive_set_peer(ctx.get(), peerPublicKey) != 1)
            {
                return false;
            }

            size_t secretLen = 0;
            if (::EVP_PKEY_derive(ctx.get(), nullptr, &secretLen) != 1)
            {
                return false;
            }

            if (secretLen == 0 || secretLen > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                return false;
            }

            outSharedSecret.resize(secretLen);
            if (::EVP_PKEY_derive(ctx.get(), reinterpret_cast<unsigned char*>(&outSharedSecret[0]), &secretLen) != 1)
            {
                outSharedSecret.clear();
                return false;
            }

            outSharedSecret.resize(secretLen);
            return true;
        }

        static bool HkdfSha256DeriveBytes(const std::string& ikmBytes, const std::string& saltBytes, const std::string& infoBytes, size_t outLength, std::string& outKeyBytes)
        {
            outKeyBytes.clear();

            if (outLength == 0)
            {
                return true;
            }

            if (outLength > 1024U * 1024U)
            {
                // 明显不合理的长度，避免被滥用。
                return false;
            }

            constexpr size_t hashLen = 32;

#if OPENSSL_VERSION_NUMBER >= 0x10101000L
            // 优先使用 OpenSSL 内置 HKDF（EVP_PKEY_HKDF）。文档说明其参数设置方式与 EVP_PKEY_derive 用法。
            std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter> pctx(::EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr));
            if (pctx.get() == nullptr)
            {
                return false;
            }

            if (::EVP_PKEY_derive_init(pctx.get()) != 1)
            {
                return false;
            }

            if (::EVP_PKEY_CTX_set_hkdf_md(pctx.get(), ::EVP_sha256()) != 1)
            {
                return false;
            }

            if (!saltBytes.empty())
            {
                if (::EVP_PKEY_CTX_set1_hkdf_salt(pctx.get(), reinterpret_cast<const unsigned char*>(saltBytes.data()), static_cast<int>(saltBytes.size())) != 1)
                {
                    return false;
                }
            }
            else
            {
                // HKDF 允许空 salt（等价于全 0 salt），这里显式传空更符合直觉。
                if (::EVP_PKEY_CTX_set1_hkdf_salt(pctx.get(), nullptr, 0) != 1)
                {
                    return false;
                }
            }

            if (::EVP_PKEY_CTX_set1_hkdf_key(pctx.get(), reinterpret_cast<const unsigned char*>(ikmBytes.data()), static_cast<int>(ikmBytes.size())) != 1)
            {
                return false;
            }

            if (!infoBytes.empty())
            {
                if (::EVP_PKEY_CTX_add1_hkdf_info(pctx.get(), reinterpret_cast<const unsigned char*>(infoBytes.data()), static_cast<int>(infoBytes.size())) != 1)
                {
                    return false;
                }
            }

            outKeyBytes.resize(outLength);
            size_t len = outLength;
            if (::EVP_PKEY_derive(pctx.get(), reinterpret_cast<unsigned char*>(&outKeyBytes[0]), &len) != 1)
            {
                outKeyBytes.clear();
                return false;
            }

            if (len != outLength)
            {
                outKeyBytes.clear();
                return false;
            }

            return true;
#else
            // --- 兼容实现：HKDF-SHA256（RFC 5869）---
            // Extract: PRK = HMAC(salt, IKM)
            std::array<unsigned char, hashLen> prk;
            unsigned int prkLen = 0;

            std::array<unsigned char, hashLen> zeroSalt;
            std::memset(&zeroSalt[0], 0, zeroSalt.size());

            const unsigned char* saltPtr = saltBytes.empty()
                ? &zeroSalt[0]
                : reinterpret_cast<const unsigned char*>(saltBytes.data());
            const int saltLen = saltBytes.empty()
                ? static_cast<int>(zeroSalt.size())
                : static_cast<int>(saltBytes.size());

            if (saltLen < 0)
            {
                return false;
            }

            if (::HMAC(::EVP_sha256(),
                saltPtr,
                saltLen,
                reinterpret_cast<const unsigned char*>(ikmBytes.data()),
                ikmBytes.size(),
                &prk[0],
                &prkLen) == nullptr)
            {
                return false;
            }

            if (prkLen != hashLen)
            {
                return false;
            }

            // Expand
            const size_t n = (outLength + hashLen - 1) / hashLen;
            if (n == 0 || n > 255)
            {
                return false;
            }

            outKeyBytes.resize(outLength);

            std::array<unsigned char, hashLen> t;
            size_t written = 0;

            std::vector<unsigned char> hmacInput;
            hmacInput.reserve(hashLen + infoBytes.size() + 1);

            for (size_t i = 1; i <= n; i++)
            {
                hmacInput.clear();
                if (i > 1)
                {
                    hmacInput.insert(hmacInput.end(), t.begin(), t.end());
                }

                if (!infoBytes.empty())
                {
                    hmacInput.insert(
                        hmacInput.end(),
                        reinterpret_cast<const unsigned char*>(infoBytes.data()),
                        reinterpret_cast<const unsigned char*>(infoBytes.data()) + infoBytes.size());
                }

                hmacInput.push_back(static_cast<unsigned char>(i));

                unsigned int tLen = 0;
                if (::HMAC(::EVP_sha256(),
                    &prk[0],
                    static_cast<int>(prk.size()),
                    &hmacInput[0],
                    hmacInput.size(),
                    &t[0],
                    &tLen) == nullptr)
                {
                    outKeyBytes.clear();
                    return false;
                }

                if (tLen != hashLen)
                {
                    outKeyBytes.clear();
                    return false;
                }

                const size_t toCopy = std::min(hashLen, outLength - written);
                std::memcpy(&outKeyBytes[written], &t[0], toCopy);
                written += toCopy;
            }

            return (written == outLength);
#endif
        }

        static bool ValidateCryptOptions(GB_EccCryptOptions& options)
        {
            if (options.aesKeyBits != 128 && options.aesKeyBits != 256)
            {
                return false;
            }

            if (options.nonceLength == 0)
            {
                options.nonceLength = 12;
            }

            if (options.gcmTagLength == 0)
            {
                options.gcmTagLength = 16;
            }

            if (options.nonceLength > 64)
            {
                return false;
            }

            if (options.gcmTagLength < 8 || options.gcmTagLength > 16)
            {
                return false;
            }

            return true;
        }

        static bool BuildHeaderBytes(uint16_t epkDerLen, const GB_EccCryptOptions& options, uint32_t plainLen, std::string& outHeaderBytes)
        {
            outHeaderBytes.clear();

            // "GBE1" || version(1) || flags(0) || epkLen(2be) || nonceLen(1) || tagLen(1) || aesKeyBits(2be) || plainLen(4be)
            const size_t headerSize = 4 + 1 + 1 + 2 + 1 + 1 + 2 + 4;

            outHeaderBytes.reserve(headerSize);
            outHeaderBytes.append("GBE1", 4);
            outHeaderBytes.push_back(static_cast<char>(1)); // version
            outHeaderBytes.push_back(static_cast<char>(0)); // flags (reserved)

            char u16Bytes[2];
            WriteUint16Be(epkDerLen, u16Bytes);
            outHeaderBytes.append(u16Bytes, 2);

            outHeaderBytes.push_back(static_cast<char>(options.nonceLength));
            outHeaderBytes.push_back(static_cast<char>(options.gcmTagLength));

            WriteUint16Be(static_cast<uint16_t>(options.aesKeyBits), u16Bytes);
            outHeaderBytes.append(u16Bytes, 2);

            char u32Bytes[4];
            WriteUint32Be(plainLen, u32Bytes);
            outHeaderBytes.append(u32Bytes, 4);

            return (outHeaderBytes.size() == headerSize);
        }

    } // anonymous namespace

    bool GB_GenerateEccKeyPair(const GB_EccKeyGenOptions& options, std::string& outPublicKeyBytes, std::string& outPrivateKeyBytes)
    {
        outPublicKeyBytes.clear();
        outPrivateKeyBytes.clear();

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        const int curveNid = GetCurveNidFromCurve(options.curve);
        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> keyPair = GenerateEcKeyPairByCurveNid(curveNid);
        if (keyPair.get() == nullptr)
        {
            return false;
        }

        // --- Export public key ---
        {
            std::unique_ptr<BIO, BioDeleter> bio(::BIO_new(::BIO_s_mem()));
            if (bio.get() == nullptr)
            {
                return false;
            }

            bool writeOk = false;
            switch (options.publicKeyFormat)
            {
            case GB_EccPublicKeyFormat::PemSubjectPublicKeyInfo:
                writeOk = (::PEM_write_bio_PUBKEY(bio.get(), keyPair.get()) == 1);
                break;
            case GB_EccPublicKeyFormat::DerSubjectPublicKeyInfo:
                writeOk = (::i2d_PUBKEY_bio(bio.get(), keyPair.get()) == 1);
                break;
            default:
                writeOk = (::PEM_write_bio_PUBKEY(bio.get(), keyPair.get()) == 1);
                break;
            }

            if (!writeOk)
            {
                return false;
            }

            if (!BioToBytes(bio.get(), outPublicKeyBytes))
            {
                return false;
            }
        }

        // --- Export private key ---
        {
            std::unique_ptr<BIO, BioDeleter> bio(::BIO_new(::BIO_s_mem()));
            if (bio.get() == nullptr)
            {
                outPublicKeyBytes.clear();
                return false;
            }

            bool writeOk = false;
            switch (options.privateKeyFormat)
            {
            case GB_EccPrivateKeyFormat::PemPkcs8:
                writeOk = (::PEM_write_bio_PKCS8PrivateKey(bio.get(), keyPair.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
                break;
            case GB_EccPrivateKeyFormat::PemSec1:
            {
                std::unique_ptr<EC_KEY, EcKeyDeleter> ecKey(::EVP_PKEY_get1_EC_KEY(keyPair.get()));
                writeOk = (ecKey.get() != nullptr) && (::PEM_write_bio_ECPrivateKey(bio.get(), ecKey.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
                break;
            }
            case GB_EccPrivateKeyFormat::DerPkcs8:
                writeOk = (::i2d_PKCS8PrivateKey_bio(bio.get(), keyPair.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
                break;
            case GB_EccPrivateKeyFormat::DerSec1:
            {
                std::unique_ptr<EC_KEY, EcKeyDeleter> ecKey(::EVP_PKEY_get1_EC_KEY(keyPair.get()));
                writeOk = (ecKey.get() != nullptr) && (::i2d_ECPrivateKey_bio(bio.get(), ecKey.get()) == 1);
                break;
            }
            default:
                writeOk = (::PEM_write_bio_PKCS8PrivateKey(bio.get(), keyPair.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
                break;
            }

            if (!writeOk)
            {
                outPublicKeyBytes.clear();
                return false;
            }

            if (!BioToBytes(bio.get(), outPrivateKeyBytes))
            {
                outPublicKeyBytes.clear();
                outPrivateKeyBytes.clear();
                return false;
            }
        }

        return true;
    }

    bool GB_EccEncrypt(const std::string& utf8PlainText, const std::string& publicKeyBytes, GB_EccPublicKeyFormat publicKeyFormat, const GB_EccCryptOptions& options, std::string& outCipherBytes)
    {
        outCipherBytes.clear();

        GB_EccCryptOptions normalizedOptions = options;
        if (!ValidateCryptOptions(normalizedOptions))
        {
            return false;
        }

        if (utf8PlainText.size() > static_cast<size_t>((std::numeric_limits<uint32_t>::max)()))
        {
            return false;
        }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> receiverPublicKey = LoadPublicKeyFromBytes(publicKeyBytes, publicKeyFormat);
        if (receiverPublicKey.get() == nullptr)
        {
            return false;
        }

        const int curveNid = GetCurveNidFromEvpPkey(receiverPublicKey.get());
        if (curveNid == 0)
        {
            return false;
        }

        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> ephemeralKeyPair = GenerateEcKeyPairByCurveNid(curveNid);
        if (ephemeralKeyPair.get() == nullptr)
        {
            return false;
        }

        // 导出临时公钥（DER SPKI）
        std::string epkDerBytes;
        if (!ExportPublicKeyToDerSpki(ephemeralKeyPair.get(), epkDerBytes))
        {
            return false;
        }

        if (epkDerBytes.size() > static_cast<size_t>((std::numeric_limits<uint16_t>::max)()))
        {
            return false;
        }

        // 生成 GCM nonce
        std::string nonceBytes;
        if (!GenerateRandomBytes(normalizedOptions.nonceLength, nonceBytes))
        {
            return false;
        }

        // ECDH 共享密钥
        std::string sharedSecretBytes;
        if (!DeriveSharedSecretEcdh(ephemeralKeyPair.get(), receiverPublicKey.get(), sharedSecretBytes))
        {
            return false;
        }
        StringCleansingGuard sharedSecretGuard(sharedSecretBytes);

        // HKDF 派生 AES key（salt 使用 nonce）
        const size_t aesKeyLen = normalizedOptions.aesKeyBits / 8;
        std::string aesKeyBytes;
        if (!HkdfSha256DeriveBytes(sharedSecretBytes, nonceBytes, normalizedOptions.hkdfInfoBytes, aesKeyLen, aesKeyBytes))
        {
            return false;
        }
        StringCleansingGuard aesKeyGuard(aesKeyBytes);

        // header（会作为 AAD 一部分进行认证）
        std::string headerBytes;
        const uint32_t plainLen = static_cast<uint32_t>(utf8PlainText.size());
        if (!BuildHeaderBytes(static_cast<uint16_t>(epkDerBytes.size()), normalizedOptions, plainLen, headerBytes))
        {
            return false;
        }

        std::string finalAadBytes = normalizedOptions.aadBytes;
        finalAadBytes.append(headerBytes);

        // AES-GCM 加密
        GB_AES::GB_AesOptions aesOptions;
        aesOptions.mode = GB_AES::GB_AesMode::Gcm;
        aesOptions.keyBits = normalizedOptions.aesKeyBits;
        aesOptions.gcmTagLength = normalizedOptions.gcmTagLength;
        aesOptions.aadBytes = finalAadBytes;

        std::string cipherBytes;
        std::string gcmTagBytes;
        if (!GB_AES::GB_AesEncrypt(utf8PlainText, aesKeyBytes, nonceBytes, aesOptions, cipherBytes, gcmTagBytes))
        {
            return false;
        }

        // payload = header || epkDer || nonce || cipher || tag
        size_t totalSize = 0;
        if (!SafeAddSizeT(headerBytes.size(), epkDerBytes.size(), totalSize))
        {
            return false;
        }
        if (!SafeAddSizeT(totalSize, nonceBytes.size(), totalSize))
        {
            return false;
        }
        if (!SafeAddSizeT(totalSize, cipherBytes.size(), totalSize))
        {
            return false;
        }
        if (!SafeAddSizeT(totalSize, gcmTagBytes.size(), totalSize))
        {
            return false;
        }

        outCipherBytes.reserve(totalSize);
        outCipherBytes.append(headerBytes);
        outCipherBytes.append(epkDerBytes);
        outCipherBytes.append(nonceBytes);
        outCipherBytes.append(cipherBytes);
        outCipherBytes.append(gcmTagBytes);

        return true;
    }

    bool GB_EccDecrypt(const std::string& cipherBytes, const std::string& privateKeyBytes, GB_EccPrivateKeyFormat privateKeyFormat, const GB_EccCryptOptions& options, std::string& outUtf8PlainText)
    {
        outUtf8PlainText.clear();

        GB_EccCryptOptions normalizedOptions = options;
        if (!ValidateCryptOptions(normalizedOptions))
        {
            return false;
        }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> receiverPrivateKey = LoadPrivateKeyFromBytes(privateKeyBytes, privateKeyFormat);
        if (receiverPrivateKey.get() == nullptr)
        {
            return false;
        }

        // 解析 header
        const size_t headerSize = 4 + 1 + 1 + 2 + 1 + 1 + 2 + 4;
        if (cipherBytes.size() < headerSize)
        {
            return false;
        }

        if (cipherBytes.compare(0, 4, "GBE1") != 0)
        {
            return false;
        }

        const unsigned char version = static_cast<unsigned char>(cipherBytes[4]);
        if (version != 1)
        {
            return false;
        }

        // flags 预留
        (void)cipherBytes[5];

        const unsigned char* u8Ptr = reinterpret_cast<const unsigned char*>(cipherBytes.data());
        const uint16_t epkLen = ReadUint16Be(u8Ptr + 6);
        const uint8_t nonceLen = static_cast<uint8_t>(u8Ptr[8]);
        const uint8_t tagLen = static_cast<uint8_t>(u8Ptr[9]);
        const uint16_t aesKeyBits = ReadUint16Be(u8Ptr + 10);
        const uint32_t plainLen = ReadUint32Be(u8Ptr + 12);

        if (epkLen == 0)
        {
            return false;
        }

        if (nonceLen == 0 || nonceLen > 64)
        {
            return false;
        }

        if (tagLen < 8 || tagLen > 16)
        {
            return false;
        }

        if ((aesKeyBits != 128 && aesKeyBits != 256) || aesKeyBits != normalizedOptions.aesKeyBits)
        {
            return false;
        }

        if (nonceLen != normalizedOptions.nonceLength || tagLen != normalizedOptions.gcmTagLength)
        {
            // options 与 payload 不一致：为了避免隐性降级/解析错误，这里直接拒绝。
            return false;
        }

        // body lengths
        size_t offset = headerSize;

        if (cipherBytes.size() < offset + epkLen + nonceLen + tagLen)
        {
            return false;
        }

        const std::string epkDerBytes = cipherBytes.substr(offset, epkLen);
        offset += epkLen;

        const std::string nonceBytes = cipherBytes.substr(offset, nonceLen);
        offset += nonceLen;

        const size_t remaining = cipherBytes.size() - offset;
        if (remaining < tagLen)
        {
            return false;
        }

        const size_t cipherLen = remaining - tagLen;

        const std::string encryptedBytes = cipherBytes.substr(offset, cipherLen);
        offset += cipherLen;

        const std::string gcmTagBytes = cipherBytes.substr(offset, tagLen);

        // 解析临时公钥
        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> ephemeralPublicKey;
        {
            std::unique_ptr<BIO, BioDeleter> bio = CreateReadOnlyBioFromBytes(epkDerBytes);
            if (bio.get() == nullptr)
            {
                return false;
            }
            EVP_PKEY* rawKey = ::d2i_PUBKEY_bio(bio.get(), nullptr);
            ephemeralPublicKey.reset(rawKey);
        }

        if (ephemeralPublicKey.get() == nullptr)
        {
            return false;
        }

        if (::EVP_PKEY_base_id(ephemeralPublicKey.get()) != EVP_PKEY_EC)
        {
            return false;
        }

        const int receiverCurveNid = GetCurveNidFromEvpPkey(receiverPrivateKey.get());
        const int ephemeralCurveNid = GetCurveNidFromEvpPkey(ephemeralPublicKey.get());
        if (receiverCurveNid == 0 || ephemeralCurveNid == 0 || receiverCurveNid != ephemeralCurveNid)
        {
            return false;
        }

        // 共享密钥（ECDH）
        std::string sharedSecretBytes;
        if (!DeriveSharedSecretEcdh(receiverPrivateKey.get(), ephemeralPublicKey.get(), sharedSecretBytes))
        {
            return false;
        }
        StringCleansingGuard sharedSecretGuard(sharedSecretBytes);

        // HKDF 派生 AES key（salt=nonce）
        const size_t aesKeyLen = normalizedOptions.aesKeyBits / 8;
        std::string aesKeyBytes;
        if (!HkdfSha256DeriveBytes(sharedSecretBytes, nonceBytes, normalizedOptions.hkdfInfoBytes, aesKeyLen, aesKeyBytes))
        {
            return false;
        }
        StringCleansingGuard aesKeyGuard(aesKeyBytes);

        // header 作为 AAD 一部分
        const std::string headerBytes = cipherBytes.substr(0, headerSize);
        std::string finalAadBytes = normalizedOptions.aadBytes;
        finalAadBytes.append(headerBytes);

        GB_AES::GB_AesOptions aesOptions;
        aesOptions.mode = GB_AES::GB_AesMode::Gcm;
        aesOptions.keyBits = normalizedOptions.aesKeyBits;
        aesOptions.gcmTagLength = normalizedOptions.gcmTagLength;
        aesOptions.aadBytes = finalAadBytes;

        std::string plainBytes;
        if (!GB_AES::GB_AesDecrypt(encryptedBytes, aesKeyBytes, nonceBytes, aesOptions, gcmTagBytes, plainBytes))
        {
            return false;
        }

        if (plainBytes.size() != static_cast<size_t>(plainLen))
        {
            return false;
        }

        outUtf8PlainText.swap(plainBytes);
        return true;
    }

    std::string GB_EccEncryptToBase64(const std::string& utf8PlainText, const std::string& publicKeyBytes, GB_EccPublicKeyFormat publicKeyFormat, const GB_EccCryptOptions& options, bool urlSafe, bool noPadding)
    {
        std::string cipherBytes;
        if (!GB_EccEncrypt(utf8PlainText, publicKeyBytes, publicKeyFormat, options, cipherBytes))
        {
            return std::string();
        }

        return ::GB_Base64Encode(cipherBytes, urlSafe, noPadding);
    }

    bool GB_EccDecryptFromBase64(const std::string& base64CipherText, const std::string& privateKeyBytes, std::string& outUtf8PlainText, GB_EccPrivateKeyFormat privateKeyFormat, const GB_EccCryptOptions& options, bool urlSafe, bool noPadding)
    {
        outUtf8PlainText.clear();

        std::string cipherBytes;
        if (!::GB_Base64Decode(base64CipherText, cipherBytes, urlSafe, noPadding))
        {
            return false;
        }

        return GB_EccDecrypt(cipherBytes, privateKeyBytes, privateKeyFormat, options, outUtf8PlainText);
    }
}


#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif
