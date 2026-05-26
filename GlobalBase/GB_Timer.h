#ifndef GLOBALBASE_TIMER_H_H
#define GLOBALBASE_TIMER_H_H

#include "GlobalBasePort.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

/**
 * @brief 获取当前系统“本地时间”，并格式化为 RFC 3339/ISO 8601 风格的字符串（UTF-8）。
 *
 * 生成形如 `YYYY-MM-DDTHH:MM:SS[.mmm][Z|±HH:MM]` 的时间文本：日期与时间以 'T' 分隔；
 * 可选输出 3 位毫秒；当启用时区后缀时，若本地时区为 UTC 则输出 'Z'，否则输出
 * 本地与 UTC 的偏移量 `±HH:MM`。返回值保证为 UTF-8 编码的 std::string（仅包含 ASCII）。
 *
 * @param withMs 是否输出毫秒（3 位，范围 000-999）。默认 true。
 * @param withTzSuffix 是否在末尾附加时区标志：
 *                     - true：输出 'Z'（UTC）或本地相对 UTC 的偏移量 `±HH:MM`；
 *                     - false：不附加任何时区信息。默认 false。
 *
 * @return 成功返回以 UTF-8 编码的本地时间字符串；极少数系统时间分解失败时返回空字符串。
 *
 * @note 当 @p withTzSuffix 为 false 时，结果不再是“严格的 RFC 3339 时间戳”（RFC 3339 要求必须带 'Z' 或 `±HH:MM`），仅表示“本地时间的 ISO 8601 形式”。
 *
 * @par 示例
 * @code
 * std::string s1 = GetLocalTimeStr();                  // "2025-08-19T11:44:44.063"
 * std::string s2 = GetLocalTimeStr(true, true);        // "2025-08-19T11:44:44.063+08:00" 或 "...Z"
 * std::string s3 = GetLocalTimeStr(false, true);       // "2025-08-19T11:44:44+08:00"
 * std::string s4 = GetLocalTimeStr(false, false);      // "2025-08-19T11:44:44"
 * @endcode
 *
 * @threadsafety 内部采用线程安全的本地/UTC 分解时间函数（Windows: localtime_s/gmtime_s；POSIX: localtime_r/gmtime_r），可在多线程环境下安全调用。
 */
GLOBALBASE_PORT std::string GetLocalTimeStr(bool withMs = true, bool withTzSuffix = false);

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

namespace GB_TimerDetail
{
    struct PeriodicTriggerState;

    template <typename Func, typename... Args>
    struct CallResult
    {
        using Type = typename std::result_of<Func && (Args&&...)>::type;
    };

    template <typename Func, typename... Args>
    using MeasureWithResultReturn = typename std::enable_if<!std::is_void<typename CallResult<Func, Args...>::Type>::value, std::pair<typename std::decay<typename CallResult<Func, Args...>::Type>::type, std::chrono::nanoseconds>>::type;

    template <typename Func, typename... Args>
    typename std::enable_if<!std::is_member_pointer<typename std::decay<Func>::type>::value, typename std::result_of<Func && (Args&&...)>::type>::type Invoke(Func&& func, Args&&... args)
    {
        return std::forward<Func>(func)(std::forward<Args>(args)...);
    }

    template <typename Func, typename... Args>
    typename std::enable_if<std::is_member_pointer<typename std::decay<Func>::type>::value, typename std::result_of<Func && (Args&&...)>::type>::type Invoke(Func&& func, Args&&... args)
    {
        return std::mem_fn(func)(std::forward<Args>(args)...);
    }
}

/**
 * @brief 轻量级计时器（支持 Start/Stop 累加、Lap 分段计时、静态 Measure 工具函数）。
 *
 * - 计时基于 std::chrono::steady_clock：单调递增，适合测量时间间隔，不依赖系统墙钟时间。
 * - 【不是线程安全的】：同一个 GB_Timer 实例不应被多个线程并发调用（Start/Stop/Lap/Elapsed 等）。
 */
class GLOBALBASE_PORT GB_Timer
{
public:
    // 等价于调用 Reset()，不在计时中。
    GB_Timer();

    // 清零并停止计时。
    void Reset();

    // 清零并启动计时。
    void Restart();

    // 启动计时（若已在计时中则无操作）。
    void Start();

    // 停止计时（若已停止则无操作）。
    void Stop();

    // 当前是否处于计时状态。
    bool IsRunning() const;

