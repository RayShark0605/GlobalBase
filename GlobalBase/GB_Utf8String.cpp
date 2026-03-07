#include "GB_Utf8String.h"
#include <unordered_set>
#include <algorithm>
#include <stdexcept>
#include <climits>
#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <array>

#if defined(_WIN32)
#include <windows.h>
#include <shlwapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "Shlwapi.lib")
#endif
#else
#include <clocale>
#include <cwchar>
#include <cerrno>
#include <locale>
#include <codecvt>
#include <langinfo.h>
#include <iconv.h>
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

    static string ReplaceAllBytesExactFind(const string& text, const string& oldValue, const string& newValue)
    {
        if (text.empty() || oldValue.empty())
        {
            return text;
        }
        if (oldValue.size() > text.size())
        {
            return text;
        }

        string out;
        out.reserve(text.size());

        size_t searchPos = 0;
        size_t matchPos = text.find(oldValue, searchPos);
        if (matchPos == string::npos)
        {
            return text;
        }

        do
        {
            if (matchPos > searchPos)
            {
                out.append(text, searchPos, matchPos - searchPos);
            }
            out += newValue;

            searchPos = matchPos + oldValue.size();
            matchPos = text.find(oldValue, searchPos);
        } while (matchPos != string::npos);

        if (searchPos < text.size())
        {
            out.append(text, searchPos, text.size() - searchPos);
        }

        return out;
    }

    static string ReplaceAllBytesKmp(const string& text, const string& oldValue, const string& newValue, bool caseSensitive)
    {
        if (caseSensitive)
        {
            return ReplaceAllBytesExactFind(text, oldValue, newValue);
        }

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


    // 与 Qt6 的 QChar::isSpace(char32_t) 保持一致的“空白”判定。
    // 规则（来自 Qt 的实现）：
    // - ASCII: U+0020 或 [U+0009..U+000D]（\t \n \v \f \r）
    // - 额外的 C0/C1 控制: U+0085
    // - 不换行空格: U+00A0
    // - 以及 Unicode Separator 类别中的空白：U+1680、U+2000..U+200A、U+2028、U+2029、U+202F、U+205F、U+3000
    static bool IsQtSpace(char32_t ucs4)
    {
        if (ucs4 == 0x20 || (ucs4 >= 0x09 && ucs4 <= 0x0D))
        {
            return true;
        }

        // 注意：Qt 对 0x85 / 0xA0 做了显式特判（它们不属于 Separator_* 类别）
        if (ucs4 == 0x85 || ucs4 == 0xA0)
        {
            return true;
        }

        if (ucs4 == 0x1680 ||
            (ucs4 >= 0x2000 && ucs4 <= 0x200A) ||
            ucs4 == 0x2028 ||
            ucs4 == 0x2029 ||
            ucs4 == 0x202F ||
            ucs4 == 0x205F ||
            ucs4 == 0x3000)
        {
            return true;
        }

        return false;
    }

    static bool IsQtSpaceByte(unsigned char ch)
    {
        return ch == 0x20 || (ch >= 0x09 && ch <= 0x0D);
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

    static int NormalizeCompareResult(int compareResult)
    {
        if (compareResult < 0)
        {
            return -1;
        }
        if (compareResult > 0)
        {
            return 1;
        }
        return 0;
    }

    static bool IsAsciiDigitCodePoint(char32_t codePoint)
    {
        return codePoint >= U'0' && codePoint <= U'9';
    }

    static size_t FindAsciiDigitRunEnd(const string& text, size_t startPos)
    {
        size_t pos = startPos;
        while (pos < text.size())
        {
            char32_t codePoint = 0;
            size_t nextPos = pos;
            internal::DecodeOneOrReplacement(text, pos, codePoint, nextPos);
            if (nextPos != pos + 1 || !IsAsciiDigitCodePoint(codePoint))
            {
                break;
            }
            pos = nextPos;
        }
        return pos;
    }

    static int CompareAsciiDigitRuns(const string& leftText, size_t leftBegin, size_t leftEnd, const string& rightText, size_t rightBegin, size_t rightEnd)
    {
        size_t leftSignificantBegin = leftBegin;
        while (leftSignificantBegin < leftEnd && leftText[leftSignificantBegin] == '0')
        {
            leftSignificantBegin++;
        }

        size_t rightSignificantBegin = rightBegin;
        while (rightSignificantBegin < rightEnd && rightText[rightSignificantBegin] == '0')
        {
            rightSignificantBegin++;
        }

        const size_t leftSignificantLength = leftEnd - leftSignificantBegin;
        const size_t rightSignificantLength = rightEnd - rightSignificantBegin;
        if (leftSignificantLength != rightSignificantLength)
        {
            return (leftSignificantLength < rightSignificantLength) ? -1 : 1;
        }

        if (leftSignificantLength > 0)
        {
            const int contentCompare = std::char_traits<char>::compare(
                leftText.data() + leftSignificantBegin,
                rightText.data() + rightSignificantBegin,
                leftSignificantLength
            );
            if (contentCompare != 0)
            {
                return NormalizeCompareResult(contentCompare);
            }
        }

        const size_t leftTotalLength = leftEnd - leftBegin;
        const size_t rightTotalLength = rightEnd - rightBegin;
        if (leftTotalLength != rightTotalLength)
        {
            return (leftTotalLength > rightTotalLength) ? -1 : 1;
        }

        return 0;
    }

    static bool FindUtf8ByteOffsetByCharIndex(const string& text, int64_t targetCharIndex, size_t& byteOffset)
    {
        if (targetCharIndex < 0)
        {
            return false;
        }

        size_t textBytePos = 0;
        int64_t textCharIndex = 0;

        while (textBytePos < text.size() && textCharIndex < targetCharIndex)
        {
            char32_t codePoint = 0;
            size_t nextPos = textBytePos;
            internal::DecodeOneOrReplacement(text, textBytePos, codePoint, nextPos);
            textBytePos = nextPos;
            textCharIndex++;
        }

        if (textCharIndex != targetCharIndex)
        {
            return false;
        }

        byteOffset = textBytePos;
        return true;
    }

    static int CompareLogicalUtf8Fallback(const string& text1Utf8, const string& text2Utf8)
    {
        size_t pos1 = 0;
        size_t pos2 = 0;

        while (pos1 < text1Utf8.size() && pos2 < text2Utf8.size())
        {
            char32_t codePoint1 = 0;
            char32_t codePoint2 = 0;
            size_t nextPos1 = pos1;
            size_t nextPos2 = pos2;
            internal::DecodeOneOrReplacement(text1Utf8, pos1, codePoint1, nextPos1);
            internal::DecodeOneOrReplacement(text2Utf8, pos2, codePoint2, nextPos2);

            if (nextPos1 == pos1 + 1 && nextPos2 == pos2 + 1 &&
                IsAsciiDigitCodePoint(codePoint1) && IsAsciiDigitCodePoint(codePoint2))
            {
                const size_t digitRunEnd1 = FindAsciiDigitRunEnd(text1Utf8, pos1);
                const size_t digitRunEnd2 = FindAsciiDigitRunEnd(text2Utf8, pos2);
                const int numberCompare = CompareAsciiDigitRuns(text1Utf8, pos1, digitRunEnd1, text2Utf8, pos2, digitRunEnd2);
                if (numberCompare != 0)
                {
                    return numberCompare;
                }

                pos1 = digitRunEnd1;
                pos2 = digitRunEnd2;
                continue;
            }

            const char32_t normalizedCodePoint1 = internal::ToLowerAscii(codePoint1);
            const char32_t normalizedCodePoint2 = internal::ToLowerAscii(codePoint2);
            if (normalizedCodePoint1 != normalizedCodePoint2)
            {
                return (normalizedCodePoint1 < normalizedCodePoint2) ? -1 : 1;
            }

            pos1 = nextPos1;
            pos2 = nextPos2;
        }

        if (pos1 < text1Utf8.size())
        {
            return 1;
        }
        if (pos2 < text2Utf8.size())
        {
            return -1;
        }
        return 0;
    }
}


namespace internal
{
    static string NormalizeEncodingName(const string& encodingName)
    {
        string normalized;
        normalized.reserve(encodingName.size());

        for (size_t i = 0; i < encodingName.size(); i++)
        {
            const unsigned char ch = static_cast<unsigned char>(encodingName[i]);
            if ((ch >= static_cast<unsigned char>('0') && ch <= static_cast<unsigned char>('9')) ||
                (ch >= static_cast<unsigned char>('A') && ch <= static_cast<unsigned char>('Z')) ||
                (ch >= static_cast<unsigned char>('a') && ch <= static_cast<unsigned char>('z')))
            {
                normalized.push_back(internal::ToLowerAsciiChar(static_cast<char>(ch)));
            }
        }

        return normalized;
    }

    static bool TryParseUnsignedInteger(const string& text, unsigned int& value)
    {
        if (text.empty())
        {
            return false;
        }

        unsigned long long result = 0;
        for (size_t i = 0; i < text.size(); i++)
        {
            const unsigned char ch = static_cast<unsigned char>(text[i]);
            if (ch < static_cast<unsigned char>('0') || ch > static_cast<unsigned char>('9'))
            {
                return false;
            }

            result = result * 10ull + static_cast<unsigned long long>(ch - static_cast<unsigned char>('0'));
            if (result > static_cast<unsigned long long>(UINT_MAX))
            {
                return false;
            }
        }

        value = static_cast<unsigned int>(result);
        return true;
    }

    static bool IsUtf8EncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "utf8" ||
            normalizedEncodingName == "cp65001";
    }

    static bool IsUtf8BomEncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "utf8sig" ||
            normalizedEncodingName == "utf8bom" ||
            normalizedEncodingName == "utf8withbom" ||
            normalizedEncodingName == "utf8signature";
    }

    static bool IsAnsiEncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "ansi" ||
            normalizedEncodingName == "acp" ||
            normalizedEncodingName == "system" ||
            normalizedEncodingName == "default" ||
            normalizedEncodingName == "current" ||
            normalizedEncodingName == "native" ||
            normalizedEncodingName == "locale";
    }

    static bool IsOemEncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "oem" ||
            normalizedEncodingName == "oemcp";
    }

