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
        std::call_once(addAllDigestsOnceFlag, []()
            {
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

    const size_t fullSize = ((dataSize + 2) / 3) * 4;

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
        const size_t chunkSize = std::min(
            remaining,
            static_cast<size_t>(std::numeric_limits<uInt>::max())
        );

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

    static bool DeriveArgon2BytesOnce(
        const std::string& passwordBytes,
        const std::string& saltBytes,
        const GB_Argon2::GB_Argon2Options& options,
        bool includeThreadsParam,
        std::string& outDerivedBytes
    )
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

    static bool DeriveArgon2Bytes(
        const std::string& passwordBytes,
        const std::string& saltBytes,
        const GB_Argon2::GB_Argon2Options& options,
        std::string& outDerivedBytes
    )
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

    static bool ValidateAesParams(
        const std::string& keyBytes,
        const std::string& ivBytes,
        const GB_AesOptions& options,
        size_t& outIvLength
    )
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
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&outBytes[0]), static_cast<int>(bytesCount)) != 1)
        {
            outBytes.clear();
            return false;
        }

        return true;
    }

    bool GB_AesEncrypt(
        const std::string& utf8PlainText,
        const std::string& keyBytes,
        const std::string& ivBytes,
        const GB_AesOptions& options,
        std::string& outCipherBytes,
        std::string& outGcmTagBytes
    )
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
                int aadOutLen = 0;
                if (::EVP_EncryptUpdate(
                    ctx.get(),
                    nullptr,
                    &aadOutLen,
                    reinterpret_cast<const unsigned char*>(normalizedOptions.aadBytes.data()),
                    static_cast<int>(normalizedOptions.aadBytes.size())) != 1)
                {
                    return false;
                }
            }

            // 5) Encrypt
            const int blockSize = ::EVP_CIPHER_block_size(cipher);
            size_t maxOutSize = utf8PlainText.size() + static_cast<size_t>(std::max(blockSize, 0));
            if (maxOutSize == 0)
            {
                maxOutSize = 1;
            }

            outCipherBytes.resize(maxOutSize);

            int outLen1 = 0;
            if (::EVP_EncryptUpdate(
                ctx.get(),
                reinterpret_cast<unsigned char*>(&outCipherBytes[0]),
                &outLen1,
                utf8PlainText.empty() ? nullptr : reinterpret_cast<const unsigned char*>(utf8PlainText.data()),
                static_cast<int>(utf8PlainText.size())) != 1)
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

            outCipherBytes.resize(static_cast<size_t>(outLen1 + outLen2));

            // 6) Get TAG
            if (normalizedOptions.gcmTagLength == 0 || normalizedOptions.gcmTagLength > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                outCipherBytes.clear();
                return false;
            }

            outGcmTagBytes.resize(normalizedOptions.gcmTagLength);

            if (::EVP_CIPHER_CTX_ctrl(
                ctx.get(),
                EVP_CTRL_GCM_GET_TAG,
                static_cast<int>(normalizedOptions.gcmTagLength),
                &outGcmTagBytes[0]) != 1)
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
        size_t maxOutSize = utf8PlainText.size() + static_cast<size_t>(std::max(blockSize, 0));
        if (maxOutSize == 0)
        {
            maxOutSize = 1;
        }

        outCipherBytes.resize(maxOutSize);

        int outLen1 = 0;
        if (::EVP_EncryptUpdate(
            ctx.get(),
            reinterpret_cast<unsigned char*>(&outCipherBytes[0]),
            &outLen1,
            utf8PlainText.empty() ? nullptr : reinterpret_cast<const unsigned char*>(utf8PlainText.data()),
            static_cast<int>(utf8PlainText.size())) != 1)
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

        outCipherBytes.resize(static_cast<size_t>(outLen1 + outLen2));
        return true;
    }

    bool GB_AesDecrypt(
        const std::string& cipherBytes,
        const std::string& keyBytes,
        const std::string& ivBytes,
        const GB_AesOptions& options,
        const std::string& gcmTagBytes,
        std::string& outUtf8PlainText
    )
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
                int aadOutLen = 0;
                if (::EVP_DecryptUpdate(
                    ctx.get(),
                    nullptr,
                    &aadOutLen,
                    reinterpret_cast<const unsigned char*>(normalizedOptions.aadBytes.data()),
                    static_cast<int>(normalizedOptions.aadBytes.size())) != 1)
                {
                    return false;
                }
            }

            // 5) Decrypt (Update)
            const int blockSize = ::EVP_CIPHER_block_size(cipher);
            size_t maxOutSize = cipherBytes.size() + static_cast<size_t>(std::max(blockSize, 0));
            if (maxOutSize == 0)
            {
                maxOutSize = 1;
            }

            outUtf8PlainText.resize(maxOutSize);

            int outLen1 = 0;
            if (::EVP_DecryptUpdate(
                ctx.get(),
                reinterpret_cast<unsigned char*>(&outUtf8PlainText[0]),
                &outLen1,
                cipherBytes.empty() ? nullptr : reinterpret_cast<const unsigned char*>(cipherBytes.data()),
                static_cast<int>(cipherBytes.size())) != 1)
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

            outUtf8PlainText.resize(static_cast<size_t>(outLen1 + outLen2));
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
        size_t maxOutSize = cipherBytes.size() + static_cast<size_t>(std::max(blockSize, 0));
        if (maxOutSize == 0)
        {
            maxOutSize = 1;
        }

        outUtf8PlainText.resize(maxOutSize);

        int outLen1 = 0;
        if (::EVP_DecryptUpdate(
            ctx.get(),
            reinterpret_cast<unsigned char*>(&outUtf8PlainText[0]),
            &outLen1,
            cipherBytes.empty() ? nullptr : reinterpret_cast<const unsigned char*>(cipherBytes.data()),
            static_cast<int>(cipherBytes.size())) != 1)
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

        outUtf8PlainText.resize(static_cast<size_t>(outLen1 + outLen2));
        return true;
    }

    std::string GB_AesEncryptToBase64(
        const std::string& utf8PlainText,
        const std::string& keyBytes,
        const std::string& ivBytes,
        const GB_AesOptions& options,
        std::string& outGcmTagBytes,
        bool urlSafe,
        bool noPadding
    )
    {
        outGcmTagBytes.clear();

        std::string cipherBytes;
        if (!GB_AesEncrypt(utf8PlainText, keyBytes, ivBytes, options, cipherBytes, outGcmTagBytes))
        {
            return std::string();
        }

        return GB_Base64Encode(cipherBytes, urlSafe, noPadding);
    }

    bool GB_AesDecryptFromBase64(
        const std::string& base64CipherText,
        const std::string& keyBytes,
        const std::string& ivBytes,
        const GB_AesOptions& options,
        const std::string& gcmTagBytes,
        std::string& outUtf8PlainText,
        bool urlSafe,
        bool noPadding
    )
    {
        outUtf8PlainText.clear();

        std::string cipherBytes;
        if (!GB_Base64Decode(base64CipherText, cipherBytes, urlSafe, noPadding))
        {
            return false;
        }

        return GB_AesDecrypt(cipherBytes, keyBytes, ivBytes, options, gcmTagBytes, outUtf8PlainText);
    }

    std::string GB_AesEncryptToBase64Packed(
        const std::string& utf8PlainText,
        const std::string& keyBytes,
        const GB_AesOptions& options,
        bool urlSafe,
        bool noPadding
    )
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

    bool GB_AesDecryptFromBase64Packed(
        const std::string& base64Packed,
        const std::string& keyBytes,
        const GB_AesOptions& options,
        std::string& outUtf8PlainText,
        bool urlSafe,
        bool noPadding
    )
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

    bool GB_DeriveAesKeyAndIv_Pbkdf2HmacSha256(
        const std::string& passwordUtf8,
        const std::string& saltBytes,
        uint32_t iterations,
        const GB_AesOptions& options,
        std::string& outKeyBytes,
        std::string& outIvBytes
    )
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


