#include "GB_Utility.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <clocale>
#include <climits>
#include <langinfo.h>
#include <cstring>
#endif


using namespace std;

void GetConsoleEncodingCode(unsigned int& code)
{
#if defined(_WIN32)
    UINT cp = ::GetConsoleOutputCP();
    if (cp == 0)
    {
        cp = ::GetACP(); // 兜底
    }
	code = cp;
#else
    const char* cur = setlocale(LC_CTYPE, nullptr);
    if (!cur || string(cur) == "C" || string(cur) == "POSIX")
    {
        setlocale(LC_CTYPE, ""); // 让它从环境继承
    }
    const char* cs = nl_langinfo(CODESET);
    if (cs && *cs)
    {
        if (strcmp(cs, "UTF-8") == 0)
        {
            code = 65001; // UTF-8
        }
        else if (strcmp(cs, "GB18030") == 0)
        {
            code = 54936; // GB18030
        }
        else if (strcmp(cs, "GBK") == 0)
        {
            code = 936; // GBK
        }
        else if (strcmp(cs, "Big5") == 0)
        {
            code = 950; // Big5
        }
        else if (strcmp(cs, "Shift_JIS") == 0)
        {
            code = 932; // Shift_JIS
        }
        else if (strcmp(cs, "CP949") == 0)
        {
            code = 949; // CP949
        }
        else if (strcmp(cs, "windows-1250") == 0)
        {
            code = 1250; // windows-1250
        }
        else if (strcmp(cs, "windows-1251") == 0)
        {
            code = 1251; // windows-1251
        }
        else if (strcmp(cs, "windows-1252") == 0)
        {
            code = 1252; // windows-1252
        }
        else if (strcmp(cs, "CP437") == 0)
        {
            code = 437; // CP437
        }
        else if (strcmp(cs, "CP850") == 0)
        {
            code = 850; // CP850
        }
        else
        {
            code = UINT_MAX; // 未知编码
        }
    }
    else
    {
        code = UINT_MAX; // 未知编码，兜底为 UTF-8
	}
#endif
}

void GetConsoleEncodingString(string& encodingString)
{
#if defined(_WIN32)
	UINT cp = ::GetConsoleOutputCP();
    if (cp == 0)
    {
        cp = ::GetACP(); // 兜底
    }
    switch (cp)
    {
        case 65001: encodingString = "UTF-8"; break;
        case 54936: encodingString = "GB18030"; break;
        case 936:   encodingString = "GBK"; break;
        case 950:   encodingString = "Big5"; break;
        case 932:   encodingString = "Shift_JIS"; break;
        case 949:   encodingString = "CP949"; break;
        case 1250:  encodingString = "windows-1250"; break;
        case 1251:  encodingString = "windows-1251"; break;
        case 1252:  encodingString = "windows-1252"; break;
        case 437:   encodingString = "CP437"; break;
        case 850:   encodingString = "CP850"; break;
        default:
        {
            char buf[32] = {};
            snprintf(buf, sizeof(buf), "CP%u", cp);
            encodingString = buf;
            break;
        }
    }
#else
    const char* cur = setlocale(LC_CTYPE, nullptr);
    if (!cur || string(cur) == "C" || string(cur) == "POSIX")
    {
        setlocale(LC_CTYPE, ""); // 让它从环境继承
    }

    const char* cs = nl_langinfo(CODESET);
    if (cs && *cs)
    {
        encodingString = string(cs);
        return;
    }
    // 兜底（极少见）
    encodingString = "UTF-8";
#endif
}

