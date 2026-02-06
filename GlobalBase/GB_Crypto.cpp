#include "GB_Crypto.h"

#include <algorithm>
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
    struct OsslProviderDeleter
    {
        void operator()(OSSL_PROVIDER* provider) const
        {
            if (provider != nullptr)
            {
                ::OSSL_PROVIDER_unload(provider);
            }
        }
    };

    static std::unique_ptr<OSSL_PROVIDER, OsslProviderDeleter>& GetDefaultProvider()
    {
        static std::unique_ptr<OSSL_PROVIDER, OsslProviderDeleter> provider;
        return provider;
    }

    static std::unique_ptr<OSSL_PROVIDER, OsslProviderDeleter>& GetLegacyProvider()
    {
        static std::unique_ptr<OSSL_PROVIDER, OsslProviderDeleter> provider;
        return provider;
    }

    static void LoadOpenSslProviders()
    {
        // "default" 通常已加载，但显式加载可提升跨平台一致性。
        GetDefaultProvider().reset(::OSSL_PROVIDER_load(nullptr, "default"));

        // "legacy" 用于部分传统算法（例如某些发行版/配置下的 MD5/MD4 等）。
        // 若加载失败也不作为错误处理。
        GetLegacyProvider().reset(::OSSL_PROVIDER_load(nullptr, "legacy"));
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

        if (!noPadding || remain == 3)
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

    static bool GenerateRandomBytes(size_t bytesCount, std::string& outBytes)
    {
        outBytes.clear();

        if (bytesCount == 0)
        {
            return false;
        }

        if (bytesCount > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        outBytes.resize(bytesCount);
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&outBytes[0]), static_cast<int>(bytesCount)) != 1)
        {
            outBytes.clear();
            return false;
        }

        return true;
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

        // Argon2 要求 m >= 8 * p（单位 KiB）。
        const uint32_t minMemoryCost = options.lanes * 8;
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

    static void EnsureOpenSslMaxThreads(uint32_t threads)
    {
        if (threads <= 1)
        {
            return;
        }

        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);

        const uint64_t currentMaxThreads = ::OSSL_get_max_threads(nullptr);
        if (currentMaxThreads < static_cast<uint64_t>(threads))
        {
            // 该设置属于 OpenSSL libctx 的全局状态：为了避免并发下“先增后降”的竞态，
            // 这里采用“只增不降”的策略。
            ::OSSL_set_max_threads(nullptr, static_cast<uint64_t>(threads));
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

        // threads > 1 需要启用 OpenSSL 内置线程池（OSSL_set_max_threads）。
        if (threads > 1)
        {
            EnsureOpenSslMaxThreads(threads);
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

        params[paramCount++] = ::OSSL_PARAM_construct_uint32(
            OSSL_KDF_PARAM_ARGON2_LANES,
            &normalizedOptions.lanes
        );
        params[paramCount++] = ::OSSL_PARAM_construct_uint32(
            OSSL_KDF_PARAM_ARGON2_MEMCOST,
            &normalizedOptions.memoryCostKiB
        );
        params[paramCount++] = ::OSSL_PARAM_construct_uint32(
            OSSL_KDF_PARAM_ITER,
            &normalizedOptions.iterations
        );

        if (useThreadsParam)
        {
            params[paramCount++] = ::OSSL_PARAM_construct_uint32(
                OSSL_KDF_PARAM_THREADS,
                &threads
            );
        }

        params[paramCount++] = ::OSSL_PARAM_construct_size_t(
            OSSL_KDF_PARAM_SIZE,
            &derivedLength
        );

        params[paramCount++] = ::OSSL_PARAM_construct_uint32(
            OSSL_KDF_PARAM_ARGON2_VERSION,
            &normalizedOptions.version
        );

#if defined(OSSL_KDF_PARAM_EARLY_CLEAN)
        params[paramCount++] = ::OSSL_PARAM_construct_uint32(
            OSSL_KDF_PARAM_EARLY_CLEAN,
            &earlyClean
        );
#else
        (void)earlyClean;
#endif

        params[paramCount++] = ::OSSL_PARAM_construct_octet_string(
            OSSL_KDF_PARAM_SALT,
            GetOctetStringPointerForOpenSsl(saltCopy),
            saltCopy.size()
        );

        if (!secretCopy.empty())
        {
            params[paramCount++] = ::OSSL_PARAM_construct_octet_string(
                OSSL_KDF_PARAM_SECRET,
                GetOctetStringPointerForOpenSsl(secretCopy),
                secretCopy.size()
            );
        }

        if (!adCopy.empty())
        {
            params[paramCount++] = ::OSSL_PARAM_construct_octet_string(
                OSSL_KDF_PARAM_ARGON2_AD,
                GetOctetStringPointerForOpenSsl(adCopy),
                adCopy.size()
            );
        }

        params[paramCount++] = ::OSSL_PARAM_construct_octet_string(
            OSSL_KDF_PARAM_PASSWORD,
            GetOctetStringPointerForOpenSsl(passwordCopy),
            passwordCopy.size()
        );

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

        size_t ivLength = normalizedOptions.ivLength;
        if (ivLength == 0)
        {
            ivLength = GetRecommendedIvLength(normalizedOptions.mode);
        }

        std::string ivBytes;
        if (!GB_Argon2::GenerateRandomBytes(ivLength, ivBytes))
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

        size_t ivLength = normalizedOptions.ivLength;
        if (ivLength == 0)
        {
            ivLength = GetRecommendedIvLength(normalizedOptions.mode);
        }

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

        size_t ivLength = normalizedOptions.ivLength;
        if (ivLength == 0)
        {
            ivLength = GetRecommendedIvLength(normalizedOptions.mode);
        }

        const size_t totalLength = keyBytesCount + ivLength;
        if (totalLength == 0 || totalLength > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        std::string derivedBytes;
        derivedBytes.resize(totalLength);

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
            derivedBytes.clear();
            return false;
        }

        outKeyBytes = derivedBytes.substr(0, keyBytesCount);
        outIvBytes = (ivLength > 0) ? derivedBytes.substr(keyBytesCount, ivLength) : std::string();

        // 尝试清理派生缓冲区（避免在内存中长时间残留）。
        if (!derivedBytes.empty())
        {
            ::OPENSSL_cleanse(&derivedBytes[0], derivedBytes.size());
        }

        return true;
    }
}

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

    struct BignumDeleter
    {
        void operator()(BIGNUM* bn) const
        {
            if (bn != nullptr)
            {
                ::BN_free(bn);
            }
        }
    };

    static void EnsureCurlInitialized()
    {
        static std::once_flag s_curlInitOnce;
        std::call_once(s_curlInitOnce, [](){
            ::curl_global_init(CURL_GLOBAL_DEFAULT);
        });
    }

    static bool CurlUrlEscapeText(const std::string& text, std::string& outEscapedText)
    {
        outEscapedText.clear();

        EnsureCurlInitialized();

        CURL* curl = ::curl_easy_init();
        if (curl == nullptr)
        {
            return false;
        }

        char* escaped = ::curl_easy_escape(curl, text.c_str(), static_cast<int>(text.size()));
        if (escaped == nullptr)
        {
            ::curl_easy_cleanup(curl);
            return false;
        }

        outEscapedText.assign(escaped);
        ::curl_free(escaped);
        ::curl_easy_cleanup(curl);
        return true;
    }

    static bool CurlUrlUnescapeText(const std::string& escapedText, std::string& outText)
    {
        outText.clear();

        EnsureCurlInitialized();

        CURL* curl = ::curl_easy_init();
        if (curl == nullptr)
        {
            return false;
        }

        int outputLength = 0;
        char* unescaped = ::curl_easy_unescape(curl, escapedText.c_str(), static_cast<int>(escapedText.size()), &outputLength);
        if (unescaped == nullptr || outputLength < 0)
        {
            if (unescaped != nullptr)
            {
                ::curl_free(unescaped);
            }
            ::curl_easy_cleanup(curl);
            return false;
        }

        outText.assign(unescaped, static_cast<size_t>(outputLength));
        ::curl_free(unescaped);
        ::curl_easy_cleanup(curl);
        return true;
    }

    static bool ReadMemoryBioToString(BIO* bio, std::string& outBytes)
    {
        outBytes.clear();
        if (bio == nullptr)
        {
            return false;
        }

        char* dataPtr = nullptr;
        const long dataLength = ::BIO_get_mem_data(bio, &dataPtr);
        if (dataLength <= 0 || dataPtr == nullptr)
        {
            return false;
        }

        outBytes.assign(dataPtr, static_cast<size_t>(dataLength));
        return true;
    }

    static const EVP_MD* GetEvpMdForRsaHashMethod(GB_RSA::GB_RsaHashMethod method)
    {
        switch (method)
        {
        case GB_RSA::GB_RsaHashMethod::Sha1:
            return ::EVP_sha1();
        case GB_RSA::GB_RsaHashMethod::Sha256:
            return ::EVP_sha256();
        case GB_RSA::GB_RsaHashMethod::Sha384:
            return ::EVP_sha384();
        case GB_RSA::GB_RsaHashMethod::Sha512:
            return ::EVP_sha512();
        default:
            return ::EVP_sha256();
        }
    }

    static bool ZlibCompressBytes(const std::string& inputBytes, int compressionLevel, std::string& outCompressedBytes)
    {
        outCompressedBytes.clear();

        if (inputBytes.empty())
        {
            return false;
        }

        const uLongf sourceLength = static_cast<uLongf>(inputBytes.size());
        const uLongf bound = ::compressBound(sourceLength);
        if (bound == 0)
        {
            return false;
        }

        outCompressedBytes.resize(static_cast<size_t>(bound));

        uLongf destLength = bound;
        const int level = (compressionLevel < -1 || compressionLevel > 9) ? Z_DEFAULT_COMPRESSION : compressionLevel;
        const int rc = ::compress2(
            reinterpret_cast<Bytef*>(&outCompressedBytes[0]),
            &destLength,
            reinterpret_cast<const Bytef*>(inputBytes.data()),
            sourceLength,
            level
        );

        if (rc != Z_OK)
        {
            outCompressedBytes.clear();
            return false;
        }

        outCompressedBytes.resize(static_cast<size_t>(destLength));
        return true;
    }

    static bool ZlibDecompressBytes(const std::string& compressedBytes, size_t uncompressedSize, std::string& outBytes)
    {
        outBytes.clear();

        if (compressedBytes.empty())
        {
            return false;
        }

        if (uncompressedSize == 0)
        {
            return false;
        }

        if (uncompressedSize > static_cast<size_t>(std::numeric_limits<uLongf>::max()))
        {
            return false;
        }

        outBytes.resize(uncompressedSize);

        uLongf destLength = static_cast<uLongf>(uncompressedSize);
        const int rc = ::uncompress(
            reinterpret_cast<Bytef*>(&outBytes[0]),
            &destLength,
            reinterpret_cast<const Bytef*>(compressedBytes.data()),
            static_cast<uLongf>(compressedBytes.size())
        );

        if (rc != Z_OK || static_cast<size_t>(destLength) != uncompressedSize)
        {
            outBytes.clear();
            return false;
        }

        return true;
    }

    static void WriteUint32Be(uint32_t value, unsigned char outBytes[4])
    {
        outBytes[0] = static_cast<unsigned char>((value >> 24) & 0xFF);
        outBytes[1] = static_cast<unsigned char>((value >> 16) & 0xFF);
        outBytes[2] = static_cast<unsigned char>((value >> 8) & 0xFF);
        outBytes[3] = static_cast<unsigned char>(value & 0xFF);
    }

    static bool ReadUint32Be(const unsigned char bytes[4], uint32_t& outValue)
    {
        outValue = (static_cast<uint32_t>(bytes[0]) << 24)
            | (static_cast<uint32_t>(bytes[1]) << 16)
            | (static_cast<uint32_t>(bytes[2]) << 8)
            | static_cast<uint32_t>(bytes[3]);
        return true;
    }

    static bool BuildRsaPayload(const std::string& utf8PlainText, const GB_RSA::GB_RsaCryptOptions& options, std::string& outPayloadBytes)
    {
        outPayloadBytes.clear();

        // Payload 格式：
        //   4 bytes  magic "GBR1"
        //   1 byte   flags（bit0=zlibCompressed）
        //   1 byte   reserved
        //   2 bytes  reserved
        //   4 bytes  originalSize (BE, uint32)
        //   4 bytes  storedSize   (BE, uint32)
        //   N bytes  storedData   (raw or zlib)
        const unsigned char magic[4] = { 'G', 'B', 'R', '1' };

        const size_t originalSize = utf8PlainText.size();
        if (originalSize > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
        {
            return false;
        }

        bool useCompressed = false;
        std::string storedBytes = utf8PlainText;

        if (options.zlibCompress && !utf8PlainText.empty())
        {
            std::string compressedBytes;
            if (ZlibCompressBytes(utf8PlainText, options.zlibCompressionLevel, compressedBytes))
            {
                if (!compressedBytes.empty() && compressedBytes.size() < utf8PlainText.size())
                {
                    storedBytes.swap(compressedBytes);
                    useCompressed = true;
                }
            }
        }

        if (storedBytes.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
        {
            return false;
        }

        outPayloadBytes.reserve(4 + 1 + 1 + 2 + 4 + 4 + storedBytes.size());
        outPayloadBytes.append(reinterpret_cast<const char*>(magic), 4);

        const unsigned char flags = useCompressed ? 0x01 : 0x00;
        outPayloadBytes.push_back(static_cast<char>(flags));
        outPayloadBytes.push_back(static_cast<char>(0)); // reserved
        outPayloadBytes.push_back(static_cast<char>(0)); // reserved
        outPayloadBytes.push_back(static_cast<char>(0)); // reserved

        unsigned char sizeBytes[4];
        WriteUint32Be(static_cast<uint32_t>(originalSize), sizeBytes);
        outPayloadBytes.append(reinterpret_cast<const char*>(sizeBytes), 4);

        WriteUint32Be(static_cast<uint32_t>(storedBytes.size()), sizeBytes);
        outPayloadBytes.append(reinterpret_cast<const char*>(sizeBytes), 4);

        outPayloadBytes.append(storedBytes);
        return true;
    }

    static bool ParseRsaPayload(const std::string& payloadBytes, std::string& outUtf8PlainText)
    {
        outUtf8PlainText.clear();

        const size_t headerSize = 4 + 1 + 1 + 2 + 4 + 4;
        if (payloadBytes.size() < headerSize)
        {
            // 兼容：若没有 header，则直接当作明文。
            outUtf8PlainText = payloadBytes;
            return true;
        }

        const unsigned char* data = reinterpret_cast<const unsigned char*>(payloadBytes.data());
        if (!(data[0] == 'G' && data[1] == 'B' && data[2] == 'R' && data[3] == '1'))
        {
            outUtf8PlainText = payloadBytes;
            return true;
        }

        const unsigned char flags = data[4];
        const bool isCompressed = (flags & 0x01) != 0;

        uint32_t originalSize = 0;
        uint32_t storedSize = 0;
        ReadUint32Be(data + 8, originalSize);
        ReadUint32Be(data + 12, storedSize);

        const size_t storedOffset = headerSize;
        if (payloadBytes.size() < storedOffset + storedSize)
        {
            return false;
        }

        const std::string storedBytes = payloadBytes.substr(storedOffset, storedSize);

        if (!isCompressed)
        {
            outUtf8PlainText = storedBytes;
            return true;
        }

        return ZlibDecompressBytes(storedBytes, static_cast<size_t>(originalSize), outUtf8PlainText);
    }

    static bool ConfigureRsaCtx(EVP_PKEY_CTX* ctx, const GB_RSA::GB_RsaCryptOptions& options)
    {
        if (ctx == nullptr)
        {
            return false;
        }

        int padding = RSA_PKCS1_OAEP_PADDING;
        switch (options.padding)
        {
        case GB_RSA::GB_RsaPaddingMode::Pkcs1V15:
            padding = RSA_PKCS1_PADDING;
            break;
        case GB_RSA::GB_RsaPaddingMode::Oaep:
            padding = RSA_PKCS1_OAEP_PADDING;
            break;
        case GB_RSA::GB_RsaPaddingMode::NoPadding:
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

        if (options.padding == GB_RSA::GB_RsaPaddingMode::Oaep)
        {
            const EVP_MD* oaepMd = GetEvpMdForRsaHashMethod(options.oaepHash);
            const EVP_MD* mgf1Md = GetEvpMdForRsaHashMethod(options.mgf1Hash);

#ifdef EVP_PKEY_CTX_set_rsa_oaep_md
            if (oaepMd != nullptr)
            {
                if (::EVP_PKEY_CTX_set_rsa_oaep_md(ctx, oaepMd) <= 0)
                {
                    return false;
                }
            }
#endif

#ifdef EVP_PKEY_CTX_set_rsa_mgf1_md
            if (mgf1Md != nullptr)
            {
                if (::EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, mgf1Md) <= 0)
                {
                    return false;
                }
            }
#endif

#ifdef EVP_PKEY_CTX_set0_rsa_oaep_label
            if (!options.oaepLabelBytes.empty())
            {
                unsigned char* labelCopy = reinterpret_cast<unsigned char*>(::OPENSSL_malloc(options.oaepLabelBytes.size()));
                if (labelCopy == nullptr)
                {
                    return false;
                }

                ::memcpy(labelCopy, options.oaepLabelBytes.data(), options.oaepLabelBytes.size());

                if (::EVP_PKEY_CTX_set0_rsa_oaep_label(ctx, labelCopy, options.oaepLabelBytes.size()) <= 0)
                {
                    ::OPENSSL_free(labelCopy);
                    return false;
                }
            }
#endif
        }

        return true;
    }

    static bool GetRsaBlockSizes(EVP_PKEY* pkey, const GB_RSA::GB_RsaCryptOptions& options, size_t& outKeySizeBytes, size_t& outMaxPlainTextBytes)
    {
        outKeySizeBytes = 0;
        outMaxPlainTextBytes = 0;

        if (pkey == nullptr)
        {
            return false;
        }

        const int keySizeBytesInt = ::EVP_PKEY_size(pkey);
        if (keySizeBytesInt <= 0)
        {
            return false;
        }

        const size_t keySizeBytes = static_cast<size_t>(keySizeBytesInt);
        outKeySizeBytes = keySizeBytes;

        switch (options.padding)
        {
        case GB_RSA::GB_RsaPaddingMode::NoPadding:
            outMaxPlainTextBytes = keySizeBytes;
            return true;

        case GB_RSA::GB_RsaPaddingMode::Pkcs1V15:
            if (keySizeBytes <= 11)
            {
                return false;
            }
            outMaxPlainTextBytes = keySizeBytes - 11;
            return true;

        case GB_RSA::GB_RsaPaddingMode::Oaep:
        default:
        {
            const EVP_MD* md = GetEvpMdForRsaHashMethod(options.oaepHash);
            if (md == nullptr)
            {
                return false;
            }

            const int digestSizeInt = ::EVP_MD_size(md);
            if (digestSizeInt <= 0)
            {
                return false;
            }

            const size_t digestSize = static_cast<size_t>(digestSizeInt);
            const size_t overhead = 2 * digestSize + 2;
            if (keySizeBytes <= overhead)
            {
                return false;
            }

            outMaxPlainTextBytes = keySizeBytes - overhead;
            return true;
        }
        }
    }

    static std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> LoadRsaPublicKey(const std::string& publicKeyBytes, GB_RSA::GB_RsaPublicKeyFormat format)
    {
        if (publicKeyBytes.empty())
        {
            return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
        }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        auto TryLoadPemSpki = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
            std::unique_ptr<BIO, BioDeleter> bio(::BIO_new_mem_buf(publicKeyBytes.data(), static_cast<int>(publicKeyBytes.size())));
            if (!bio)
            {
                return {};
            }

            EVP_PKEY* pkey = ::PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
            return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
        };

        auto TryLoadPemPkcs1 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
            std::unique_ptr<BIO, BioDeleter> bio(::BIO_new_mem_buf(publicKeyBytes.data(), static_cast<int>(publicKeyBytes.size())));
            if (!bio)
            {
                return {};
            }

            std::unique_ptr<RSA, RsaDeleter> rsa(::PEM_read_bio_RSAPublicKey(bio.get(), nullptr, nullptr, nullptr));
            if (!rsa)
            {
                return {};
            }

            std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey(::EVP_PKEY_new());
            if (!pkey)
            {
                return {};
            }

            if (::EVP_PKEY_assign_RSA(pkey.get(), rsa.release()) != 1)
            {
                return {};
            }

            return pkey;
        };

        auto TryLoadDerSpki = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(publicKeyBytes.data());
            EVP_PKEY* pkey = ::d2i_PUBKEY(nullptr, &p, static_cast<long>(publicKeyBytes.size()));
            return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
        };

        auto TryLoadDerPkcs1 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(publicKeyBytes.data());
            std::unique_ptr<RSA, RsaDeleter> rsa(::d2i_RSAPublicKey(nullptr, &p, static_cast<long>(publicKeyBytes.size())));
            if (!rsa)
            {
                return {};
            }

            std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey(::EVP_PKEY_new());
            if (!pkey)
            {
                return {};
            }

            if (::EVP_PKEY_assign_RSA(pkey.get(), rsa.release()) != 1)
            {
                return {};
            }

            return pkey;
        };

        if (format == GB_RSA::GB_RsaPublicKeyFormat::PemSubjectPublicKeyInfo)
        {
            return TryLoadPemSpki();
        }
        if (format == GB_RSA::GB_RsaPublicKeyFormat::PemPkcs1)
        {
            return TryLoadPemPkcs1();
        }
        if (format == GB_RSA::GB_RsaPublicKeyFormat::DerSubjectPublicKeyInfo)
        {
            return TryLoadDerSpki();
        }
        if (format == GB_RSA::GB_RsaPublicKeyFormat::DerPkcs1)
        {
            return TryLoadDerPkcs1();
        }

        // Auto: 依次尝试
        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey = TryLoadPemSpki();
        if (pkey)
        {
            return pkey;
        }
        pkey = TryLoadPemPkcs1();
        if (pkey)
        {
            return pkey;
        }
        pkey = TryLoadDerSpki();
        if (pkey)
        {
            return pkey;
        }
        return TryLoadDerPkcs1();
    }

    static std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> LoadRsaPrivateKey(const std::string& privateKeyBytes, GB_RSA::GB_RsaPrivateKeyFormat format)
    {
        if (privateKeyBytes.empty())
        {
            return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>();
        }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        auto TryLoadPemPrivateKey = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
            std::unique_ptr<BIO, BioDeleter> bio(::BIO_new_mem_buf(privateKeyBytes.data(), static_cast<int>(privateKeyBytes.size())));
            if (!bio)
            {
                return {};
            }

            EVP_PKEY* pkey = ::PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
            return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
        };

        auto TryLoadPemPkcs1 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
            std::unique_ptr<BIO, BioDeleter> bio(::BIO_new_mem_buf(privateKeyBytes.data(), static_cast<int>(privateKeyBytes.size())));
            if (!bio)
            {
                return {};
            }

            std::unique_ptr<RSA, RsaDeleter> rsa(::PEM_read_bio_RSAPrivateKey(bio.get(), nullptr, nullptr, nullptr));
            if (!rsa)
            {
                return {};
            }

            std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey(::EVP_PKEY_new());
            if (!pkey)
            {
                return {};
            }

            if (::EVP_PKEY_assign_RSA(pkey.get(), rsa.release()) != 1)
            {
                return {};
            }

            return pkey;
        };

        auto TryLoadDerAutoPrivateKey = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(privateKeyBytes.data());
            EVP_PKEY* pkey = ::d2i_AutoPrivateKey(nullptr, &p, static_cast<long>(privateKeyBytes.size()));
            return std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>(pkey);
        };

        auto TryLoadDerPkcs1 = [&]() -> std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(privateKeyBytes.data());
            std::unique_ptr<RSA, RsaDeleter> rsa(::d2i_RSAPrivateKey(nullptr, &p, static_cast<long>(privateKeyBytes.size())));
            if (!rsa)
            {
                return {};
            }

            std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey(::EVP_PKEY_new());
            if (!pkey)
            {
                return {};
            }

            if (::EVP_PKEY_assign_RSA(pkey.get(), rsa.release()) != 1)
            {
                return {};
            }

            return pkey;
        };

        if (format == GB_RSA::GB_RsaPrivateKeyFormat::PemPkcs8)
        {
            return TryLoadPemPrivateKey();
        }
        if (format == GB_RSA::GB_RsaPrivateKeyFormat::PemPkcs1)
        {
            // 兼容：既尝试 PEM_read_bio_PrivateKey，也尝试传统 RSA PRIVATE KEY
            std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey = TryLoadPemPrivateKey();
            if (pkey)
            {
                return pkey;
            }
            return TryLoadPemPkcs1();
        }
        if (format == GB_RSA::GB_RsaPrivateKeyFormat::DerPkcs8)
        {
            return TryLoadDerAutoPrivateKey();
        }
        if (format == GB_RSA::GB_RsaPrivateKeyFormat::DerPkcs1)
        {
            return TryLoadDerPkcs1();
        }

        // Auto
        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey = TryLoadPemPrivateKey();
        if (pkey)
        {
            return pkey;
        }
        pkey = TryLoadDerAutoPrivateKey();
        if (pkey)
        {
            return pkey;
        }
        pkey = TryLoadPemPkcs1();
        if (pkey)
        {
            return pkey;
        }
        return TryLoadDerPkcs1();
    }

    static bool WritePublicKeyToBytes(EVP_PKEY* pkey, GB_RSA::GB_RsaPublicKeyFormat format, std::string& outPublicKeyBytes)
    {
        outPublicKeyBytes.clear();

        std::unique_ptr<BIO, BioDeleter> bio(::BIO_new(BIO_s_mem()));
        if (!bio)
        {
            return false;
        }

        int ok = 0;
        if (format == GB_RSA::GB_RsaPublicKeyFormat::PemSubjectPublicKeyInfo)
        {
            ok = ::PEM_write_bio_PUBKEY(bio.get(), pkey);
        }
        else if (format == GB_RSA::GB_RsaPublicKeyFormat::PemPkcs1)
        {
            std::unique_ptr<RSA, RsaDeleter> rsa(::EVP_PKEY_get1_RSA(pkey));
            if (!rsa)
            {
                return false;
            }
            ok = ::PEM_write_bio_RSAPublicKey(bio.get(), rsa.get());
        }
        else if (format == GB_RSA::GB_RsaPublicKeyFormat::DerSubjectPublicKeyInfo)
        {
            ok = ::i2d_PUBKEY_bio(bio.get(), pkey);
        }
        else if (format == GB_RSA::GB_RsaPublicKeyFormat::DerPkcs1)
        {
            std::unique_ptr<RSA, RsaDeleter> rsa(::EVP_PKEY_get1_RSA(pkey));
            if (!rsa)
            {
                return false;
            }
            ok = ::i2d_RSAPublicKey_bio(bio.get(), rsa.get());
        }
        else
        {
            ok = ::PEM_write_bio_PUBKEY(bio.get(), pkey);
        }

        if (ok != 1)
        {
            return false;
        }

        return ReadMemoryBioToString(bio.get(), outPublicKeyBytes);
    }

    static bool WritePrivateKeyToBytes(EVP_PKEY* pkey, GB_RSA::GB_RsaPrivateKeyFormat format, std::string& outPrivateKeyBytes)
    {
        outPrivateKeyBytes.clear();

        std::unique_ptr<BIO, BioDeleter> bio(::BIO_new(BIO_s_mem()));
        if (!bio)
        {
            return false;
        }

        int ok = 0;
        if (format == GB_RSA::GB_RsaPrivateKeyFormat::PemPkcs8)
        {
            // PEM_write_bio_PrivateKey 会写出 PKCS#8（未加密）。
            ok = ::PEM_write_bio_PrivateKey(bio.get(), pkey, nullptr, nullptr, 0, nullptr, nullptr);
        }
        else if (format == GB_RSA::GB_RsaPrivateKeyFormat::PemPkcs1)
        {
            std::unique_ptr<RSA, RsaDeleter> rsa(::EVP_PKEY_get1_RSA(pkey));
            if (!rsa)
            {
                return false;
            }
            ok = ::PEM_write_bio_RSAPrivateKey(bio.get(), rsa.get(), nullptr, nullptr, 0, nullptr, nullptr);
        }
        else if (format == GB_RSA::GB_RsaPrivateKeyFormat::DerPkcs8)
        {
            // i2d_PrivateKey_bio：通常会输出 PKCS#8 unencrypted PrivateKeyInfo。
            ok = ::i2d_PrivateKey_bio(bio.get(), pkey);
        }
        else if (format == GB_RSA::GB_RsaPrivateKeyFormat::DerPkcs1)
        {
            std::unique_ptr<RSA, RsaDeleter> rsa(::EVP_PKEY_get1_RSA(pkey));
            if (!rsa)
            {
                return false;
            }
            ok = ::i2d_RSAPrivateKey_bio(bio.get(), rsa.get());
        }
        else
        {
            ok = ::PEM_write_bio_PrivateKey(bio.get(), pkey, nullptr, nullptr, 0, nullptr, nullptr);
        }

        if (ok != 1)
        {
            return false;
        }

        return ReadMemoryBioToString(bio.get(), outPrivateKeyBytes);
    }
}