    // 获取“总耗时”的 std::chrono::nanoseconds 形式。
    // 返回的是“自上次 Reset/Restart 以来”的累计耗时，包含多次 Start/Stop 的累计。
    std::chrono::nanoseconds ElapsedNanosecondsDuration() const;

    // 获取累计耗时（纳秒）。等价于 ElapsedNanosecondsDuration().count() 的 int64_t 转换。
    int64_t ElapsedNanoseconds() const;

    // 获取累计耗时（微秒）。由 duration_cast 进行单位换算；整数单位转换会截断小数部分。
    int64_t ElapsedMicroseconds() const;

    // 获取累计耗时（毫秒）。由 duration_cast 进行单位换算；整数单位转换会截断小数部分。
    int64_t ElapsedMilliseconds() const;

    // 获取累计耗时（秒）。
    double ElapsedSeconds() const;

    /**
     * @brief 获取与上一次 Lap 之间的“分段耗时”（纳秒）。
     *
     * 语义：
     * - delta = currentElapsed - lastLapElapsed
     * - lastLapElapsed = currentElapsed
     * - 返回 delta（纳秒）
     *
     * 说明：
     * - 第一次调用 Lap* 时，lastLapElapsed 初始为 0，因此返回“从开始/累计以来”的耗时。
     * - 当 timer 停止时，currentElapsed 固定为 accumulated，因此 Lap 在停止状态下仍可计算“自上次 Lap 以来”的增量。
     */
    int64_t LapNanoseconds();

    // 分段耗时（微秒）。
    int64_t LapMicroseconds();

    // 分段耗时（毫秒）。
    int64_t LapMilliseconds();

    /**
     * @brief 将纳秒数格式化为更易读的字符串。
     *
     * 规则：
     * - |ns| < 1,000           -> "xxx ns"
     * - |ns| < 1,000,000       -> "xxx.xxx us"
     * - |ns| < 1,000,000,000   -> "xxx.xxx ms"
     * - 否则                   -> "xxx.xxx s"
     *
     * @param nanoseconds 输入纳秒数。
     * @return 格式化后的字符串。
     */
    static std::string FormatNanoseconds(int64_t nanoseconds);

    /**
     * @brief 测量一个可调用对象 func 的执行耗时。
     *
     * @tparam Func 可调用类型（函数/函数对象/lambda 等）。
     * @tparam Args 参数包类型。
     * @param func 被测可调用对象。
     * @param args 调用参数。
     * @return 执行耗时（std::chrono::nanoseconds）。
     *
     * @note 若 func 抛出异常，本函数会原样重新抛出异常；异常路径下调用方无法获得返回耗时。
     */
    template <typename Func, typename... Args>
    static std::chrono::nanoseconds Measure(Func&& func, Args&&... args);

    /**
     * @brief 测量一个可调用对象 func 的执行耗时，并返回 func 的返回值。
     *
     * @tparam Func 可调用类型。
     * @tparam Args 参数包类型。
     * @param func 被测可调用对象。
     * @param args 调用参数。
     * @return {func 的返回值, 执行耗时}。
     *
     * @note 仅当 func(args...) 的返回类型非 void 时参与重载；若 func 抛出异常，本函数会原样重新抛出异常，调用方无法获得返回值和耗时。
     */
    template <typename Func, typename... Args>
    static GB_TimerDetail::MeasureWithResultReturn<Func, Args...> MeasureWithResult(Func&& func, Args&&... args);

private:
    // 计时起点（仅在 running=true 时有效，用于计算 now - startTime）。
    std::chrono::steady_clock::time_point startTime;

    // 已累计的耗时（多次 Start/Stop 的累计）。
    std::chrono::nanoseconds accumulated;

    // 是否正在计时。
    bool running = false;

    // 上一次 Lap 时刻对应的累计耗时（用于分段计时）。
    std::chrono::nanoseconds lastLapElapsed;
};

/**
 * @brief 作用域计时器（RAII）：构造时启动，析构时上报。
 *
 * 上报策略：
 * - 若提供 callback：析构时优先调用 callback(name, elapsedNs)；
 * - 否则若 out != nullptr：向 out 输出一行日志。
 *
 * @note 析构函数内部会捕获所有异常，避免析构期间异常逃逸导致 std::terminate。
 */
class GLOBALBASE_PORT GB_ScopeTimer
{
public:
    /**
     * @brief 上报回调函数类型。
     * @param name 计时器名字（构造时传入）。
     * @param elapsedNs 耗时（纳秒，int64_t）。
     */
    using Callback = std::function<void(const std::string& name, int64_t elapsedNs)>;

