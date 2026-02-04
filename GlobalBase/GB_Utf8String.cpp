#include "GB_Utf8String.h"
#include <unordered_set>
#include <stdexcept>
#include <climits>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <clocale>
#include <cwchar>
#include <cerrno>
#include <locale>
#include <codecvt>
#include <langinfo.h>
#endif
#ifndef GB_DISABLE_POSIX_SETLOCALE_AUTO_INIT
#define GB_DISABLE_POSIX_SETLOCALE_AUTO_INIT 0
#endif


using std::string;
using std::wstring;
using std::vector;
using std::unordered_set;
using std::runtime_error;
using std::range_error;


namespace internal
{
#if defined(_WIN32)
    static int ToWinApiLengthChecked(size_t length)
    {
        if (length > static_cast<size_t>(INT_MAX))
        {
            throw runtime_error("Input string too large for Win32 API.");
        }
        return static_cast<int>(length);
    }
#endif

    // 从 s[pos] 解码一个 UTF-8 码点：
    // 成功：返回 true，写出 codePoint 与 nextPos（下一个字节位置）
    // 失败：返回 false，仅前进一个字节（nextPos = pos + 1），调用方可按“原始字节”处理
    static bool DecodeOne(const string& s, size_t pos, char32_t& codePoint, size_t& nextPos)
    {
        const size_t n = s.size();
        if (pos >= n)
        {
            codePoint = 0;
            nextPos = n;
            return false;
        }

        unsigned char b0 = static_cast<unsigned char>(s[pos]);
        if (b0 < 0x80)
        {
            codePoint = b0;
            nextPos = pos + 1;
            return true;
        }

        int len = 0;
        char32_t cp = 0;

        if ((b0 & 0xE0) == 0xC0) { len = 2; cp = (b0 & 0x1F); }
        else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = (b0 & 0x0F); }
        else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = (b0 & 0x07); }
        else
        {
            nextPos = pos + 1; // 非法起始字节
            return false;
        }

        if (pos + len > n)
        {
            nextPos = pos + 1; // 截断
            return false;
        }

        for (int i = 1; i < len; i++)
        {
            unsigned char bx = static_cast<unsigned char>(s[pos + i]);
            if ((bx & 0xC0) != 0x80)
            {
                nextPos = pos + 1; // 非 10xxxxxx
                return false;
            }
            cp = (cp << 6) | (bx & 0x3F);
        }

        // RFC 3629：最短编码、合法范围、排除代理项
        if ((len == 2 && cp < 0x80) ||
            (len == 3 && cp < 0x800) ||
            (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
        {
            nextPos = pos + 1;
            return false;
        }

        codePoint = cp;
        nextPos = pos + len;
        return true;
    }

    static bool DecodeSingleChar(const string& s, char32_t& codePoint)
    {
        size_t nextPos = 0;
        if (!DecodeOne(s, 0, codePoint, nextPos))
        {
            return false;
        }
        return nextPos == s.size(); // 必须恰好一个码点
    }

    static char32_t ToLowerAscii(char32_t cp)
    {
        // 仅 ASCII 大小写折叠
        if (cp >= U'A' && cp <= U'Z')
        {
            return cp + (U'a' - U'A');
        }
        return cp;
    }

    // 统一的“读一个码点”：如果 internal::DecodeOne 失败，就把该字节当作 U+FFFD 消费 1 字节
    static void DecodeOneOrReplacement(const string& s, size_t pos, char32_t& cp, size_t& nextPos)
    {
        if (!internal::DecodeOne(s, pos, cp, nextPos))
        {
            cp = 0xFFFDu;
            nextPos = pos + 1; // 失败时按 1 字节前进，保持可数性
        }
    }

    // 仅 ASCII 的大小写转换，避免受本地化影响
    static char ToLowerAsciiChar(char ch)
    {
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
    static char ToUpperAsciiChar(char ch)
    {
        return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
    }


    static unsigned char NormalizeAsciiCaseByte(unsigned char byteValue, bool caseSensitive)
    {
        if (!caseSensitive && byteValue >= static_cast<unsigned char>('A') && byteValue <= static_cast<unsigned char>('Z'))
        {
            return static_cast<unsigned char>(byteValue - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
        }
        return byteValue;
    }

    static vector<size_t> BuildKmpLpsBytes(const string& pattern, bool caseSensitive)
    {
        const size_t m = pattern.size();
        vector<size_t> lps(m, 0);

        size_t len = 0;
        size_t i = 1;
        while (i < m)
        {
            const unsigned char a = NormalizeAsciiCaseByte(static_cast<unsigned char>(pattern[i]), caseSensitive);
            const unsigned char b = NormalizeAsciiCaseByte(static_cast<unsigned char>(pattern[len]), caseSensitive);

            if (a == b)
            {
                len++;
                lps[i] = len;
                i++;
            }
            else if (len != 0)
            {
                len = lps[len - 1];
            }
            else
            {
                lps[i] = 0;
                i++;
            }
        }
        return lps;
    }

    static string ReplaceAllBytesKmp(const string& text, const string& oldValue, const string& newValue, bool caseSensitive)
    {
        if (text.empty() || oldValue.empty())
        {
            return text;
        }
        if (oldValue.size() > text.size())
        {
            return text;
        }

        const size_t m = oldValue.size();
        const vector<size_t> lps = BuildKmpLpsBytes(oldValue, caseSensitive);

        string out;
        out.reserve(text.size());

        size_t i = 0;                 // text 字节索引
        size_t j = 0;                 // oldValue 已匹配长度
        size_t lastCopyPos = 0;       // 上一次复制到 out 的 text 字节位置

        while (i < text.size())
        {
            const unsigned char t = NormalizeAsciiCaseByte(static_cast<unsigned char>(text[i]), caseSensitive);
            const unsigned char p = NormalizeAsciiCaseByte(static_cast<unsigned char>(oldValue[j]), caseSensitive);

            if (t == p)
            {
                i++;
                j++;
                if (j == m)
                {
                    const size_t matchEnd = i;
                    const size_t matchStart = matchEnd - m;

                    // 追加匹配之前的内容
                    if (matchStart > lastCopyPos)
                    {
                        out.append(text, lastCopyPos, matchStart - lastCopyPos);
                    }
                    // 追加替换内容
                    out += newValue;

                    // 非重叠替换：从 matchEnd 继续搜索
                    lastCopyPos = matchEnd;
                    j = 0;
                }
            }
            else
            {
                if (j != 0)
                {
                    j = lps[j - 1];
                }
                else
                {
                    i++;
                }
            }
        }

        // 追加尾部剩余
        if (lastCopyPos < text.size())
        {
            out.append(text, lastCopyPos, text.size() - lastCopyPos);
        }

        return out;
    }

    static bool IsValidUnicode(uint32_t cp)
    {
        // Unicode 标准平面范围：U+0000 ~ U+10FFFF，排除代理区
        return cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
    }

#ifndef _WIN32
    // 说明：这里的“ANSI”指当前 LC_CTYPE locale 的多字节编码（如 zh_CN.GB18030）。
    // 若当前是 "C"/"POSIX"（7-bit ASCII），请先 setlocale 到合适的本地编码。
    static void EnsureLocaleInitialized()
    {
#if GB_DISABLE_POSIX_SETLOCALE_AUTO_INIT
        // 由调用方自行负责设置合适的进程 locale（例如 setlocale(LC_CTYPE, "")）。
        return;
#else
        // 注意：setlocale 会影响进程全局 locale（并非线程安全）。
        // 这里用 call_once 保证：
        // 1) 只在第一次需要时做一次初始化；
        // 2) 降低多线程竞争导致的风险（但无法阻止外部线程同时调用 setlocale）。
        static std::once_flag onceFlag;
        std::call_once(onceFlag, []()
            {
                const char* cur = setlocale(LC_CTYPE, nullptr);
                if (!cur || string(cur) == "C" || string(cur) == "POSIX")
                {
                    // 从环境继承（如 LANG/LC_ALL/LC_CTYPE），让 mbsrtowcs/wcsrtombs 有机会按本地多字节编码工作。
                    setlocale(LC_CTYPE, "");
                }
            });
#endif
    }

    static wstring Utf8ToWString_Posix(const string& utf8Str)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> c8; // C++11 可用
        return c8.from_bytes(utf8Str);
    }

    static string WStringToUtf8_Posix(const wstring& ws)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> c8;
        return c8.to_bytes(ws);
    }
#endif // !_WIN32

    static bool IsAllAscii(const string& s)
    {
        for (unsigned char ch : s)
        {
            if (ch >= 0x80)
            {
                return false;
            }
        }
        return true;
    }

    static bool HasUtf8Bom(const string& text)
    {
        if (text.size() < 3)
        {
            return false;
        }

        const unsigned char b0 = static_cast<unsigned char>(text[0]);
        const unsigned char b1 = static_cast<unsigned char>(text[1]);
        const unsigned char b2 = static_cast<unsigned char>(text[2]);
        return b0 == 0xEF && b1 == 0xBB && b2 == 0xBF;
    }

    static string ToLowerAsciiString(const string& text)
    {
        string result = text;
        for (size_t i = 0; i < result.size(); i++)
        {
            result[i] = internal::ToLowerAsciiChar(result[i]);
        }
        return result;
    }

    static bool IsCurrentAnsiUtf8()
    {
#if defined(_WIN32)
        return ::GetACP() == CP_UTF8;
#else
        internal::EnsureLocaleInitialized();
        const char* codeset = nl_langinfo(CODESET);
        if (!codeset)
        {
            return false;
        }

        const string codesetLower = internal::ToLowerAsciiString(string(codeset));
        if (codesetLower.find("utf-8") != string::npos)
        {
            return true;
        }
        if (codesetLower.find("utf8") != string::npos)
        {
            return true;
        }
        return false;
#endif
    }

    static bool IsNonCharacter(char32_t codePoint)
    {
        if (codePoint >= 0xFDD0u && codePoint <= 0xFDEFu)
        {
            return true;
        }
        if ((codePoint & 0xFFFEu) == 0xFFFEu && codePoint <= 0x10FFFFu)
        {
            return true;
        }
        return false;
    }

    static bool IsCjk(char32_t codePoint)
    {
        // CJK Unified Ideographs + Extensions (常用范围)
        if (codePoint >= 0x4E00u && codePoint <= 0x9FFFu)
        {
            return true;
        }
        if (codePoint >= 0x3400u && codePoint <= 0x4DBFu)
        {
            return true;
        }
        if (codePoint >= 0x20000u && codePoint <= 0x2A6DFu)
        {
            return true;
        }
        if (codePoint >= 0x2A700u && codePoint <= 0x2B73Fu)
        {
            return true;
        }
        if (codePoint >= 0x2B740u && codePoint <= 0x2B81Fu)
        {
            return true;
        }
        if (codePoint >= 0x2B820u && codePoint <= 0x2CEAFu)
        {
            return true;
        }
        if (codePoint >= 0xF900u && codePoint <= 0xFAFFu)
        {
            return true;
        }
        return false;
    }

    static bool IsHiragana(char32_t codePoint)
    {
        return codePoint >= 0x3040u && codePoint <= 0x309Fu;
    }

    static bool IsKatakana(char32_t codePoint)
    {
        return (codePoint >= 0x30A0u && codePoint <= 0x30FFu) ||
            (codePoint >= 0x31F0u && codePoint <= 0x31FFu);
    }

    static bool IsHangul(char32_t codePoint)
    {
        return (codePoint >= 0xAC00u && codePoint <= 0xD7AFu) ||
            (codePoint >= 0x1100u && codePoint <= 0x11FFu);
    }

    static bool IsCommonWhitespace(char32_t codePoint)
    {
        return codePoint == 0x20u || codePoint == 0x09u || codePoint == 0x0Au ||
            codePoint == 0x0Du || codePoint == 0x3000u;
    }

    static int ScoreCodePoint(char32_t codePoint, bool isFirstCodePoint)
    {
        // UTF-8 BOM 常作为首字符出现：忽略它
        if (isFirstCodePoint && codePoint == 0xFEFFu)
        {
            return 0;
        }

        if (codePoint == 0u)
        {
            return -50;
        }

        if (codePoint == 0xFFFDu)
        {
            return -30;
        }

        if (IsNonCharacter(codePoint))
        {
            return -10;
        }

        if (IsCommonWhitespace(codePoint))
        {
            return 1;
        }

        // 控制字符（排除 \t \n \r）
        if (codePoint < 0x20u || codePoint == 0x7Fu)
        {
            return -20;
        }
        if (codePoint >= 0x80u && codePoint < 0xA0u)
        {
            return -20;
        }

        // ASCII 可见字符
        if (codePoint >= 0x21u && codePoint <= 0x7Eu)
        {
            if ((codePoint >= U'a' && codePoint <= U'z') || (codePoint >= U'A' && codePoint <= U'Z'))
            {
                return 3;
            }
            if (codePoint >= U'0' && codePoint <= U'9')
            {
                return 2;
            }
            return 1;
        }

        // 拉丁扩展（带重音等）
        if (codePoint >= 0x00A0u && codePoint <= 0x024Fu)
        {
            return 3;
        }

        // 西里尔/希腊等常见文字
        if (codePoint >= 0x0370u && codePoint <= 0x052Fu)
        {
            return 3;
        }

        if (IsCjk(codePoint))
        {
            return 6;
        }
        if (IsHiragana(codePoint) || IsKatakana(codePoint))
        {
            return 5;
        }
        if (IsHangul(codePoint))
        {
            return 5;
        }

        // Emoji/符号等：允许，但不给太多分
        if (codePoint >= 0x1F300u && codePoint <= 0x1FAFFu)
        {
            return 1;
        }

        // 其它可打印字符：给一点点分
        return 1;
    }

    static int ComputeQualityScoreFromUtf8AssumingValid(const string& utf8Text)
    {
        size_t pos = 0;
        bool isFirstCodePoint = true;
        int score = 0;

        while (pos < utf8Text.size())
        {
            char32_t codePoint = 0;
            size_t nextPos = pos;
            if (!internal::DecodeOne(utf8Text, pos, codePoint, nextPos))
            {
                // 理论上不应发生（调用方应确保 utf8Text 为合法 UTF-8）
                return INT_MIN;
            }

            score += internal::ScoreCodePoint(codePoint, isFirstCodePoint);
            isFirstCodePoint = false;
            pos = nextPos;
        }

        return score;
    }

    static int ComputeQualityScoreFromWideString(const wstring& wideString)
    {
        bool isFirstCodePoint = true;
        int score = 0;

#if defined(_WIN32)
        for (size_t i = 0; i < wideString.size(); i++)
        {
            const wchar_t w = wideString[i];
            char32_t codePoint = static_cast<char32_t>(w);

            // 处理 UTF-16 代理项对
            if (w >= 0xD800 && w <= 0xDBFF)
            {
                if (i + 1 < wideString.size())
                {
                    const wchar_t w2 = wideString[i + 1];
                    if (w2 >= 0xDC00 && w2 <= 0xDFFF)
                    {
                        const uint32_t high = static_cast<uint32_t>(w - 0xD800);
                        const uint32_t low = static_cast<uint32_t>(w2 - 0xDC00);
                        codePoint = static_cast<char32_t>(0x10000u + ((high << 10) | low));
                        i++;
                    }
                    else
                    {
                        // 孤立高代理项
                        codePoint = 0xFFFDu;
                    }
                }
                else
                {
                    codePoint = 0xFFFDu;
                }
            }
            else if (w >= 0xDC00 && w <= 0xDFFF)
            {
                // 孤立低代理项
                codePoint = 0xFFFDu;
            }

            score += internal::ScoreCodePoint(codePoint, isFirstCodePoint);
            isFirstCodePoint = false;
        }
#else
        for (size_t i = 0; i < wideString.size(); i++)
        {
            const char32_t codePoint = static_cast<char32_t>(wideString[i]);
            score += internal::ScoreCodePoint(codePoint, isFirstCodePoint);
            isFirstCodePoint = false;
        }
#endif
        return score;
    }

    static int ComputeQualityScoreFromAnsiBytes(const string& ansiBytes, bool& decodedOk)
    {
        decodedOk = false;

        if (ansiBytes.empty())
        {
            decodedOk = true;
            return 0;
        }

#if defined(_WIN32)
        const UINT codePage = CP_ACP;

        int wideLength = ::MultiByteToWideChar(
            codePage,
            MB_ERR_INVALID_CHARS,
            ansiBytes.data(),
            internal::ToWinApiLengthChecked(ansiBytes.size()),
            nullptr,
            0
        );
        if (wideLength <= 0)
        {
            decodedOk = false;
            return INT_MIN;
        }

        wstring wideString(static_cast<size_t>(wideLength), L'\0');
        const int written = ::MultiByteToWideChar(
            codePage,
            MB_ERR_INVALID_CHARS,
            ansiBytes.data(),
            internal::ToWinApiLengthChecked(ansiBytes.size()),
            &wideString[0],
            wideLength
        );
        if (written <= 0)
        {
            decodedOk = false;
            return INT_MIN;
        }

        decodedOk = true;
        return internal::ComputeQualityScoreFromWideString(wideString);

#else
        internal::EnsureLocaleInitialized();

        const char* src = ansiBytes.c_str();
        mbstate_t state = mbstate_t{};
        errno = 0;
        const size_t wideLength = mbsrtowcs(nullptr, &src, 0, &state);
        if (wideLength == static_cast<size_t>(-1))
        {
            decodedOk = false;
            return INT_MIN;
        }

        wstring wideString(wideLength, L'\0');
        src = ansiBytes.c_str();
        state = mbstate_t{};
        errno = 0;
        const size_t written = mbsrtowcs(&wideString[0], &src, wideLength, &state);
        if (written == static_cast<size_t>(-1))
        {
            decodedOk = false;
            return INT_MIN;
        }

        if (written < wideString.size())
        {
            wideString.resize(written);
        }

        decodedOk = true;
        return internal::ComputeQualityScoreFromWideString(wideString);
#endif
    }
    static unordered_set<char32_t> BuildTrimSet(const string& trimCharsUtf8)
    {
        unordered_set<char32_t> st;
        size_t pos = 0;
        while (pos < trimCharsUtf8.size())
        {
            char32_t cp = 0;
            size_t nextPos = pos;
            internal::DecodeOneOrReplacement(trimCharsUtf8, pos, cp, nextPos);
            st.insert(cp);
            pos = nextPos;
        }
        return st;
    }

    static string TrimLeftImpl(const string& s, const unordered_set<char32_t>& trimSet)
    {
        size_t pos = 0;
        while (pos < s.size())
        {
            char32_t cp = 0;
            size_t nextPos = pos;
            internal::DecodeOneOrReplacement(s, pos, cp, nextPos);
            if (trimSet.find(cp) == trimSet.end())
            {
                break;
            }
            pos = nextPos;
        }
        return s.substr(pos);
    }

    static string TrimRightImpl(const string& s, const unordered_set<char32_t>& trimSet)
    {
        // 从左到右扫描，记录最后一个“非修剪码点”的末尾字节位置
        size_t pos = 0;
        size_t lastNonTrimEnd = 0;
        bool seenNonTrim = false;

        while (pos < s.size())
        {
            char32_t cp = 0;
            size_t nextPos = pos;
            internal::DecodeOneOrReplacement(s, pos, cp, nextPos);
            if (trimSet.find(cp) == trimSet.end())
            {
                seenNonTrim = true;
                lastNonTrimEnd = nextPos;
            }
            pos = nextPos;
        }

        if (!seenNonTrim)
        {
            return {};
        }
        return s.substr(0, lastNonTrimEnd);
    }

    enum class AnsiEncodingFamily
    {
        utf8,
        gbkLike,
        big5Like,
        shiftJisLike,
        eucKrLike,
        singleByte,
        unknown
    };

    static AnsiEncodingFamily GetAnsiEncodingFamily()
    {
#if defined(_WIN32)
        const UINT ansiCodePage = ::GetACP();
        if (ansiCodePage == CP_UTF8)
        {
            return AnsiEncodingFamily::utf8;
        }

        // 常见 East Asian 多字节 ANSI 代码页
        if (ansiCodePage == 936u || ansiCodePage == 54936u)
        {
            return AnsiEncodingFamily::gbkLike;     // GBK / GB18030
        }
        if (ansiCodePage == 950u)
        {
            return AnsiEncodingFamily::big5Like;    // Big5
        }
        if (ansiCodePage == 932u)
        {
            return AnsiEncodingFamily::shiftJisLike; // Shift-JIS
        }
        if (ansiCodePage == 949u)
        {
            return AnsiEncodingFamily::eucKrLike;   // EUC-KR / CP949 系
        }

        // 其余多数为单字节（125x、874 等）
        return AnsiEncodingFamily::singleByte;
#else
        EnsureLocaleInitialized();
        const char* codeset = nl_langinfo(CODESET);
        if (codeset == nullptr)
        {
            return AnsiEncodingFamily::unknown;
        }

        const string codesetLower = ToLowerAsciiString(string(codeset));
        if (codesetLower.find("utf-8") != string::npos || codesetLower.find("utf8") != string::npos)
        {
            return AnsiEncodingFamily::utf8;
        }
        if (codesetLower.find("gb18030") != string::npos || codesetLower.find("gbk") != string::npos || codesetLower.find("cp936") != string::npos)
        {
            return AnsiEncodingFamily::gbkLike;
        }
        if (codesetLower.find("big5") != string::npos || codesetLower.find("cp950") != string::npos)
        {
            return AnsiEncodingFamily::big5Like;
        }
        if (codesetLower.find("shift_jis") != string::npos || codesetLower.find("sjis") != string::npos || codesetLower.find("cp932") != string::npos)
        {
            return AnsiEncodingFamily::shiftJisLike;
        }
        if (codesetLower.find("euc-kr") != string::npos || codesetLower.find("cp949") != string::npos)
        {
            return AnsiEncodingFamily::eucKrLike;
        }

        return AnsiEncodingFamily::unknown;
#endif
    }

    struct ByteStats
    {
        size_t totalBytes = 0;
        size_t nullBytes = 0;
        size_t suspiciousControlBytes = 0;
        size_t nonAsciiBytes = 0;
        size_t continuationBytes = 0;
    };

    static ByteStats GetByteStats(const string& bytes)
    {
        ByteStats stats;
        stats.totalBytes = bytes.size();

        for (size_t i = 0; i < bytes.size(); i++)
        {
            const unsigned char ch = static_cast<unsigned char>(bytes[i]);

            if (ch == 0u)
            {
                stats.nullBytes++;
                continue;
            }

            if (ch >= 0x80u)
            {
                stats.nonAsciiBytes++;
                if (ch <= 0xBFu)
                {
                    stats.continuationBytes++;
                }
            }

            // 控制字符（排除 \t \n \r）
            if ((ch < 0x20u && ch != '\t' && ch != '\n' && ch != '\r') || ch == 0x7Fu)
            {
                stats.suspiciousControlBytes++;
            }
        }

        return stats;
    }

    static bool LooksLikeTextBytes(const ByteStats& stats)
    {
        if (stats.totalBytes == 0)
        {
            return false;
        }

        // 小样本：容忍少量控制字符，但不接受 NUL
        if (stats.totalBytes <= 16)
        {
            if (stats.nullBytes > 0)
            {
                return false;
            }
            return stats.suspiciousControlBytes <= 2;
        }

        // NUL 占比 >= 12.5%：高度可疑（UTF-16 / 二进制）
        if (stats.nullBytes > 0 && stats.nullBytes * 8 >= stats.totalBytes)
        {
            return false;
        }

        // 可疑控制字节占比 > 2%：更像二进制
        if (stats.suspiciousControlBytes * 50 >= stats.totalBytes)
        {
            return false;
        }

        return true;
    }

    struct Utf8Strength
    {
        size_t length2Count = 0;
        size_t length3Count = 0;
        size_t length4Count = 0;
        int qualityScore = 0;
    };

    static bool ComputeUtf8Strength(const string& text, Utf8Strength& strength)
    {
        size_t pos = 0;
        bool isFirst = true;

        while (pos < text.size())
        {
            char32_t cp = 0;
            size_t nextPos = pos;
            if (!DecodeOne(text, pos, cp, nextPos))
            {
                return false;
            }

            const size_t byteCount = nextPos - pos;
            if (byteCount == 2)
            {
                strength.length2Count++;
            }
            else if (byteCount == 3)
            {
                strength.length3Count++;
            }
            else if (byteCount == 4)
            {
                strength.length4Count++;
            }

            strength.qualityScore += ScoreCodePoint(cp, isFirst);
            isFirst = false;
            pos = nextPos;
        }

        return true;
    }

    static int ClampInt(int value, int minValue, int maxValue)
    {
        if (value < minValue)
        {
            return minValue;
        }
        if (value > maxValue)
        {
            return maxValue;
        }
        return value;
    }

    // ---- ANSI 字节形态评分（只在常见 East Asian 多字节编码下启用）----

    static int ComputeGbkLikePairScore(const string& bytes)
    {
        const size_t n = bytes.size();
        size_t i = 0;
        int validPairs = 0;
        int invalidBytes = 0;

        while (i < n)
        {
            const unsigned char b0 = static_cast<unsigned char>(bytes[i]);
            if (b0 < 0x80u)
            {
                i++;
                continue;
            }

            if (b0 >= 0x81u && b0 <= 0xFEu && i + 1 < n)
            {
                const unsigned char b1 = static_cast<unsigned char>(bytes[i + 1]);
                if (b1 >= 0x40u && b1 <= 0xFEu && b1 != 0x7Fu)
                {
                    validPairs++;
                    i += 2;
                    continue;
                }
            }

            invalidBytes++;
            i++;
        }

        return validPairs * 3 - invalidBytes * 6;
    }

    static int ComputeBig5LikePairScore(const string& bytes)
    {
        const size_t n = bytes.size();
        size_t i = 0;
        int validPairs = 0;
        int invalidBytes = 0;

        while (i < n)
        {
            const unsigned char b0 = static_cast<unsigned char>(bytes[i]);
            if (b0 < 0x80u)
            {
                i++;
                continue;
            }

            if (b0 >= 0x81u && b0 <= 0xFEu && i + 1 < n)
            {
                const unsigned char b1 = static_cast<unsigned char>(bytes[i + 1]);
                const bool isTrail = (b1 >= 0x40u && b1 <= 0x7Eu) || (b1 >= 0xA1u && b1 <= 0xFEu);
                if (isTrail)
                {
                    validPairs++;
                    i += 2;
                    continue;
                }
            }

            invalidBytes++;
            i++;
        }

        return validPairs * 3 - invalidBytes * 6;
    }

    static int ComputeShiftJisLikePairScore(const string& bytes)
    {
        const size_t n = bytes.size();
        size_t i = 0;
        int validPairs = 0;
        int invalidBytes = 0;

        while (i < n)
        {
            const unsigned char b0 = static_cast<unsigned char>(bytes[i]);
            if (b0 < 0x80u)
            {
                i++;
                continue;
            }

            const bool isLead = (b0 >= 0x81u && b0 <= 0x9Fu) || (b0 >= 0xE0u && b0 <= 0xFCu);
            if (isLead && i + 1 < n)
            {
                const unsigned char b1 = static_cast<unsigned char>(bytes[i + 1]);
                const bool isTrail = (b1 >= 0x40u && b1 <= 0x7Eu) || (b1 >= 0x80u && b1 <= 0xFCu);
                if (isTrail)
                {
                    validPairs++;
                    i += 2;
                    continue;
                }
            }

            invalidBytes++;
            i++;
        }

        return validPairs * 3 - invalidBytes * 6;
    }

    static int ComputeEucKrLikePairScore(const string& bytes)
    {
        const size_t n = bytes.size();
        size_t i = 0;
        int validPairs = 0;
        int invalidBytes = 0;

        while (i < n)
        {
            const unsigned char b0 = static_cast<unsigned char>(bytes[i]);
            if (b0 < 0x80u)
            {
                i++;
                continue;
            }

            if (b0 >= 0xA1u && b0 <= 0xFEu && i + 1 < n)
            {
                const unsigned char b1 = static_cast<unsigned char>(bytes[i + 1]);
                if (b1 >= 0xA1u && b1 <= 0xFEu)
                {
                    validPairs++;
                    i += 2;
                    continue;
                }
            }

            invalidBytes++;
            i++;
        }

        return validPairs * 3 - invalidBytes * 6;
    }

    static int ComputeAnsiPairScore(const string& bytes)
    {
        const AnsiEncodingFamily family = GetAnsiEncodingFamily();
        if (family == AnsiEncodingFamily::gbkLike)
        {
            return ComputeGbkLikePairScore(bytes);
        }
        if (family == AnsiEncodingFamily::big5Like)
        {
            return ComputeBig5LikePairScore(bytes);
        }
        if (family == AnsiEncodingFamily::shiftJisLike)
        {
            return ComputeShiftJisLikePairScore(bytes);
        }
        if (family == AnsiEncodingFamily::eucKrLike)
        {
            return ComputeEucKrLikePairScore(bytes);
        }
        return 0;
    }
}