namespace GB_RSA
{
    bool GB_GenerateRsaKeyPair(const GB_RsaKeyGenOptions& options, std::string& outPublicKeyBytes, std::string& outPrivateKeyBytes)
    {
        outPublicKeyBytes.clear();
        outPrivateKeyBytes.clear();

        if (options.keyBits < 512 || options.keyBits > 16384)
        {
            return false;
        }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EnsureOpenSslProvidersLoaded();
#endif

        std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter> ctx(::EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
        if (!ctx)
        {
            return false;
        }

        if (::EVP_PKEY_keygen_init(ctx.get()) <= 0)
        {
            return false;
        }

        if (::EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), static_cast<int>(options.keyBits)) <= 0)
        {
            return false;
        }

        std::unique_ptr<BIGNUM, BignumDeleter> pubExp(::BN_new());
        if (!pubExp)
        {
            return false;
        }

        if (::BN_set_word(pubExp.get(), static_cast<unsigned long>(options.publicExponent)) != 1)
        {
            return false;
        }

#ifdef EVP_PKEY_CTX_set1_rsa_keygen_pubexp
        if (::EVP_PKEY_CTX_set1_rsa_keygen_pubexp(ctx.get(), pubExp.get()) <= 0)
        {
            return false;
        }
#else
        if (::EVP_PKEY_CTX_set_rsa_keygen_pubexp(ctx.get(), pubExp.get()) <= 0)
        {
            return false;
        }
        // 注意：EVP_PKEY_CTX_set_rsa_keygen_pubexp() 可能不拷贝 pubExp，因此不能 free。
        pubExp.release();
#endif