    /**
     * @brief 构造并启动一个作用域计时器。
     *
     * @param timerName 计时器名称（用于日志/回调标识）。
     * @param outputStream 若 callback 为空，则析构时向该流输出日志；允许为 nullptr（表示不输出）。
     * @param reportCallback 析构时的回调；若非空则优先使用回调上报。
     */
    explicit GB_ScopeTimer(const std::string& timerName, std::ostream* outputStream = &std::cerr, Callback reportCallback = Callback());
    ~GB_ScopeTimer() noexcept;

    GB_ScopeTimer(const GB_ScopeTimer&) = delete;
    GB_ScopeTimer& operator=(const GB_ScopeTimer&) = delete;

private:
    // 内部计时器（构造时 Restart）。
    GB_Timer timer;

    // 计时器名称。
    std::string name;

    // 输出流（callback 为空时使用；允许为 nullptr）。
    std::ostream* out = nullptr;

    // 上报回调（优先级高于 out）。
    Callback callback;
};

#define GB_CONCAT_INNER(a, b) a##b
#define GB_CONCAT(a, b) GB_CONCAT_INNER(a, b)

/**
 * @brief 声明一个作用域计时器变量（变量名基于 __LINE__ 自动生成）。
 */
#define GB_SCOPE_TIMER(name) GB_ScopeTimer GB_CONCAT(gbScopeTimer_, __LINE__)(name)

 /**
  * @brief 跨平台周期触发器：每隔指定周期触发一次回调。
  *
  * 设计目标：
  * - 使用 std::chrono::steady_clock 作为时间基准，避免系统时间被调整造成的累计误差。
  * - 使用 std::condition_variable 等待下一次触发时刻；空闲等待期间不主动轮询，不占用 CPU。
  * - 支持同步和异步两种回调模式。
  * - 支持固定延迟、固定频率跳过、固定频率追赶三种调度策略。
  *
  * 重要说明：
  * - 本类不是硬实时定时器。实际精度受操作系统调度粒度、线程优先级、系统负载、callback 执行时间影响。
  * - Stop()/析构会等待内部线程退出；若 callback 本身长期阻塞，Stop()/析构也会被阻塞。
  * - 可以在 callback 内调用 Stop() 请求停止；若此时触发线程就是当前线程，内部会改为 detach 当前线程并保证线程只访问内部共享状态。
  * - callback 抛出的异常会被内部捕获；若设置了 ExceptionHandler，则传给异常处理器，否则吞掉，避免工作线程异常逃逸导致进程终止。
  * - Start()/Stop() 内部做了生命周期串行化，可由普通控制线程调用；Stop() 也可以在 callback 内安全调用。
  */
class GLOBALBASE_PORT GB_PeriodicTrigger
{
public:
    /**
     * @brief callback 的执行模式。
     */
    enum class CallbackMode
    {
        // 同步回调：由定时线程直接执行 callback。优点是线程少、开销低、顺序严格；缺点是慢 callback 会影响下一次调度。
        Synchronous,

        // 异步回调：定时线程只负责按节拍产生事件，callback 交给内部回调线程串行执行。优点是定时线程不被普通 callback 阻塞；缺点是需要处理回调排队和溢出。
        Asynchronous
    };

    /**
     * @brief 定时调度策略。
     */
    enum class SchedulePolicy
    {
        // 固定延迟：本次事件处理完成后再等待一个 period。同步模式下等价于“callback 结束后再等 period”；异步模式下等价于“事件派发/入队后再等 period”。
        FixedDelay,

        // 固定频率 + 跳过：按 startTime + k * period 的固定节拍触发；若已经错过一个或多个节拍，则跳到下一个未来节拍。默认推荐，能避免累计误差和无界追赶。
        FixedRateSkip,

        // 固定频率 + 追赶：按 startTime + k * period 的固定节拍触发；若已经落后，则连续触发，直到追上当前进度。适合不能丢逻辑 tick 的仿真/采样补偿类任务。
        FixedRateCatchUp
    };

    /**
     * @brief 异步模式下，待回调队列达到 maxPendingCallbacks 后的处理策略。
     */
    enum class AsyncOverflowPolicy
    {
        // 丢弃本次新产生的回调事件。定时线程不阻塞，优先保证事件生产节拍稳定。
        DropNewest,

        // 丢弃队列中最旧的回调事件，再放入新事件。适合只关心较新状态的刷新类任务。
        DropOldest,

        // 阻塞定时线程，直到队列有空间。不会丢异步回调，但定时节拍可能因此滞后。
        BlockTimer
    };