#if defined(_WIN32)
    static bool ContainsNullCharacter(const string& text)
    {
        return text.find('\0') != string::npos;
    }
#endif

    static bool IsAsciiEncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "ascii" ||
            normalizedEncodingName == "usascii" ||
            normalizedEncodingName == "ansix341968" ||
            normalizedEncodingName == "iso646us";
    }

    static bool IsLatin1EncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "latin1" ||
            normalizedEncodingName == "latin" ||
            normalizedEncodingName == "iso88591" ||
            normalizedEncodingName == "cp819" ||
            normalizedEncodingName == "ibm819";
    }

    static bool IsUtf16EncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "utf16";
    }

    static bool IsUtf16LeEncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "utf16le";
    }

    static bool IsUtf16BeEncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "utf16be";
    }

    static bool IsUtf32EncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "utf32";
    }

    static bool IsUtf32LeEncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "utf32le";
    }

    static bool IsUtf32BeEncodingName(const string& normalizedEncodingName)
    {
        return normalizedEncodingName == "utf32be";
    }

    static void AppendUtf8CodePoint(string& utf8Text, uint32_t codePoint)
    {
        if (!internal::IsValidUnicode(codePoint))
        {
            throw runtime_error("Invalid Unicode code point.");
        }

        if (codePoint <= 0x7Fu)
        {
            utf8Text.push_back(static_cast<char>(codePoint));
            return;
        }

        if (codePoint <= 0x7FFu)
        {
            utf8Text.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
            utf8Text.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
            return;
        }

        if (codePoint <= 0xFFFFu)
        {
            utf8Text.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
            utf8Text.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
            utf8Text.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
            return;
        }

        utf8Text.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
        utf8Text.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        utf8Text.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        utf8Text.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    }

    static uint16_t ReadUint16(const string& rawBytes, size_t offset, bool littleEndian)
    {
        const unsigned char byte0 = static_cast<unsigned char>(rawBytes[offset]);
        const unsigned char byte1 = static_cast<unsigned char>(rawBytes[offset + 1]);

        if (littleEndian)
        {
            return static_cast<uint16_t>(byte0 | (static_cast<uint16_t>(byte1) << 8));
        }

        return static_cast<uint16_t>((static_cast<uint16_t>(byte0) << 8) | byte1);
    }

    static uint32_t ReadUint32(const string& rawBytes, size_t offset, bool littleEndian)
    {
        const unsigned char byte0 = static_cast<unsigned char>(rawBytes[offset]);
        const unsigned char byte1 = static_cast<unsigned char>(rawBytes[offset + 1]);
        const unsigned char byte2 = static_cast<unsigned char>(rawBytes[offset + 2]);
        const unsigned char byte3 = static_cast<unsigned char>(rawBytes[offset + 3]);

        if (littleEndian)
        {
            return static_cast<uint32_t>(byte0) |
                (static_cast<uint32_t>(byte1) << 8) |
                (static_cast<uint32_t>(byte2) << 16) |
                (static_cast<uint32_t>(byte3) << 24);
        }

        return (static_cast<uint32_t>(byte0) << 24) |
            (static_cast<uint32_t>(byte1) << 16) |
            (static_cast<uint32_t>(byte2) << 8) |
            static_cast<uint32_t>(byte3);
    }

    static string Utf16BytesToUtf8(const string& rawBytes, bool littleEndian, bool requireBom, const string& encodingName)
    {
        if ((rawBytes.size() % 2) != 0)
        {
            throw runtime_error("UTF-16 byte count must be even for encoding: " + encodingName);
        }

        size_t offset = 0;
        bool actualLittleEndian = littleEndian;

        if (rawBytes.size() >= 2)
        {
            const unsigned char byte0 = static_cast<unsigned char>(rawBytes[0]);
            const unsigned char byte1 = static_cast<unsigned char>(rawBytes[1]);
            if (byte0 == 0xFFu && byte1 == 0xFEu)
            {
                actualLittleEndian = true;
                offset = 2;
            }
            else if (byte0 == 0xFEu && byte1 == 0xFFu)
            {
                actualLittleEndian = false;
                offset = 2;
            }
            else if (requireBom)
            {
                throw runtime_error("UTF-16 input without BOM is ambiguous for encoding: " + encodingName);
            }
        }
        else if (requireBom)
        {
            throw runtime_error("UTF-16 input requires a BOM when encoding name is UTF-16.");
        }

        string utf8Text;
        utf8Text.reserve(rawBytes.size() * 2);

        while (offset < rawBytes.size())
        {
            const uint16_t firstWord = ReadUint16(rawBytes, offset, actualLittleEndian);
            offset += 2;

            uint32_t codePoint = firstWord;
            if (firstWord >= 0xD800u && firstWord <= 0xDBFFu)
            {
                if (offset >= rawBytes.size())
                {
                    throw runtime_error("Incomplete UTF-16 surrogate pair for encoding: " + encodingName);
                }

                const uint16_t secondWord = ReadUint16(rawBytes, offset, actualLittleEndian);
                if (secondWord < 0xDC00u || secondWord > 0xDFFFu)
                {
                    throw runtime_error("Invalid UTF-16 surrogate pair for encoding: " + encodingName);
                }

                offset += 2;
                codePoint = 0x10000u +
                    ((static_cast<uint32_t>(firstWord - 0xD800u) << 10) |
                        static_cast<uint32_t>(secondWord - 0xDC00u));
            }
            else if (firstWord >= 0xDC00u && firstWord <= 0xDFFFu)
            {
                throw runtime_error("Unpaired UTF-16 low surrogate for encoding: " + encodingName);
            }

            AppendUtf8CodePoint(utf8Text, codePoint);
        }

        return utf8Text;
    }

    static string Utf32BytesToUtf8(const string& rawBytes, bool littleEndian, bool requireBom, const string& encodingName)
    {
        if ((rawBytes.size() % 4) != 0)
        {
            throw runtime_error("UTF-32 byte count must be divisible by 4 for encoding: " + encodingName);
        }

        size_t offset = 0;
        bool actualLittleEndian = littleEndian;

        if (rawBytes.size() >= 4)
        {
            const unsigned char byte0 = static_cast<unsigned char>(rawBytes[0]);
            const unsigned char byte1 = static_cast<unsigned char>(rawBytes[1]);
            const unsigned char byte2 = static_cast<unsigned char>(rawBytes[2]);
            const unsigned char byte3 = static_cast<unsigned char>(rawBytes[3]);

            if (byte0 == 0xFFu && byte1 == 0xFEu && byte2 == 0x00u && byte3 == 0x00u)
            {
                actualLittleEndian = true;
                offset = 4;
            }
            else if (byte0 == 0x00u && byte1 == 0x00u && byte2 == 0xFEu && byte3 == 0xFFu)
            {
                actualLittleEndian = false;
                offset = 4;
            }
            else if (requireBom)
            {
                throw runtime_error("UTF-32 input without BOM is ambiguous for encoding: " + encodingName);
            }
        }
        else if (requireBom)
        {
            throw runtime_error("UTF-32 input requires a BOM when encoding name is UTF-32.");
        }

        string utf8Text;
        utf8Text.reserve(rawBytes.size());

        while (offset < rawBytes.size())
        {
            const uint32_t codePoint = ReadUint32(rawBytes, offset, actualLittleEndian);
            offset += 4;
            AppendUtf8CodePoint(utf8Text, codePoint);
        }

        return utf8Text;
    }

    static string Latin1ToUtf8(const string& rawBytes)
    {
        string utf8;
        utf8.reserve(rawBytes.size() * 2);

        for (size_t i = 0; i < rawBytes.size(); i++)
        {
            const unsigned char byteValue = static_cast<unsigned char>(rawBytes[i]);
            if (byteValue < 0x80u)
            {
                utf8.push_back(static_cast<char>(byteValue));
            }
            else
            {
                utf8.push_back(static_cast<char>(0xC0u | (byteValue >> 6)));
                utf8.push_back(static_cast<char>(0x80u | (byteValue & 0x3Fu)));
            }
        }

        return utf8;
    }

#if defined(_WIN32)
    static UINT ResolveIso8859WindowsCodePage(const string& normalizedEncodingName)
    {
        if (normalizedEncodingName.size() <= 7 || normalizedEncodingName.compare(0, 7, "iso8859") != 0)
        {
            return 0u;
        }

        const string suffix = normalizedEncodingName.substr(7);
        unsigned int isoPart = 0;
        if (!TryParseUnsignedInteger(suffix, isoPart))
        {
            return 0u;
        }

        switch (isoPart)
        {
        case 1u: return 28591u;
        case 2u: return 28592u;
        case 3u: return 28593u;
        case 4u: return 28594u;
        case 5u: return 28595u;
        case 6u: return 28596u;
        case 7u: return 28597u;
        case 8u: return 28598u;
        case 9u: return 28599u;
        case 13u: return 28603u;
        case 15u: return 28605u;
        default:
            return 0u;
        }
    }

    static UINT ResolveEncodingNameToWindowsCodePage(const string& encodingName)
    {
        const string normalizedEncodingName = NormalizeEncodingName(encodingName);
        if (normalizedEncodingName.empty())
        {
            throw runtime_error("Encoding name is empty.");
        }

        if (IsUtf8EncodingName(normalizedEncodingName))
        {
            return CP_UTF8;
        }
        if (IsAsciiEncodingName(normalizedEncodingName))
        {
            return 20127u;
        }
        if (normalizedEncodingName == "utf16")
        {
            return 1200u;
        }
        if (normalizedEncodingName == "utf16le")
        {
            return 1200u;
        }
        if (normalizedEncodingName == "utf16be")
        {
            return 1201u;
        }
        if (normalizedEncodingName == "utf32")
        {
            return 12000u;
        }
        if (normalizedEncodingName == "utf32le")
        {
            return 12000u;
        }
        if (normalizedEncodingName == "utf32be")
        {
            return 12001u;
        }
        if (normalizedEncodingName == "gbk" || normalizedEncodingName == "xgbk")
        {
            return 936u;
        }
        if (normalizedEncodingName == "gb2312" || normalizedEncodingName == "euccn" ||
            normalizedEncodingName == "gb231280" || normalizedEncodingName == "gb23121980")
        {
            return 936u;
        }
        if (normalizedEncodingName == "gb18030" || normalizedEncodingName == "gb180302000" ||
            normalizedEncodingName == "gb180302005")
        {
            return 54936u;
        }
        if (normalizedEncodingName == "big5")
        {
            return 950u;
        }
        if (normalizedEncodingName == "big5hkscs" || normalizedEncodingName == "bigfivehkscs")
        {
            return 950u;
        }
        if (normalizedEncodingName == "shiftjis" || normalizedEncodingName == "sjis" ||
            normalizedEncodingName == "windows31j" || normalizedEncodingName == "mskanji")
        {
            return 932u;
        }
        if (normalizedEncodingName == "eucjp")
        {
            return 51932u;
        }
        if (normalizedEncodingName == "euckr" || normalizedEncodingName == "ksc5601" ||
            normalizedEncodingName == "ksx1001" || normalizedEncodingName == "uhc")
        {
            return 949u;
        }
        if (normalizedEncodingName == "koi8r")
        {
            return 20866u;
        }
        if (normalizedEncodingName == "koi8u")
        {
            return 21866u;
        }

        const UINT isoCodePage = ResolveIso8859WindowsCodePage(normalizedEncodingName);
        if (isoCodePage != 0u)
        {
            return isoCodePage;
        }

        unsigned int parsedCodePage = 0;
        if (TryParseUnsignedInteger(normalizedEncodingName, parsedCodePage))
        {
            return static_cast<UINT>(parsedCodePage);
        }
        if (normalizedEncodingName.size() > 2 && normalizedEncodingName.compare(0, 2, "cp") == 0 &&
            TryParseUnsignedInteger(normalizedEncodingName.substr(2), parsedCodePage))
        {
            return static_cast<UINT>(parsedCodePage);
        }
        if (normalizedEncodingName.size() > 7 && normalizedEncodingName.compare(0, 7, "windows") == 0 &&
            TryParseUnsignedInteger(normalizedEncodingName.substr(7), parsedCodePage))
        {
            return static_cast<UINT>(parsedCodePage);
        }
        if (normalizedEncodingName.size() > 2 && normalizedEncodingName.compare(0, 2, "ms") == 0 &&
            TryParseUnsignedInteger(normalizedEncodingName.substr(2), parsedCodePage))
        {
            return static_cast<UINT>(parsedCodePage);
        }
        if (normalizedEncodingName.size() > 3 && normalizedEncodingName.compare(0, 3, "ibm") == 0 &&
            TryParseUnsignedInteger(normalizedEncodingName.substr(3), parsedCodePage))
        {
            return static_cast<UINT>(parsedCodePage);
        }

        throw runtime_error("Unsupported encoding name: " + encodingName);
    }

    static wstring ConvertBytesToWideStringByCodePage(const string& rawBytes, UINT codePage)
    {
        if (rawBytes.empty())
        {
            return {};
        }

        DWORD flags = MB_ERR_INVALID_CHARS;
        int wideLength = ::MultiByteToWideChar(
            codePage,
            flags,
            rawBytes.data(),
            internal::ToWinApiLengthChecked(rawBytes.size()),
            nullptr,
            0
        );

        if (wideLength <= 0)
        {
            const DWORD lastError = ::GetLastError();
            if (lastError == ERROR_INVALID_FLAGS)
            {
                flags = 0;
                wideLength = ::MultiByteToWideChar(
                    codePage,
                    flags,
                    rawBytes.data(),
                    internal::ToWinApiLengthChecked(rawBytes.size()),
                    nullptr,
                    0
                );
            }
        }

        if (wideLength <= 0)
        {
            throw runtime_error("MultiByteToWideChar failed for specified source encoding.");
        }

        wstring wideString(static_cast<size_t>(wideLength), L'\0');
        const int written = ::MultiByteToWideChar(
            codePage,
            flags,
            rawBytes.data(),
            internal::ToWinApiLengthChecked(rawBytes.size()),
            &wideString[0],
            wideLength
        );
        if (written <= 0)
        {
            throw runtime_error("MultiByteToWideChar failed for specified source encoding.");
        }

        return wideString;
    }
