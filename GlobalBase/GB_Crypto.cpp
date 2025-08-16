#include "GB_Crypto.h"
#include <vector>
#include <cstdint>
#include <algorithm>
#include <stdint.h>
#include <limits>
#include <fstream>
#include <random>
#include <cassert>
#include <sstream>

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

    // ---------- 规范化 ----------
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

    class BigInt
    {

    public:
        typedef unsigned long base_t;
        static int base_char;
        static int base;
        static int basebitnum;
        static int basebitchar;
        static int basebit;
    private:
        friend class Rsa;
    public:
        friend BigInt operator + (const BigInt& a, const BigInt& b);
        friend BigInt operator - (const BigInt& a, const BigInt& b);
        friend BigInt operator * (const BigInt& a, const BigInt& b);
        friend BigInt operator / (const BigInt& a, const BigInt& b);
        friend BigInt operator % (const BigInt& a, const BigInt& b);
        friend bool operator < (const BigInt& a, const BigInt& b);
        friend bool operator <= (const BigInt& a, const BigInt& b);
        friend bool operator == (const BigInt& a, const BigInt& b);
        friend bool operator != (const BigInt& a, const BigInt& b) { return !(a == b); }
        // 重载版本	
        friend BigInt operator + (const BigInt& a, const long b) { BigInt t(b); return a + t; }
        friend BigInt operator - (const BigInt& a, const long b) { BigInt t(b); return a - t; }
        friend BigInt operator * (const BigInt& a, const long b) { BigInt t(b); return a * t; }
        friend BigInt operator / (const BigInt& a, const long b) { BigInt t(b); return a / t; }
        friend BigInt operator % (const BigInt& a, const long b) { BigInt t(b); return a % t; }
        friend bool operator < (const BigInt& a, const long b) { BigInt t(b); return a < t; }
        friend bool operator <= (const BigInt& a, const  long b) { BigInt t(b); return a <= t; }
        friend bool operator == (const BigInt& a, const long b) { BigInt t(b); return a == t; }
        friend bool operator != (const BigInt& a, const long b) { BigInt t(b); return !(a == t); };
        friend ostream& operator << (ostream& out, const BigInt& a);
        friend BigInt operator << (const BigInt& a, unsigned int n);
    public:
        typedef vector<base_t> data_t;

        typedef const vector<base_t> const_data_t;
        BigInt& trim()
        {
            int count = 0;
            // 检查不为0的元素的数量
            for (data_t::reverse_iterator it = _data.rbegin(); it != _data.rend(); it++)
            {
                if ((*it) == 0)
                {
                    count++;
                }
                else
                {
                    break;
                }
            }
            if (count == _data.size())// 只有零的情况保留
            {
                count--;
            }
            for (int i = 0; i < count; i++)
            {
                _data.pop_back();
            }
            return *this;
        }
        friend class bit;
        class bit
        {
        public:
            size_t size();
            bool at(size_t i);
            bit(const BigInt& a);
        private:
            vector<base_t> _bitvec;
            size_t _size;
        };
        //大数幂模运算	
        BigInt moden(const BigInt& exp, const BigInt& p)const;
        /* 用扩展的欧几里得算法求乘法逆元 */
        BigInt extendEuclid(const BigInt& m);
    public:
        BigInt() :_isnegative(false) { _data.push_back(0); }

        BigInt(const string& num) :_data(), _isnegative(false) { copyFromHexString(num); trim(); }

        BigInt(const long n) :_isnegative(false) { copyFromLong(n); }

        BigInt(const_data_t data) :_data(data), _isnegative(false) { trim(); }

        BigInt& operator =(string s)
        {
            _data.clear();
            _isnegative = false;
            copyFromHexString(s);
            trim();
            return *this;
        }
        BigInt(const BigInt& a, bool isnegative) :_data(a._data), _isnegative(isnegative) {}
        BigInt& operator =(const long n)
        {
            _data.clear();
            copyFromLong(n);
            return *this;
        }
    public:
        static BigInt Zero;
        static BigInt One;
        static BigInt Two;

        uint32_t modUint(uint32_t m) const
        {
            if (m == 0)
            {
                return 0;
            }
            uint64_t r = 0;
            for (long long i = static_cast<long long>(_data.size()) - 1; i >= 0; --i)
            {
                r = ((r << 32) + _data[static_cast<size_t>(i)]) % m; // 每 limb 32 位
            }
            return static_cast<uint32_t>(r);
        }
    private:
        bool smallThan(const BigInt& a) const; // 判断绝对值是否小于
        bool smallOrEquals(const BigInt& a) const; // 判断绝对值是否小于相等
        bool equals(const BigInt& a) const; // 判断绝对值是否相等

        BigInt& leftShift(const unsigned int n);
        BigInt& rightShift(const unsigned int n);
        BigInt& add(const BigInt& b);

        BigInt& sub(const BigInt& b);

        void copyFromHexString(const string& s)
        {
            string str(s);
            if (str.length() && str.at(0) == '-')
            {
                if (str.length() > 1)
                    _isnegative = true;
                str = str.substr(1);
            }
            int count = (8 - (str.length() % 8)) % 8;
            string temp;

            for (int i = 0; i < count; ++i)
                temp.push_back(0);

            str = temp + str;

            for (int i = 0; i < str.length(); i += BigInt::base_char)
            {
                base_t sum = 0;
                for (int j = 0; j < base_char; ++j)
                {
                    char ch = str[i + j];

                    ch = hex2Uchar(ch);
                    sum = ((sum << 4) | (ch));
                }
                _data.push_back(sum);
            }
            reverse(_data.begin(), _data.end());
        }
        char hex2Uchar(char ch)
        {
            static char table[] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
            if (isdigit(ch))
                ch -= '0';
            else if (islower(ch))
                ch -= 'a' - 10;
            else if (isupper(ch))
                ch -= 'A' - 10;

            return table[ch];
        }

        void copyFromLong(const long n)
        {
            long a = n;
            if (a < 0)
            {
                _isnegative = true;
                a = -a;
            }
            do
            {
                BigInt::base_t ch = (a & (BigInt::base));
                _data.push_back(ch);
                a = a >> (BigInt::basebitnum);
            } while (a);
        }
        static void div(const BigInt& a, const BigInt& b, BigInt& result, BigInt& ca);
        
    private:
        vector<base_t> _data;
        //数据存储	
        bool _isnegative;
    };

    int BigInt::base_char = 8;
    int BigInt::base = 0xFFFFFFFF;
    int BigInt::basebit = 5; // 2^5
    int BigInt::basebitchar = 0x1F;
    int BigInt::basebitnum = 32;
    BigInt BigInt::Zero(0);
    BigInt BigInt::One(1);
    BigInt BigInt::Two(2);

    BigInt operator + (const BigInt& a, const BigInt& b)
    {
        BigInt ca(a);
        return ca.add(b);
    }

    BigInt operator - (const BigInt& a, const BigInt& b)
    {
        BigInt ca(a);
        return ca.sub(b);
    }