        EVP_PKEY* generatedKey = nullptr;
        if (::EVP_PKEY_keygen(ctx.get(), &generatedKey) <= 0)
        {
            return false;
        }

        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> pkey(generatedKey);

        if (!WritePublicKeyToBytes(pkey.get(), options.publicKeyFormat, outPublicKeyBytes))
        {
            outPublicKeyBytes.clear();
            outPrivateKeyBytes.clear();
            return false;
        }

        if (!WritePrivateKeyToBytes(pkey.get(), options.privateKeyFormat, outPrivateKeyBytes))
        {
            outPublicKeyBytes.clear();
            outPrivateKeyBytes.clear();
            return false;
        }

        return true;
    }

    bool GB_RsaEncrypt(const std::string& utf8PlainText, const std::string& publicKeyBytes, GB_RsaPublicKeyFormat publicKeyFormat, const GB_RsaCryptOptions& options, std::string& outCipherBytes)
    {
        outCipherBytes.clear();

        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> publicKey = LoadRsaPublicKey(publicKeyBytes, publicKeyFormat);
        if (!publicKey)
        {
            return false;
        }

        std::string payloadBytes;
        if (!BuildRsaPayload(utf8PlainText, options, payloadBytes))
        {
            return false;
        }

        size_t keySizeBytes = 0;
        size_t maxPlainTextBytes = 0;
        if (!GetRsaBlockSizes(publicKey.get(), options, keySizeBytes, maxPlainTextBytes))
        {
            return false;
        }

        if (maxPlainTextBytes == 0)
        {
            return false;
        }

        size_t offset = 0;
        while (offset < payloadBytes.size())
        {
            const size_t remaining = payloadBytes.size() - offset;
            const size_t chunkSize = std::min(remaining, maxPlainTextBytes);

            const unsigned char* inPtr = reinterpret_cast<const unsigned char*>(payloadBytes.data() + offset);

            std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter> ctx(::EVP_PKEY_CTX_new(publicKey.get(), nullptr));
            if (!ctx)
            {
                outCipherBytes.clear();
                return false;
            }

            if (::EVP_PKEY_encrypt_init(ctx.get()) <= 0)
            {
                outCipherBytes.clear();
                return false;
            }

            if (!ConfigureRsaCtx(ctx.get(), options))
            {
                outCipherBytes.clear();
                return false;
            }

            size_t outLen = 0;
            if (::EVP_PKEY_encrypt(ctx.get(), nullptr, &outLen, inPtr, chunkSize) <= 0)
            {
                outCipherBytes.clear();
                return false;
            }

            std::string encryptedBlock;
            encryptedBlock.resize(outLen);

            if (::EVP_PKEY_encrypt(ctx.get(), reinterpret_cast<unsigned char*>(&encryptedBlock[0]), &outLen, inPtr, chunkSize) <= 0)
            {
                outCipherBytes.clear();
                return false;
            }

            encryptedBlock.resize(outLen);
            outCipherBytes.append(encryptedBlock);

            offset += chunkSize;
        }

        return true;
    }

    bool GB_RsaDecrypt(const std::string& cipherBytes, const std::string& privateKeyBytes, GB_RsaPrivateKeyFormat privateKeyFormat, const GB_RsaCryptOptions& options, std::string& outUtf8PlainText)
    {
        outUtf8PlainText.clear();

        std::unique_ptr<EVP_PKEY, EvpPkeyDeleter> privateKey = LoadRsaPrivateKey(privateKeyBytes, privateKeyFormat);
        if (!privateKey)
        {
            return false;
        }

        size_t keySizeBytes = 0;
        size_t maxPlainTextBytes = 0;
        if (!GetRsaBlockSizes(privateKey.get(), options, keySizeBytes, maxPlainTextBytes))
        {
            return false;
        }

        if (keySizeBytes == 0)
        {
            return false;
        }

        if (cipherBytes.empty() || (cipherBytes.size() % keySizeBytes) != 0)
        {
            return false;
        }

        std::string payloadBytes;

        for (size_t offset = 0; offset < cipherBytes.size(); offset += keySizeBytes)
        {
            const unsigned char* inPtr = reinterpret_cast<const unsigned char*>(cipherBytes.data() + offset);

            std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter> ctx(::EVP_PKEY_CTX_new(privateKey.get(), nullptr));
            if (!ctx)
            {
                payloadBytes.clear();
                return false;
            }

            if (::EVP_PKEY_decrypt_init(ctx.get()) <= 0)
            {
                payloadBytes.clear();
                return false;
            }

            if (!ConfigureRsaCtx(ctx.get(), options))
            {
                payloadBytes.clear();
                return false;
            }

            size_t outLen = 0;
            if (::EVP_PKEY_decrypt(ctx.get(), nullptr, &outLen, inPtr, keySizeBytes) <= 0)
            {
                payloadBytes.clear();
                return false;
            }

            std::string decryptedChunk;
            decryptedChunk.resize(outLen);

            if (::EVP_PKEY_decrypt(ctx.get(), reinterpret_cast<unsigned char*>(&decryptedChunk[0]), &outLen, inPtr, keySizeBytes) <= 0)
            {
                payloadBytes.clear();
                return false;
            }

            decryptedChunk.resize(outLen);
            payloadBytes.append(decryptedChunk);
        }

        return ParseRsaPayload(payloadBytes, outUtf8PlainText);
    }

    std::string GB_RsaEncryptToBase64(const std::string& utf8PlainText, const std::string& publicKeyBytes, GB_RsaPublicKeyFormat publicKeyFormat, const GB_RsaCryptOptions& options, bool urlSafe, bool noPadding, bool urlEscape)
    {
        std::string cipherBytes;
        if (!GB_RsaEncrypt(utf8PlainText, publicKeyBytes, publicKeyFormat, options, cipherBytes))
        {
            return std::string();
        }

        std::string base64Text = GB_Base64Encode(cipherBytes, urlSafe, noPadding);
        if (base64Text.empty())
        {
            return std::string();
        }

        if (!urlEscape)
        {
            return base64Text;
        }

        std::string escaped;
        if (!CurlUrlEscapeText(base64Text, escaped))
        {
            return std::string();
        }

        return escaped;
    }

    bool GB_RsaDecryptFromBase64(const std::string& base64CipherText, const std::string& privateKeyBytes, GB_RsaPrivateKeyFormat privateKeyFormat, const GB_RsaCryptOptions& options, std::string& outUtf8PlainText, bool urlSafe, bool noPadding, bool urlEscaped)
    {
        outUtf8PlainText.clear();

        std::string base64Text = base64CipherText;
        if (urlEscaped)
        {
            if (!CurlUrlUnescapeText(base64CipherText, base64Text))
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


#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif