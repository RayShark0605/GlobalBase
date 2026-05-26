#include "GB_Timer.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <limits>

namespace
{
    using PeriodicClock = std::chrono::steady_clock;
    using PeriodicTimePoint = PeriodicClock::time_point;
    using PeriodicNanoseconds = std::chrono::nanoseconds;

    bool GetLocalTm(const std::time_t& timeValue, std::tm& localTime)
    {
#if defined(_WIN32)
        return localtime_s(&localTime, &timeValue) == 0;
#else
        return localtime_r(&timeValue, &localTime) != nullptr;
#endif
    }

    bool GetUtcTm(const std::time_t& timeValue, std::tm& utcTime)
    {
#if defined(_WIN32)
        return gmtime_s(&utcTime, &timeValue) == 0;
#else
        return gmtime_r(&timeValue, &utcTime) != nullptr;
#endif
    }

    long GetCurrentUtcOffsetSeconds(const std::time_t& timeValue, const std::tm& localTime, const std::tm& utcTime)
    {
#if defined(__USE_BSD) || defined(__USE_MISC) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        (void)timeValue;
        (void)utcTime;
        return static_cast<long>(localTime.tm_gmtoff);
#elif defined(_WIN32)
        (void)utcTime;
        std::tm localCopy = localTime;
        const __time64_t localAsUtcEpoch = _mkgmtime64(&localCopy);
        if (localAsUtcEpoch == static_cast<__time64_t>(-1))
        {
            return 0;
        }

        return static_cast<long>(std::difftime(static_cast<std::time_t>(localAsUtcEpoch), timeValue));
#else
        std::tm localCopy = localTime;
        std::tm utcCopy = utcTime;
        localCopy.tm_isdst = -1;
        utcCopy.tm_isdst = -1;

        const std::time_t localEpoch = std::mktime(&localCopy);
        const std::time_t utcAsLocalEpoch = std::mktime(&utcCopy);
        if (localEpoch == static_cast<std::time_t>(-1) || utcAsLocalEpoch == static_cast<std::time_t>(-1))
        {
            return 0;
        }

        return static_cast<long>(std::difftime(localEpoch, utcAsLocalEpoch));
#endif
    }

    uint64_t AddUnsignedSaturated(uint64_t left, uint64_t right)
    {
        const uint64_t maxValue = (std::numeric_limits<uint64_t>::max)();
        if (maxValue - left < right)
        {
            return maxValue;
        }

        return left + right;
    }

    PeriodicTimePoint AddPositiveNanosecondsSaturated(const PeriodicTimePoint& timePoint, const PeriodicNanoseconds& duration)
    {
        if (duration.count() <= 0)
        {
            return timePoint;
        }

        const PeriodicNanoseconds remaining = std::chrono::duration_cast<PeriodicNanoseconds>(PeriodicTimePoint::max() - timePoint);
        if (duration > remaining)
        {
            return PeriodicTimePoint::max();
        }

        return timePoint + duration;
    }

    PeriodicTimePoint GetScheduledTime(const PeriodicTimePoint& firstScheduledTime, const PeriodicNanoseconds& period, uint64_t triggerIndex)
    {
        if (triggerIndex <= 1)
        {
            return firstScheduledTime;
        }

        const uint64_t offsetCount = triggerIndex - 1;
        const int64_t periodCount = period.count();
        if (periodCount <= 0)
        {
            return firstScheduledTime;
        }

        const uint64_t maxInt64 = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
        if (offsetCount > maxInt64 / static_cast<uint64_t>(periodCount))
        {
            return PeriodicTimePoint::max();
        }

        return AddPositiveNanosecondsSaturated(firstScheduledTime, PeriodicNanoseconds(periodCount * static_cast<int64_t>(offsetCount)));
    }

