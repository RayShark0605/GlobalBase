#include "GB_Crypto.h"
#include <vector>
#include <cstdint>
#include <algorithm>

using namespace std;

namespace internal
{
    // 忽略常见 ASCII 空白：空格、水平制表、回车、换行、垂直制表、换页
    static bool IsAsciiSpace(unsigned char ch)
    {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
    }

    enum class DecodeTableKind
    {
        Std,    // A-Z a-z 0-9 + /
        Url,    // A-Z a-z 0-9 - _
        Both    // 两者都接受（非严格模式下更宽松）
    };

    static const unsigned char* GetB64DecodeTable(DecodeTableKind kind)
    {
        static unsigned char tableStd[256];
        static unsigned char tableUrl[256];
        static unsigned char tableBoth[256];
        static bool inited = false;

        if (!inited)
        {
            fill(tableStd, tableStd + 256, 0xFF);
            fill(tableUrl, tableUrl + 256, 0xFF);
            fill(tableBoth, tableBoth + 256, 0xFF);

            static const char* alphaStd = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            static const char* alphaUrl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

            for (int i = 0; i < 64; ++i)
            {
                tableStd[static_cast<unsigned char>(alphaStd[i])] = static_cast<unsigned char>(i);
                tableUrl[static_cast<unsigned char>(alphaUrl[i])] = static_cast<unsigned char>(i);
                tableBoth[static_cast<unsigned char>(alphaStd[i])] = static_cast<unsigned char>(i);
                tableBoth[static_cast<unsigned char>(alphaUrl[i])] = static_cast<unsigned char>(i);
            }
            inited = true;
        }

        switch (kind)
        {
        case DecodeTableKind::Std:  return tableStd;
        case DecodeTableKind::Url:  return tableUrl;
        default:                    return tableBoth;
        }
    }

    static uint32_t RotL32(uint32_t x, uint32_t s)
    {
        return (x << s) | (x >> (32u - s));
    }

    static uint32_t LoadLE32(const uint8_t* p)
    {
        return (uint32_t)p[0]
            | ((uint32_t)p[1] << 8)
            | ((uint32_t)p[2] << 16)
            | ((uint32_t)p[3] << 24);
    }

    static void StoreLE32(uint32_t v, uint8_t* p)
    {
        p[0] = (uint8_t)(v & 0xFFu);
        p[1] = (uint8_t)((v >> 8) & 0xFFu);
        p[2] = (uint8_t)((v >> 16) & 0xFFu);
        p[3] = (uint8_t)((v >> 24) & 0xFFu);
    }

    // 每轮的循环左移位数（RFC 1321 / 公开资料一致）
    static const uint32_t S[64] =
    {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };

    // 常量表 K[i] = floor(2^32 * abs(sin(i+1)))，使用预计算值
    static const uint32_t K[64] =
    {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
        0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
        0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
        0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
        0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
        0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };

    static inline uint32_t RotR32(uint32_t x, uint32_t n)
    {
        return (x >> n) | (x << (32u - n));
    }
    static inline uint64_t RotR64(uint64_t x, uint64_t n)
    {
        return (x >> n) | (x << (64u - n));
    }
    static inline uint32_t LoadBE32(const uint8_t* p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    static inline void StoreBE32(uint32_t v, uint8_t* p)
    {
        p[0] = uint8_t((v >> 24) & 0xFFu);
        p[1] = uint8_t((v >> 16) & 0xFFu);
        p[2] = uint8_t((v >> 8) & 0xFFu);
        p[3] = uint8_t(v & 0xFFu);
    }
    static inline uint64_t LoadBE64(const uint8_t* p)
    {
        return (uint64_t(p[0]) << 56) | (uint64_t(p[1]) << 48) | (uint64_t(p[2]) << 40) | (uint64_t(p[3]) << 32) |
            (uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) | (uint64_t(p[6]) << 8) | uint64_t(p[7]);
    }
    static inline void StoreBE64(uint64_t v, uint8_t* p)
    {
        p[0] = uint8_t((v >> 56) & 0xFFu);
        p[1] = uint8_t((v >> 48) & 0xFFu);
        p[2] = uint8_t((v >> 40) & 0xFFu);
        p[3] = uint8_t((v >> 32) & 0xFFu);
        p[4] = uint8_t((v >> 24) & 0xFFu);
        p[5] = uint8_t((v >> 16) & 0xFFu);
        p[6] = uint8_t((v >> 8) & 0xFFu);
        p[7] = uint8_t(v & 0xFFu);
    }

    // -------- SHA-256 专用 --------
    static inline uint32_t Ch32(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    static inline uint32_t Maj32(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
    static inline uint32_t BigSigma0_32(uint32_t x) { return RotR32(x, 2) ^ RotR32(x, 13) ^ RotR32(x, 22); } // Σ0
    static inline uint32_t BigSigma1_32(uint32_t x) { return RotR32(x, 6) ^ RotR32(x, 11) ^ RotR32(x, 25); } // Σ1
    static inline uint32_t SmallSigma0_32(uint32_t x) { return RotR32(x, 7) ^ RotR32(x, 18) ^ (x >> 3); }     // σ0
    static inline uint32_t SmallSigma1_32(uint32_t x) { return RotR32(x, 17) ^ RotR32(x, 19) ^ (x >> 10); }   // σ1

    static const uint32_t K256[64] =
    {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };

    // -------- SHA-512 专用 --------
    static inline uint64_t Ch64(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (~x & z); }
    static inline uint64_t Maj64(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (x & z) ^ (y & z); }
    static inline uint64_t BigSigma0_64(uint64_t x) { return RotR64(x, 28) ^ RotR64(x, 34) ^ RotR64(x, 39); } // Σ0
    static inline uint64_t BigSigma1_64(uint64_t x) { return RotR64(x, 14) ^ RotR64(x, 18) ^ RotR64(x, 41); } // Σ1
    static inline uint64_t SmallSigma0_64(uint64_t x) { return RotR64(x, 1) ^ RotR64(x, 8) ^ (x >> 7); }       // σ0
    static inline uint64_t SmallSigma1_64(uint64_t x) { return RotR64(x, 19) ^ RotR64(x, 61) ^ (x >> 6); }     // σ1

    static const uint64_t K512[80] =
    {
        0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
        0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
        0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
        0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
    };
}

string GB_Base64Encode(const string& bytes, bool urlSafe, bool noPadding)
{
    const char* alphabet = urlSafe
        ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const unsigned char* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t len = bytes.size();

    string out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= len)
    {
        uint32_t chunk = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
        out.push_back(alphabet[(chunk >> 18) & 0x3F]);
        out.push_back(alphabet[(chunk >> 12) & 0x3F]);
        out.push_back(alphabet[(chunk >> 6) & 0x3F]);
        out.push_back(alphabet[chunk & 0x3F]);
        i += 3;
    }

    const size_t remain = len - i;
    if (remain == 1)
    {
        uint32_t chunk = (uint32_t(data[i]) << 16);
        out.push_back(alphabet[(chunk >> 18) & 0x3F]);
        out.push_back(alphabet[(chunk >> 12) & 0x3F]);
        if (!noPadding)
        {
            out.push_back('=');
            out.push_back('=');
        }
    }
    else if (remain == 2)
    {
        uint32_t chunk = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(alphabet[(chunk >> 18) & 0x3F]);
        out.push_back(alphabet[(chunk >> 12) & 0x3F]);
        out.push_back(alphabet[(chunk >> 6) & 0x3F]);
        if (!noPadding)
        {
            out.push_back('=');
        }
    }

    return out;
}

string GB_Base64Decode(const string& base64Info, bool strictMode, bool urlSafe, bool noPadding)
{
    using internal::DecodeTableKind;

    const unsigned char* table = internal::GetB64DecodeTable(
        strictMode ? (urlSafe ? DecodeTableKind::Url : DecodeTableKind::Std)
        : DecodeTableKind::Both
    );

    // 1) 过滤空白（严格模式下不允许出现任何空白）
    string filtered;
    filtered.reserve(base64Info.size());
    for (unsigned char ch : base64Info)
    {
        if (internal::IsAsciiSpace(ch))
        {
            if (strictMode)
            {
                return {}; // 严格模式：空白即非法
            }
            continue;       // 宽松模式：忽略空白（兼容 MIME）
        }
        filtered.push_back(static_cast<char>(ch));
    }

    // 2) 严格 + “需要填充”的模式：长度必须是 4 的倍数
    const size_t origMod = filtered.size() % 4;
    if (strictMode && !noPadding && (origMod != 0))
    {
        return {};
    }
    // 严格 + “无填充”模式：禁止出现 '='
    if (strictMode && noPadding)
    {
        if (filtered.find('=') != string::npos)
        {
            return {};
        }
    }

    // 3) 根据长度自动补齐（RFC 4648 常见实践；URL-safe 常省略 '='）
    if (origMod == 1)
    {
        return {}; // 不可恢复的非法长度
    }
    else if (origMod == 2)
    {
        filtered.append("==");
    }
    else if (origMod == 3)
    {
        filtered.push_back('=');
    }

    // 4) 预估输出容量（考虑 '=' 填充）
    const size_t blocks = filtered.size() / 4;
    size_t reserveBytes = blocks * 3;
    if (!filtered.empty())
    {
        if (filtered.back() == '=') { reserveBytes--; }
        if (filtered.size() >= 2 && filtered[filtered.size() - 2] == '=') { reserveBytes--; }
    }

    string out;
    out.reserve(reserveBytes);

    // 5) 分组解码（严格模式下校验“零填充位”）
    for (size_t i = 0; i < filtered.size(); i += 4)
    {
        const unsigned char c0 = static_cast<unsigned char>(filtered[i + 0]);
        const unsigned char c1 = static_cast<unsigned char>(filtered[i + 1]);
        const unsigned char c2 = static_cast<unsigned char>(filtered[i + 2]);
        const unsigned char c3 = static_cast<unsigned char>(filtered[i + 3]);

        if (c0 == '=' || c1 == '=')
        {
            return {}; // '=' 只能出现在末两位
        }

        const unsigned char v0 = table[c0];
        const unsigned char v1 = table[c1];
        if (v0 == 0xFF || v1 == 0xFF)
        {
            return {}; // 非法字符
        }

        if (c2 == '=' && c3 != '=')
        {
            return {}; // 若第 3 位是 '='，第 4 位也必须是 '='
        }

        if (c2 == '=' && c3 == '=')
        {
            // 只有 1 个输出字节；严格模式下校验 v1 低 4 位应为 0
            if (strictMode && ((v1 & 0x0F) != 0))
            {
                return {};
            }
            out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
            continue;
        }

        const unsigned char v2 = table[c2];
        if (v2 == 0xFF)
        {
            return {};
        }

        if (c3 == '=')
        {
            // 2 个输出字节；严格模式下校验 v2 低 2 位应为 0
            if (strictMode && ((v2 & 0x03) != 0))
            {
                return {};
            }
            out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
            out.push_back(static_cast<char>(((v1 & 0x0F) << 4) | (v2 >> 2)));
            continue;
        }

        const unsigned char v3 = table[c3];
        if (v3 == 0xFF)
        {
            return {};
        }

        // 3 个输出字节
        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
        out.push_back(static_cast<char>(((v1 & 0x0F) << 4) | (v2 >> 2)));
        out.push_back(static_cast<char>(((v2 & 0x03) << 6) | v3));
    }

    return out;
}

string GB_GetMd5(const string& input)
{
    // 1) 拷贝并填充：追加 0x80，再补 0x00 至 (len % 64 == 56)
    vector<uint8_t> buf;
    buf.reserve(input.size() + 64);
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(input.data()),
        reinterpret_cast<const uint8_t*>(input.data()) + input.size());

    buf.push_back(0x80u);
    while ((buf.size() % 64u) != 56u)
    {
        buf.push_back(0x00u);
    }

    // 2) 追加原消息长度（比特数）的小端 64 位（按 2^64 取模）
    const uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8ull;
    for (int i = 0; i < 8; ++i)
    {
        buf.push_back(static_cast<uint8_t>((bitLen >> (8 * i)) & 0xFFu));
    }

    // 3) 初始化 IV
    uint32_t a0 = 0x67452301u;
    uint32_t b0 = 0xefcdab89u;
    uint32_t c0 = 0x98badcfeu;
    uint32_t d0 = 0x10325476u;

    // 4) 逐块（64 字节）压缩
    for (size_t off = 0; off < buf.size(); off += 64)
    {
        uint32_t m[16];
        for (int j = 0; j < 16; ++j)
        {
            m[j] = internal::LoadLE32(&buf[off + j * 4]);
        }

        uint32_t a = a0, b = b0, c = c0, d = d0;

        for (uint32_t i = 0; i < 64; ++i)
        {
            uint32_t f, g;

            if (i < 16)
            {
                f = (b & c) | (~b & d);
                g = i;
            }
            else if (i < 32)
            {
                f = (d & b) | (~d & c);
                g = (5u * i + 1u) & 0x0Fu;
            }
            else if (i < 48)
            {
                f = b ^ c ^ d;
                g = (3u * i + 5u) & 0x0Fu;
            }
            else
            {
                f = c ^ (b | ~d);
                g = (7u * i) & 0x0Fu;
            }

            uint32_t temp = d;
            f = f + a + internal::K[i] + m[g];
            d = c;
            c = b;
            b = b + internal::RotL32(f, internal::S[i]);
            a = temp;
        }

        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    // 5) 输出：A||B||C||D 的小端字节序，再转十六进制小写
    uint8_t digest[16];
    internal::StoreLE32(a0, &digest[0]);
    internal::StoreLE32(b0, &digest[4]);
    internal::StoreLE32(c0, &digest[8]);
    internal::StoreLE32(d0, &digest[12]);

    static const char* hexDigits = "0123456789abcdef";
    string hex(32, '\0');
    for (int i = 0; i < 16; ++i)
    {
        hex[2 * i + 0] = hexDigits[digest[i] >> 4];
        hex[2 * i + 1] = hexDigits[digest[i] & 0x0F];
    }
    return hex;
}

string GB_GetSha256(const string& input)
{
    // 1) 填充：追加 0x80，再补 0x00 直至 (len % 64 == 56)，随后追加 64 位“比特长度”的大端
    vector<uint8_t> buf;
    buf.reserve(input.size() + 64);
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(input.data()), reinterpret_cast<const uint8_t*>(input.data()) + input.size());
    buf.push_back(0x80u);
    while ((buf.size() % 64u) != 56u)
    {
        buf.push_back(0x00u);
    }
    const uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8ull;
    uint8_t len64[8];
    internal::StoreBE64(bitLen, len64);
    buf.insert(buf.end(), len64, len64 + 8);

    // 2) 初始向量（FIPS 180-4）
    uint32_t h0 = 0x6a09e667u, h1 = 0xbb67ae85u, h2 = 0x3c6ef372u, h3 = 0xa54ff53au;
    uint32_t h4 = 0x510e527fu, h5 = 0x9b05688cu, h6 = 0x1f83d9abu, h7 = 0x5be0cd19u;

    // 3) 逐块压缩
    uint32_t w[64];
    for (size_t off = 0; off < buf.size(); off += 64)
    {
        for (int t = 0; t < 16; ++t)
        {
            w[t] = internal::LoadBE32(&buf[off + t * 4]);
        }
        for (int t = 16; t < 64; ++t)
        {
            w[t] = internal::SmallSigma1_32(w[t - 2]) + w[t - 7] + internal::SmallSigma0_32(w[t - 15]) + w[t - 16];
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;

        for (int t = 0; t < 64; ++t)
        {
            uint32_t t1 = h + internal::BigSigma1_32(e) + internal::Ch32(e, f, g) + internal::K256[t] + w[t];
            uint32_t t2 = internal::BigSigma0_32(a) + internal::Maj32(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e; h5 += f; h6 += g; h7 += h;
    }

    // 4) 输出（大端序）-> 小写十六进制
    uint8_t digest[32];
    internal::StoreBE32(h0, &digest[0]);
    internal::StoreBE32(h1, &digest[4]);
    internal::StoreBE32(h2, &digest[8]);
    internal::StoreBE32(h3, &digest[12]);
    internal::StoreBE32(h4, &digest[16]);
    internal::StoreBE32(h5, &digest[20]);
    internal::StoreBE32(h6, &digest[24]);
    internal::StoreBE32(h7, &digest[28]);

    static const char* hexDigits = "0123456789abcdef";
    string hex(64, '\0');
    for (int i = 0; i < 32; ++i)
    {
        hex[2 * i + 0] = hexDigits[digest[i] >> 4];
        hex[2 * i + 1] = hexDigits[digest[i] & 0x0F];
    }
    return hex;
}

string GB_GetSha512(const string& input)
{
    // 1) 填充：追加 0x80，再补 0x00 直至 (len % 128 == 112)，随后追加 128 位“比特长度”的大端
    vector<uint8_t> buf;
    buf.reserve(input.size() + 128);
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(input.data()),
        reinterpret_cast<const uint8_t*>(input.data()) + input.size());
    buf.push_back(0x80u);
    while ((buf.size() % 128u) != 112u)
    {
        buf.push_back(0x00u);
    }
    // 128-bit length = high(64) || low(64). 对于一般应用，输入长度 < 2^61 字节，故：
    const uint64_t bytes = static_cast<uint64_t>(input.size());
    const uint64_t bitLenLow = bytes << 3;         // 低 64 位
    const uint64_t bitLenHigh = bytes >> 61;        // 左移3越界的高位（通常为0）
    uint8_t len128[16];
    internal::StoreBE64(bitLenHigh, &len128[0]);
    internal::StoreBE64(bitLenLow, &len128[8]);
    buf.insert(buf.end(), len128, len128 + 16);

    // 2) 初始向量（FIPS 180-4）
    uint64_t h0 = 0x6a09e667f3bcc908ULL, h1 = 0xbb67ae8584caa73bULL;
    uint64_t h2 = 0x3c6ef372fe94f82bULL, h3 = 0xa54ff53a5f1d36f1ULL;
    uint64_t h4 = 0x510e527fade682d1ULL, h5 = 0x9b05688c2b3e6c1fULL;
    uint64_t h6 = 0x1f83d9abfb41bd6bULL, h7 = 0x5be0cd19137e2179ULL;

    // 3) 逐块压缩
    uint64_t w[80];
    for (size_t off = 0; off < buf.size(); off += 128)
    {
        for (int t = 0; t < 16; ++t)
        {
            w[t] = internal::LoadBE64(&buf[off + t * 8]);
        }
        for (int t = 16; t < 80; ++t)
        {
            w[t] = internal::SmallSigma1_64(w[t - 2]) + w[t - 7] + internal::SmallSigma0_64(w[t - 15]) + w[t - 16];
        }

        uint64_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;

        for (int t = 0; t < 80; ++t)
        {
            uint64_t t1 = h + internal::BigSigma1_64(e) + internal::Ch64(e, f, g) + internal::K512[t] + w[t];
            uint64_t t2 = internal::BigSigma0_64(a) + internal::Maj64(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e; h5 += f; h6 += g; h7 += h;
    }

    // 4) 输出（大端序）-> 小写十六进制
    uint8_t digest[64];
    internal::StoreBE64(h0, &digest[0]);
    internal::StoreBE64(h1, &digest[8]);
    internal::StoreBE64(h2, &digest[16]);
    internal::StoreBE64(h3, &digest[24]);
    internal::StoreBE64(h4, &digest[32]);
    internal::StoreBE64(h5, &digest[40]);
    internal::StoreBE64(h6, &digest[48]);
    internal::StoreBE64(h7, &digest[56]);

    static const char* hexDigits = "0123456789abcdef";
    string hex(128, '\0');
    for (int i = 0; i < 64; ++i)
    {
        hex[2 * i + 0] = hexDigits[digest[i] >> 4];
        hex[2 * i + 1] = hexDigits[digest[i] & 0x0F];
    }
    return hex;
}