bool SetConsoleEncoding(unsigned int codePageId)
{
#if defined(_WIN32)
    if (!::SetConsoleOutputCP(codePageId)) // 同时设置输出与输入码页
    {
        return false;
    }
    if (!::SetConsoleCP(codePageId)) // 回滚输出码页（尽力而为）
    {
        ::SetConsoleOutputCP(::GetConsoleOutputCP());
        return false;
    }
    return true;
#else
    // POSIX: 只能设置进程locale，不保证改变终端的实际显示编码
    auto trySet = [](const char* loc) -> bool
        {
            if (!loc || !*loc)
            {
                return false;
            }
            const char* ret = setlocale(LC_CTYPE, loc);
            return ret != nullptr;
        };

    // 根据当前语言地域，优先尝试“保持语言地域 + 更换字符集”的形式
    auto tryWithCurrentTerritory = [&](const char* charset) -> bool
        {
            const char* cur = setlocale(LC_CTYPE, nullptr);
            if (!cur || strcmp(cur, "C") == 0 || strcmp(cur, "POSIX") == 0)
            {
                return false;
            }
            const char* dot = strchr(cur, '.');
            string base = dot ? string(cur, static_cast<size_t>(dot - cur)) : string(cur);
            if (base.empty())
            {
                return false;
            }
            string cand = base + "." + charset;
            return trySet(cand.c_str());
        };

    // 候选locale列表（不同发行版可用性不同）
    vector<const char*> candidates;

    switch (codePageId)
    {
    case 65001: // UTF-8
    {
        if (tryWithCurrentTerritory("UTF-8"))
        {
            // 再用 CODESET 校验
            const char* cs = nl_langinfo(CODESET);
            return (cs && strcmp(cs, "UTF-8") == 0);
        }
        candidates = {
            "C.UTF-8",        // Debian/Ubuntu等常见
            "en_US.UTF-8",
            "zh_CN.UTF-8",
            ".UTF-8",         // 一些libc接受仅指定字符集
            "UTF-8"           // 极少数实现
        };
        break;
    }
    case 54936: // GB18030
        if (tryWithCurrentTerritory("GB18030")) return true;
        candidates = { "zh_CN.GB18030", ".GB18030" };
        break;
    case 936:   // GBK
        if (tryWithCurrentTerritory("GBK")) return true;
        candidates = { "zh_CN.GBK", "zh_CN.GB2312", ".GBK" };
        break;
    case 950:   // Big5
        if (tryWithCurrentTerritory("BIG5")) return true;
        candidates = { "zh_TW.BIG5", "zh_HK.BIG5", ".BIG5" };
        break;
    case 932:   // Shift_JIS
        if (tryWithCurrentTerritory("SHIFT_JIS")) return true;
        candidates = { "ja_JP.SJIS", "ja_JP.Shift_JIS", ".SJIS", ".SHIFT_JIS" };
        break;
    case 949:   // CP949
        if (tryWithCurrentTerritory("CP949")) return true;
        candidates = { "ko_KR.CP949", "ko_KR.EUC-KR", ".CP949", ".EUC-KR" };
        break;
    case 1250:  // Windows-1250（可能无该locale）
        if (tryWithCurrentTerritory("CP1250")) return true;
        candidates = { "cs_CZ.CP1250", ".CP1250" };
        break;
    case 1251:  // Windows-1251
        if (tryWithCurrentTerritory("CP1251")) return true;
        candidates = { "ru_RU.CP1251", ".CP1251" };
        break;
    case 1252:  // Windows-1252
        if (tryWithCurrentTerritory("CP1252")) return true;
        candidates = { "en_US.CP1252", ".CP1252" };
        break;
    case 437:   // CP437 → 退回 C/POSIX（近似 US-ASCII）
        candidates = { "C", "POSIX" };
        break;
    case 850:   // CP850 → 近似 ISO-8859-1
        candidates = { "en_US.ISO-8859-1", "de_DE.ISO-8859-1", ".ISO-8859-1" };
        break;
    default:
        return false; // 未知/不支持
    }

    for (const char* cand : candidates)
    {
        if (trySet(cand))
        {
            return true;
        }
    }
    return false;
#endif
}

bool SetConsoleEncodingToUtf8()
{
    return SetConsoleEncoding(65001);
}