string GB_MakeUtf8String(const char* s)
{
    if (!s)
    {
        return {};
    }
    return string(s);
}

string GB_MakeUtf8String(char32_t utf8Char)
{
    uint32_t u = static_cast<uint32_t>(utf8Char);
    string out;

    if (!internal::IsValidUnicode(u))
    {
        // 用 U+FFFD 作为替代
        u = 0xFFFD;
    }

    if (u <= 0x7F)
    {
        out.push_back(static_cast<char>(u));
    }
    else if (u <= 0x7FF)
    {
        out.push_back(static_cast<char>(0xC0 | (u >> 6)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    }
    else if (u <= 0xFFFF)
    {
        out.push_back(static_cast<char>(0xE0 | (u >> 12)));
        out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    }
    else // u <= 0x10FFFF
    {
        out.push_back(static_cast<char>(0xF0 | (u >> 18)));
        out.push_back(static_cast<char>(0x80 | ((u >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    }

    return out;
}

string GB_Utf8ToAnsi(const string& utf8Str)
{
    if (utf8Str.empty())
    {
        return {};
    }
#if defined(_WIN32)
    // UTF-8 -> UTF-16
    const int wlen = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.data(), internal::ToWinApiLengthChecked(utf8Str.size()), nullptr, 0);
    if (wlen <= 0)
    {
        throw runtime_error("MultiByteToWideChar(CP_UTF8) failed (size).");
    }
    wstring ws(static_cast<size_t>(wlen), L'\0');
    const int wwritten = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.data(), internal::ToWinApiLengthChecked(utf8Str.size()), &ws[0], wlen);
    if (wwritten <= 0)
    {
        throw runtime_error("MultiByteToWideChar(CP_UTF8) failed (convert).");
    }

    // UTF-16 -> ANSI(ACP)
    // 说明：CP_ACP 为系统 ANSI 代码页；不同机器可能不同，且会被用户修改。
    const int alen = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (alen <= 0)
    {
        throw runtime_error("WideCharToMultiByte(CP_ACP) failed (size).");
    }
    string ansi(static_cast<size_t>(alen), '\0');
    BOOL usedDefaultChar = FALSE;
    const int awritten = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, ws.data(), static_cast<int>(ws.size()), &ansi[0], alen, nullptr, &usedDefaultChar);
    if (awritten <= 0)
    {
        throw runtime_error("WideCharToMultiByte(CP_ACP) failed (convert).");
    }
    // 这里不把 usedDefaultChar 当作错误抛出；如需“严格模式”可改为检测后抛异常。
    return ansi;

#else
    if (utf8Str.empty())
    {
        return {};
    }

    internal::EnsureLocaleInitialized();

    // UTF-8 -> wstring
    wstring ws;
    try
    {
        ws = internal::Utf8ToWString_Posix(utf8Str);
    }
    catch (const range_error&)
    {
        throw runtime_error("UTF-8 decoding failed.");
    }

    // wstring -> 本地多字节（依赖 LC_CTYPE）
    const wchar_t* src = ws.c_str();
    mbstate_t st = mbstate_t{};
    // 1) 预计算所需字节数（不含终止 '\0'）
    errno = 0;
    size_t need = wcsrtombs(nullptr, &src, 0, &st);
    if (need == static_cast<size_t>(-1))
    {
        throw runtime_error("Local multibyte encoding failed (wstring -> bytes).");
    }

    if (need == 0)
    {
        return {};
    }

    string out(need, '\0');
    src = ws.c_str();
    st = mbstate_t{};
    errno = 0;
    size_t written = wcsrtombs(&out[0], &src, need, &st);
    if (written == static_cast<size_t>(-1))
    {
        throw runtime_error("Local multibyte encoding failed (wstring -> bytes).");
    }
    if (written < out.size())
    {
        out.resize(written);
    }
    return out;
#endif
}

string GB_AnsiToUtf8(const string& ansiStr)
{
    if (ansiStr.empty())
    {
        return {};
    }
#if defined(_WIN32)
    // ANSI(ACP) -> UTF-16
    const int wlen = ::MultiByteToWideChar(CP_ACP, 0, ansiStr.data(), internal::ToWinApiLengthChecked(ansiStr.size()), nullptr, 0);
    if (wlen <= 0)
    {
        throw runtime_error("MultiByteToWideChar(CP_ACP) failed (size).");
    }
    wstring ws(static_cast<size_t>(wlen), L'\0');
    const int wwritten = ::MultiByteToWideChar(CP_ACP, 0, ansiStr.data(), internal::ToWinApiLengthChecked(ansiStr.size()), &ws[0], wlen);
    if (wwritten <= 0)
    {
        throw runtime_error("MultiByteToWideChar(CP_ACP) failed (convert).");
    }

    // UTF-16 -> UTF-8
    const int u8len = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (u8len <= 0)
    {
        throw runtime_error("WideCharToMultiByte(CP_UTF8) failed (size).");
    }
    string utf8(static_cast<size_t>(u8len), '\0');
    const int u8written = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), &utf8[0], u8len, nullptr, nullptr);
    if (u8written <= 0)
    {
        throw runtime_error("WideCharToMultiByte(CP_UTF8) failed (convert).");
    }
    return utf8;

#else
    if (ansiStr.empty())
    {
        return {};
    }

    internal::EnsureLocaleInitialized();

    // 本地多字节 -> wstring（依赖 LC_CTYPE）
    const char* src = ansiStr.c_str();
    mbstate_t st = mbstate_t{};
    errno = 0;
    // 1) 计算需要的 wchar_t 数量（不含终止 L'\0'）
    size_t wlen = mbsrtowcs(nullptr, &src, 0, &st);
    if (wlen == static_cast<size_t>(-1))
    {
        throw runtime_error("Local multibyte decoding failed (bytes -> wstring).");
    }

    if (wlen == 0)
    {
        return {};
    }

    wstring ws(wlen, L'\0');
    src = ansiStr.c_str();
    st = mbstate_t{};
    errno = 0;
    size_t wwritten = mbsrtowcs(&ws[0], &src, wlen, &st);
    if (wwritten == static_cast<size_t>(-1))
    {
        throw runtime_error("Local multibyte decoding failed (bytes -> wstring).");
    }
    if (wwritten < ws.size())
    {
        ws.resize(wwritten);
    }

    // wstring -> UTF-8
    try
    {
        return internal::WStringToUtf8_Posix(ws);
    }
    catch (const range_error&)
    {
        throw runtime_error("UTF-8 encoding failed.");
    }
#endif
}

bool GB_IsUtf8(const string& text)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        // 严格：一旦解码失败立即判 false（DecodeOne 已按 RFC 3629 检查最短编码/代理项等）
        if (!internal::DecodeOne(text, pos, cp, nextPos))
        {
            return false;
        }
        pos = nextPos;
    }
    return true;
}