#else
    static string TrimAsciiWhitespace(const string& text)
    {
        size_t first = 0;
        while (first < text.size())
        {
            const unsigned char ch = static_cast<unsigned char>(text[first]);
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v')
            {
                break;
            }
            first++;
        }

        size_t last = text.size();
        while (last > first)
        {
            const unsigned char ch = static_cast<unsigned char>(text[last - 1]);
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v')
            {
                break;
            }
            last--;
        }

        return text.substr(first, last - first);
    }

    static string ToUpperAsciiString(const string& text)
    {
        string result = text;
        for (size_t i = 0; i < result.size(); i++)
        {
            const char ch = result[i];
            result[i] = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
        }
        return result;
    }

    static void AddUniqueEncodingCandidate(vector<string>& candidates, const string& candidate)
    {
        if (candidate.empty())
        {
            return;
        }

        if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
        {
            candidates.push_back(candidate);
        }
    }

    static void AddWindowsCodePageCandidates(vector<string>& candidates, unsigned int codePage)
    {
        AddUniqueEncodingCandidate(candidates, "CP" + std::to_string(codePage));
        AddUniqueEncodingCandidate(candidates, "WINDOWS-" + std::to_string(codePage));
        AddUniqueEncodingCandidate(candidates, std::to_string(codePage));
    }

    static vector<string> ResolveEncodingNameToPosixCandidates(const string& encodingName)
    {
        const string trimmedEncodingName = TrimAsciiWhitespace(encodingName);
        const string normalizedEncodingName = NormalizeEncodingName(trimmedEncodingName);
        if (normalizedEncodingName.empty())
        {
            throw runtime_error("Encoding name is empty.");
        }

        vector<string> candidates;
        candidates.reserve(8);

        AddUniqueEncodingCandidate(candidates, trimmedEncodingName);
        AddUniqueEncodingCandidate(candidates, ToUpperAsciiString(trimmedEncodingName));

        if (IsUtf8EncodingName(normalizedEncodingName))
        {
            AddUniqueEncodingCandidate(candidates, "UTF-8");
            return candidates;
        }
        if (IsAsciiEncodingName(normalizedEncodingName))
        {
            AddUniqueEncodingCandidate(candidates, "ASCII");
            AddUniqueEncodingCandidate(candidates, "US-ASCII");
            return candidates;
        }
        if (normalizedEncodingName == "utf16")
        {
            AddUniqueEncodingCandidate(candidates, "UTF-16");
            return candidates;
        }
        if (normalizedEncodingName == "utf16le")
        {
            AddUniqueEncodingCandidate(candidates, "UTF-16LE");
            return candidates;
        }
        if (normalizedEncodingName == "utf16be")
        {
            AddUniqueEncodingCandidate(candidates, "UTF-16BE");
            return candidates;
        }
        if (normalizedEncodingName == "utf32")
        {
            AddUniqueEncodingCandidate(candidates, "UTF-32");
            return candidates;
        }
        if (normalizedEncodingName == "utf32le")
        {
            AddUniqueEncodingCandidate(candidates, "UTF-32LE");
            return candidates;
        }
        if (normalizedEncodingName == "utf32be")
        {
            AddUniqueEncodingCandidate(candidates, "UTF-32BE");
            return candidates;
        }
        if (normalizedEncodingName == "gbk" || normalizedEncodingName == "xgbk")
        {
            AddUniqueEncodingCandidate(candidates, "GBK");
            AddUniqueEncodingCandidate(candidates, "CP936");
            return candidates;
        }
        if (normalizedEncodingName == "gb2312" || normalizedEncodingName == "euccn" ||
            normalizedEncodingName == "gb231280" || normalizedEncodingName == "gb23121980")
        {
            AddUniqueEncodingCandidate(candidates, "GB2312");
            AddUniqueEncodingCandidate(candidates, "EUC-CN");
            AddUniqueEncodingCandidate(candidates, "CP936");
            return candidates;
        }
        if (normalizedEncodingName == "gb18030" || normalizedEncodingName == "gb180302000" ||
            normalizedEncodingName == "gb180302005")
        {
            AddUniqueEncodingCandidate(candidates, "GB18030");
            return candidates;
        }
        if (normalizedEncodingName == "big5")
        {
            AddUniqueEncodingCandidate(candidates, "BIG5");
            AddUniqueEncodingCandidate(candidates, "CP950");
            return candidates;
        }
        if (normalizedEncodingName == "big5hkscs" || normalizedEncodingName == "bigfivehkscs")
        {
            AddUniqueEncodingCandidate(candidates, "BIG5-HKSCS");
            AddUniqueEncodingCandidate(candidates, "BIG5HKSCS");
            AddUniqueEncodingCandidate(candidates, "BIG5");
            return candidates;
        }
        if (normalizedEncodingName == "shiftjis" || normalizedEncodingName == "sjis" ||
            normalizedEncodingName == "windows31j" || normalizedEncodingName == "mskanji")
        {
            AddUniqueEncodingCandidate(candidates, "SHIFT_JIS");
            AddUniqueEncodingCandidate(candidates, "CP932");
            AddUniqueEncodingCandidate(candidates, "WINDOWS-31J");
            return candidates;
        }
        if (normalizedEncodingName == "eucjp")
        {
            AddUniqueEncodingCandidate(candidates, "EUC-JP");
            AddUniqueEncodingCandidate(candidates, "EUCJP");
            AddUniqueEncodingCandidate(candidates, "CP51932");
            return candidates;
        }
        if (normalizedEncodingName == "euckr")
        {
            AddUniqueEncodingCandidate(candidates, "EUC-KR");
            AddUniqueEncodingCandidate(candidates, "CP949");
            return candidates;
        }
        if (normalizedEncodingName == "ksc5601" || normalizedEncodingName == "ksx1001" || normalizedEncodingName == "uhc")
        {
            AddUniqueEncodingCandidate(candidates, "CP949");
            AddUniqueEncodingCandidate(candidates, "UHC");
            AddUniqueEncodingCandidate(candidates, "EUC-KR");
            return candidates;
        }
        if (normalizedEncodingName == "koi8r")
        {
            AddUniqueEncodingCandidate(candidates, "KOI8-R");
            return candidates;
        }
        if (normalizedEncodingName == "koi8u")
        {
            AddUniqueEncodingCandidate(candidates, "KOI8-U");
            return candidates;
        }

        if (normalizedEncodingName.size() > 7 && normalizedEncodingName.compare(0, 7, "iso8859") == 0)
        {
            const string suffix = normalizedEncodingName.substr(7);
            unsigned int isoPart = 0;
            if (TryParseUnsignedInteger(suffix, isoPart))
            {
                AddUniqueEncodingCandidate(candidates, "ISO-8859-" + std::to_string(isoPart));
                return candidates;
            }
        }

        unsigned int codePage = 0;
        if (TryParseUnsignedInteger(normalizedEncodingName, codePage))
        {
            AddWindowsCodePageCandidates(candidates, codePage);
            return candidates;
        }
        if (normalizedEncodingName.size() > 2 && normalizedEncodingName.compare(0, 2, "cp") == 0 &&
            TryParseUnsignedInteger(normalizedEncodingName.substr(2), codePage))
        {
            AddWindowsCodePageCandidates(candidates, codePage);
            return candidates;
        }
        if (normalizedEncodingName.size() > 7 && normalizedEncodingName.compare(0, 7, "windows") == 0 &&
            TryParseUnsignedInteger(normalizedEncodingName.substr(7), codePage))
        {
            AddWindowsCodePageCandidates(candidates, codePage);
            return candidates;
        }
        if (normalizedEncodingName.size() > 2 && normalizedEncodingName.compare(0, 2, "ms") == 0 &&
            TryParseUnsignedInteger(normalizedEncodingName.substr(2), codePage))
        {
            AddWindowsCodePageCandidates(candidates, codePage);
            return candidates;
        }
        if (normalizedEncodingName.size() > 3 && normalizedEncodingName.compare(0, 3, "ibm") == 0 &&
            TryParseUnsignedInteger(normalizedEncodingName.substr(3), codePage))
        {
            AddUniqueEncodingCandidate(candidates, "IBM" + std::to_string(codePage));
            AddWindowsCodePageCandidates(candidates, codePage);
            return candidates;
        }

        return candidates;
    }


    static vector<string> ResolveCurrentLocaleEncodingCandidates()
    {
        EnsureLocaleInitialized();

        const char* codeset = nl_langinfo(CODESET);
        if (codeset == nullptr || *codeset == '\0')
        {
            throw runtime_error("Cannot determine current locale character encoding.");
        }

        vector<string> candidates;
        candidates.reserve(6);

        const string localeEncoding(codeset);
        AddUniqueEncodingCandidate(candidates, localeEncoding);
        AddUniqueEncodingCandidate(candidates, ToUpperAsciiString(localeEncoding));

        const string normalizedEncodingName = NormalizeEncodingName(localeEncoding);
        if (IsUtf8EncodingName(normalizedEncodingName))
        {
            AddUniqueEncodingCandidate(candidates, "UTF-8");
        }
        else if (normalizedEncodingName == "gbk" || normalizedEncodingName == "xgbk")
        {
            AddUniqueEncodingCandidate(candidates, "GBK");
            AddUniqueEncodingCandidate(candidates, "CP936");
        }
        else if (normalizedEncodingName == "gb18030")
        {
            AddUniqueEncodingCandidate(candidates, "GB18030");
        }
        else if (normalizedEncodingName == "gb2312" || normalizedEncodingName == "euccn")
        {
            AddUniqueEncodingCandidate(candidates, "GB2312");
            AddUniqueEncodingCandidate(candidates, "EUC-CN");
            AddUniqueEncodingCandidate(candidates, "CP936");
        }
        else if (normalizedEncodingName == "big5")
        {
            AddUniqueEncodingCandidate(candidates, "BIG5");
            AddUniqueEncodingCandidate(candidates, "CP950");
        }
        else if (normalizedEncodingName == "big5hkscs")
        {
            AddUniqueEncodingCandidate(candidates, "BIG5-HKSCS");
            AddUniqueEncodingCandidate(candidates, "BIG5");
        }
        else if (normalizedEncodingName == "shiftjis" || normalizedEncodingName == "sjis" ||
            normalizedEncodingName == "windows31j" || normalizedEncodingName == "mskanji")
        {
            AddUniqueEncodingCandidate(candidates, "SHIFT_JIS");
            AddUniqueEncodingCandidate(candidates, "WINDOWS-31J");
            AddUniqueEncodingCandidate(candidates, "CP932");
        }
        else if (normalizedEncodingName == "eucjp")
        {
            AddUniqueEncodingCandidate(candidates, "EUC-JP");
            AddUniqueEncodingCandidate(candidates, "CP51932");
        }
        else if (normalizedEncodingName == "euckr" || normalizedEncodingName == "ksc5601" ||
            normalizedEncodingName == "ksx1001" || normalizedEncodingName == "uhc")
        {
            AddUniqueEncodingCandidate(candidates, "EUC-KR");
            AddUniqueEncodingCandidate(candidates, "CP949");
            AddUniqueEncodingCandidate(candidates, "UHC");
        }

        return candidates;
    }

    class IconvHandle
    {
    public:
        explicit IconvHandle(iconv_t handleValue)
            : handle(handleValue)
        {
        }

        ~IconvHandle()
        {
            if (handle != reinterpret_cast<iconv_t>(-1))
            {
                iconv_close(handle);
            }
        }

        iconv_t Get() const
        {
            return handle;
        }

        IconvHandle(const IconvHandle&) = delete;
        IconvHandle& operator=(const IconvHandle&) = delete;

    private:
        iconv_t handle = reinterpret_cast<iconv_t>(-1);
    };

    static string ConvertBytesByIconv(const string& inputBytes, const string& fromEncoding, const string& toEncoding)
    {
        iconv_t iconvDescriptor = iconv_open(toEncoding.c_str(), fromEncoding.c_str());
        if (iconvDescriptor == reinterpret_cast<iconv_t>(-1))
        {
            throw runtime_error("iconv_open failed for conversion: " + fromEncoding + " -> " + toEncoding);
        }

        IconvHandle iconvHandle(iconvDescriptor);

        size_t outputCapacity = inputBytes.empty() ? static_cast<size_t>(32) : (inputBytes.size() * 4 + 32);
        string output(outputCapacity, '\0');

        char* inputPtr = const_cast<char*>(inputBytes.data());
        size_t inputBytesLeft = inputBytes.size();
        char* outputPtr = &output[0];
        size_t outputBytesLeft = output.size();

        while (true)
        {
            errno = 0;
            const size_t result = iconv(iconvHandle.Get(), &inputPtr, &inputBytesLeft, &outputPtr, &outputBytesLeft);
            if (result != static_cast<size_t>(-1))
            {
                break;
            }

            if (errno == E2BIG)
            {
                const size_t usedSize = output.size() - outputBytesLeft;
                output.resize(output.size() * 2 + 32);
                outputPtr = &output[0] + usedSize;
                outputBytesLeft = output.size() - usedSize;
                continue;
            }
            if (errno == EILSEQ)
            {
                throw runtime_error("Invalid byte sequence for conversion: " + fromEncoding + " -> " + toEncoding);
            }
            if (errno == EINVAL)
            {
                throw runtime_error("Incomplete multibyte sequence for conversion: " + fromEncoding + " -> " + toEncoding);
            }

            throw runtime_error("iconv conversion failed for conversion: " + fromEncoding + " -> " + toEncoding);
        }

        while (true)
        {
            errno = 0;
            const size_t result = iconv(iconvHandle.Get(), nullptr, nullptr, &outputPtr, &outputBytesLeft);
            if (result != static_cast<size_t>(-1))
            {
                break;
            }

            if (errno == E2BIG)
            {
                const size_t usedSize = output.size() - outputBytesLeft;
                output.resize(output.size() * 2 + 32);
                outputPtr = &output[0] + usedSize;
                outputBytesLeft = output.size() - usedSize;
                continue;
            }

            throw runtime_error("iconv flush failed for conversion: " + fromEncoding + " -> " + toEncoding);
        }

        output.resize(output.size() - outputBytesLeft);
        return output;
    }

    static string ConvertBytesToUtf8ByIconv(const string& rawBytes, const string& fromEncoding)
    {
        return ConvertBytesByIconv(rawBytes, fromEncoding, "UTF-8");
    }

    static string ConvertUtf8ToBytesByIconv(const string& utf8Bytes, const string& toEncoding)
    {
        return ConvertBytesByIconv(utf8Bytes, "UTF-8", toEncoding);
    }

    static string ConvertCurrentAnsiBytesToUtf8_Posix(const string& ansiBytes)
    {
        const vector<string> candidates = ResolveCurrentLocaleEncodingCandidates();
        for (size_t i = 0; i < candidates.size(); i++)
        {
            try
            {
                return ConvertBytesToUtf8ByIconv(ansiBytes, candidates[i]);
            }
            catch (const runtime_error& ex)
            {
                const string errorMessage = ex.what();
                if (errorMessage.find("iconv_open failed") != string::npos)
                {
                    continue;
                }
                throw;
            }
        }

        throw runtime_error("Current locale character encoding is not supported by iconv.");
    }

    static string ConvertUtf8ToCurrentAnsiBytes_Posix(const string& utf8Bytes)
    {
        const vector<string> candidates = ResolveCurrentLocaleEncodingCandidates();
        for (size_t i = 0; i < candidates.size(); i++)
        {
            try
            {
                return ConvertUtf8ToBytesByIconv(utf8Bytes, candidates[i]);
            }
            catch (const runtime_error& ex)
            {
                const string errorMessage = ex.what();
                if (errorMessage.find("iconv_open failed") != string::npos)
                {
                    continue;
                }
                throw;
            }
        }

        throw runtime_error("Current locale character encoding is not supported by iconv.");
    }

    static bool CanDecodeAsCurrentAnsi_Posix(const string& text)
    {
        const vector<string> candidates = ResolveCurrentLocaleEncodingCandidates();

        for (size_t i = 0; i < candidates.size(); i++)
        {
            try
            {
                ConvertBytesToUtf8ByIconv(text, candidates[i]);
                return true;
            }
            catch (const runtime_error& ex)
            {
                const string errorMessage = ex.what();
                if (errorMessage.find("iconv_open failed") != string::npos)
                {
                    continue;
                }
                if (errorMessage.find("Invalid byte sequence") != string::npos ||
                    errorMessage.find("Incomplete multibyte sequence") != string::npos)
                {
                    return false;
                }
            }
        }

        return false;
    }