    /**
     * @brief 每次触发 callback 时传入的事件信息。
     */
    struct Event
    {
        // 逻辑触发序号，从 1 开始。FixedRateSkip 跳过节拍时，该序号可能不连续。
        uint64_t triggerIndex = 0;

        // 实际开始执行 callback 的序号，从 1 开始。异步模式下，被丢弃的队列事件不会占用该序号。
        uint64_t callbackIndex = 0;

        // 本次 callback 之前，因为 FixedRateSkip 策略跳过的节拍数量。
        uint64_t skippedTriggerCount = 0;

        // 本触发器启动以来累计跳过的节拍数量。
        uint64_t totalSkippedTriggerCount = 0;

        // 异步模式下，启动以来因队列溢出累计丢弃的回调事件数量。
        uint64_t totalDroppedAsyncCallbackCount = 0;

        // 当前 callback 开始执行时，异步队列中尚未执行的事件数量；同步模式下恒为 0。
        size_t pendingCallbackCount = 0;

        // 配置的触发周期。
        std::chrono::nanoseconds period = std::chrono::nanoseconds(0);

        // 本次事件理论上应该触发的 steady_clock 时刻。
        std::chrono::steady_clock::time_point scheduledTime;

        // 定时线程实际产生本次事件的 steady_clock 时刻。
        std::chrono::steady_clock::time_point actualTriggerTime;

        // callback 实际开始执行的 steady_clock 时刻。
        std::chrono::steady_clock::time_point callbackStartTime;

        // actualTriggerTime - scheduledTime；若未迟到则为 0。
        std::chrono::nanoseconds triggerDelay = std::chrono::nanoseconds(0);

        // callbackStartTime - scheduledTime；若未迟到则为 0。
        std::chrono::nanoseconds callbackStartDelay = std::chrono::nanoseconds(0);
    };

    /**
     * @brief 启动配置。
     */
    struct Options
    {
        // 触发周期。必须 > 0。虽然常见用法是毫秒级定时，但这里使用 nanoseconds 以支持更通用的 chrono 周期。
        std::chrono::nanoseconds period = std::chrono::milliseconds(1000);

        // 回调执行模式。默认同步执行，线程数最少、行为最直接。
        CallbackMode callbackMode = CallbackMode::Synchronous;

        // 调度策略。默认固定频率跳过，尽量避免累计误差，同时避免 callback 太慢时无界追赶。
        SchedulePolicy schedulePolicy = SchedulePolicy::FixedRateSkip;

        // 异步回调队列满时的处理策略。仅 callbackMode == CallbackMode::Asynchronous 时生效。
        AsyncOverflowPolicy asyncOverflowPolicy = AsyncOverflowPolicy::DropNewest;

        // 异步模式下最多允许排队的回调事件数。0 表示不限制队列长度；默认 1024，避免 callback 长时间过慢导致内存无限增长。
        size_t maxPendingCallbacks = 1024;

        // 是否在 Start 后立即触发第一次 callback。false 表示第一次触发发生在 period 之后。
        bool triggerImmediately = false;
    };

    using Callback = std::function<void(const Event& triggerEvent)>;
    using ExceptionHandler = std::function<void(std::exception_ptr exceptionPtr)>;

    GB_PeriodicTrigger();
    ~GB_PeriodicTrigger() noexcept;

    GB_PeriodicTrigger(const GB_PeriodicTrigger&) = delete;
    GB_PeriodicTrigger& operator=(const GB_PeriodicTrigger&) = delete;
    GB_PeriodicTrigger(GB_PeriodicTrigger&&) = delete;
    GB_PeriodicTrigger& operator=(GB_PeriodicTrigger&&) = delete;

    /**
     * @brief 使用毫秒周期启动触发器。
     *
     * @param periodMilliseconds 触发周期，单位毫秒，必须 > 0。
     * @param triggerCallback 每次触发时执行的回调。
     * @param callbackMode 回调模式：同步或异步。
     * @param schedulePolicy 定时调度策略。
     * @return 启动成功返回 true；参数无效返回 false；线程创建失败时返回 false。
     */
    bool Start(int64_t periodMilliseconds, Callback triggerCallback, CallbackMode callbackMode = CallbackMode::Synchronous, SchedulePolicy schedulePolicy = SchedulePolicy::FixedRateSkip);

