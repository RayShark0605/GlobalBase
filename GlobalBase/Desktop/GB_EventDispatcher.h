#ifndef GLOBALBASE_EVENT_DISPATCHER_H_H
#define GLOBALBASE_EVENT_DISPATCHER_H_H

#include "GB_SystemResult.h"
#include "../GB_Variant.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief 事件分发模式。
 */
enum class GB_EventDispatchMode : uint16_t
{
    /** @brief 调用线程同步执行订阅回调。 */
    Direct = 0,

    /** @brief 事件先进入内部队列，由分发线程串行执行订阅回调。 */
    Queued = 1
};

/**
 * @brief 异步队列满时的处理策略。
 */
enum class GB_EventQueueOverflowPolicy : uint16_t
{
    /** @brief 丢弃本次新事件，避免阻塞事件源线程。 */
    DropNewest = 0,

    /** @brief 丢弃队列中最旧的事件，再放入本次新事件。 */
    DropOldest = 1
};

/**
 * @brief 停止异步分发线程时的队列处理方式。
 */
enum class GB_EventDispatcherStopMode : uint16_t
{
    /** @brief 停止接收新事件，尽量处理完已排队事件后退出。 */
    Drain = 0,

    /** @brief 停止接收新事件，丢弃尚未处理的排队事件后退出。 */
    Discard = 1
};

/**
 * @brief 通用事件对象。
 *
 * 说明：
 * - eventName 是事件类型名，不能为空，建议使用模块前缀，例如 "SystemClipboard.Changed"；
 * - sourceName 是事件源或业务来源，可为空；
 * - payload 承载主要事件数据，attributes 承载可选扩展字段；
 * - sequenceId 与 timestampMilliseconds 由 GB_EventDispatcher 在发布时补齐；
 * - 所有 std::string 均约定为 UTF-8 编码。
 */
struct GLOBALBASE_PORT GB_Event
{
    std::string eventName = "";
    std::string sourceName = "";
    GB_Variant payload;
    GB_VariantMap attributes;
    uint64_t sequenceId = 0;
    uint64_t timestampMilliseconds = 0;

    GB_Event();
    explicit GB_Event(const std::string& eventName);
    GB_Event(const std::string& eventName, const GB_Variant& payload, const std::string& sourceName = std::string());
    GB_Event(const std::string& eventName, GB_Variant&& payload, const std::string& sourceName = std::string());

    bool IsValid() const;
    bool HasPayload() const;
    bool HasAttribute(const std::string& attributeName) const;
    GB_Variant GetAttribute(const std::string& attributeName) const;
    void SetAttribute(const std::string& attributeName, const GB_Variant& value);
    void SetAttribute(const std::string& attributeName, GB_Variant&& value);
};

/**
 * @brief 事件订阅标识。
 *
 * 说明：token 只用于取消订阅，不拥有 GB_EventDispatcher 生命周期。
 */
struct GLOBALBASE_PORT GB_EventSubscriptionToken
{
    uint64_t dispatcherId = 0;
    uint64_t subscriptionId = 0;

    bool IsValid() const;
    void Reset();
    explicit operator bool() const;
    bool operator==(const GB_EventSubscriptionToken& other) const;
    bool operator!=(const GB_EventSubscriptionToken& other) const;
};

/**
 * @brief 事件分发器配置。
 */
struct GB_EventDispatcherOptions
{
    GB_EventDispatchMode dispatchMode = GB_EventDispatchMode::Direct;
    GB_EventQueueOverflowPolicy overflowPolicy = GB_EventQueueOverflowPolicy::DropNewest;
    size_t maxQueueSize = 1024;
    std::string dispatcherName = "";
};

/**
 * @brief 轻量通用事件分发器。
 *
 * 说明：
 * - 支持精确事件名订阅与全量订阅；
 * - Dispatch() 始终同步分发，Post() 始终异步入队，Publish() 按 options.dispatchMode 选择同步或异步；
 * - 执行回调前会复制当前订阅列表，因此回调内部可以安全调用 Subscribe()/Unsubscribe()；
 * - 回调抛出的异常会被捕获并计数，若设置了异常处理器则传递给处理器；
 * - 异步模式使用单分发线程串行执行回调，保证同一个分发器内事件处理顺序稳定；
 * - 本类析构时会停止异步线程并丢弃尚未处理的排队事件，避免析构阶段继续扩大回调范围；
 * - 可在回调内调用 Stop() 请求停止，但不得在该回调返回前销毁当前分发器对象；外部线程应在对象析构前再次调用 Stop() 或等待回调返回；
 * - Stop() 开始后，新进入的同步 Dispatch() 会返回 Cancelled，避免停止完成后仍启动新的同步回调。
 */
class GLOBALBASE_PORT GB_EventDispatcher final
{
public:
    using Callback = std::function<void(const GB_Event& event)>;
    using ExceptionHandler = std::function<void(const GB_Event& event, const GB_EventSubscriptionToken& subscriptionToken, std::exception_ptr exceptionPtr)>;

    GB_EventDispatcher();
    explicit GB_EventDispatcher(const GB_EventDispatcherOptions& options);
    ~GB_EventDispatcher() noexcept;