#endif
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
    if (usedDefaultChar == TRUE)
    {
        throw runtime_error("WideCharToMultiByte(CP_ACP) would lose information for one or more characters.");
    }
    return ansi;

#else
    if (internal::IsCurrentAnsiUtf8())
    {
        if (!GB_IsUtf8(utf8Str))
        {
            throw runtime_error("Input bytes are not valid UTF-8.");
        }
        return utf8Str;
    }

    return internal::ConvertUtf8ToCurrentAnsiBytes_Posix(utf8Str);
#endif
}

string GB_AnsiToUtf8(const string& ansiStr)
{
    if (ansiStr.empty())
    {
        return {};
    }
#if defined(_WIN32)
    const wstring ws = internal::ConvertBytesToWideStringByCodePage(ansiStr, CP_ACP);
    return GB_WStringToUtf8(ws);
#else
    if (internal::IsCurrentAnsiUtf8())
    {
        if (!GB_IsUtf8(ansiStr))
        {
            throw runtime_error("Input bytes are not valid UTF-8.");
        }
        return ansiStr;
    }

    return internal::ConvertCurrentAnsiBytesToUtf8_Posix(ansiStr);
#endif
}


string GB_BytesToUtf8(const string& rawBytes, const string& encodingName)
{
    if (rawBytes.empty())
    {
        return {};
    }

    const string normalizedEncodingName = internal::NormalizeEncodingName(encodingName);
    if (normalizedEncodingName.empty())
    {
        throw runtime_error("Encoding name is empty.");
    }

    if (internal::IsUtf8EncodingName(normalizedEncodingName))
    {
        if (!GB_IsUtf8(rawBytes))
        {
            throw runtime_error("Input bytes are not valid UTF-8.");
        }
        return rawBytes;
    }

    if (internal::IsUtf8BomEncodingName(normalizedEncodingName))
    {
        if (!GB_IsUtf8(rawBytes))
        {
            throw runtime_error("Input bytes are not valid UTF-8.");
        }
        if (internal::HasUtf8Bom(rawBytes))
        {
            return rawBytes.substr(3);
        }
        return rawBytes;
    }

    if (internal::IsAnsiEncodingName(normalizedEncodingName))
    {
#if defined(_WIN32)
        const wstring wideString = internal::ConvertBytesToWideStringByCodePage(rawBytes, CP_ACP);
        return GB_WStringToUtf8(wideString);
#else
        if (internal::IsCurrentAnsiUtf8())
        {
            if (!GB_IsUtf8(rawBytes))
            {
                throw runtime_error("Input bytes are not valid UTF-8.");
            }
            return rawBytes;
        }

        return internal::ConvertCurrentAnsiBytesToUtf8_Posix(rawBytes);
#endif
    }

    if (internal::IsAsciiEncodingName(normalizedEncodingName))
    {
        if (!internal::IsAllAscii(rawBytes))
        {
            throw runtime_error("Input bytes are not valid ASCII.");
        }
        return rawBytes;
    }

    if (internal::IsLatin1EncodingName(normalizedEncodingName))
    {
        return internal::Latin1ToUtf8(rawBytes);
    }

#if defined(_WIN32)
    if (internal::IsOemEncodingName(normalizedEncodingName))
    {
        const std::wstring wideString = internal::ConvertBytesToWideStringByCodePage(rawBytes, CP_OEMCP);
        return GB_WStringToUtf8(wideString);
    }
#else
    if (internal::IsOemEncodingName(normalizedEncodingName))
    {
        throw runtime_error("OEM code page conversion is only supported on Windows.");
    }
#endif

    if (internal::IsUtf16EncodingName(normalizedEncodingName))
    {
        return internal::Utf16BytesToUtf8(rawBytes, true, true, encodingName);
    }

    if (internal::IsUtf16LeEncodingName(normalizedEncodingName))
    {
        return internal::Utf16BytesToUtf8(rawBytes, true, false, encodingName);
    }

    if (internal::IsUtf16BeEncodingName(normalizedEncodingName))
    {
        return internal::Utf16BytesToUtf8(rawBytes, false, false, encodingName);
    }

    if (internal::IsUtf32EncodingName(normalizedEncodingName))
    {
        return internal::Utf32BytesToUtf8(rawBytes, true, true, encodingName);
    }

    if (internal::IsUtf32LeEncodingName(normalizedEncodingName))
    {
        return internal::Utf32BytesToUtf8(rawBytes, true, false, encodingName);
    }

    if (internal::IsUtf32BeEncodingName(normalizedEncodingName))
    {
        return internal::Utf32BytesToUtf8(rawBytes, false, false, encodingName);
    }

#if defined(_WIN32)
    const UINT codePage = internal::ResolveEncodingNameToWindowsCodePage(encodingName);
    if (codePage != CP_UTF8 && !::IsValidCodePage(codePage))
    {
        throw runtime_error("Unsupported Windows code page for encoding: " + encodingName);
    }

    const wstring wideString = internal::ConvertBytesToWideStringByCodePage(rawBytes, codePage);
    return GB_WStringToUtf8(wideString);
#else
    const vector<string> candidates = internal::ResolveEncodingNameToPosixCandidates(encodingName);
    for (size_t i = 0; i < candidates.size(); i++)
    {
        try
        {
            return internal::ConvertBytesToUtf8ByIconv(rawBytes, candidates[i]);
        }
        catch (const runtime_error& ex)
        {
            const string errorMessage = ex.what();
            if (errorMessage.find("iconv_open failed") != string::npos)
            {
                continue;
            }
            throw;
        }
    }

    throw runtime_error("Unsupported encoding name on current POSIX iconv implementation: " + encodingName);
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

    int wideLength = ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(), internal::ToWinApiLengthChecked(text.size()), nullptr, 0);

    if (wideLength > 0)
    {
        return true;
    }

    const DWORD lastError = ::GetLastError();
    if (lastError == ERROR_INVALID_FLAGS)
    {
        // 个别环境下可能不支持 MB_ERR_INVALID_CHARS，退化为“能否转换”
        wideLength = ::MultiByteToWideChar(codePage, 0, text.data(), internal::ToWinApiLengthChecked(text.size()), nullptr, 0);
        return wideLength > 0;
    }

    return false;