bool GB_IsAnsi(const string& text)
{
    if (text.empty())
    {
        return true;
    }

    // 若系统“ANSI 代码页/locale”本身就是 UTF-8，那么 ANSI 与 UTF-8 在这里等价
    if (internal::IsCurrentAnsiUtf8())
    {
        return GB_IsUtf8(text);
    }

#if defined(_WIN32)
    const UINT codePage = CP_ACP;

    int wideLength = ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);

    if (wideLength > 0)
    {
        return true;
    }

    const DWORD lastError = ::GetLastError();
    if (lastError == ERROR_INVALID_FLAGS)
    {
        // 个别环境下可能不支持 MB_ERR_INVALID_CHARS，退化为“能否转换”
        wideLength = ::MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        return wideLength > 0;
    }

    return false;

#else
    internal::EnsureLocaleInitialized();

    const char* src = text.c_str();
    mbstate_t state = mbstate_t{};
    errno = 0;

    const size_t wideLength = mbsrtowcs(nullptr, &src, 0, &state);
    return wideLength != static_cast<size_t>(-1);
#endif
}

bool GB_LooksLikeUtf8(const string& text)
{
    if (text.empty())
    {
        return false;
    }

    // BOM 是强线索
    if (internal::HasUtf8Bom(text))
    {
        return true;
    }

    // 纯 ASCII 无法区分 UTF-8 与 ANSI
    if (internal::IsAllAscii(text))
    {
        return false;
    }

    const internal::ByteStats byteStats = internal::GetByteStats(text);
    if (!internal::LooksLikeTextBytes(byteStats))
    {
        return false;
    }

    // 严格 UTF-8 合法性校验
    if (!GB_IsUtf8(text))
    {
        return false;
    }

    // 若当前“ANSI”本身就是 UTF-8，那么这里直接认为更像 UTF-8
    if (internal::IsCurrentAnsiUtf8())
    {
        return true;
    }

    internal::Utf8Strength utf8Strength;
    if (!internal::ComputeUtf8Strength(text, utf8Strength))
    {
        return false;
    }

    // 防止“二进制碰巧合法 UTF-8”
    if (utf8Strength.qualityScore < -20)
    {
        return false;
    }

    const double continuationRatio = (byteStats.nonAsciiBytes > 0) ? static_cast<double>(byteStats.continuationBytes) / static_cast<double>(byteStats.nonAsciiBytes) : 0;

    int utf8Confidence = 0;
    utf8Confidence += static_cast<int>(utf8Strength.length4Count) * 12;
    utf8Confidence += static_cast<int>(utf8Strength.length3Count) * 6;
    utf8Confidence += static_cast<int>(utf8Strength.length2Count) * 2;

    if (continuationRatio >= 0.62)
    {
        utf8Confidence += 10;
    }
    else if (continuationRatio >= 0.55)
    {
        utf8Confidence += 6;
    }
    else if (continuationRatio >= 0.48)
    {
        utf8Confidence += 2;
    }
    else
    {
        utf8Confidence -= 6;
    }

    utf8Confidence += internal::ClampInt(utf8Strength.qualityScore / 10, -10, 10);

    const internal::AnsiEncodingFamily family = internal::GetAnsiEncodingFamily();
    if (family == internal::AnsiEncodingFamily::singleByte || family == internal::AnsiEncodingFamily::unknown)
    {
        // 单字节 ANSI 下：严格 UTF-8 且非 ASCII，通常就应该判 UTF-8
        return utf8Confidence >= 0;
    }

    // 多字节 East Asian ANSI：对比一下“字节形态”得分
    const int ansiPairScore = internal::ComputeAnsiPairScore(text);

    if (continuationRatio >= 0.58)
    {
        // continuationRatio 偏高是 UTF-8 的强特征（CJK UTF-8 常见 ~2/3）
        utf8Confidence += 6;
    }

    const int scoreMargin = 6; // 越大越保守
    return utf8Confidence >= ansiPairScore + scoreMargin;
}