    uint64_t GetFirstScheduleIndexAfter(const PeriodicTimePoint& firstScheduledTime, const PeriodicNanoseconds& period, const PeriodicTimePoint& nowTime, uint64_t minTriggerIndex)
    {
        const PeriodicTimePoint minScheduledTime = GetScheduledTime(firstScheduledTime, period, minTriggerIndex);
        if (minScheduledTime > nowTime)
        {
            return minTriggerIndex;
        }

        const int64_t periodCount = period.count();
        if (periodCount <= 0)
        {
            return minTriggerIndex;
        }

        const PeriodicNanoseconds elapsed = std::chrono::duration_cast<PeriodicNanoseconds>(nowTime - firstScheduledTime);
        if (elapsed.count() < 0)
        {
            return minTriggerIndex;
        }

        const uint64_t elapsedPeriodCount = static_cast<uint64_t>(elapsed.count() / periodCount);
        const uint64_t alignedTriggerIndex = AddUnsignedSaturated(elapsedPeriodCount, 2);
        return std::max(alignedTriggerIndex, minTriggerIndex);
    }

    void JoinThreadIfNeeded(std::thread& workerThread)
    {
        if (!workerThread.joinable())
        {
            return;
        }

        if (workerThread.get_id() == std::this_thread::get_id())
        {
            workerThread.detach();
            return;
        }

        workerThread.join();
    }

    int64_t GetNonNegativeDurationCount(const PeriodicTimePoint& beginTime, const PeriodicTimePoint& endTime)
    {
        if (endTime <= beginTime)
        {
            return 0;
        }

        return std::chrono::duration_cast<PeriodicNanoseconds>(endTime - beginTime).count();
    }

    uint64_t GetAbsoluteInt64Value(int64_t value)
    {
        if (value >= 0)
        {
            return static_cast<uint64_t>(value);
        }

        return static_cast<uint64_t>(-(value + 1)) + 1;
    }
}

namespace GB_TimerDetail
{
    struct PeriodicTriggerState
    {
        GB_PeriodicTrigger::Options options;
        GB_PeriodicTrigger::Callback callback;
        GB_PeriodicTrigger::ExceptionHandler exceptionHandler;

        std::mutex stateMutex;
        std::condition_variable stateCond;
        std::atomic_bool isRunning{ false };
        std::thread::id timerThreadId;
        std::thread::id callbackThreadId;

        std::mutex callbackQueueMutex;
        std::condition_variable callbackQueueCond;
        std::deque<GB_PeriodicTrigger::Event> callbackQueue;
        bool callbackThreadStopping = false;
        bool callbackDrainPending = false;

        std::atomic<uint64_t> dispatchedCallbackCount{ 0 };
        std::atomic<uint64_t> skippedTriggerCount{ 0 };
        std::atomic<uint64_t> droppedAsyncCallbackCount{ 0 };
    };
}

std::string GetLocalTimeStr(bool withMs, bool withTzSuffix)
{
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);

    std::tm localTime = {};
    std::tm utcTime = {};
    if (!GetLocalTm(timeValue, localTime) || !GetUtcTm(timeValue, utcTime))
    {
        return std::string();
    }

    int millisecondPart = 0;
    if (withMs)
    {
        const std::chrono::milliseconds millisecondsSinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
        int64_t millisecondCount = static_cast<int64_t>(millisecondsSinceEpoch.count() % 1000);
        if (millisecondCount < 0)
        {
            millisecondCount += 1000;
        }
        millisecondPart = static_cast<int>(millisecondCount);
    }

    const long offsetSeconds = withTzSuffix ? GetCurrentUtcOffsetSeconds(timeValue, localTime, utcTime) : 0;

    char buffer[64] = { 0 };
    int length = 0;
    if (withMs)
    {
        length = std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03d", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday, localTime.tm_hour, localTime.tm_min, localTime.tm_sec, millisecondPart);
    }
    else
    {
        length = std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday, localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
    }

    if (length <= 0 || static_cast<size_t>(length) >= sizeof(buffer))
    {
        return std::string();
    }

    std::string result(buffer, static_cast<size_t>(length));
    if (!withTzSuffix)
    {
        return result;
    }

    if (offsetSeconds == 0)
    {
        result.push_back('Z');
        return result;
    }

    const char sign = offsetSeconds >= 0 ? '+' : '-';
    const long absOffsetSeconds = offsetSeconds >= 0 ? offsetSeconds : static_cast<long>(-static_cast<long long>(offsetSeconds));
    const int hourOffset = static_cast<int>(absOffsetSeconds / 3600);
    const int minuteOffset = static_cast<int>((absOffsetSeconds % 3600) / 60);

    char timeZoneBuffer[8] = { 0 };
    const int timeZoneLength = std::snprintf(timeZoneBuffer, sizeof(timeZoneBuffer), "%c%02d:%02d", sign, hourOffset, minuteOffset);
    if (timeZoneLength <= 0 || static_cast<size_t>(timeZoneLength) >= sizeof(timeZoneBuffer))
    {
        return std::string();
    }

    result.append(timeZoneBuffer, static_cast<size_t>(timeZoneLength));
    return result;
}

using Clock = std::chrono::steady_clock;

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
    startTime = Clock::now();
    running = true;
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

    const std::chrono::nanoseconds delta = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - startTime);
    accumulated += delta;
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
    return std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
}

int64_t GB_Timer::LapNanoseconds()
{
    const std::chrono::nanoseconds currentElapsed = ElapsedNanosecondsDuration();
    const std::chrono::nanoseconds delta = currentElapsed - lastLapElapsed;
    lastLapElapsed = currentElapsed;
    return static_cast<int64_t>(delta.count());
}

int64_t GB_Timer::LapMicroseconds()
{
    const std::chrono::nanoseconds delta = std::chrono::nanoseconds(LapNanoseconds());
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(delta).count());
}

int64_t GB_Timer::LapMilliseconds()
{
    const std::chrono::nanoseconds delta = std::chrono::nanoseconds(LapNanoseconds());
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
}

std::string GB_Timer::FormatNanoseconds(int64_t nanoseconds)
{
    const uint64_t absNanoseconds = GetAbsoluteInt64Value(nanoseconds);

    char buffer[64] = { 0 };
    int length = 0;

    if (absNanoseconds < 1000ULL)
    {
        length = std::snprintf(buffer, sizeof(buffer), "%lld ns", static_cast<long long>(nanoseconds));
    }
    else if (absNanoseconds < 1000000ULL)
    {
        length = std::snprintf(buffer, sizeof(buffer), "%.3f us", static_cast<double>(nanoseconds) / 1000.0);
    }
    else if (absNanoseconds < 1000000000ULL)
    {
        length = std::snprintf(buffer, sizeof(buffer), "%.3f ms", static_cast<double>(nanoseconds) / 1000000.0);
    }
    else
    {
        length = std::snprintf(buffer, sizeof(buffer), "%.3f s", static_cast<double>(nanoseconds) / 1000000000.0);
    }

    if (length <= 0 || static_cast<size_t>(length) >= sizeof(buffer))
    {
        return std::string();
    }

    return std::string(buffer, static_cast<size_t>(length));
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
        const std::chrono::nanoseconds elapsed = timer.ElapsedNanosecondsDuration();

        if (callback)
        {
            callback(name, static_cast<int64_t>(elapsed.count()));
            return;
        }

        if (out != nullptr)
        {
            (*out) << "[GB_ScopeTimer] " << name << " took " << GB_Timer::FormatNanoseconds(static_cast<int64_t>(elapsed.count())) << '\n';
        }
    }
    catch (...)
    {
        // 避免析构期间异常逃逸。
    }
}

GB_PeriodicTrigger::GB_PeriodicTrigger()
{
}

GB_PeriodicTrigger::~GB_PeriodicTrigger() noexcept
{
    try
    {
        Stop(false);
    }
    catch (...)
    {
        // 析构函数不能抛异常。
    }
}

bool GB_PeriodicTrigger::Start(int64_t periodMilliseconds, Callback triggerCallback, CallbackMode callbackMode, SchedulePolicy schedulePolicy)
{
    if (periodMilliseconds <= 0)
    {
        return false;
    }

    const int64_t maxMilliseconds = (std::numeric_limits<int64_t>::max)() / 1000000;
    if (periodMilliseconds > maxMilliseconds)
    {
        return false;
    }

    return Start(std::chrono::nanoseconds(periodMilliseconds * 1000000), std::move(triggerCallback), callbackMode, schedulePolicy);
}

bool GB_PeriodicTrigger::Start(std::chrono::nanoseconds period, Callback triggerCallback, CallbackMode callbackMode, SchedulePolicy schedulePolicy)
{
    Options triggerOptions;
    triggerOptions.period = period;
    triggerOptions.callbackMode = callbackMode;
    triggerOptions.schedulePolicy = schedulePolicy;
    return Start(triggerOptions, std::move(triggerCallback));
}

bool GB_PeriodicTrigger::Start(const Options& triggerOptions, Callback triggerCallback)
{
    if (!IsValidOptions(triggerOptions) || !triggerCallback)
    {
        return false;
    }

    std::lock_guard<std::mutex> startLock(startMutex);
    std::lock_guard<std::mutex> operationLock(operationMutex);
    StopInternal(false);

    std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> newState;
    try
    {
        newState = std::make_shared<GB_TimerDetail::PeriodicTriggerState>();
        newState->options = triggerOptions;
        newState->callback = std::move(triggerCallback);
        newState->isRunning.store(true, std::memory_order_release);

        std::lock_guard<std::mutex> lock(lifecycleMutex);
        newState->exceptionHandler = exceptionHandler;
        state = newState;

        if (triggerOptions.callbackMode == CallbackMode::Asynchronous)
        {
            callbackThread = std::thread(&GB_PeriodicTrigger::AsyncCallbackThreadFunc, newState);
        }

        timerThread = std::thread(&GB_PeriodicTrigger::TimerThreadFunc, newState);
    }
    catch (...)
    {
        if (newState)
        {
            RequestStop(newState, false);
        }

        std::thread localTimerThread;
        std::thread localCallbackThread;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            if (state == newState)
            {
                state.reset();
            }
            if (timerThread.joinable())
            {
                localTimerThread = std::move(timerThread);
            }
            if (callbackThread.joinable())
            {
                localCallbackThread = std::move(callbackThread);
            }
        }

        JoinThreadIfNeeded(localTimerThread);
        JoinThreadIfNeeded(localCallbackThread);
        return false;
    }

    return true;
}

void GB_PeriodicTrigger::Stop(bool drainPendingCallbacks)
{
    std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> localState;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        localState = state;
    }

    if (IsCurrentWorkerThread(localState))
    {
        StopInternal(drainPendingCallbacks);
        return;
    }

    std::lock_guard<std::mutex> operationLock(operationMutex);
    StopInternal(drainPendingCallbacks);
}

void GB_PeriodicTrigger::StopInternal(bool drainPendingCallbacks)
{
    std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> localState;
    std::thread localTimerThread;
    std::thread localCallbackThread;

    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        localState = state;
        if (localState)
        {
            RequestStop(localState, drainPendingCallbacks);
        }

        if (timerThread.joinable())
        {
            localTimerThread = std::move(timerThread);
        }

        if (callbackThread.joinable())
        {
            localCallbackThread = std::move(callbackThread);
        }
    }

    JoinThreadIfNeeded(localTimerThread);
    JoinThreadIfNeeded(localCallbackThread);
}

bool GB_PeriodicTrigger::IsRunning() const
{
    std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> localState;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        localState = state;
    }

    return localState && localState->isRunning.load(std::memory_order_acquire);
}

void GB_PeriodicTrigger::SetExceptionHandler(ExceptionHandler handler)
{
    std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> localState;
    ExceptionHandler localExceptionHandler;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        exceptionHandler = std::move(handler);
        localExceptionHandler = exceptionHandler;
        localState = state;
    }

    if (localState)
    {
        std::lock_guard<std::mutex> lock(localState->stateMutex);
        localState->exceptionHandler = localExceptionHandler;
    }
}

uint64_t GB_PeriodicTrigger::GetDispatchedCallbackCount() const
{
    std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> localState;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        localState = state;
    }

    return localState ? localState->dispatchedCallbackCount.load(std::memory_order_relaxed) : 0;
}