#else
    return internal::CanDecodeAsCurrentAnsi_Posix(text);
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

vector<char32_t> GB_Utf8StringToChar32Vector(const string& utf8Str)
{
    const size_t size = GB_GetUtf8Length(utf8Str);
    vector<char32_t> result;
    result.reserve(size);

    size_t pos = 0;
    while (pos < utf8Str.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;
        bool ok = internal::DecodeOne(utf8Str, pos, cp, nextPos);
        if (ok)
        {
            result.push_back(cp);
        }
        pos = nextPos;
    }
    result.shrink_to_fit();
    return result;
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

vector<string> GB_Utf8Split(const string& textUtf8, char32_t delimiter, bool removeEmptySections)
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

    if (removeEmptySections)
    {
        parts.erase(std::remove_if(parts.begin(), parts.end(), [](const string& s) { return s.empty(); }), parts.end());
    }

    return parts;
}

bool GB_Utf8Equals(const std::string& text1Utf8, const std::string& text2Utf8, bool caseSensitive)
{
    if (text1Utf8.size() != text2Utf8.size())
    {
        return false;
    }

    if (caseSensitive)
    {
        return text1Utf8 == text2Utf8;
    }

    for (size_t i = 0; i < text1Utf8.size(); i++)
    {
        const unsigned char b1 = internal::NormalizeAsciiCaseByte(static_cast<unsigned char>(text1Utf8[i]), false);
        const unsigned char b2 = internal::NormalizeAsciiCaseByte(static_cast<unsigned char>(text2Utf8[i]), false);
        if (b1 != b2)
        {
            return false;
        }
    }
    return true;
}

