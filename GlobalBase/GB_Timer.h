#ifndef GLOBALBASE_TIMER_H_H
#define GLOBALBASE_TIMER_H_H

#include "GlobalBasePort.h"
#include <string>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <type_traits>
#include <utility>

/**
 * @brief 获取当前系统“本地时间”，并格式化为 RFC 3339/ISO 8601 风格的字符串（UTF-8）。
 *
 * 生成形如 `YYYY-MM-DDTHH:MM:SS[.mmm][Z|±HH:MM]` 的时间文本：日期与时间以 'T' 分隔；
 * 可选输出 3 位毫秒；当启用时区后缀时，若本地时区为 UTC 则输出 'Z'，否则输出
 * 本地与 UTC 的偏移量 `±HH:MM`。返回值保证为 UTF-8 编码的 std::string（仅包含 ASCII）。
 *
 * @param withMs       是否输出毫秒（3 位，范围 000–999）。默认 true。
 * @param withTzSuffix 是否在末尾附加时区标志：
 *                     - true：输出 'Z'（UTC）或本地相对 UTC 的偏移量 `±HH:MM`；
 *                     - false：不附加任何时区信息。默认 false。
 *
 * @return 以 UTF-8 编码的本地时间字符串。
 *
 * @note 当 @p withTzSuffix 为 false 时，结果不再是“严格的 RFC 3339 时间戳”（RFC 3339 要求必须带 'Z' 或 `±HH:MM`），仅表示“本地时间的 ISO 8601 形式”。
 *
 * @par 示例
 * @code
 * std::string s1 = GetLocalTimeStr();                  // "2025-08-19T11:44:44.063"
 * std::string s2 = GetLocalTimeStr(true,  true);       // "2025-08-19T11:44:44.063+08:00" 或 "…Z"
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

/**
 * @brief 轻量级计时器（支持 Start/Stop 累加、Lap 分段计时、静态 Measure 工具函数）。
 *
 * - 计时基于 std::chrono::steady_clock：单调递增，适合测量时间间隔，不依赖系统墙钟时间。
 * - 【不是线程安全的】：
 *   同一个 GB_Timer 实例不应被多个线程并发调用（Start/Stop/Lap/Elapsed 等）。
 */
class GLOBALBASE_PORT GB_Timer
{
public:
    // 等价于调用 Reset()，不在计时中
    GB_Timer();

    // 清零并停止计时
    void Reset();

    // 清零并启动计时
    void Restart();

    // 启动计时（若已在计时中则无操作）
    void Start();

    // 停止计时（若已停止则无操作）
    void Stop();

    // 当前是否处于计时状态
    bool IsRunning() const;

    // 获取“总耗时”的 std::chrono::nanoseconds 形式
    // 返回的是“自上次 Reset/Restart 以来”的累计耗时，包含多次 Start/Stop 的累计
    std::chrono::nanoseconds ElapsedNanosecondsDuration() const;

    // 获取累计耗时（纳秒）。等价于 ElapsedNanosecondsDuration().count() 的 int64_t 转换
    int64_t ElapsedNanoseconds() const;

    // 获取累计耗时（微秒）。由 duration_cast 进行单位换算；整数单位转换会截断小数部分
    int64_t ElapsedMicroseconds() const;

    // 获取累计耗时（毫秒）。elapsed milliseconds（int64_t），向下截断
    int64_t ElapsedMilliseconds() const;

    // 获取累计耗时（秒）
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
    * - 当 timer 停止时，currentElapsed 固定为 accumulated，因此 Lap 在停止状态下仍可计算“自上次 Lap 以来”的增量，
    *   但若停止后未再 Start，则增量通常为 0。
    */
    int64_t LapNanoseconds();

    // 分段耗时（微秒）
    int64_t LapMicroseconds();

    // 分段耗时（毫秒）
    int64_t LapMilliseconds();