    GB_EventDispatcher(const GB_EventDispatcher&) = delete;
    GB_EventDispatcher& operator=(const GB_EventDispatcher&) = delete;
    GB_EventDispatcher(GB_EventDispatcher&&) = delete;
    GB_EventDispatcher& operator=(GB_EventDispatcher&&) = delete;

    static GB_EventDispatcherOptions MakeDirectOptions(const std::string& dispatcherName = std::string());
    static GB_EventDispatcherOptions MakeQueuedOptions(size_t maxQueueSize = 1024, GB_EventQueueOverflowPolicy overflowPolicy = GB_EventQueueOverflowPolicy::DropNewest, const std::string& dispatcherName = std::string());

    GB_EventDispatcherOptions GetOptions() const;
    uint64_t GetDispatcherId() const;

    GB_SystemResult Start();

    /**
     * @brief 停止接收新事件并停止异步分发线程。
     *
     * 说明：
     * - 从普通外部线程调用时，会等待异步工作线程以及已经开始的同步分发回调完成；
     * - 从当前分发器的事件回调内调用时只请求停止，不等待当前回调自身，避免自等待死锁；
     * - Stop() 返回后，调用方仍不应让其他线程继续并发调用 Dispatch()/Post()/Publish()。
     */
    GB_SystemResult Stop(GB_EventDispatcherStopMode stopMode = GB_EventDispatcherStopMode::Drain);

    GB_SystemResult WaitIdle();
    bool WaitIdleFor(uint64_t timeoutMilliseconds);
    bool IsRunning() const;
    bool IsAcceptingEvents() const;

    GB_SystemResult Subscribe(const std::string& eventName, const Callback& callback, GB_EventSubscriptionToken& subscriptionToken);
    GB_SystemResult SubscribeAll(const Callback& callback, GB_EventSubscriptionToken& subscriptionToken);
    GB_SystemResult Unsubscribe(const GB_EventSubscriptionToken& subscriptionToken);
    GB_SystemResult ClearSubscriptions();
    bool HasSubscription(const GB_EventSubscriptionToken& subscriptionToken) const;
    size_t GetSubscriptionCount() const;
    size_t GetSubscriptionCount(const std::string& eventName) const;

    GB_SystemResult Dispatch(const GB_Event& event);
    GB_SystemResult Dispatch(const std::string& eventName, const GB_Variant& payload = GB_Variant(), const std::string& sourceName = std::string());
    GB_SystemResult Post(const GB_Event& event);
    GB_SystemResult Post(const std::string& eventName, const GB_Variant& payload = GB_Variant(), const std::string& sourceName = std::string());
    GB_SystemResult Publish(const GB_Event& event);
    GB_SystemResult Publish(const std::string& eventName, const GB_Variant& payload = GB_Variant(), const std::string& sourceName = std::string());

    void SetExceptionHandler(ExceptionHandler exceptionHandler);

    size_t GetPendingEventCount() const;
    uint64_t GetDispatchedEventCount() const;
    uint64_t GetDroppedEventCount() const;
    uint64_t GetCallbackExceptionCount() const;
    void ResetStatistics();

    static bool IsValidDispatchModeValue(uint64_t dispatchModeValue);
    static bool IsValidOverflowPolicyValue(uint64_t overflowPolicyValue);
    static bool IsValidStopModeValue(uint64_t stopModeValue);
    static uint64_t GetCurrentTimestampMilliseconds();
    static GB_Event MakeEvent(const std::string& eventName, const GB_Variant& payload = GB_Variant(), const std::string& sourceName = std::string());

private:
    struct SubscriptionEntry
    {
        GB_EventSubscriptionToken subscriptionToken;
        std::string eventName;
        bool matchAll = false;
        Callback callback;
    };

    struct SubscriptionSnapshot
    {
        GB_EventSubscriptionToken subscriptionToken;
        Callback callback;
    };

    GB_Event PrepareEvent(const GB_Event& event);
    GB_SystemResult ValidateEvent(const GB_Event& event, const std::string& operationName) const;
    GB_SystemResult DispatchPreparedEvent(const GB_Event& event, bool countActiveDispatch);
    void WorkerLoop();
    void FinishActiveDispatch();
    std::vector<SubscriptionSnapshot> CopySubscriptionsForEventLocked(const std::string& eventName) const;
    bool IsIdleLocked() const;

private:
    const uint64_t dispatcherId;
    GB_EventDispatcherOptions options;

    mutable std::mutex operationMutex;
    mutable std::mutex stateMutex;
    std::condition_variable eventQueueCond;
    std::condition_variable idleCond;

    std::deque<GB_Event> eventQueue;
    std::vector<SubscriptionEntry> subscriptions;
    std::thread workerThread;
    ExceptionHandler exceptionHandler;

    bool isAcceptingEvents = true;
    bool isStopping = false;
    bool isWorkerStarted = false;
    bool isWorkerJoining = false;
    size_t activeDispatchCount = 0;

    uint64_t nextSubscriptionId = 1;
    uint64_t nextEventSequenceId = 1;
    uint64_t dispatchedEventCount = 0;
    uint64_t droppedEventCount = 0;
    uint64_t callbackExceptionCount = 0;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_EVENT_DISPATCHER_H_H