bool GB_LooksLikeAnsi(const string& text)
{
    if (text.empty())
    {
        return false;
    }

    // 纯 ASCII 无法区分 UTF-8 与 ANSI
    if (internal::IsAllAscii(text))
    {
        return false;
    }

    // 若当前“ANSI”本身就是 UTF-8，那么 LooksLikeAnsi 退化为 LooksLikeUtf8
    if (internal::IsCurrentAnsiUtf8())
    {
        return GB_LooksLikeUtf8(text);
    }

    const internal::ByteStats byteStats = internal::GetByteStats(text);
    if (!internal::LooksLikeTextBytes(byteStats))
    {
        return false;
    }

    // 先确保按 ANSI 规则可解码
    if (!GB_IsAnsi(text))
    {
        return false;
    }

    // 如果严格 UTF-8 不通过，那就更可能是 ANSI（或其它非 UTF-8 编码）
    if (!GB_IsUtf8(text))
    {
        return true;
    }

    // 同时是合法 UTF-8 与合法 ANSI：只有在“ANSI 字节形态”明显更强时才判 ANSI
    internal::Utf8Strength utf8Strength;
    if (!internal::ComputeUtf8Strength(text, utf8Strength))
    {
        return false;
    }

    if (utf8Strength.qualityScore < -20)
    {
        return false;
    }

    const double continuationRatio = (byteStats.nonAsciiBytes > 0) ? static_cast<double>(byteStats.continuationBytes) / static_cast<double>(byteStats.nonAsciiBytes) : 0;

    // continuationRatio 很高且存在 3/4 字节码点时，通常非常像 UTF-8
    if (continuationRatio >= 0.60 && (utf8Strength.length3Count + utf8Strength.length4Count) > 0)
    {
        return false;
    }

    int utf8Confidence = 0;
    utf8Confidence += static_cast<int>(utf8Strength.length4Count) * 12;
    utf8Confidence += static_cast<int>(utf8Strength.length3Count) * 6;
    utf8Confidence += static_cast<int>(utf8Strength.length2Count) * 2;

    if (continuationRatio >= 0.62)
    {
        utf8Confidence += 10;
    }
    else if (continuationRatio >= 0.55)
    {
        utf8Confidence += 6;
    }
    else if (continuationRatio >= 0.48)
    {
        utf8Confidence += 2;
    }
    else
    {
        utf8Confidence -= 6;
    }

    utf8Confidence += internal::ClampInt(utf8Strength.qualityScore / 10, -10, 10);

    const int ansiPairScore = internal::ComputeAnsiPairScore(text);

    const int scoreMargin = 6;
    return ansiPairScore >= utf8Confidence + scoreMargin;
}

