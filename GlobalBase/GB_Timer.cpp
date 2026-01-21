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

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

GB_Timer::GB_Timer()
{
    Reset();
}

void GB_Timer::Reset()
{
    accumulated = std::chrono::nanoseconds(0);
    lastLapElapsed = std::chrono::nanoseconds(0);
    running = false;
    startTime = Clock::now();
}

void GB_Timer::Restart()
{
    accumulated = std::chrono::nanoseconds(0);
    lastLapElapsed = std::chrono::nanoseconds(0);
    running = true;
    startTime = Clock::now();
}

void GB_Timer::Start()
{
    if (running)
    {
        return;
    }

    startTime = Clock::now();
    running = true;
}

void GB_Timer::Stop()
{
    if (!running)
    {
        return;
    }

    accumulated += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - startTime);
    running = false;
}

bool GB_Timer::IsRunning() const
{
    return running;
}

std::chrono::nanoseconds GB_Timer::ElapsedNanosecondsDuration() const
{
    if (!running)
    {
        return accumulated;
    }

    const std::chrono::nanoseconds delta = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - startTime);

    return accumulated + delta;
}

int64_t GB_Timer::ElapsedNanoseconds() const
{
    return static_cast<int64_t>(ElapsedNanosecondsDuration().count());
}

int64_t GB_Timer::ElapsedMicroseconds() const
{
    const std::chrono::nanoseconds elapsed = ElapsedNanosecondsDuration();
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
}

int64_t GB_Timer::ElapsedMilliseconds() const
{
    const std::chrono::nanoseconds elapsed = ElapsedNanosecondsDuration();
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

double GB_Timer::ElapsedSeconds() const
{
    const std::chrono::nanoseconds elapsed = ElapsedNanosecondsDuration();
    const double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
    return seconds;
}

int64_t GB_Timer::LapNanoseconds()
{
    const auto currentElapsed = ElapsedNanosecondsDuration();
    const auto delta = currentElapsed - lastLapElapsed;
    lastLapElapsed = currentElapsed;
    return static_cast<int64_t>(delta.count());
}

int64_t GB_Timer::LapMicroseconds()
{
    const auto delta = std::chrono::nanoseconds(LapNanoseconds());
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(delta).count());
}

int64_t GB_Timer::LapMilliseconds()
{
    const auto delta = std::chrono::nanoseconds(LapNanoseconds());
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
}

std::string GB_Timer::FormatNanoseconds(int64_t nanoseconds)
{
    const double absNs = nanoseconds >= 0 ? static_cast<double>(nanoseconds) : -static_cast<double>(nanoseconds);

    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(3);

    if (absNs < 1000.0)
    {
        oss.precision(0);
        oss << nanoseconds << " ns";
        return oss.str();
    }

    const double us = static_cast<double>(nanoseconds) / 1000.0;
    if (absNs < 1000.0 * 1000.0)
    {
        oss << us << " us";
        return oss.str();
    }

    const double ms = static_cast<double>(nanoseconds) / 1000000.0;
    if (absNs < 1000.0 * 1000.0 * 1000.0)
    {
        oss << ms << " ms";
        return oss.str();
    }

    const double s = static_cast<double>(nanoseconds) / 1000000000.0;
    oss << s << " s";
    return oss.str();
}

GB_ScopeTimer::GB_ScopeTimer(const std::string& timerName, std::ostream* outputStream, Callback reportCallback)
    : name(timerName), out(outputStream), callback(std::move(reportCallback))
{
    timer.Restart();
}

GB_ScopeTimer::~GB_ScopeTimer() noexcept
{
    try
    {
        const auto elapsed = timer.ElapsedNanosecondsDuration();
        const double elapsedMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsed).count();

        if (callback)
        {
            callback(name, static_cast<int64_t>(elapsed.count()));
            return;
        }

        if (out != nullptr)
        {
            (*out) << "[GB_ScopeTimer] " << name << " took " << elapsedMs << " ms\n";
        }
    }
    catch (...)
    {
        // 避免析构期间 terminate
    }
}