int GB_Utf8CompareLogical(const std::string& text1Utf8, const std::string& text2Utf8)
{
    if (text1Utf8 == text2Utf8)
    {
        return 0;
    }

    if (internal::IsAllAscii(text1Utf8) && internal::IsAllAscii(text2Utf8))
    {
        return internal::CompareLogicalUtf8Fallback(text1Utf8, text2Utf8);
    }

#if defined(_WIN32)
    if (internal::ContainsNullCharacter(text1Utf8) || internal::ContainsNullCharacter(text2Utf8))
    {
        return internal::CompareLogicalUtf8Fallback(text1Utf8, text2Utf8);
    }

    if (!GB_IsUtf8(text1Utf8) || !GB_IsUtf8(text2Utf8))
    {
        return internal::CompareLogicalUtf8Fallback(text1Utf8, text2Utf8);
    }

    try
    {
        const std::wstring text1Wide = GB_Utf8ToWString(text1Utf8);
        const std::wstring text2Wide = GB_Utf8ToWString(text2Utf8);
        return internal::NormalizeCompareResult(::StrCmpLogicalW(text1Wide.c_str(), text2Wide.c_str()));
    }
    catch (const std::exception&)
    {
        return internal::CompareLogicalUtf8Fallback(text1Utf8, text2Utf8);
    }
#else
    return internal::CompareLogicalUtf8Fallback(text1Utf8, text2Utf8);
#endif
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

int64_t GB_Utf8Find(const string& text, const string& needle, bool caseSensitive, int64_t startPos)
{
    if (startPos < 0)
    {
        return -1;
    }

    size_t startBytePos = 0;
    if (!internal::FindUtf8ByteOffsetByCharIndex(text, startPos, startBytePos))
    {
        return -1;
    }

    if (needle.empty())
    {
        return startPos;
    }

    // —— ASCII + 大小写敏感：字节级快速路径（ASCII 下“字节偏移 == 码点偏移”）—— //
    if (caseSensitive && internal::IsAllAscii(text) && internal::IsAllAscii(needle))
    {
        const size_t matchPos = text.find(needle, startBytePos);
        if (matchPos == string::npos)
        {
            return -1;
        }
        return static_cast<int64_t>(matchPos);
    }

    // 1) 预解码模式串到码点数组（并可选 ASCII 折叠）
    vector<char32_t> pat;
    {
        size_t pos = 0;
        while (pos < needle.size())
        {
            char32_t codePoint = 0;
            size_t nextPos = pos;
            internal::DecodeOneOrReplacement(needle, pos, codePoint, nextPos);
            if (!caseSensitive)
            {
                codePoint = internal::ToLowerAscii(codePoint);
            }
            pat.push_back(codePoint);
            pos = nextPos;
        }
    }

    const size_t patLength = pat.size();
    if (patLength == 0)
    {
        return startPos;
    }

    // 2) 计算 KMP 的前缀函数（LPS）
    vector<size_t> lps(patLength, 0);
    {
        size_t matchedLength = 0;
        size_t i = 1;
        while (i < patLength)
        {
            if (pat[i] == pat[matchedLength])
            {
                matchedLength++;
                lps[i] = matchedLength;
                i++;
            }
            else if (matchedLength != 0)
            {
                matchedLength = lps[matchedLength - 1];
            }
            else
            {
                lps[i] = 0;
                i++;
            }
        }
    }

    // 3) 从 startPos 开始流式解码 text 并进行 KMP 匹配（无需整串展开为码点向量）
    size_t matchedLength = 0;     // 已匹配 pat[0..matchedLength-1]
    size_t textBytePos = startBytePos;
    int64_t textCharIndex = startPos;

    while (textBytePos < text.size())
    {
        char32_t codePoint = 0;
        size_t nextPos = textBytePos;
        internal::DecodeOneOrReplacement(text, textBytePos, codePoint, nextPos);
        if (!caseSensitive)
        {
            codePoint = internal::ToLowerAscii(codePoint);
        }

        while (matchedLength > 0 && codePoint != pat[matchedLength])
        {
            matchedLength = lps[matchedLength - 1];
        }
        if (codePoint == pat[matchedLength])
        {
            matchedLength++;
            if (matchedLength == patLength)
            {
                return textCharIndex - static_cast<int64_t>(patLength) + 1;
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


string GB_Utf8Simplified(const string& utf8Str)
{
    if (utf8Str.empty())
    {
        return {};
    }

    // ASCII 快速路径（无需 UTF-8 解码）
    if (internal::IsAllAscii(utf8Str))
    {
        string out;
        out.reserve(utf8Str.size());

        bool hasPendingSpace = false;
        for (size_t i = 0; i < utf8Str.size(); i++)
        {
            const unsigned char ch = static_cast<unsigned char>(utf8Str[i]);
            if (internal::IsQtSpaceByte(ch))
            {
                // 仅在已经输出过内容时，才可能需要在后续插入一个空格
                if (!out.empty())
                {
                    hasPendingSpace = true;
                }
                continue;
            }

            if (hasPendingSpace)
            {
                out.push_back(' ');
                hasPendingSpace = false;
            }

            out.push_back(static_cast<char>(ch));
        }

        return out;
    }

    // 通用路径：按码点扫描并折叠空白
    string out;
    out.reserve(utf8Str.size()); // 上界：折叠后只会更短

    bool hasPendingSpace = false;

    size_t pos = 0;
    while (pos < utf8Str.size())
    {
        char32_t cp = 0;
        size_t nextPos = pos;

        const bool ok = internal::DecodeOne(utf8Str, pos, cp, nextPos);
        if (!ok)
        {
            // 输入非合法 UTF-8：按字节原样透传（不参与空白折叠）
            if (hasPendingSpace)
            {
                out.push_back(' ');
                hasPendingSpace = false;
            }
            out.push_back(utf8Str[pos]);
            pos = pos + 1;
            continue;
        }

        if (internal::IsQtSpace(cp))
        {
            if (!out.empty())
            {
                hasPendingSpace = true;
            }
        }
        else
        {
            if (hasPendingSpace)
            {
                out.push_back(' ');
                hasPendingSpace = false;
            }
            out.append(utf8Str, pos, nextPos - pos);
        }

        pos = nextPos;
    }

    return out;
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

string GB_Utf8VFormat(const char* format, std::va_list args)
{
    if (!format)
    {
        throw runtime_error("GB_Utf8VFormat: format is null.");
    }

    // 常见场景下，格式化后的字符串往往很短。先用栈缓冲区尝试一次，避免每次都进行堆分配。
    std::array<char, 1024> smallBuffer{};

    std::va_list argsCopy;
    va_copy(argsCopy, args);
    const int smallWritten = std::vsnprintf(smallBuffer.data(), smallBuffer.size(), format, argsCopy);
    va_end(argsCopy);

    if (smallWritten >= 0 && static_cast<size_t>(smallWritten) < smallBuffer.size())
    {
        return string(smallBuffer.data(), static_cast<size_t>(smallWritten));
    }

    size_t required = 0;
    if (smallWritten >= 0)
    {
        required = static_cast<size_t>(smallWritten);
    }
#if defined(_WIN32)
    else
    {
        // 兼容极少数旧 CRT：vsnprintf 在截断时可能返回 -1，这里退回到 _vscprintf 计算所需长度。
        va_copy(argsCopy, args);
        const int requiredInt = _vscprintf(format, argsCopy);
        va_end(argsCopy);

        if (requiredInt < 0)
        {
            throw runtime_error("GB_Utf8VFormat: _vscprintf failed.");
        }
        required = static_cast<size_t>(requiredInt);
    }
#else
    else
    {
        throw runtime_error("GB_Utf8VFormat: vsnprintf failed.");
    }
#endif

    if (required > (std::numeric_limits<size_t>::max)() - 1)
    {
        throw runtime_error("GB_Utf8VFormat: formatted string too large.");
    }

    const size_t bufferSize = required + 1;
    vector<char> buffer(bufferSize);

    va_copy(argsCopy, args);
    const int written = std::vsnprintf(buffer.data(), buffer.size(), format, argsCopy);
    va_end(argsCopy);

    if (written < 0)
    {
        throw runtime_error("GB_Utf8VFormat: vsnprintf failed.");
    }
    if (static_cast<size_t>(written) >= buffer.size())
    {
        throw runtime_error("GB_Utf8VFormat: vsnprintf truncated unexpectedly.");
    }

    return string(buffer.data(), static_cast<size_t>(written));
}

string GB_Utf8Format(const char* format, ...)
{
    if (!format)
    {
        throw runtime_error("GB_Utf8Format: format is null.");
    }

    std::va_list args;
    va_start(args, format);

    struct VaListGuard
    {
        std::va_list& args;
        explicit VaListGuard(std::va_list& a)
            : args(a)
        {
        }
        ~VaListGuard()
        {
            va_end(args);
        }

        VaListGuard(const VaListGuard&) = delete;
        VaListGuard& operator=(const VaListGuard&) = delete;
    } guard(args);

    return GB_Utf8VFormat(format, args);
}