uint64_t GB_PeriodicTrigger::GetSkippedTriggerCount() const
{
    std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> localState;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        localState = state;
    }

    return localState ? localState->skippedTriggerCount.load(std::memory_order_relaxed) : 0;
}

uint64_t GB_PeriodicTrigger::GetDroppedAsyncCallbackCount() const
{
    std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> localState;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        localState = state;
    }

    return localState ? localState->droppedAsyncCallbackCount.load(std::memory_order_relaxed) : 0;
}

bool GB_PeriodicTrigger::IsValidOptions(const Options& triggerOptions)
{
    if (triggerOptions.period.count() <= 0)
    {
        return false;
    }

    if (triggerOptions.callbackMode != CallbackMode::Synchronous && triggerOptions.callbackMode != CallbackMode::Asynchronous)
    {
        return false;
    }

    if (triggerOptions.schedulePolicy != SchedulePolicy::FixedDelay && triggerOptions.schedulePolicy != SchedulePolicy::FixedRateSkip && triggerOptions.schedulePolicy != SchedulePolicy::FixedRateCatchUp)
    {
        return false;
    }

    if (triggerOptions.asyncOverflowPolicy != AsyncOverflowPolicy::DropNewest && triggerOptions.asyncOverflowPolicy != AsyncOverflowPolicy::DropOldest && triggerOptions.asyncOverflowPolicy != AsyncOverflowPolicy::BlockTimer)
    {
        return false;
    }

    return true;
}

void GB_PeriodicTrigger::TimerThreadFunc(std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> triggerState)
{
    {
        std::lock_guard<std::mutex> lock(triggerState->stateMutex);
        triggerState->timerThreadId = std::this_thread::get_id();
    }

    try
    {
        const Options triggerOptions = triggerState->options;
        const Nanoseconds period = triggerOptions.period;
        const TimePoint startTime = Clock::now();
        const TimePoint firstScheduledTime = triggerOptions.triggerImmediately ? startTime : AddPositiveNanosecondsSaturated(startTime, period);

        uint64_t triggerIndex = 1;
        TimePoint nextScheduledTime = firstScheduledTime;
        uint64_t skippedCountForNextCallback = 0;

        while (triggerState->isRunning.load(std::memory_order_acquire))
        {
            if (!WaitUntilOrStop(triggerState, nextScheduledTime))
            {
                break;
            }

            const TimePoint actualTriggerTime = Clock::now();

            Event triggerEvent;
            triggerEvent.triggerIndex = triggerIndex;
            triggerEvent.skippedTriggerCount = skippedCountForNextCallback;
            triggerEvent.period = period;
            triggerEvent.scheduledTime = nextScheduledTime;
            triggerEvent.actualTriggerTime = actualTriggerTime;
            triggerEvent.triggerDelay = PeriodicNanoseconds(GetNonNegativeDurationCount(nextScheduledTime, actualTriggerTime));
            triggerEvent.totalSkippedTriggerCount = triggerState->skippedTriggerCount.load(std::memory_order_relaxed);
            triggerEvent.totalDroppedAsyncCallbackCount = triggerState->droppedAsyncCallbackCount.load(std::memory_order_relaxed);

            DispatchCallback(triggerState, triggerOptions, triggerEvent);
            if (!triggerState->isRunning.load(std::memory_order_acquire))
            {
                break;
            }

            skippedCountForNextCallback = 0;
            const TimePoint afterDispatchTime = Clock::now();

            if (triggerOptions.schedulePolicy == SchedulePolicy::FixedDelay)
            {
                triggerIndex = AddUnsignedSaturated(triggerIndex, 1);
                nextScheduledTime = AddPositiveNanosecondsSaturated(afterDispatchTime, period);
                continue;
            }

            if (triggerOptions.schedulePolicy == SchedulePolicy::FixedRateCatchUp)
            {
                triggerIndex = AddUnsignedSaturated(triggerIndex, 1);
                nextScheduledTime = GetScheduledTime(firstScheduledTime, period, triggerIndex);
                continue;
            }

            const uint64_t candidateTriggerIndex = AddUnsignedSaturated(triggerIndex, 1);
            const uint64_t alignedTriggerIndex = GetFirstScheduleIndexAfter(firstScheduledTime, period, afterDispatchTime, candidateTriggerIndex);
            if (alignedTriggerIndex > candidateTriggerIndex)
            {
                skippedCountForNextCallback = alignedTriggerIndex - candidateTriggerIndex;
                triggerState->skippedTriggerCount.fetch_add(skippedCountForNextCallback, std::memory_order_relaxed);
            }

            triggerIndex = alignedTriggerIndex;
            nextScheduledTime = GetScheduledTime(firstScheduledTime, period, triggerIndex);
        }
    }
    catch (...)
    {
        HandleCallbackException(triggerState, std::current_exception());
    }

    triggerState->isRunning.store(false, std::memory_order_release);
    triggerState->stateCond.notify_all();

    {
        std::lock_guard<std::mutex> lock(triggerState->callbackQueueMutex);
        if (!triggerState->callbackThreadStopping)
        {
            triggerState->callbackThreadStopping = true;
            triggerState->callbackDrainPending = false;
            triggerState->callbackQueue.clear();
        }
    }
    triggerState->callbackQueueCond.notify_all();
}

void GB_PeriodicTrigger::AsyncCallbackThreadFunc(std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> triggerState)
{
    {
        std::lock_guard<std::mutex> lock(triggerState->stateMutex);
        triggerState->callbackThreadId = std::this_thread::get_id();
    }

    try
    {
        while (true)
        {
            Event triggerEvent;
            size_t pendingCallbackCount = 0;
            {
                std::unique_lock<std::mutex> lock(triggerState->callbackQueueMutex);
                triggerState->callbackQueueCond.wait(lock, [triggerState]()
                    {
                        return triggerState->callbackThreadStopping || !triggerState->callbackQueue.empty();
                    });

                if (triggerState->callbackQueue.empty())
                {
                    break;
                }

                if (triggerState->callbackThreadStopping && !triggerState->callbackDrainPending)
                {
                    triggerState->callbackQueue.clear();
                    triggerState->callbackQueueCond.notify_all();
                    break;
                }

                triggerEvent = triggerState->callbackQueue.front();
                triggerState->callbackQueue.pop_front();
                pendingCallbackCount = triggerState->callbackQueue.size();
                triggerState->callbackQueueCond.notify_all();
            }

            InvokeCallbackNoThrow(triggerState, triggerEvent, pendingCallbackCount);
        }
    }
    catch (...)
    {
        HandleCallbackException(triggerState, std::current_exception());
        RequestStop(triggerState, false);
    }
}

bool GB_PeriodicTrigger::WaitUntilOrStop(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, const TimePoint& deadline)
{
    std::unique_lock<std::mutex> lock(triggerState->stateMutex);
    if (!triggerState->isRunning.load(std::memory_order_acquire))
    {
        return false;
    }

    return !triggerState->stateCond.wait_until(lock, deadline, [triggerState]()
        {
            return !triggerState->isRunning.load(std::memory_order_acquire);
        });
}

bool GB_PeriodicTrigger::DispatchCallback(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, const Options& triggerOptions, Event& triggerEvent)
{
    if (triggerOptions.callbackMode == CallbackMode::Asynchronous)
    {
        return EnqueueAsyncCallback(triggerState, triggerOptions, triggerEvent);
    }

    InvokeCallbackNoThrow(triggerState, triggerEvent, 0);
    return true;
}

bool GB_PeriodicTrigger::EnqueueAsyncCallback(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, const Options& triggerOptions, Event& triggerEvent)
{
    std::exception_ptr exceptionPtr;
    bool shouldNotify = false;

    {
        std::unique_lock<std::mutex> lock(triggerState->callbackQueueMutex);

        if (triggerState->callbackThreadStopping)
        {
            return false;
        }

        const size_t maxPendingCallbacks = triggerOptions.maxPendingCallbacks;
        if (maxPendingCallbacks > 0 && triggerState->callbackQueue.size() >= maxPendingCallbacks)
        {
            if (triggerOptions.asyncOverflowPolicy == AsyncOverflowPolicy::DropNewest)
            {
                triggerState->droppedAsyncCallbackCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            if (triggerOptions.asyncOverflowPolicy == AsyncOverflowPolicy::DropOldest)
            {
                triggerState->callbackQueue.pop_front();
                triggerState->droppedAsyncCallbackCount.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                triggerState->callbackQueueCond.wait(lock, [triggerState, maxPendingCallbacks]()
                    {
                        return triggerState->callbackThreadStopping || triggerState->callbackQueue.size() < maxPendingCallbacks;
                    });

                if (triggerState->callbackThreadStopping)
                {
                    return false;
                }
            }
        }

        triggerEvent.totalDroppedAsyncCallbackCount = triggerState->droppedAsyncCallbackCount.load(std::memory_order_relaxed);
        try
        {
            triggerState->callbackQueue.push_back(triggerEvent);
            shouldNotify = true;
        }
        catch (...)
        {
            triggerState->droppedAsyncCallbackCount.fetch_add(1, std::memory_order_relaxed);
            exceptionPtr = std::current_exception();
        }
    }

    if (exceptionPtr)
    {
        HandleCallbackException(triggerState, exceptionPtr);
        return false;
    }

    if (shouldNotify)
    {
        triggerState->callbackQueueCond.notify_one();
    }
    return true;
}

void GB_PeriodicTrigger::InvokeCallbackNoThrow(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, Event triggerEvent, size_t pendingCallbackCount)
{
    if (!triggerState->callback)
    {
        return;
    }

    triggerEvent.callbackIndex = triggerState->dispatchedCallbackCount.fetch_add(1, std::memory_order_relaxed) + 1;
    triggerEvent.pendingCallbackCount = pendingCallbackCount;
    triggerEvent.callbackStartTime = Clock::now();
    triggerEvent.callbackStartDelay = PeriodicNanoseconds(GetNonNegativeDurationCount(triggerEvent.scheduledTime, triggerEvent.callbackStartTime));
    triggerEvent.totalDroppedAsyncCallbackCount = triggerState->droppedAsyncCallbackCount.load(std::memory_order_relaxed);

    try
    {
        triggerState->callback(triggerEvent);
    }
    catch (...)
    {
        HandleCallbackException(triggerState, std::current_exception());
    }
}

void GB_PeriodicTrigger::HandleCallbackException(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, std::exception_ptr exceptionPtr)
{
    ExceptionHandler localExceptionHandler;
    try
    {
        std::lock_guard<std::mutex> lock(triggerState->stateMutex);
        localExceptionHandler = triggerState->exceptionHandler;
    }
    catch (...)
    {
        return;
    }

    if (!localExceptionHandler)
    {
        return;
    }

    try
    {
        localExceptionHandler(exceptionPtr);
    }
    catch (...)
    {
        // 避免异常处理器本身导致工作线程退出。
    }
}

bool GB_PeriodicTrigger::IsCurrentWorkerThread(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState)
{
    if (!triggerState)
    {
        return false;
    }

    const std::thread::id currentThreadId = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(triggerState->stateMutex);
    return triggerState->timerThreadId == currentThreadId || triggerState->callbackThreadId == currentThreadId;
}

void GB_PeriodicTrigger::RequestStop(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, bool drainPendingCallbacks)
{
    if (!triggerState)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(triggerState->stateMutex);
        triggerState->isRunning.store(false, std::memory_order_release);
    }
    triggerState->stateCond.notify_all();

    {
        std::lock_guard<std::mutex> lock(triggerState->callbackQueueMutex);
        triggerState->callbackThreadStopping = true;
        triggerState->callbackDrainPending = drainPendingCallbacks;
        if (!drainPendingCallbacks)
        {
            triggerState->callbackQueue.clear();
        }
    }
    triggerState->callbackQueueCond.notify_all();
}