string GB_WStringToUtf8(const wstring& ws)
{
#if defined(_WIN32)
    if (ws.empty())
    {
        return {};
    }

    // 1) 计算所需字节数（不含 '\0'）
    const int sizeRequired = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (sizeRequired <= 0)
    {
        throw runtime_error("WideCharToMultiByte failed (size).");
    }

    // 2) 实际转换
    string result(static_cast<size_t>(sizeRequired), '\0');
    int written = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ws.data(), static_cast<int>(ws.size()), &result[0], sizeRequired, nullptr, nullptr);
    if (written <= 0)
    {
        throw runtime_error("WideCharToMultiByte failed (convert).");
    }
    return result;
#else
    if (ws.empty())
    {
        return {};
    }

    std::wstring_convert<std::codecvt_utf8<wchar_t>> cvt;
    try
    {
        return cvt.to_bytes(ws);
    }
    catch (const range_error&)
    {
        throw runtime_error("GB_WStringToUtf8 conversion failed.");
    }
#endif
}

wstring GB_Utf8ToWString(const string& utf8Str)
{
#if defined(_WIN32)
    if (utf8Str.empty())
    {
        return {};
    }

    // 1) 计算需要的 wchar_t 数量（不含 '\0'）
    const int sizeRequired = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.data(), internal::ToWinApiLengthChecked(utf8Str.size()), nullptr, 0);
    if (sizeRequired <= 0)
    {
        throw runtime_error("MultiByteToWideChar failed (size).");
    }

    // 2) 实际转换
    wstring result(static_cast<size_t>(sizeRequired), L'\0');
    int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.data(), internal::ToWinApiLengthChecked(utf8Str.size()), &result[0], sizeRequired);
    if (written <= 0)
    {
        throw runtime_error("MultiByteToWideChar failed (convert).");
    }
    return result;
