#include "GB_Timer.h"
#include <ctime>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <locale>

using namespace std;

string GetLocalTimeStr(bool withMs, bool withTzSuffix)
{
    // 1) 拿到当前时刻
    const chrono::system_clock::time_point now = chrono::system_clock::now();
    const time_t tt = chrono::system_clock::to_time_t(now);

    // 2) 拆成年月日时分秒（本地 & UTC）——用线程安全版本
    tm localTm;
    tm utcTm;
#if defined(_WIN32)
    localtime_s(&localTm, &tt);
    gmtime_s(&utcTm, &tt);
#else
    localtime_r(&tt, &localTm);
    gmtime_r(&tt, &utcTm);
#endif

    // 3) 毫秒部分（来自 epoch 毫秒取模 1000，与时区无关）
    long msPart = 0;
    if (withMs)
    {
        const auto msSinceEpoch = chrono::duration_cast<chrono::milliseconds>(now.time_since_epoch());
        msPart = static_cast<long>(msSinceEpoch.count() % 1000);
    }

    // 4) 计算本地相对 UTC 的秒级偏移（含夏令时）
    //    技巧：分别对 localTm 与 utcTm 调用 mktime（都按“本地”解释），其差值即偏移秒数
    tm localCopy = localTm;
    tm utcCopy = utcTm;
    localCopy.tm_isdst = -1; // 让库自己判定 DST
    utcCopy.tm_isdst = -1;
    const time_t localEpoch = mktime(&localCopy);
    const time_t utcAsLocal = mktime(&utcCopy);
    long offsetSeconds = static_cast<long>(difftime(localEpoch, utcAsLocal));

    // 5) 按 RFC 3339 格式化
    ostringstream oss;
    oss.imbue(locale::classic()); // 保证数字/分隔符为 ASCII（UTF-8 子集）

    oss << put_time(&localTm, "%Y-%m-%dT%H:%M:%S");
    if (withMs)
    {
        oss << '.' << setw(3) << setfill('0') << msPart;
    }

    if (withTzSuffix)
    {
        if (offsetSeconds == 0)
        {
            oss << 'Z';
        }
        else
        {
            const char sign = offsetSeconds >= 0 ? '+' : '-';
            long absOffset = offsetSeconds >= 0 ? offsetSeconds : -offsetSeconds;
            const int hh = static_cast<int>(absOffset / 3600);
            const int mm = static_cast<int>((absOffset % 3600) / 60);
            oss << sign
                << std::setw(2) << std::setfill('0') << hh
                << ':'
                << std::setw(2) << std::setfill('0') << mm;
        }
    }
    return oss.str();
}