    /**
     * @brief 将纳秒数格式化为更易读的字符串。
     *
     * 规则（当前实现）：
     * - |ns| < 1,000           -> "xxx ns"
     * - |ns| < 1,000,000       -> "xxx.xxx us"
     * - |ns| < 1,000,000,000   -> "xxx.xxx ms"
     * - 否则                   -> "xxx.xxx s"
     *
     * 说明：
     * - 保留符号（负值会显示为负数）。
     * - 小于 1000ns 时不显示小数。
     *
     * @param nanoseconds 输入纳秒数。
     * @return 格式化后的字符串。
     */
    static std::string FormatNanoseconds(int64_t nanoseconds);

    /**
    * @brief 测量一个可调用对象 func 的执行耗时（无返回值版本）。
    *
    * 用法示例：
    * @code
    * auto elapsed = GB_Timer::Measure([&](){ DoSomething(); });
    * @endcode
    *
    * 异常与保证：
    * - 若 func 正常返回：返回耗时（nanoseconds）。
    * - 若 func 抛出异常：本函数仍会先计算耗时，然后【原样重新抛出】异常；
    *   由于异常被重新抛出，调用方无法获得本函数的返回值。
    *
    * @tparam Func 可调用类型（函数/函数对象/lambda 等）
    * @tparam Args 参数包类型
    * @param func 被测可调用对象
    * @param args 调用参数
    * @return 执行耗时（std::chrono::nanoseconds）
    */
    template <typename Func, typename... Args>
    static std::chrono::nanoseconds Measure(Func&& func, Args&&... args);

    /**
     * @brief 测量一个可调用对象 func 的执行耗时（带返回值版本）。
     *
     * 返回：{result, elapsed}
     *
     * 约束：
     * - 仅当 func(args...) 的返回类型非 void 时参与重载（SFINAE）。
     *
     * 异常与保证：
     * - 若 func 正常返回：返回 {result, elapsed}。
     * - 若 func 抛出异常：本函数仍会先计算耗时，然后【原样重新抛出】异常；
     *   由于异常被重新抛出，调用方无法获得 pair 返回值。
     *
     * 注意：
     * - result 会通过 std::move 放入 pair（避免不必要的拷贝）。
     *
     * @tparam Func 可调用类型
     * @tparam Args 参数包类型
     * @param func 被测可调用对象
     * @param args 调用参数
     * @return {func 的返回值, 执行耗时}
     */
    template <typename Func, typename... Args>
    static typename std::enable_if<
        !std::is_void<decltype(std::declval<Func>()(std::declval<Args>()...))>::value,
        std::pair<
        typename std::decay<decltype(std::declval<Func>()(std::declval<Args>()...))>::type,
        std::chrono::nanoseconds>>::type
    MeasureWithResult(Func&& func, Args&&... args);

private:
    // 计时起点（仅在 running=true 时有效，用于计算 now-startTime）
    std::chrono::steady_clock::time_point startTime;

    // 已累计的耗时（多次 Start/Stop 的累计）
    std::chrono::nanoseconds accumulated;

    // 是否正在计时
    bool running = false;

    // 上一次 Lap 时刻对应的累计耗时（用于分段计时）
    std::chrono::nanoseconds lastLapElapsed;
};

/**
 * @brief 作用域计时器（RAII）：构造时启动，析构时上报。
 *
 * 典型用法：
 * @code
 * void Foo()
 * {
 *     GB_SCOPE_TIMER("Foo");
 *     // ... do work ...
 * } // <- 作用域结束时自动打印或回调上报
 * @endcode
 *
 * 上报策略：
 * - 若提供 callback：析构时优先调用 callback(name, elapsedNs)
 * - 否则若 out != nullptr：向 out 输出一行日志（毫秒）
 *
 * 重要提示（异常安全）：
 * - 析构函数不应抛异常；若析构期间抛出异常，且此时已有异常在传播，将可能触发 std::terminate。
 * - 因此：callback 建议保证不抛异常（或内部自行捕获处理）。
 */