#else
    if (utf8Str.empty())
    {
        return {};
    }

    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    try
    {
        return conv.from_bytes(utf8Str);
    }
    catch (const range_error&)
    {
        throw runtime_error("GB_Utf8ToWString conversion failed.");
    }
#endif
}

// 获取 UTF-8 字符串的长度（以 UTF-8 字符/码点 为单位）
size_t GB_GetUtf8Length(const string& utf8Str)
{
    size_t len = 0;
    size_t pos = 0;
    while (pos < utf8Str.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        internal::DecodeOne(utf8Str, pos, cp, nextPos); // 成功或失败都前进
        pos = nextPos;
        len++; // 非法字节按 1 个“字符”统计
    }
    return len;
}

char32_t GB_GetUtf8Char(const string& utf8Str, int64_t index)
{
    // 不抛异常，不用可选类型：失败返回一个不可能出现的值 0x110000（> U+10FFFF）
    static constexpr char32_t kInvalidCodePoint = 0x110000;

    if (index < 0)
    {
        return kInvalidCodePoint;
    }

    size_t pos = 0;
    int64_t curIndex = 0;

    while (pos < utf8Str.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        bool ok = internal::DecodeOne(utf8Str, pos, cp, nextPos); // 失败也会 nextPos = pos + 1

        if (curIndex == index)
        {
            if (!ok)
            {
                return kInvalidCodePoint;              // 该“字符”本身就是非法起始字节
            }
            return cp;
        }

        pos = nextPos;
        curIndex++;                        // 非法字节按“一个字符”计数，与你前面 API 约定一致
    }
    return kInvalidCodePoint;
}