#ifdef small
#undef small
#endif
    BigInt operator * (const BigInt& a, const BigInt& b)
    {
        if (a == (BigInt::Zero) || b == (BigInt::Zero))
            return BigInt::Zero;

        const BigInt& big = a._data.size() > b._data.size() ? a : b;
        const BigInt& small = (&big) == (&a) ? b : a;

        BigInt result(0);

        BigInt::bit bt(small);
        for (long long i = bt.size() - 1; i >= 0; i--)
        {
            if (bt.at(i))
            {
                BigInt temp(big, false);
                temp.leftShift(static_cast<unsigned int>(i));
                result.add(temp);
            }
        }
        result._isnegative = !(a._isnegative == b._isnegative);
        return result;
    }

    BigInt operator / (const BigInt& a, const BigInt& b)
    {
        assert(b != (BigInt::Zero));
        if (a.equals(b)) // 绝对值相等
        {
            return (a._isnegative == b._isnegative) ? BigInt(1) : BigInt(-1);
        }
        else if (a.smallThan(b)) // 绝对值小于
        {
            return BigInt::Zero;
        }
        else
        {
            BigInt result, ca;
            BigInt::div(a, b, result, ca);
            return result;
        }
    }

    BigInt operator % (const BigInt& a, const BigInt& b)
    {
        assert(b != (BigInt::Zero));
        if (a.equals(b))
        {
            return BigInt::Zero;
        }
        else if (a.smallThan(b))
        {
            return a;
        }
        else
        {
            BigInt result, ca;
            BigInt::div(a, b, result, ca);
            return ca;
        }
    }

    void BigInt::div(const BigInt& a, const BigInt& b, BigInt& result, BigInt& ca)
    {
        // 1.复制a,b
        BigInt cb(b, false);
        ca._isnegative = false;
        ca._data = a._data;

        BigInt::bit bit_b(cb);
        // 位数对齐
        while (true) // 绝对值小于
        {
            BigInt::bit bit_a(ca);
            long long len = bit_a.size() - bit_b.size();
            BigInt temp;
            // 找到移位的
            while (len >= 0)
            {
                temp = cb << static_cast<unsigned int>(len);
                if (temp.smallOrEquals(ca))
                {
                    break;
                }
                len--;
            }
            if (len < 0)
            {
                break;
            }
            BigInt::base_t n = 0;
            while (temp.smallOrEquals(ca))
            {
                ca.sub(temp);
                n++;
            }
            BigInt kk(n);
            if (len)
            {
                kk.leftShift(static_cast<unsigned int>(len));
            }
            result.add(kk);
        }
        result.trim();
    }

    bool operator < (const BigInt& a, const BigInt& b)
    {
        if (a._isnegative == b._isnegative)
        {
            if (!a._isnegative)
            {
                return a.smallThan(b);
            }
            else
            {
                return !(a.smallOrEquals(b));
            }
        }
        else
        {
            return !a._isnegative;
        }
    }

    bool operator <= (const BigInt& a, const BigInt& b)
    {
        if (a._isnegative == b._isnegative)
        {   // 同号
            if (!a._isnegative)
            {
                return a.smallOrEquals(b);
            }
            else
            {
                return !(a.smallThan(b));
            }
        }
        else // 异号
        {
            return !a._isnegative;
        }
    }

    bool operator == (const BigInt& a, const BigInt& b)
    {
        return a._data == b._data && a._isnegative == b._isnegative;
    }

    ostream& operator << (ostream& out, const BigInt& a)
    {
        static char hex[] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
        if (a._isnegative)
        {
            out << "-";
        }
        BigInt::base_t T = 0x0F;
        string str;
        for (BigInt::data_t::const_iterator it = a._data.begin(); it != a._data.end(); it++)
        {
            BigInt::base_t ch = (*it);
            for (int j = 0; j < BigInt::base_char; j++)
            {
                str.push_back(hex[ch & (T)]);
                ch = ch >> 4;
            }
        }
        reverse(str.begin(), str.end());
        out << str;
        return out;
    }

    BigInt operator <<(const BigInt& a, unsigned int n)
    {
        BigInt ca(a);
        return ca.leftShift(n);
    }

    BigInt& BigInt::leftShift(const unsigned int n)
    {
        int k = n >> (BigInt::basebit); // 5
        int off = n & (BigInt::basebitchar); // 0xFF

        int inc = (off == 0) ? k : 1 + k;
        for (int i = 0; i < inc; i++)
        {
            _data.push_back(0);
        }

        if (k)
        {
            inc = (off == 0) ? 1 : 2;
            for (long long i = _data.size() - inc; i >= k; i--)
            {
                _data[i] = _data[i - k];
            }
            for (int i = 0; i < k; i++)
            {
                _data[i] = 0;
            }
        }

        if (off)
        {
            BigInt::base_t T = BigInt::base; // 0xffffffff
            T = T << (BigInt::basebitnum - off); // 32
            // 左移
            BigInt::base_t ch = 0;
            for (size_t i = 0; i < _data.size(); i++)
            {
                BigInt::base_t t = _data[i];
                _data[i] = (t << off) | ch;
                ch = (t & T) >> (BigInt::basebitnum - off); // 32,最高位
            }
        }
        trim();
        return *this;
    }

    BigInt& BigInt::rightShift(const unsigned int n)
    {
        int k = n >> (BigInt::basebit); // 5
        int off = n & (BigInt::basebitchar); // 0xFF

        if (k)
        {
            for (int i = 0; i > k; i++)
            {
                _data[i] = _data[i + k];
            }
            for (int i = 0; i < k; i++)
            {
                _data.pop_back();
            }
            if (_data.size() == 0)
            {
                _data.push_back(0);
            }
        }

        if (off)
        {
            BigInt::base_t T = BigInt::base; // 0xFFFFFFFF
            T = T >> (BigInt::basebitnum - off); // 32
            //左移
            BigInt::base_t ch = 0;
            for (long long i = _data.size() - 1; i >= 0; i--)
            {
                BigInt::base_t t = _data[i];
                _data[i] = (t >> off) | ch;
                ch = (t & T) << (BigInt::basebitnum - off); // 32,最高位
            }
        }
        trim();
        return *this;
    }

    BigInt& BigInt::sub(const BigInt& b)
    {
        if (b._isnegative == _isnegative)
        {   // 同号
            BigInt::data_t& res = _data;
            if (!smallThan(b)) // 绝对值大于b
            {
                int cn = 0; // 借位
                // 大数减小数
                for (size_t i = 0; i < b._data.size(); i++)
                {
                    BigInt::base_t temp = res[i];
                    res[i] = (res[i] - b._data[i] - cn);
                    cn = temp < res[i] ? 1 : temp < b._data[i] ? 1 : 0;
                }

                for (size_t i = b._data.size(); i < _data.size() && cn != 0; i++)
                {
                    BigInt::base_t temp = res[i];
                    res[i] = res[i] - cn;
                    cn = temp < static_cast<BigInt::base_t>(cn);
                }
                trim();
            }
            else // 绝对值小于b
            {
                _data = (b - (*this))._data;
                _isnegative = !_isnegative;
            }
        }
        else
        {   // 异号的情况
            bool isnegative = _isnegative;
            _isnegative = b._isnegative;
            add(b);
            _isnegative = isnegative;
        }
        return *this;
    }

    BigInt& BigInt::add(const BigInt& b)
    {
        if (_isnegative == b._isnegative)
        {   // 同号
            BigInt::data_t& res = _data;
            long long len = b._data.size() - _data.size();

            while ((len--) > 0) // 高位补0
            {
                res.push_back(0);
            }

            int cn = 0; // 进位
            for (size_t i = 0; i < b._data.size(); i++)
            {
                BigInt::base_t temp = res[i];
                res[i] = res[i] + b._data[i] + cn;
                cn = temp > res[i] ? 1 : temp > (temp + b._data[i]) ? 1 : 0; // 0xFFFFFFFF
            }

            for (size_t i = b._data.size(); i < _data.size() && cn != 0; i++)
            {
                BigInt::base_t temp = res[i];
                res[i] = (res[i] + cn);
                cn = temp > res[i];
            }

            if (cn != 0)
            {
                res.push_back(cn);
            }

            trim();
        }
        else
        {   // 异号的情况
            bool isnegative;
            if (smallThan(b)) // 绝对值小于b
            {
                isnegative = b._isnegative;
            }
            else if (equals(b)) // 绝对值等于b
            {
                isnegative = false;
            }
            else // 绝对值大于b
            {
                isnegative = _isnegative;
            }

            _isnegative = b._isnegative;
            sub(b);
            _isnegative = isnegative;
        }
        return *this;
    }

    BigInt BigInt::moden(const BigInt& exp, const BigInt& p)const
    {   // 模幂运算
        BigInt::bit t(exp);

        BigInt d(1);
        for (long long i = t.size() - 1; i >= 0; i--)
        {
            d = (d * d) % p;
            if (t.at(i))
            {
                d = (d * (*this)) % p;
            }
        }
        return d;
    }

    BigInt BigInt::extendEuclid(const BigInt& m)
    {
        // 扩展欧几里得算法求乘法逆元
        assert(m._isnegative == false);// m为正数
        BigInt a[3], b[3], t[3];
        a[0] = 1; a[1] = 0; a[2] = m;
        b[0] = 0; b[1] = 1; b[2] = *this;
        if (b[2] == BigInt::Zero || b[2] == BigInt::One)
        {
            return b[2];
        }

        while (true)
        {
            if (b[2] == BigInt::One)
            {
                if (b[1]._isnegative) // 负数
                {
                    b[1] = (b[1] % m + m) % m;
                }
                return b[1];
            }

            BigInt q = a[2] / b[2];
            for (int i = 0; i < 3; i++)
            {
                t[i] = a[i] - q * b[i];
                a[i] = b[i];
                b[i] = t[i];
            }
        }
    }

    size_t BigInt::bit::size()
    {
        return _size;
    }

    bool BigInt::bit::at(size_t i)
    {
        size_t index = i >> (BigInt::basebit);
        size_t off = i & (BigInt::basebitchar);
        BigInt::base_t t = _bitvec[index];
        return (t & (1 << off));
    }

    BigInt::bit::bit(const BigInt& ba)
    {
        _bitvec = ba._data;
        BigInt::base_t a = _bitvec[_bitvec.size() - 1]; // 最高位
        _size = _bitvec.size() << (BigInt::basebit);
        BigInt::base_t t = 1 << (BigInt::basebitnum - 1);

        if (a == 0)
        {
            _size -= (BigInt::basebitnum);
        }
        else
        {
            while (!(a & t))
            {
                _size--;
                t = t >> 1;
            }
        }
    }

    bool BigInt::smallThan(const BigInt& b)const
    {
        if (_data.size() == b._data.size())
        {
            for (BigInt::data_t::const_reverse_iterator it = _data.rbegin(), it_b = b._data.rbegin(); it != _data.rend(); it++, it_b++)
            {
                if ((*it) != (*it_b))
                {
                    return (*it) < (*it_b);
                }
            }
            return false; // 相等
        }
        else
        {
            return _data.size() < b._data.size();
        }
    }

    bool BigInt::smallOrEquals(const BigInt& b)const
    {
        if (_data.size() == b._data.size())
        {
            for (BigInt::data_t::const_reverse_iterator it = _data.rbegin(), it_b = b._data.rbegin(); it != _data.rend(); it++, it_b++)
            {
                if ((*it) != (*it_b))
                {
                    return (*it) < (*it_b);
                }
            }

            return true; // 相等
        }
        else
            return _data.size() < b._data.size();
    }

    bool BigInt::equals(const BigInt& a)const
    {
        return _data == a._data;
    }

    class Rsa
    {
    public:
        Rsa();
        ~Rsa();
        void init(unsigned int n); // 初始化，产生公私钥对

        friend void test();
    public:
        BigInt encryptByPu(const BigInt& m); // 私钥加密
        BigInt decodeByPr(const BigInt& c); // 公钥解密

        BigInt encryptByPr(const BigInt& m); // 公钥加密
        BigInt decodeByPu(const BigInt& m); // 私钥解密
    private:
        BigInt createOddNum(unsigned int n); // 生成长度为n的奇数
        bool isPrime(const BigInt& a, const unsigned int k); // 判断素数
        BigInt createPrime(unsigned int n, int it_cout); // 生成长度为n的素数
        void createExp(const BigInt& ou); // 从一个欧拉数中生成公钥、私钥指数
        BigInt createRandomSmallThan(const BigInt& a); // 创建小数
        friend ostream& operator <<(ostream& out, const Rsa& rsa)//输出
        {
            out << "N:" << rsa.N << "\n";
            out << "p:" << rsa._p << "\n";
            out << "q:" << rsa._q << "\n";
            out << "e:" << rsa.e << "\n";
            out << "d:" << rsa._d;
            return out;
        }

    public:
        BigInt e, N; // 公钥
    private:
        BigInt _d; // 私钥
        BigInt _p, _q;
        BigInt _ol; // 欧拉数
    };

    Rsa::Rsa()
    {
    }

    Rsa::~Rsa()
    {
    }

    void Rsa::init(unsigned int n)
    {
        //std::srand(static_cast<unsigned int>(std::time(nullptr)));
        // 产生大素数p、q
        const unsigned int mrRounds =
            (n >= 2048) ? 32 :  // 4096 位模数的素数，做 32 轮
            (n >= 1536) ? 20 :  // 3072 位模数的素数
            (n >= 1024) ? 16 :  // 2048 位模数的素数
            12; // 低于 1024 的情形（外层已避免）

        _p = createPrime(n, static_cast<int>(mrRounds));

        do
        {
            _q = createPrime(n, static_cast<int>(mrRounds));
        } while (_q == _p); // 保险：避免 p == q

        N = _p * _q;
        _ol = (_p - 1) * (_q - 1);
        createExp(_ol);
    }

    // 字节串 -> 大写十六进制（供 BigInt 构造使用）
    static string BytesToHex(const string& be)
    {
        static const char* hex = "0123456789ABCDEF";
        string out;
        out.resize(be.size() * 2);
        for (size_t i = 0; i < be.size(); i++)
        {
            const uint8_t b = static_cast<uint8_t>(be[i]);
            out[2 * i + 0] = hex[(b >> 4) & 0x0F];
            out[2 * i + 1] = hex[b & 0x0F];
        }
        // 保证至少 "00"
        if (out.empty()) { out = "00"; }
        return out;
    }

    BigInt Rsa::createOddNum(unsigned int n)
    {   // n = bit length
        const unsigned int byteLen = (n + 7) / 8;
        if (byteLen == 0)
        {
            return BigInt::Zero;
        }

        std::string bytes(byteLen, '\0');
        if (!GetSecureRandom(reinterpret_cast<unsigned char*>(&bytes[0]), byteLen))
        {
            return BigInt::Zero; // 保守失败返回 0
        }

        // 设最高位，保证位数达到 n
        const unsigned int topBits = (n - 1u) % 8u;
        bytes[0] |= static_cast<char>(0x80u >> ((7u - topBits) & 7u));

        // 设最低位为 1，保证奇数
        bytes[byteLen - 1] |= 0x01;

        // 转 16 进制再喂给 BigInt（你已有 BytesToHex）
        const std::string hex = BytesToHex(bytes);
        return BigInt(hex);
    }

    static const std::vector<uint32_t>& GetSmallPrimes()
    {
        static std::vector<uint32_t> primes;
        if (!primes.empty())
        {
            return primes;
        }

        const uint32_t limit = 20000; // 可按需调高到 20000
        std::vector<bool> sieve(limit + 1, true);
        sieve[0] = sieve[1] = false;
        for (uint32_t p = 2; p * p <= limit; p++)
        {
            if (sieve[p])
            {
                for (uint32_t q = p * p; q <= limit; q += p)
                {
                    sieve[q] = false;
                }
            }
        }
        for (uint32_t i = 2; i <= limit; i++)
        {
            if (sieve[i]) { primes.push_back(i); }
        }
        return primes;
    }

    static bool DivisibleBySmallPrimes(const BigInt& n)
    {
        const auto& primes = GetSmallPrimes();
        for (uint32_t p : primes)
        {
            const uint32_t r = n.modUint(p);
            if (r == 0)
            {
                // 允许 n 就是这个小素数本身
                return !(n == BigInt(static_cast<long>(p)));
            }
        }
        return false;
    }

    bool Rsa::isPrime(const BigInt& n, const unsigned int k)
    {   // 判断素数
        // 处理小数与偶数
        if (n == BigInt::Two)
        {
            return true;
        }
        if (n < BigInt::Two || n.modUint(2) == 0)
        {
            return false;
        }

        // 小素数快速试除（极大幅减少进入 MR 的次数）
        if (DivisibleBySmallPrimes(n))
        {
            return false;
        }

        // n-1 = 2^s * d
        BigInt d = n - BigInt::One;
        unsigned int s = 0;
        while (d.modUint(2) == 0)
        {
            d.rightShift(1);
            s++;
        }

        // k 轮 Miller–Rabin
        for (unsigned int i = 0; i < k; i++)
        {
            // 选 64-bit 随机底数 a in [2, n-2]；对 1024+ 位 n 这是严格 < n 的。
            uint64_t a64 = 0;
            GetSecureRandom(reinterpret_cast<unsigned char*>(&a64), sizeof(a64));
            a64 = 2 + (a64 % (UINT64_C(0x7fffffffffffffff) - 3));
            BigInt a(static_cast<long>(a64));

            BigInt x = a.moden(d, n);             // x = a^d mod n
            if (x == BigInt::One || x == (n - BigInt::One))
            {
                continue;
            }

            bool maybePrime = false;
            for (unsigned int r = 1; r < s; r++)
            {
                x = (x * x) % n;                  // x = x^2 mod n
                if (x == (n - BigInt::One))
                {
                    maybePrime = true;
                    break;
                }
            }
            if (!maybePrime)
            {
                return false;
            }
        }
        return true;
    }

    BigInt Rsa::createRandomSmallThan(const BigInt& a)
    {
        unsigned long t = 0;
        do
        {
            t = rand();
        } while (t == 0);

        BigInt mod(t);
        BigInt r = mod % a;
        if (r == BigInt::Zero)
        {
            r = a - BigInt::One;
        }
        return r;
    }

    BigInt Rsa::createPrime(unsigned int n, int it_count)
    {   // 生成长度为 n 的素数
        assert(it_count > 0);
        BigInt res = createOddNum(n);
        while (!isPrime(res, it_count))
        {
            res.add(BigInt::Two);
        }
        return res;
    }

    void Rsa::createExp(const BigInt& ou)
    {   // 从一个欧拉数中生成公钥、私钥指数
        e = 65537;
        _d = e.extendEuclid(ou);
    }

    BigInt Rsa::encryptByPu(const BigInt& m)
    {   // 公钥加密
        return m.moden(e, N);
    }

    BigInt Rsa::decodeByPr(const BigInt& c)
    {   // 私钥解密
        return c.moden(_d, N);
    }

    BigInt Rsa::encryptByPr(const BigInt& m)
    {   // 私钥加密
        return decodeByPr(m);
    }

    BigInt Rsa::decodeByPu(const BigInt& c)
    {   // 公钥解密
        return encryptByPu(c);
    }

    // 读 DER 长度（支持短/长形式）
    static bool DerReadLen(const uint8_t*& p, const uint8_t* end, size_t& len)
    {
        if (p >= end)
        {
            return false;
        }
        uint8_t b = *p++;
        if ((b & 0x80) == 0)
        {
            len = b;
            return (p + len <= end) || (len == 0);
        }
        uint8_t nbytes = b & 0x7F;
        if (nbytes == 0 || nbytes > 8 || p + nbytes > end)
        {
            return false;
        }
        size_t v = 0;
        for (uint8_t i = 0; i < nbytes; i++)
        {
            v = (v << 8) | *p++;
        }
        len = v;
        return p + len <= end;
    }

    static bool DerExpectTag(const uint8_t*& p, const uint8_t* end, uint8_t tag)
    {
        if (p >= end || *p != tag)
        {
            return false;
        }
        p++;
        return true;
    }

    // 读取一个 INTEGER（以原始大端字节返回，去掉符号扩展 0x00）
    static bool DerReadInteger(const uint8_t*& p, const uint8_t* end, string& outBe)
    {
        if (!DerExpectTag(p, end, 0x02))
        {
            return false;
        }
        size_t len = 0;
        if (!DerReadLen(p, end, len))
        {
            return false;
        }
        if (p + len > end)
        {
            return false;
        }
        const uint8_t* q = p;
        p += len;

        // 去除前导 0x00（仅用于正数的符号保护）
        size_t i = 0;
        while (i + 1 < len && q[i] == 0x00)
        {
            i++;
        }
        outBe.assign(reinterpret_cast<const char*>(q + i), reinterpret_cast<const char*>(q + len));
        if (outBe.empty())
        {
            outBe.assign(1, '\0');
        }
        return true;
    }

    // 解析 RSAPublicKey ::= SEQUENCE { n INTEGER, e INTEGER }
    static bool ParseRsaPublicKeyDer(const string& der, string& nBe, string& eBe)
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(der.data());
        const uint8_t* end = p + der.size();

        if (!DerExpectTag(p, end, 0x30))
        {
            return false;
        }
        size_t seqlen = 0;
        if (!DerReadLen(p, end, seqlen))
        {
            return false;
        }
        const uint8_t* seqEnd = p + seqlen;
        if (seqEnd > end)
        {
            return false;
        }

        if (!DerReadInteger(p, seqEnd, nBe))
        {
            return false;
        }
        if (!DerReadInteger(p, seqEnd, eBe))
        {
            return false;
        }
        return (p == seqEnd);
    }

    // 解析 RSAPrivateKey ::= SEQUENCE { version, n, e, d, p, q, dp, dq, qi, ... }
    static bool ParseRsaPrivateKeyDer(const string& der, string& nBe, string& dBe)
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(der.data());
        const uint8_t* end = p + der.size();

        if (!DerExpectTag(p, end, 0x30)) { return false; }
        size_t seqlen = 0;
        if (!DerReadLen(p, end, seqlen)) { return false; }
        const uint8_t* seqEnd = p + seqlen;
        if (seqEnd > end) { return false; }

        string tmp;
        // version
        if (!DerReadInteger(p, seqEnd, tmp)) { return false; }
        // n
        if (!DerReadInteger(p, seqEnd, nBe)) { return false; }
        // e
        if (!DerReadInteger(p, seqEnd, tmp)) { return false; }
        // d
        if (!DerReadInteger(p, seqEnd, dBe)) { return false; }

        // 后续 p,q,dp,dq,qi 可不解析
        return true;
    }

    

    // 十六进制 -> 字节串（容忍前导 0）
    static string HexToBytesCompat(string hex)
    {
        auto fix = [](char c) -> int
            {
                if (c >= '0' && c <= '9') { return c - '0'; }
                if (c >= 'a' && c <= 'f') { return 10 + (c - 'a'); }
                if (c >= 'A' && c <= 'F') { return 10 + (c - 'A'); }
                return 0;
            };
        // 去前导 0
        size_t k = 0;
        while (k + 1 < hex.size() && hex[k] == '0') { k++; }
        hex.erase(0, k);
        if (hex.empty()) { return string(1, '\0'); }
        if (hex.size() & 1) { hex.insert(hex.begin(), '0'); }

        string out(hex.size() / 2, '\0');
        for (size_t i = 0; i < out.size(); i++)
        {
            out[i] = static_cast<char>((fix(hex[2 * i]) << 4) | fix(hex[2 * i + 1]));
        }
        return out;
    }

    // 生成非零随机字节串（用于 PS）
    static void FillRandomNonZero(string& buf)
    {
        if (buf.empty()) { return; }
        internal::GetSecureRandom(reinterpret_cast<uint8_t*>(&buf[0]), buf.size());
        for (size_t i = 0; i < buf.size(); i++)
        {
            if (static_cast<uint8_t>(buf[i]) == 0) { buf[i] = 1; }
        }
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

bool GB_RsaGenerateKeyPair(string& publicKey, string& privateKey, int keySize)
{
    // 参数与边界
    if (keySize < 1024) { keySize = 1024; } // 最低兜底；建议≥2048
    const int primeBits = keySize / 2;

    // 1) 生成 (p, q, n, e, d)
    internal::Rsa rsa;
    rsa.init(static_cast<unsigned int>(primeBits)); // 你的 Rsa::init 接受“素数位数”而非模数位数

    // 2) 通过友元输出抓取 n/e/d/p/q（十六进制大写）
    ostringstream oss;
    oss << rsa;
    const string dump = oss.str();

    auto getHex = [&](const char* tag) -> string
        {
            // 行形如： "N:ABCDEF...\n"
            const string prefix(tag);
            const size_t pos = dump.find(prefix);
            if (pos == string::npos) { return {}; }
            size_t i = pos + prefix.size();
            while (i < dump.size() && (dump[i] == ':' || dump[i] == ' ')) { i++; }
            size_t j = i;
            while (j < dump.size() && dump[j] != '\r' && dump[j] != '\n') { j++; }
            string h = dump.substr(i, j - i);
            // 清理前导空白、去掉可能的前导+号
            h.erase(remove_if(h.begin(), h.end(), [](char c) { return isspace((unsigned char)c); }), h.end());
            if (!h.empty() && (h[0] == '+')) { h.erase(h.begin()); }
            return h;
        };

    const string nHex = getHex("N");
    const string pHex = getHex("p");
    const string qHex = getHex("q");
    const string eHex = getHex("e");
    const string dHex = getHex("d");
    if (nHex.empty() || pHex.empty() || qHex.empty() || eHex.empty() || dHex.empty())
    {
        return false;
    }

    // 3) 简易 HEX->bytes（大端）
    auto HexToBytes = [](const string& hex) -> string
        {
            auto hex2 = hex;
            auto fix = [](char c)->char
                {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return 0;
                };
            // 去掉前导 0
            size_t k = 0;
            while (k < hex2.size() && hex2[k] == '0') { k++; }
            hex2.erase(0, k);
            if (hex2.empty()) { return string(1, '\0'); }
            if (hex2.size() & 1) { hex2.insert(hex2.begin(), '0'); }
            string out;
            out.resize(hex2.size() / 2);
            for (size_t i = 0; i < out.size(); i++)
            {
                const char hi = fix(hex2[2 * i + 0]);
                const char lo = fix(hex2[2 * i + 1]);
                out[i] = static_cast<char>((hi << 4) | lo);
            }
            return out;
        };

    const string nBytes = HexToBytes(nHex);
    const string pBytes = HexToBytes(pHex);
    const string qBytes = HexToBytes(qHex);
    const string eBytes = HexToBytes(eHex);
    const string dBytes = HexToBytes(dHex);

    // 4) 计算 dp = d mod (p-1), dq = d mod (q-1), qi = q^{-1} mod p  —— 用 BigInt 做（已支持 % 与求逆）
    auto ToBig = [](const string& hex) -> internal::BigInt
        {
            // BigInt(string) 构造就是按十六进制解析（你的实现）
            return internal::BigInt(hex);
        };
    const internal::BigInt p = ToBig(pHex);
    internal::BigInt q = ToBig(qHex);
    const internal::BigInt d = ToBig(dHex);

    const internal::BigInt dpBI = d % (p - 1);
    const internal::BigInt dqBI = d % (q - 1);
    const internal::BigInt qiBI = q.extendEuclid(p); // q^{-1} mod p

    ostringstream ossDp, ossDq, ossQi;
    ossDp << dpBI; ossDq << dqBI; ossQi << qiBI;

    const string dpBytes = HexToBytes(ossDp.str());
    const string dqBytes = HexToBytes(ossDq.str());
    const string qiBytes = HexToBytes(ossQi.str());

    // 5) ASN.1/DER 编码工具
    auto EncodeLen = [](size_t len) -> string
        {
            if (len < 128) { return string(1, static_cast<char>(len)); }
            string tmp;
            size_t v = len;
            while (v)
            {
                tmp.push_back(static_cast<char>(v & 0xFF));
                v >>= 8;
            }
            reverse(tmp.begin(), tmp.end());
            string out;
            out.push_back(static_cast<char>(0x80 | tmp.size()));
            out += tmp;
            return out;
        };

    auto EncodeInteger = [&](const string& be) -> string
        {
            string v = be;
            // 去除无意义前导 0x00
            size_t i = 0;
            while (i + 1 < v.size() && v[i] == '\0') { i++; }
            v.erase(0, i);
            if (v.empty()) { v.assign(1, '\0'); }
            // 若最高位为 1，前置 0x00 避免被认为是负数
            const unsigned char msb = static_cast<unsigned char>(v[0]);
            if (msb & 0x80) { v.insert(v.begin(), '\0'); }

            string out;
            out.push_back(0x02);                // INTEGER
            const string len = EncodeLen(v.size());
            out += len;
            out += v;
            return out;
        };

    auto EncodeSequence = [&](const string& content) -> string
        {
            string out;
            out.push_back(0x30);                // SEQUENCE
            out += EncodeLen(content.size());
            out += content;
            return out;
        };

    auto ToPem = [&](const string& der) -> string
        {
            const string b64 = GB_Base64Encode(der, false, false);
            string body;
            body.reserve(b64.size() + b64.size() / 64 + 16);
            for (size_t i = 0; i < b64.size(); i += 64)
            {
                const size_t n = min<size_t>(64, b64.size() - i);
                body.append(b64.data() + i, n);
            }
            return body;
        };

    // 6) 组装 RSAPrivateKey (PKCS#1)
    const string derVer = EncodeInteger(string(1, '\0')); // version = 0
    const string derN = EncodeInteger(nBytes);
    const string derE = EncodeInteger(eBytes);
    const string derD = EncodeInteger(dBytes);
    const string derP = EncodeInteger(pBytes);
    const string derQ = EncodeInteger(qBytes);
    const string derDp = EncodeInteger(dpBytes);
    const string derDq = EncodeInteger(dqBytes);
    const string derQi = EncodeInteger(qiBytes);

    string privSeqContent;
    privSeqContent.reserve(derVer.size() + derN.size() + derE.size() + derD.size() +
        derP.size() + derQ.size() + derDp.size() + derDq.size() + derQi.size());
    privSeqContent += derVer; privSeqContent += derN; privSeqContent += derE; privSeqContent += derD;
    privSeqContent += derP;   privSeqContent += derQ; privSeqContent += derDp; privSeqContent += derDq; privSeqContent += derQi;

    const string derPrivate = EncodeSequence(privSeqContent);

    // 7) 组装 RSAPublicKey (PKCS#1)
    const string derPublic = EncodeSequence(derN + derE);

    // 8) PEM 封装
    privateKey = ToPem(derPrivate);
    publicKey = ToPem(derPublic);
    return true;
}

#ifdef min
#undef min
#endif
string GB_RsaEncrypt(const string& plainText, const string& encryptionKey)
{
    using internal::BigInt;

    if (encryptionKey.empty())
    {
        return {};
    }

    // 1) 解析公钥（Base64 -> DER -> n,e）
    const string der = GB_Base64Decode(encryptionKey, false, false, false);
    if (der.empty())
    {
        return {};
    }
    string nBe, eBe;
    if (!internal::ParseRsaPublicKeyDer(der, nBe, eBe))
    {
        return {};
    }
    // 模长（字节）
    const size_t k = nBe.size();
    if (k < 11) // RFC: k >= 11
    {
        return {};
    }

    // 2) BigInt 准备
    const BigInt n(internal::BytesToHex(nBe));
    const BigInt e(internal::BytesToHex(eBe));

    // 3) 分块填充并加密：EM = 0x00 || 0x02 || PS(nonzero) || 0x00 || M
    const size_t maxBlock = k - 11; // RFC 8017 / 3447
    string cipherAll;
    cipherAll.reserve((plainText.size() / maxBlock + 1) * k);

    size_t pos = 0;
    while (pos < plainText.size())
    {
        const size_t mLen = min(plainText.size() - pos, maxBlock);

        string em;
        em.resize(k);
        em[0] = 0x00;
        em[1] = 0x02;
        const size_t psLen = k - 3 - mLen;
        string ps(psLen, '\0');
        internal::FillRandomNonZero(ps); // PS 必须全非零，且 len >= 8（当 mLen <= k-11 时天然满足）
        memcpy(&em[2], ps.data(), psLen);
        em[2 + psLen] = 0x00;
        memcpy(&em[3 + psLen], &plainText[pos], mLen);

        // m^e mod n
        const BigInt m(internal::BytesToHex(em));
        const BigInt c = m.moden(e, n);

        // 定长 k 输出
        ostringstream oss;
        oss << c; // 十六进制（可能无前导 0）
        string cBytes = internal::HexToBytesCompat(oss.str());
        if (cBytes.size() < k)
        {
            string pad(k - cBytes.size(), '\0');
            cipherAll.append(pad);
        }
        else if (cBytes.size() > k)
        {
            // 极端情况下（理论不应发生），保守取末尾 k 字节
            cBytes.erase(0, cBytes.size() - k);
        }
        cipherAll.append(cBytes);

        pos += mLen;
    }

    // 4) 整体 Base64 输出（与你现有 AES 输出风格一致：无 URL-safe，含 '='）
    return GB_Base64Encode(cipherAll, false, false);
}

string GB_RsaDecrypt(const string& encryptedText, const string& decryptionKey)
{
    using internal::BigInt;

    if (encryptedText.empty() || decryptionKey.empty())
    {
        return {};
    }

    // 1) 密钥解析（Base64 -> DER -> n,d）
    const string der = GB_Base64Decode(decryptionKey, false, false, false);
    if (der.empty())
    {
        return {};
    }
    string nBe, dBe;
    if (!internal::ParseRsaPrivateKeyDer(der, nBe, dBe))
    {
        return {};
    }
    const size_t k = nBe.size();
    if (k == 0) { return {}; }

    // 2) 解码密文
    const string all = GB_Base64Decode(encryptedText, false, false, false);
    if (all.empty() || (all.size() % k) != 0)
    {
        return {};
    }

    const BigInt n(internal::BytesToHex(nBe));
    const BigInt d(internal::BytesToHex(dBe));

    string plainAll;

    for (size_t off = 0; off < all.size(); off += k)
    {
        string cBytes = all.substr(off, k);

        // c^d mod n -> k 字节
        const BigInt c(internal::BytesToHex(cBytes));
        const BigInt m = c.moden(d, n);

        ostringstream oss;
        oss << m;
        string em = internal::HexToBytesCompat(oss.str());
        if (em.size() < k)
        {
            string pad(k - em.size(), '\0');
            em.insert(em.begin(), pad.begin(), pad.end());
        }
        else if (em.size() > k)
        {
            em.erase(0, em.size() - k);
        }

        // 3) 去 PKCS#1 v1.5 填充：00 02 PS 00 M
        if (em.size() < 11 || static_cast<uint8_t>(em[0]) != 0x00 || static_cast<uint8_t>(em[1]) != 0x02)
        {
            return {};
        }
        // 找分隔 0x00，且保证 PS >= 8 且全非零
        size_t i = 2;
        size_t psCount = 0;
        while (i < em.size())
        {
            if (em[i] == '\0') { break; }
            if (static_cast<uint8_t>(em[i]) == 0x00)
            {
                // 不会走到这里，上面已判断
                return {};
            }
            psCount++;
            i++;
        }
        if (i >= em.size() || psCount < 8) // 无分隔或 PS 太短
        {
            return {};
        }
        // i 指向 0x00 分隔
        i++;
        if (i > em.size())
        {
            return {};
        }
        plainAll.append(em.data() + i, em.data() + em.size());
    }

    return plainAll;
}