class GLOBALBASE_PORT GB_ScopeTimer
{
public:
    /**
     * @brief 上报回调函数类型。
     * @param name  计时器名字（构造时传入）
     * @param elapsedNs  耗时（纳秒，int64_t）
     */
    using Callback = std::function<void(const std::string& name, int64_t elapsedNs)>;

    /**
     * @brief 构造并启动一个作用域计时器。
     *
     * @param timerName 计时器名称（用于日志/回调标识）
     * @param outputStream 若 callback 为空，则析构时向该流输出日志；允许为 nullptr（表示不输出）
     * @param reportCallback 析构时的回调；若非空则优先使用回调上报
     *
     * 说明：
     * - 当前实现会在构造函数体内调用 timer.Restart()，因此构造后立即开始计时。
     * - outputStream 默认是 &std::cerr。
     */
    explicit GB_ScopeTimer(const std::string& timerName, std::ostream* outputStream = &std::cerr, Callback reportCallback = Callback());

    /**
     * @brief 析构：停止并上报耗时。
     *
     * 行为：
     * - 计算 elapsed = timer.ElapsedNanosecondsDuration()
     * - 若 callback 非空：调用 callback(name, elapsedNs) 并返回
     * - 否则若 out 非空：输出 "[GB_ScopeTimer] name took xxx ms"
     *
     * 注意：
     * - 强烈建议 callback 不要抛异常。
     */
    ~GB_ScopeTimer() noexcept;

private:
    // 内部计时器（构造时 Restart）
    GB_Timer timer;

    // 计时器名称
    std::string name;

    // 输出流（callback 为空时使用；允许为 nullptr）
    std::ostream* out = nullptr;

    // 上报回调（优先级高于 out）
    Callback callback;
};

#define GB_CONCAT_INNER(a, b) a##b
#define GB_CONCAT(a, b) GB_CONCAT_INNER(a, b)

/**
 * @brief 声明一个作用域计时器变量（名字唯一，基于 __LINE__）。
 *
 * 用法：
 * @code
 * void Foo()
 * {
 *     GB_SCOPE_TIMER("Foo");
 *     ...
 * }
 * @endcode
 *
 * 注意：
 * - 宏会在当前作用域生成一个局部变量，作用域结束时触发析构上报。
 * - name 参数建议传入稳定字符串或 std::string。
 */
#define GB_SCOPE_TIMER(name) GB_ScopeTimer GB_CONCAT(gbScopeTimer_, __LINE__)(name)


// ------------------------
// Templates
// ------------------------
template <typename Func, typename... Args>
std::chrono::nanoseconds GB_Timer::Measure(Func&& func, Args&&... args)
{
    const std::chrono::steady_clock::time_point beginTime = std::chrono::steady_clock::now();
    try
    {
        std::forward<Func>(func)(std::forward<Args>(args)...);
    }
    catch (...)
    {
        const std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - beginTime);
        (void)elapsed;
        throw;
    }

    const std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - beginTime);
}

template <typename Func, typename... Args>
static typename std::enable_if<
    !std::is_void<decltype(std::declval<Func>()(std::declval<Args>()...))>::value,
    std::pair<
    typename std::decay<decltype(std::declval<Func>()(std::declval<Args>()...))>::type,
    std::chrono::nanoseconds>>::type
GB_Timer::MeasureWithResult(Func&& func, Args&&... args)
{
    const std::chrono::steady_clock::time_point beginTime = std::chrono::steady_clock::now();
    try
    {
        auto result = std::forward<Func>(func)(std::forward<Args>(args)...);
        const std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - beginTime);
        return std::make_pair(std::move(result), elapsed);
    }
    catch (...)
    {
        const std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - beginTime);
        (void)elapsed;
        throw;
    }
}

#ifdef _MSC_VER
#  pragma warning(pop)
#endif


#endif