string GB_Utf8Substr(const string& utf8Str, int64_t start, int64_t length)
{
    if (start < 0 || length < 0)
    {
        return {}; // 不支持负索引；负长度视为空
    }

    // 快速返回：空串
    if (utf8Str.empty() || length == 0)
    {
        return {};
    }

    size_t pos = 0;
    int64_t charIndex = 0;

    // 1) 找到起始码点对应的字节偏移
    size_t startByte = string::npos;
    while (pos < utf8Str.size() && charIndex < start)
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        internal::DecodeOne(utf8Str, pos, cp, nextPos);
        pos = nextPos;
        charIndex++;
    }
    if (charIndex < start)
    {
        return {}; // 起始 >= 总长度
    }
    startByte = pos;

    // 2) 继续前进 length 个码点，得到结束字节偏移
    int64_t remain = length;
    while (pos < utf8Str.size() && remain > 0)
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        internal::DecodeOne(utf8Str, pos, cp, nextPos);
        pos = nextPos;
        remain--;
    }
    const size_t endByte = pos; // 若提前结束，endByte==size()
    return utf8Str.substr(startByte, endByte - startByte);
}

string GB_Utf8ToLower(const string& utf8Str)
{
    string out;
    out.reserve(utf8Str.size()); // 最终长度不会超过原串

    size_t pos = 0;
    while (pos < utf8Str.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        bool ok = internal::DecodeOne(utf8Str, pos, cp, nextPos);

        // 对于 ASCII（单字节、且 < 0x80），做大小写转换
        if (ok && nextPos == pos + 1)
        {
            unsigned char b0 = static_cast<unsigned char>(utf8Str[pos]);
            if (b0 < 0x80)
            {
                out.push_back(internal::ToLowerAsciiChar(static_cast<char>(b0)));
                pos = nextPos;
                continue;
            }
        }

        // 非 ASCII 或解码失败：原样拷贝这段字节
        out.append(utf8Str.data() + pos, utf8Str.data() + nextPos);
        pos = nextPos;
    }
    return out;
}

string GB_Utf8ToUpper(const string& utf8Str)
{
    string out;
    out.reserve(utf8Str.size());

    size_t pos = 0;
    while (pos < utf8Str.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        bool ok = internal::DecodeOne(utf8Str, pos, cp, nextPos);

        if (ok && nextPos == pos + 1)
        {
            unsigned char b0 = static_cast<unsigned char>(utf8Str[pos]);
            if (b0 < 0x80)
            {
                out.push_back(internal::ToUpperAsciiChar(static_cast<char>(b0)));
                pos = nextPos;
                continue;
            }
        }

        out.append(utf8Str.data() + pos, utf8Str.data() + nextPos);
        pos = nextPos;
    }
    return out;
}

vector<string> GB_Utf8Split(const string& textUtf8, char32_t delimiter)
{
    vector<string> parts;

    size_t tokenStart = 0;
    size_t pos = 0;
    while (pos < textUtf8.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        bool ok = internal::DecodeOne(textUtf8, pos, cp, nextPos);
        if (!ok)
        {
            // 非法字节：按原样跳过一个字节（注意：不能把 delimiter 与字节直接比较）
            pos++;
            continue;
        }

        if (cp == delimiter)
        {
            parts.emplace_back(textUtf8.substr(tokenStart, pos - tokenStart));
            tokenStart = nextPos;
        }
        pos = nextPos;
    }

    parts.emplace_back(textUtf8.substr(tokenStart));
    return parts;
}

bool GB_Utf8StartsWith(const string& textUtf8, const string& targetUtf8, bool caseSensitive)
{
    // 与 string::rfind("",0)==0 的语义一致：空目标串恒为 true
    if (targetUtf8.empty())
    {
        return true;
    }

    // ASCII 快速路径：大小写敏感，且两端均为纯 ASCII，直接做字节前缀比较
    auto isAllAscii = [](const string& s) -> bool {
        for (unsigned char ch : s)
        {
            if (ch >= 0x80)
            {
                return false;
            }
        }
        return true;
    };
    if (caseSensitive && isAllAscii(textUtf8) && isAllAscii(targetUtf8))
    {
        if (textUtf8.size() < targetUtf8.size())
        {
            return false;
        }
        return std::char_traits<char>::compare(textUtf8.data(), targetUtf8.data(), targetUtf8.size()) == 0;
    }

    // 通用路径：逐码点对齐比较（不解整串，流式解码）
    size_t posText = 0;
    size_t posPat = 0;

    while (posPat < targetUtf8.size())
    {
        if (posText >= textUtf8.size())
        {
            return false; // 模式未耗尽而文本已到结尾
        }

        char32_t cpText = 0, cpPat = 0;
        size_t nextText = posText, nextPat = posPat;

        internal::DecodeOneOrReplacement(textUtf8, posText, cpText, nextText);
        internal::DecodeOneOrReplacement(targetUtf8, posPat, cpPat, nextPat);

        if (!caseSensitive)
        {
            cpText = internal::ToLowerAscii(cpText);
            cpPat = internal::ToLowerAscii(cpPat);
        }

        if (cpText != cpPat)
        {
            return false;
        }

        posText = nextText;
        posPat = nextPat;
    }

    // 成功消费完整的模式串
    return true;
}

bool GB_Utf8EndsWith(const string& textUtf8, const string& targetUtf8, bool caseSensitive)
{
    // 1) 空目标：恒真
    if (targetUtf8.empty())
    {
        return true;
    }

    // 2) ASCII + 大小写敏感：字节后缀快速路径

    if (caseSensitive && internal::IsAllAscii(textUtf8) && internal::IsAllAscii(targetUtf8))
    {
        if (targetUtf8.size() > textUtf8.size())
        {
            return false;
        }
        const size_t off = textUtf8.size() - targetUtf8.size();
        // 手写比较，避免额外依赖
        for (size_t i = 0; i < targetUtf8.size(); i++)
        {
            if (textUtf8[off + i] != targetUtf8[i])
            {
                return false;
            }
        }
        return true;
    }

    // 3) 通用路径：按“码点”比较（非法字节用 U+FFFD 消费 1 字节）
    // 3.1 解码模式串
    vector<char32_t> pat;
    pat.reserve(targetUtf8.size()); // 上界，不会越界
    {
        size_t pos = 0;
        while (pos < targetUtf8.size())
        {
            char32_t cp = 0;
            size_t nextPos = pos;
            internal::DecodeOneOrReplacement(targetUtf8, pos, cp, nextPos);
            if (!caseSensitive)
            {
                cp = internal::ToLowerAscii(cp); // 仅 ASCII 折叠，保持与库里其他函数一致
            }
            pat.push_back(cp);
            pos = nextPos;
        }
    }

    const size_t m = pat.size();
    if (m == 0)
    {
        // 正常情况下不会出现（即使都是非法字节也会得到若干 U+FFFD），兜底返回 true
        return true;
    }

    // 3.2 流式解码 text，只保留“最后 m 个码点”
    vector<char32_t> ring(m);
    size_t written = 0;

    {
        size_t pos = 0;
        while (pos < textUtf8.size())
        {
            char32_t cp = 0;
            size_t nextPos = pos;
            internal::DecodeOneOrReplacement(textUtf8, pos, cp, nextPos);
            if (!caseSensitive)
            {
                cp = internal::ToLowerAscii(cp);
            }
            ring[written % m] = cp;
            written++;
            pos = nextPos;
        }
    }

    if (written < m)
    {
        // 文本的码点数 < 模式码点数 → 不可能是后缀
        return false;
    }

    // 3.3 比较最后 m 个码点是否与 pat 一致
    const size_t start = (written - m) % m;
    for (size_t i = 0; i < m; i++)
    {
        if (ring[(start + i) % m] != pat[i])
        {
            return false;
        }
    }
    return true;
}