    /**
     * @brief 使用 chrono 纳秒周期启动触发器。
     *
     * @param period 触发周期，必须 > 0。
     * @param triggerCallback 每次触发时执行的回调。
     * @param callbackMode 回调模式：同步或异步。
     * @param schedulePolicy 定时调度策略。
     * @return 启动成功返回 true；参数无效返回 false；线程创建失败时返回 false。
     */
    bool Start(std::chrono::nanoseconds period, Callback triggerCallback, CallbackMode callbackMode = CallbackMode::Synchronous, SchedulePolicy schedulePolicy = SchedulePolicy::FixedRateSkip);

    /**
     * @brief 使用完整配置启动触发器。
     *
     * 若参数有效且当前触发器已经在运行，会先 Stop(false) 停止旧任务，再启动新任务。参数无效时不会影响当前正在运行的任务。
     *
     * @param triggerOptions 启动配置。
     * @param triggerCallback 每次触发时执行的回调。
     * @return 启动成功返回 true；参数无效返回 false；线程创建失败时返回 false。
     */
    bool Start(const Options& triggerOptions, Callback triggerCallback);

    /**
     * @brief 停止触发器并等待内部线程退出。
     *
     * @param drainPendingCallbacks 仅异步模式有效：
     *                              - false：停止后丢弃尚未执行的排队回调，仅等待当前正在执行的 callback 返回；
     *                              - true ：停止后尽量执行完已经排队的回调，再退出回调线程。
     */
    void Stop(bool drainPendingCallbacks = false);

    // 当前是否正在运行。
    bool IsRunning() const;

    /**
     * @brief 设置 callback 异常处理器。
     *
     * callback 抛出异常时，内部线程会捕获异常并调用该处理器。
     * 若处理器本身抛出异常，该异常会被吞掉，以避免工作线程异常逃逸。
     */
    void SetExceptionHandler(ExceptionHandler handler);

    // 获取实际开始执行的 callback 数量。异步模式下，被队列溢出策略丢弃的事件不会计入。
    uint64_t GetDispatchedCallbackCount() const;

    // 获取因 FixedRateSkip 策略累计跳过的逻辑节拍数量。
    uint64_t GetSkippedTriggerCount() const;

    // 获取异步模式下因队列溢出累计丢弃的回调事件数量。
    uint64_t GetDroppedAsyncCallbackCount() const;

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Nanoseconds = std::chrono::nanoseconds;

    static void TimerThreadFunc(std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> triggerState);
    static void AsyncCallbackThreadFunc(std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> triggerState);
    static bool IsValidOptions(const Options& triggerOptions);
    static bool WaitUntilOrStop(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, const TimePoint& deadline);
    static bool DispatchCallback(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, const Options& triggerOptions, Event& triggerEvent);
    static bool EnqueueAsyncCallback(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, const Options& triggerOptions, Event& triggerEvent);
    static void InvokeCallbackNoThrow(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, Event triggerEvent, size_t pendingCallbackCount);
    static void HandleCallbackException(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, std::exception_ptr exceptionPtr);
    static void RequestStop(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState, bool drainPendingCallbacks);
    static bool IsCurrentWorkerThread(const std::shared_ptr<GB_TimerDetail::PeriodicTriggerState>& triggerState);

    void StopInternal(bool drainPendingCallbacks);

private:
    // 串行化 Start()/Stop() 控制流程，避免多个控制线程并发移动/等待内部线程对象。
    mutable std::mutex operationMutex;

    mutable std::mutex startMutex;
    mutable std::mutex lifecycleMutex;
    std::shared_ptr<GB_TimerDetail::PeriodicTriggerState> state;
    std::thread timerThread;
    std::thread callbackThread;
    ExceptionHandler exceptionHandler;
};

// ------------------------
// Templates
// ------------------------
template <typename Func, typename... Args>
std::chrono::nanoseconds GB_Timer::Measure(Func&& func, Args&&... args)
{
    const std::chrono::steady_clock::time_point beginTime = std::chrono::steady_clock::now();
    GB_TimerDetail::Invoke(std::forward<Func>(func), std::forward<Args>(args)...);
    const std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - beginTime);
}

template <typename Func, typename... Args>
GB_TimerDetail::MeasureWithResultReturn<Func, Args...> GB_Timer::MeasureWithResult(Func&& func, Args&&... args)
{
    using ResultType = typename std::decay<typename GB_TimerDetail::CallResult<Func, Args...>::Type>::type;

    const std::chrono::steady_clock::time_point beginTime = std::chrono::steady_clock::now();
    ResultType result(GB_TimerDetail::Invoke(std::forward<Func>(func), std::forward<Args>(args)...));
    const std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
    const std::chrono::nanoseconds elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - beginTime);
    return std::make_pair(std::move(result), elapsed);
}

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
