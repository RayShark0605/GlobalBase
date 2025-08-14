#include "GB_Crypto.h"
#include <vector>
#include <cstdint>
#include <algorithm>
#include <stdint.h>
#include <limits>
#include <fstream>
#include <random>

#if defined(_WIN32)
#  include <windows.h>
#  include <bcrypt.h>
#  if defined(_MSC_VER)
#    pragma comment(lib, "bcrypt.lib")
#  endif
#endif

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

    // ---------- CSPRNG：优先系统随机，失败退回 random_device ----------
    static bool GetSecureRandom(uint8_t* buf, size_t len)
    {
#if defined(_WIN32)
        if (BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        {
            return true;
        }
#endif
#if !defined(_WIN32)
        ifstream urnd("/dev/urandom", ios::in | ios::binary);
        if (urnd.good())
        {
            urnd.read(reinterpret_cast<char*>(buf), static_cast<streamsize>(len));
            if (urnd.gcount() == static_cast<streamsize>(len))
            {
                return true;
            }
        }
#endif
        // 兜底：非阻塞非加密保证，仅作为最后手段
        random_device rd;
        for (size_t i = 0; i < len; i++)
        {
            buf[i] = static_cast<uint8_t>(rd());
        }
        return true;
    }

    // ---------- PKCS#7 ----------
    static void Pkcs7Pad(string& buf)
    {
        const size_t bs = 16;
        const size_t rem = buf.size() % bs;
        const uint8_t pad = static_cast<uint8_t>((rem == 0) ? bs : (bs - rem));
        for (size_t i = 0; i < static_cast<size_t>(pad); i++)
        {
            buf.push_back(static_cast<char>(pad));
        }
    }

    static bool Pkcs7Unpad(string& buf)
    {
        if (buf.empty() || (buf.size() % 16) != 0)
        {
            return false;
        }
        const uint8_t pad = static_cast<uint8_t>(buf.back());
        if (pad == 0 || pad > 16)
        {
            return false;
        }
        const size_t n = buf.size();
        for (size_t i = 0; i < static_cast<size_t>(pad); i++)
        {
            if (static_cast<uint8_t>(buf[n - 1 - i]) != pad)
            {
                return false;
            }
        }
        buf.resize(n - pad);
        return true;
    }

    // ---------- AES 常量（SBox/InvSBox/Rcon） ----------
    static const uint8_t kSBox[256] = {
        0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
        0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
        0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
        0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
        0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
        0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
        0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
        0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
        0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
        0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
        0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
        0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
        0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
        0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
        0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
        0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
    };
    static const uint8_t kInvSBox[256] = {
        0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
        0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
        0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
        0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
        0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
        0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
        0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
        0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
        0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
        0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
        0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
        0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
        0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
        0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
        0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
        0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
    };
    static const uint32_t kRcon[10] = {
        0x01000000u,0x02000000u,0x04000000u,0x08000000u,0x10000000u,
        0x20000000u,0x40000000u,0x80000000u,0x1B000000u,0x36000000u
    };

    static uint32_t RotWord(uint32_t w) { return (w << 8) | (w >> 24); }
    static uint32_t SubWord(uint32_t w)
    {
        return (uint32_t)kSBox[(w >> 24) & 0xFF] << 24
            | (uint32_t)kSBox[(w >> 16) & 0xFF] << 16
            | (uint32_t)kSBox[(w >> 8) & 0xFF] << 8
            | (uint32_t)kSBox[(w) & 0xFF];
    }
    static uint8_t GFMul(uint8_t a, uint8_t b)
    {
        uint8_t p = 0;
        for (int i = 0; i < 8; i++)
        {
            if (b & 1) { p ^= a; }
            const bool hi = (a & 0x80u) != 0;
            a <<= 1;
            if (hi) { a ^= 0x1Bu; }
            b >>= 1;
        }
        return p;
    }

    // 仅支持 AES-256（32 字节密钥）
    static bool ExpandKey256(const string& key32, vector<uint8_t>& roundKeys)
    {
        if (key32.size() != 32)
        {
            return false;
        }
        const int nb = 4, nk = 8, nr = 14;
        const int words = nb * (nr + 1);
        vector<uint32_t> w(words, 0);

        for (int i = 0; i < nk; i++)
        {
            const uint8_t b0 = (uint8_t)key32[4 * i + 0];
            const uint8_t b1 = (uint8_t)key32[4 * i + 1];
            const uint8_t b2 = (uint8_t)key32[4 * i + 2];
            const uint8_t b3 = (uint8_t)key32[4 * i + 3];
            w[i] = (uint32_t)b0 << 24 | (uint32_t)b1 << 16 | (uint32_t)b2 << 8 | (uint32_t)b3;
        }
        for (int i = nk; i < words; i++)
        {
            uint32_t temp = w[i - 1];
            if (i % nk == 0)
            {
                temp = SubWord(RotWord(temp)) ^ kRcon[(i / nk) - 1];
            }
            else if (i % nk == 4)
            {
                temp = SubWord(temp);
            }
            w[i] = w[i - nk] ^ temp;
        }

        roundKeys.resize(16 * (nr + 1));
        for (int r = 0; r <= nr; r++)
        {
            for (int c = 0; c < 4; c++)
            {
                const uint32_t word = w[r * 4 + c];
                roundKeys[r * 16 + 4 * c + 0] = (uint8_t)((word >> 24) & 0xFF);
                roundKeys[r * 16 + 4 * c + 1] = (uint8_t)((word >> 16) & 0xFF);
                roundKeys[r * 16 + 4 * c + 2] = (uint8_t)((word >> 8) & 0xFF);
                roundKeys[r * 16 + 4 * c + 3] = (uint8_t)((word) & 0xFF);
            }
        }
        return true;
    }

    static void AddRoundKey(uint8_t* s, const uint8_t* rk)
    {
        for (int i = 0; i < 16; i++) { s[i] ^= rk[i]; }
    }
    static void SubBytes(uint8_t* s)
    {
        for (int i = 0; i < 16; i++) { s[i] = kSBox[s[i]]; }
    }
    static void InvSubBytes(uint8_t* s)
    {
        for (int i = 0; i < 16; i++) { s[i] = kInvSBox[s[i]]; }
    }
    static void ShiftRows(uint8_t* s)
    {
        uint8_t t;
        t = s[1];  s[1] = s[5];  s[5] = s[9];  s[9] = s[13]; s[13] = t;
        uint8_t t0 = s[2], t1 = s[6];
        s[2] = s[10]; s[6] = s[14]; s[10] = t0; s[14] = t1;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
    }
    static void InvShiftRows(uint8_t* s)
    {
        uint8_t t;
        t = s[13]; s[13] = s[9];  s[9] = s[5];  s[5] = s[1];  s[1] = t;
        uint8_t t0 = s[2], t1 = s[6];
        s[2] = s[10]; s[6] = s[14]; s[10] = t0; s[14] = t1;
        t = s[3];  s[3] = s[7];  s[7] = s[11]; s[11] = s[15]; s[15] = t;
    }
    static void MixColumns(uint8_t* s)
    {
        for (int c = 0; c < 4; c++)
        {
            const int i = 4 * c;
            const uint8_t a0 = s[i + 0], a1 = s[i + 1], a2 = s[i + 2], a3 = s[i + 3];
            s[i + 0] = (uint8_t)(GFMul(a0, 2) ^ GFMul(a1, 3) ^ a2 ^ a3);
            s[i + 1] = (uint8_t)(a0 ^ GFMul(a1, 2) ^ GFMul(a2, 3) ^ a3);
            s[i + 2] = (uint8_t)(a0 ^ a1 ^ GFMul(a2, 2) ^ GFMul(a3, 3));
            s[i + 3] = (uint8_t)(GFMul(a0, 3) ^ a1 ^ a2 ^ GFMul(a3, 2));
        }
    }
    static void InvMixColumns(uint8_t* s)
    {
        for (int c = 0; c < 4; c++)
        {
            const int i = 4 * c;
            const uint8_t a0 = s[i + 0], a1 = s[i + 1], a2 = s[i + 2], a3 = s[i + 3];
            s[i + 0] = (uint8_t)(GFMul(a0, 14) ^ GFMul(a1, 11) ^ GFMul(a2, 13) ^ GFMul(a3, 9));
            s[i + 1] = (uint8_t)(GFMul(a0, 9) ^ GFMul(a1, 14) ^ GFMul(a2, 11) ^ GFMul(a3, 13));
            s[i + 2] = (uint8_t)(GFMul(a0, 13) ^ GFMul(a1, 9) ^ GFMul(a2, 14) ^ GFMul(a3, 11));
            s[i + 3] = (uint8_t)(GFMul(a0, 11) ^ GFMul(a1, 13) ^ GFMul(a2, 9) ^ GFMul(a3, 14));
        }
    }
    static void EncryptBlock(const uint8_t* in, uint8_t* out, const vector<uint8_t>& rk)
    {
        uint8_t s[16];
        for (int i = 0; i < 16; i++) { s[i] = in[i]; }
        const int nr = 14;

        AddRoundKey(s, &rk[0]);
        for (int r = 1; r < nr; r++)
        {
            SubBytes(s);
            ShiftRows(s);
            MixColumns(s);
            AddRoundKey(s, &rk[16 * r]);
        }
        SubBytes(s);
        ShiftRows(s);
        AddRoundKey(s, &rk[16 * nr]);

        for (int i = 0; i < 16; i++) { out[i] = s[i]; }
    }
    static void DecryptBlock(const uint8_t* in, uint8_t* out, const vector<uint8_t>& rk)
    {
        uint8_t s[16];
        for (int i = 0; i < 16; i++) { s[i] = in[i]; }
        const int nr = 14;

        AddRoundKey(s, &rk[16 * nr]);
        for (int r = nr - 1; r >= 1; r--)
        {
            InvShiftRows(s);
            InvSubBytes(s);
            AddRoundKey(s, &rk[16 * r]);
            InvMixColumns(s);
        }
        InvShiftRows(s);
        InvSubBytes(s);
        AddRoundKey(s, &rk[0]);

        for (int i = 0; i < 16; i++) { out[i] = s[i]; }
    }

    // ---------- 规范化（按你提出的规则） ----------
    static bool NormalizeKey32(const string& keyMaterial, bool flexible, string& keyOut)
    {
        if (!flexible)
        {
            if (keyMaterial.size() != 32) { return false; }
            keyOut = keyMaterial;
            return true;
        }
        keyOut.clear();
        if (keyMaterial.size() >= 32)
        {
            keyOut.assign(keyMaterial.data(), keyMaterial.data() + 32); // 截断
        }
        else
        {
            keyOut = keyMaterial;
            keyOut.resize(32, '\0'); // 后补 0
        }
        return true;
    }

    static bool NormalizeIv16(const string& ivMaterial, bool flexible, string& ivOut, bool& usedRandom)
    {
        usedRandom = false;
        if (ivMaterial.empty())
        {
            ivOut.resize(16);
            GetSecureRandom(reinterpret_cast<uint8_t*>(&ivOut[0]), 16);
            usedRandom = true;
            return true;
        }
        if (!flexible)
        {
            if (ivMaterial.size() != 16) { return false; }
            ivOut = ivMaterial;
            return true;
        }
        if (ivMaterial.size() >= 16)
        {
            ivOut.assign(ivMaterial.data(), ivMaterial.data() + 16); // 截断
        }
        else
        {
            ivOut = ivMaterial;
            ivOut.resize(16, '\0'); // 后补 0
        }
        // 注意：这里会导致确定性 IV，不安全；仅在你明确需要兼容时启用 flexible。
        return true;
    }
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

string GB_Aes256Encrypt(const string& plainBytes, const string& keyMaterial, const string& ivMaterial, bool urlSafe, bool noPadding, bool flexibleKeyIv)
{
    // 1) 规范化 key/iv（按需截断/补0；iv 为空则随机）
    string key32;
    if (!internal::NormalizeKey32(keyMaterial, flexibleKeyIv, key32))
    {
        return {};
    }
    string iv16;
    bool usedRandom = false;
    if (!internal::NormalizeIv16(ivMaterial, flexibleKeyIv, iv16, usedRandom))
    {
        return {};
    }

    // 2) 展开轮密钥（AES-256）
    vector<uint8_t> rk;
    if (!internal::ExpandKey256(key32, rk))
    {
        return {};
    }

    // 3) PKCS#7 填充
    string buf = plainBytes;
    internal::Pkcs7Pad(buf); // PKCS#7（RFC 5652）

    // 4) CBC 加密（IV 前缀）
    string out;
    out.resize(iv16.size() + buf.size());

    // 前 16 字节写入 IV
    for (int i = 0; i < 16; i++) { out[i] = iv16[i]; }

    uint8_t prev[16];
    for (int i = 0; i < 16; i++) { prev[i] = (uint8_t)iv16[i]; }

    for (size_t off = 0; off < buf.size(); off += 16)
    {
        uint8_t block[16];
        for (int i = 0; i < 16; i++) { block[i] = (uint8_t)buf[off + i] ^ prev[i]; }
        uint8_t enc[16];
        internal::EncryptBlock(block, enc, rk);
        for (int i = 0; i < 16; i++)
        {
            out[16 + off + i] = (char)enc[i];
            prev[i] = enc[i];
        }
    }

    // 5) Base64 输出
    return GB_Base64Encode(out, urlSafe, noPadding);
}

string GB_Aes256Decrypt(const string& base64CipherWithIv, const string& keyMaterial, bool strictMode, bool urlSafe, bool noPadding, bool flexibleKey)
{
    const string all = GB_Base64Decode(base64CipherWithIv, strictMode, urlSafe, noPadding);
    if (all.size() < 16 || ((all.size() - 16) % 16) != 0)
    {
        return {};
    }

    // 1) 取回前缀 IV
    string iv16(all.data(), all.data() + 16);
    const string cipher(all.data() + 16, all.data() + all.size());

    // 2) 规范化 key
    string key32;
    if (!internal::NormalizeKey32(keyMaterial, flexibleKey, key32))
    {
        return {};
    }

    // 3) 展开轮密钥
    vector<uint8_t> rk;
    if (!internal::ExpandKey256(key32, rk))
    {
        return {};
    }

    // 4) CBC 解密
    string out;
    out.resize(cipher.size());

    uint8_t prev[16];
    for (int i = 0; i < 16; i++) { prev[i] = (uint8_t)iv16[i]; }

    for (size_t off = 0; off < cipher.size(); off += 16)
    {
        uint8_t dec[16];
        internal::DecryptBlock(reinterpret_cast<const uint8_t*>(cipher.data() + off), dec, rk);
        for (int i = 0; i < 16; i++)
        {
            out[off + i] = (char)(dec[i] ^ prev[i]);
        }
        for (int i = 0; i < 16; i++) { prev[i] = (uint8_t)cipher[off + i]; }
    }

    // 5) 去 PKCS#7
    if (!internal::Pkcs7Unpad(out))
    {
        return {};
    }
    return out;
}