int64_t GB_Utf8Find(const string& text, const string& needle, bool caseSensitive)
{
    // 1) 预解码模式串到码点数组（并可选 ASCII 折叠）
    vector<char32_t> pat;
    {
        size_t pos = 0;
        while (pos < needle.size())
        {
            char32_t cp = 0;
            size_t nextPos = pos;
            internal::DecodeOneOrReplacement(needle, pos, cp, nextPos);
            if (!caseSensitive)
            {
                cp = internal::ToLowerAscii(cp);
            }
            pat.push_back(cp);
            pos = nextPos;
        }
    }

    const size_t m = pat.size();
    if (m == 0)
    {
        return 0; // 与 string::find("") 一致
    }
    // 2) 计算 KMP 的前缀函数（LPS）
    vector<size_t> lps(m, 0);
    {
        size_t len = 0;
        size_t i = 1;
        while (i < m)
        {
            if (pat[i] == pat[len])
            {
                len++;
                lps[i] = len;
                i++;
            }
            else if (len != 0)
            {
                len = lps[len - 1];
            }
            else
            {
                lps[i] = 0;
                i++;
            }
        }
    }

    // 3) 流式解码 text 并进行 KMP 匹配（无需整串展开为码点向量）
    size_t j = 0;                // 已匹配 pat[0..j-1]
    size_t textBytePos = 0;      // 字节位置
    int64_t textCharIndex = 0;   // 已读码点数量（也就是当前码点索引）

    while (textBytePos < text.size())
    {
        char32_t cp = 0;
        size_t nextPos = textBytePos;
        internal::DecodeOneOrReplacement(text, textBytePos, cp, nextPos);
        if (!caseSensitive)
        {
            cp = internal::ToLowerAscii(cp);
        }

        while (j > 0 && cp != pat[j])
        {
            j = lps[j - 1];
        }
        if (cp == pat[j])
        {
            j++;
            if (j == m)
            {
                // 命中：起始“码点偏移” = 当前码点索引 - m + 1
                return textCharIndex - static_cast<int64_t>(m) + 1;
            }
        }

        textBytePos = nextPos;
        textCharIndex++;
    }

    return -1;
}

int64_t GB_Utf8FindLast(const string& text, const string& needle, bool caseSensitive)
{
    if (needle.empty())
    {
        return static_cast<int64_t>(GB_GetUtf8Length(text));
    }

    // —— ASCII + 大小写敏感：字节级快速路径（ASCII 下“字节偏移 == 码点偏移”）—— //
    if (caseSensitive && internal::IsAllAscii(text) && internal::IsAllAscii(needle))
    {
        const size_t pos = text.rfind(needle);
        if (pos == string::npos)
        {
            return -1;
        }
        return static_cast<int64_t>(pos);
    }

    // 1) 预解码模式串为码点序列，并按需做 ASCII 折叠
    vector<char32_t> pat;
    {
        size_t pos = 0;
        while (pos < needle.size())
        {
            char32_t cp = 0;
            size_t nextPos = pos;
            internal::DecodeOneOrReplacement(needle, pos, cp, nextPos);
            if (!caseSensitive)
            {
                cp = internal::ToLowerAscii(cp);
            }
            pat.push_back(cp);
            pos = nextPos;
        }
    }

    const size_t m = pat.size();
    if (m == 0)
    {
        // 理论上不会走到（非法字节也会转为 U+FFFD），兜底与上面保持一致
        return static_cast<int64_t>(GB_GetUtf8Length(text));
    }
    // 2) 计算 KMP 的 LPS（最长真前后缀）表
    vector<size_t> lps(m, 0);
    {
        size_t len = 0;
        size_t i = 1;
        while (i < m)
        {
            if (pat[i] == pat[len])
            {
                len++;
                lps[i] = len;
                i++;
            }
            else if (len != 0)
            {
                len = lps[len - 1];
            }
            else
            {
                lps[i] = 0;
                i++;
            }
        }
    }

    // 3) 前向扫描 text，记录“最后一次命中”的起始码点索引
    size_t j = 0;                   // 已匹配 pat[0..j-1]
    size_t textBytePos = 0;         // 当前字节位置
    int64_t textCharIndex = 0;      // 已消费的码点数（当前码点索引）
    int64_t lastMatchIndex = -1;    // 结果：最后一次命中的起始码点索引

    while (textBytePos < text.size())
    {
        char32_t cp = 0;
        size_t nextPos = textBytePos;
        internal::DecodeOneOrReplacement(text, textBytePos, cp, nextPos);
        if (!caseSensitive)
        {
            cp = internal::ToLowerAscii(cp);
        }

        while (j > 0 && cp != pat[j])
        {
            j = lps[j - 1];
        }
        if (cp == pat[j])
        {
            j++;
            if (j == m)
            {
                // 记录命中位置（起始码点偏移）
                lastMatchIndex = textCharIndex - static_cast<int64_t>(m) + 1;
                // 继续搜索以支持重叠匹配
                j = lps[j - 1];
            }
        }

        textBytePos = nextPos;
        textCharIndex++;
    }

    return lastMatchIndex;
}

string GB_Utf8Trim(const string& utf8Str, const string& trimChars)
{
    if (utf8Str.empty() || trimChars.empty())
    {
        return utf8Str;
    }

    // ASCII 快速路径
    if (internal::IsAllAscii(utf8Str) && internal::IsAllAscii(trimChars))
    {
        const size_t first = utf8Str.find_first_not_of(trimChars);
        if (first == string::npos)
        {
            return string();
        }
        const size_t last = utf8Str.find_last_not_of(trimChars);
        return utf8Str.substr(first, last - first + 1);
    }

    const unordered_set<char32_t> trimSet = internal::BuildTrimSet(trimChars);
    // 左修剪后再右修剪（两次单次线性扫描）
    return internal::TrimRightImpl(internal::TrimLeftImpl(utf8Str, trimSet), trimSet);
}

string GB_Utf8TrimLeft(const string& utf8Str, const string& trimChars)
{
    if (utf8Str.empty() || trimChars.empty())
    {
        return utf8Str;
    }

    // ASCII 快速路径
    if (internal::IsAllAscii(utf8Str) && internal::IsAllAscii(trimChars))
    {
        const size_t first = utf8Str.find_first_not_of(trimChars);
        if (first == string::npos)
        {
            return string();
        }
        return utf8Str.substr(first);
    }

    const unordered_set<char32_t> trimSet = internal::BuildTrimSet(trimChars);
    return internal::TrimLeftImpl(utf8Str, trimSet);
}

string GB_Utf8TrimRight(const string& utf8Str, const string& trimChars)
{
    if (utf8Str.empty() || trimChars.empty())
    {
        return utf8Str;
    }

    // ASCII 快速路径
    if (internal::IsAllAscii(utf8Str) && internal::IsAllAscii(trimChars))
    {
        size_t last = utf8Str.find_last_not_of(trimChars);
        if (last == string::npos)
        {
            return string();
        }
        return utf8Str.substr(0, last + 1);
    }

    const unordered_set<char32_t> trimSet = internal::BuildTrimSet(trimChars);
    return internal::TrimRightImpl(utf8Str, trimSet);
}

string GB_Utf8Replace(const string& utf8Str, const string& oldValue, const string& newValue, bool caseSensitive)
{
    // 空串或空模式：直接返回
    if (utf8Str.empty() || oldValue.empty())
    {
        return utf8Str;
    }

    // 说明：
    // 1) 本库的“大小写不敏感”仅对 ASCII 字母生效；非 ASCII 内容按字节精确匹配。
    // 2) UTF-8 是自同步编码：合法 UTF-8 子串的首字节不可能落在另一个码点的续字节(10xxxxxx)上，
    //    因此对合法 UTF-8 文本，按字节序列查找/替换不会产生“从码点内部开始匹配”的伪命中。
    // 3) 这里使用字节级 KMP 做一次线性扫描与拼接，避免 GB_Utf8Substr + GB_Utf8Find 循环导致的 O(N^2) 退化。

    return internal::ReplaceAllBytesKmp(utf8Str, oldValue, newValue, caseSensitive);
}

