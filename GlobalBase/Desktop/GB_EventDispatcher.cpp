#include "GB_EventDispatcher.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <new>
#include <utility>

namespace
{
    static std::atomic<uint64_t>& GetGlobalDispatcherIdSeed()
    {
        static std::atomic<uint64_t> dispatcherIdSeed(1);
        return dispatcherIdSeed;
    }

    static uint64_t AllocateDispatcherId()
    {
        uint64_t dispatcherId = GetGlobalDispatcherIdSeed().fetch_add(1, std::memory_order_relaxed);
        if (dispatcherId == 0)
        {
            dispatcherId = GetGlobalDispatcherIdSeed().fetch_add(1, std::memory_order_relaxed);
        }

        return dispatcherId == 0 ? 1 : dispatcherId;
    }

    static std::string ResolveOperationName(const std::string& operationName, const std::string& defaultOperationName)
    {
        return operationName.empty() ? defaultOperationName : operationName;
    }

    static bool IsValidEventName(const std::string& eventName)
    {
        return !eventName.empty();
    }

    static uint64_t TakeNextId(uint64_t& nextId)
    {
        if (nextId == 0)
        {
            nextId = 1;
        }

        const uint64_t currentId = nextId;
        if (nextId == (std::numeric_limits<uint64_t>::max)())
        {
            nextId = 1;
        }
        else
        {
            nextId++;
        }

        return currentId;
    }

    static GB_EventDispatcherOptions NormalizeOptions(const GB_EventDispatcherOptions& options)
    {
        GB_EventDispatcherOptions normalizedOptions = options;
        if (!GB_EventDispatcher::IsValidDispatchModeValue(static_cast<uint64_t>(normalizedOptions.dispatchMode)))
        {
            normalizedOptions.dispatchMode = GB_EventDispatchMode::Direct;
        }

        if (!GB_EventDispatcher::IsValidOverflowPolicyValue(static_cast<uint64_t>(normalizedOptions.overflowPolicy)))
        {
            normalizedOptions.overflowPolicy = GB_EventQueueOverflowPolicy::DropNewest;
        }

        return normalizedOptions;
    }

    static GB_SystemResult MakeInvalidEventNameResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"事件名不能为空。");
    }

    static GB_SystemResult MakeInvalidCallbackResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"事件回调不能为空。");
    }

    static GB_SystemResult MakeInvalidSubscriptionTokenResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, operationName, u8"订阅 token 无效，无法取消订阅。");
    }

    static GB_SystemResult MakeQueueFullResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, operationName, u8"事件队列已达到容量上限，按 DropNewest 策略丢弃本次事件。");
    }

    static GB_SystemResult MakeAllocationFailedResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"分配事件分发内部资源失败。");
    }

    static GB_SystemResult MakeInternalFailedResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InternalError, operationName, u8"事件分发内部状态处理失败。");
    }

    struct DispatchThreadFrame
    {
        const GB_EventDispatcher* dispatcher = nullptr;
        DispatchThreadFrame* previousFrame = nullptr;
    };

    static thread_local DispatchThreadFrame* currentDispatchThreadFrame = nullptr;

    class DispatchThreadScope final
    {
    public:
        explicit DispatchThreadScope(const GB_EventDispatcher* dispatcher) noexcept
        {
            frame.dispatcher = dispatcher;
            frame.previousFrame = currentDispatchThreadFrame;
            currentDispatchThreadFrame = &frame;
        }

        ~DispatchThreadScope() noexcept
        {
            currentDispatchThreadFrame = frame.previousFrame;
        }

        DispatchThreadScope(const DispatchThreadScope&) = delete;
        DispatchThreadScope& operator=(const DispatchThreadScope&) = delete;

    private:
        DispatchThreadFrame frame;
    };

    static bool IsDispatchingOnCurrentThread(const GB_EventDispatcher* dispatcher) noexcept
    {
        for (const DispatchThreadFrame* frame = currentDispatchThreadFrame; frame != nullptr; frame = frame->previousFrame)
        {
            if (frame->dispatcher == dispatcher)
            {
                return true;
            }
        }

        return false;
    }

}

GB_Event::GB_Event()
{
}

GB_Event::GB_Event(const std::string& eventName)
    : eventName(eventName)
{
}

GB_Event::GB_Event(const std::string& eventName, const GB_Variant& payload, const std::string& sourceName)
    : eventName(eventName)
    , sourceName(sourceName)
    , payload(payload)
{
}

GB_Event::GB_Event(const std::string& eventName, GB_Variant&& payload, const std::string& sourceName)
    : eventName(eventName)
    , sourceName(sourceName)
    , payload(std::move(payload))
{
}

bool GB_Event::IsValid() const
{
    return IsValidEventName(eventName);
}

bool GB_Event::HasPayload() const
{
    return !payload.IsEmpty();
}

bool GB_Event::HasAttribute(const std::string& attributeName) const
{
    return attributes.find(attributeName) != attributes.end();
}

GB_Variant GB_Event::GetAttribute(const std::string& attributeName) const
{
    const GB_VariantMap::const_iterator iter = attributes.find(attributeName);
    if (iter == attributes.end())
    {
        return GB_Variant();
    }

    return iter->second;
}

void GB_Event::SetAttribute(const std::string& attributeName, const GB_Variant& value)
{
    if (attributeName.empty())
    {
        return;
    }

    attributes[attributeName] = value;
}

void GB_Event::SetAttribute(const std::string& attributeName, GB_Variant&& value)
{
    if (attributeName.empty())
    {
        return;
    }

    attributes[attributeName] = std::move(value);
}

bool GB_EventSubscriptionToken::IsValid() const
{
    return dispatcherId != 0 && subscriptionId != 0;
}

void GB_EventSubscriptionToken::Reset()
{
    dispatcherId = 0;
    subscriptionId = 0;
}

GB_EventSubscriptionToken::operator bool() const
{
    return IsValid();
}

bool GB_EventSubscriptionToken::operator==(const GB_EventSubscriptionToken& other) const
{
    return dispatcherId == other.dispatcherId && subscriptionId == other.subscriptionId;
}

bool GB_EventSubscriptionToken::operator!=(const GB_EventSubscriptionToken& other) const
{
    return !(*this == other);
}

GB_EventDispatcher::GB_EventDispatcher()
    : dispatcherId(AllocateDispatcherId())
    , options(NormalizeOptions(GB_EventDispatcherOptions()))
{
}

GB_EventDispatcher::GB_EventDispatcher(const GB_EventDispatcherOptions& options)
    : dispatcherId(AllocateDispatcherId())
    , options(NormalizeOptions(options))
{
}

GB_EventDispatcher::~GB_EventDispatcher() noexcept
{
    try
    {
        Stop(GB_EventDispatcherStopMode::Discard);
    }
    catch (...)
    {
    }
}

GB_EventDispatcherOptions GB_EventDispatcher::MakeDirectOptions(const std::string& dispatcherName)
{
    GB_EventDispatcherOptions options;
    options.dispatchMode = GB_EventDispatchMode::Direct;
    options.overflowPolicy = GB_EventQueueOverflowPolicy::DropNewest;
    options.maxQueueSize = 1024;
    options.dispatcherName = dispatcherName;
    return options;
}

GB_EventDispatcherOptions GB_EventDispatcher::MakeQueuedOptions(const size_t maxQueueSize, const GB_EventQueueOverflowPolicy overflowPolicy, const std::string& dispatcherName)
{
    GB_EventDispatcherOptions options;
    options.dispatchMode = GB_EventDispatchMode::Queued;
    options.overflowPolicy = overflowPolicy;
    options.maxQueueSize = maxQueueSize;
    options.dispatcherName = dispatcherName;
    return NormalizeOptions(options);
}

GB_EventDispatcherOptions GB_EventDispatcher::GetOptions() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return options;
}

uint64_t GB_EventDispatcher::GetDispatcherId() const
{
    return dispatcherId;
}

GB_SystemResult GB_EventDispatcher::Start()
{
    std::unique_lock<std::mutex> operationLock(operationMutex);
    std::thread completedWorkerThread;

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (isWorkerJoining)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_EventDispatcher::Start", u8"事件分发线程正在由其他线程回收，不能并发启动。");
        }

        if (isWorkerStarted)
        {
            if (isStopping)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_EventDispatcher::Start", u8"事件分发线程正在停止，不能在停止完成前重新启动。");
            }

            isAcceptingEvents = true;
            return GB_SystemResult::Succeeded(u8"GB_EventDispatcher::Start", u8"事件分发线程已经启动。");
        }

        if (workerThread.joinable())
        {
            if (workerThread.get_id() == std::this_thread::get_id())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_EventDispatcher::Start", u8"不能在尚未退出的事件分发线程内部重新启动分发器。");
            }

            completedWorkerThread = std::move(workerThread);
            isWorkerJoining = true;
        }
    }

    if (completedWorkerThread.joinable())
    {
        completedWorkerThread.join();
        std::lock_guard<std::mutex> lock(stateMutex);
        isWorkerJoining = false;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        isAcceptingEvents = true;
        isStopping = false;

        try
        {
            workerThread = std::thread(&GB_EventDispatcher::WorkerLoop, this);
            isWorkerStarted = true;
        }
        catch (const std::bad_alloc&)
        {
            isAcceptingEvents = false;
            isStopping = false;
            return MakeAllocationFailedResult(u8"GB_EventDispatcher::Start");
        }
        catch (...)
        {
            isAcceptingEvents = false;
            isStopping = false;
            return MakeInternalFailedResult(u8"GB_EventDispatcher::Start");
        }
    }

    eventQueueCond.notify_all();
    return GB_SystemResult::Succeeded(u8"GB_EventDispatcher::Start", u8"已启动事件分发线程。");
}

GB_SystemResult GB_EventDispatcher::Stop(const GB_EventDispatcherStopMode stopMode)
{
    if (!IsValidStopModeValue(static_cast<uint64_t>(stopMode)))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, u8"GB_EventDispatcher::Stop", u8"停止模式不是有效的 GB_EventDispatcherStopMode 值。");
    }

    const bool calledFromDispatcherCallback = IsDispatchingOnCurrentThread(this);
    std::unique_lock<std::mutex> operationLock(operationMutex);
    std::thread localWorkerThread;
    bool calledFromWorkerThread = false;

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        isAcceptingEvents = false;
        isStopping = true;

        if (stopMode == GB_EventDispatcherStopMode::Discard && !eventQueue.empty())
        {
            droppedEventCount += static_cast<uint64_t>(eventQueue.size());
            eventQueue.clear();
        }

        if (workerThread.joinable())
        {
            calledFromWorkerThread = workerThread.get_id() == std::this_thread::get_id();
            if (!calledFromWorkerThread)
            {
                localWorkerThread = std::move(workerThread);
                isWorkerJoining = true;
            }
        }
        else if (!isWorkerStarted)
        {
            isStopping = false;
        }
    }

    eventQueueCond.notify_all();
    idleCond.notify_all();
    operationLock.unlock();

    if (localWorkerThread.joinable())
    {
        localWorkerThread.join();
        std::lock_guard<std::mutex> lock(stateMutex);
        isWorkerStarted = false;
        isWorkerJoining = false;
        isStopping = false;
    }

    if (!calledFromDispatcherCallback)
    {
        std::unique_lock<std::mutex> lock(stateMutex);
        idleCond.wait(lock, [this]()
            {
                return IsIdleLocked();
            });
    }

    if (calledFromDispatcherCallback)
    {
        const std::string message = calledFromWorkerThread ? u8"已在事件分发线程内请求停止；线程将在当前回调返回后退出，并由后续 Start()、Stop() 或析构过程回收。" : u8"已在同步事件回调内请求停止；当前回调返回后分发器才会完全空闲。";
        return GB_SystemResult::Succeeded(u8"GB_EventDispatcher::Stop", message);
    }

    return GB_SystemResult::Succeeded(u8"GB_EventDispatcher::Stop", u8"已停止事件分发线程，并等待当前已经开始的同步分发回调完成。");
}

GB_SystemResult GB_EventDispatcher::WaitIdle()
{
    if (IsDispatchingOnCurrentThread(this))
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, u8"GB_EventDispatcher::WaitIdle", u8"不能在当前分发器的事件回调内等待自身空闲，否则会形成自等待死锁。");
    }

    std::unique_lock<std::mutex> lock(stateMutex);

    idleCond.wait(lock, [this]()
        {
            return IsIdleLocked();
        });

    return GB_SystemResult::Succeeded(u8"GB_EventDispatcher::WaitIdle", u8"事件队列已经空闲。");
}

bool GB_EventDispatcher::WaitIdleFor(const uint64_t timeoutMilliseconds)
{
    if (IsDispatchingOnCurrentThread(this))
    {
        return false;
    }

    std::unique_lock<std::mutex> lock(stateMutex);

    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    return idleCond.wait_until(lock, deadline, [this]()
        {
            return IsIdleLocked();
        });
}

bool GB_EventDispatcher::IsRunning() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return isWorkerStarted && !isStopping && !isWorkerJoining;
}

bool GB_EventDispatcher::IsAcceptingEvents() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return isAcceptingEvents;
}

GB_SystemResult GB_EventDispatcher::Subscribe(const std::string& eventName, const Callback& callback, GB_EventSubscriptionToken& subscriptionToken)
{
    const std::string operationName = u8"GB_EventDispatcher::Subscribe";
    subscriptionToken.Reset();

    if (!IsValidEventName(eventName))
    {
        return MakeInvalidEventNameResult(operationName);
    }

    if (!callback)
    {
        return MakeInvalidCallbackResult(operationName);
    }

    try
    {
        std::lock_guard<std::mutex> lock(stateMutex);

        SubscriptionEntry subscriptionEntry;
        subscriptionEntry.subscriptionToken.dispatcherId = dispatcherId;
        subscriptionEntry.subscriptionToken.subscriptionId = TakeNextId(nextSubscriptionId);
        subscriptionEntry.eventName = eventName;
        subscriptionEntry.matchAll = false;
        subscriptionEntry.callback = callback;

        subscriptions.push_back(subscriptionEntry);
        subscriptionToken = subscriptionEntry.subscriptionToken;
    }
    catch (const std::bad_alloc&)
    {
        subscriptionToken.Reset();
        return MakeAllocationFailedResult(operationName);
    }
    catch (...)
    {
        subscriptionToken.Reset();
        return MakeInternalFailedResult(operationName);
    }

    return GB_SystemResult::Succeeded(operationName, u8"已添加事件订阅。");
}

GB_SystemResult GB_EventDispatcher::SubscribeAll(const Callback& callback, GB_EventSubscriptionToken& subscriptionToken)
{
    const std::string operationName = u8"GB_EventDispatcher::SubscribeAll";
    subscriptionToken.Reset();

    if (!callback)
    {
        return MakeInvalidCallbackResult(operationName);
    }

    try
    {
        std::lock_guard<std::mutex> lock(stateMutex);

        SubscriptionEntry subscriptionEntry;
        subscriptionEntry.subscriptionToken.dispatcherId = dispatcherId;
        subscriptionEntry.subscriptionToken.subscriptionId = TakeNextId(nextSubscriptionId);
        subscriptionEntry.matchAll = true;
        subscriptionEntry.callback = callback;

        subscriptions.push_back(subscriptionEntry);
        subscriptionToken = subscriptionEntry.subscriptionToken;
    }
    catch (const std::bad_alloc&)
    {
        subscriptionToken.Reset();
        return MakeAllocationFailedResult(operationName);
    }
    catch (...)
    {
        subscriptionToken.Reset();
        return MakeInternalFailedResult(operationName);
    }

    return GB_SystemResult::Succeeded(operationName, u8"已添加全量事件订阅。");
}

GB_SystemResult GB_EventDispatcher::Unsubscribe(const GB_EventSubscriptionToken& subscriptionToken)
{
    const std::string operationName = u8"GB_EventDispatcher::Unsubscribe";
    if (!subscriptionToken.IsValid() || subscriptionToken.dispatcherId != dispatcherId)
    {
        return MakeInvalidSubscriptionTokenResult(operationName);
    }

    std::lock_guard<std::mutex> lock(stateMutex);
    for (size_t index = 0; index < subscriptions.size(); index++)
    {
        if (subscriptions[index].subscriptionToken == subscriptionToken)
        {
            subscriptions.erase(subscriptions.begin() + static_cast<std::ptrdiff_t>(index));
            return GB_SystemResult::Succeeded(operationName, u8"已取消事件订阅。");
        }
    }

    return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, operationName, u8"未找到指定订阅 token 对应的订阅。");
}

GB_SystemResult GB_EventDispatcher::ClearSubscriptions()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    subscriptions.clear();
    return GB_SystemResult::Succeeded(u8"GB_EventDispatcher::ClearSubscriptions", u8"已清空全部事件订阅。");
}

bool GB_EventDispatcher::HasSubscription(const GB_EventSubscriptionToken& subscriptionToken) const
{
    if (!subscriptionToken.IsValid() || subscriptionToken.dispatcherId != dispatcherId)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(stateMutex);
    for (size_t index = 0; index < subscriptions.size(); index++)
    {
        if (subscriptions[index].subscriptionToken == subscriptionToken)
        {
            return true;
        }
    }

    return false;
}

size_t GB_EventDispatcher::GetSubscriptionCount() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return subscriptions.size();
}

size_t GB_EventDispatcher::GetSubscriptionCount(const std::string& eventName) const
{
    if (!IsValidEventName(eventName))
    {
        return 0;
    }

    size_t subscriptionCount = 0;
    std::lock_guard<std::mutex> lock(stateMutex);
    for (size_t index = 0; index < subscriptions.size(); index++)
    {
        if (!subscriptions[index].matchAll && subscriptions[index].eventName == eventName)
        {
            subscriptionCount++;
        }
    }

    return subscriptionCount;
}

GB_SystemResult GB_EventDispatcher::Dispatch(const GB_Event& event)
{
    const std::string operationName = u8"GB_EventDispatcher::Dispatch";
    const GB_SystemResult validateResult = ValidateEvent(event, operationName);
    if (validateResult.IsFailed())
    {
        return validateResult;
    }

    try
    {
        const GB_Event preparedEvent = PrepareEvent(event);
        return DispatchPreparedEvent(preparedEvent, true);
    }
    catch (const std::bad_alloc&)
    {
        return MakeAllocationFailedResult(operationName);
    }
    catch (...)
    {
        return MakeInternalFailedResult(operationName);
    }
}

GB_SystemResult GB_EventDispatcher::Dispatch(const std::string& eventName, const GB_Variant& payload, const std::string& sourceName)
{
    return Dispatch(MakeEvent(eventName, payload, sourceName));
}

GB_SystemResult GB_EventDispatcher::Post(const GB_Event& event)
{
    const std::string operationName = u8"GB_EventDispatcher::Post";
    const GB_SystemResult validateResult = ValidateEvent(event, operationName);
    if (validateResult.IsFailed())
    {
        return validateResult;
    }

    const GB_SystemResult startResult = Start();
    if (startResult.IsFailed())
    {
        return startResult;
    }

    GB_Event preparedEvent;
    try
    {
        preparedEvent = PrepareEvent(event);
    }
    catch (const std::bad_alloc&)
    {
        return MakeAllocationFailedResult(operationName);
    }
    catch (...)
    {
        return MakeInternalFailedResult(operationName);
    }

    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (!isAcceptingEvents)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, operationName, u8"事件分发器当前不接收新事件。");
        }

        if (options.maxQueueSize > 0 && eventQueue.size() >= options.maxQueueSize)
        {
            if (options.overflowPolicy == GB_EventQueueOverflowPolicy::DropNewest)
            {
                droppedEventCount++;
                return MakeQueueFullResult(operationName);
            }

            eventQueue.pop_front();
            droppedEventCount++;
        }

        try
        {
            eventQueue.push_back(std::move(preparedEvent));
            shouldNotify = true;
        }
        catch (const std::bad_alloc&)
        {
            droppedEventCount++;
            return MakeAllocationFailedResult(operationName);
        }
        catch (...)
        {
            droppedEventCount++;
            return MakeInternalFailedResult(operationName);
        }
    }

    if (shouldNotify)
    {
        eventQueueCond.notify_one();
    }

    return GB_SystemResult::Succeeded(operationName, u8"事件已进入异步分发队列。");
}

GB_SystemResult GB_EventDispatcher::Post(const std::string& eventName, const GB_Variant& payload, const std::string& sourceName)
{
    return Post(MakeEvent(eventName, payload, sourceName));
}

GB_SystemResult GB_EventDispatcher::Publish(const GB_Event& event)
{
    const GB_EventDispatcherOptions localOptions = GetOptions();
    if (localOptions.dispatchMode == GB_EventDispatchMode::Queued)
    {
        return Post(event);
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (!isAcceptingEvents)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, u8"GB_EventDispatcher::Publish", u8"事件分发器当前不接收新事件。");
        }
    }

    return Dispatch(event);
}

GB_SystemResult GB_EventDispatcher::Publish(const std::string& eventName, const GB_Variant& payload, const std::string& sourceName)
{
    return Publish(MakeEvent(eventName, payload, sourceName));
}

void GB_EventDispatcher::SetExceptionHandler(ExceptionHandler exceptionHandler)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    this->exceptionHandler = std::move(exceptionHandler);
}

size_t GB_EventDispatcher::GetPendingEventCount() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return eventQueue.size();
}

uint64_t GB_EventDispatcher::GetDispatchedEventCount() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return dispatchedEventCount;
}

uint64_t GB_EventDispatcher::GetDroppedEventCount() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return droppedEventCount;
}

uint64_t GB_EventDispatcher::GetCallbackExceptionCount() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return callbackExceptionCount;
}

void GB_EventDispatcher::ResetStatistics()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    dispatchedEventCount = 0;
    droppedEventCount = 0;
    callbackExceptionCount = 0;
}

bool GB_EventDispatcher::IsValidDispatchModeValue(const uint64_t dispatchModeValue)
{
    return dispatchModeValue == static_cast<uint64_t>(GB_EventDispatchMode::Direct) || dispatchModeValue == static_cast<uint64_t>(GB_EventDispatchMode::Queued);
}

bool GB_EventDispatcher::IsValidOverflowPolicyValue(const uint64_t overflowPolicyValue)
{
    return overflowPolicyValue == static_cast<uint64_t>(GB_EventQueueOverflowPolicy::DropNewest) || overflowPolicyValue == static_cast<uint64_t>(GB_EventQueueOverflowPolicy::DropOldest);
}

bool GB_EventDispatcher::IsValidStopModeValue(const uint64_t stopModeValue)
{
    return stopModeValue == static_cast<uint64_t>(GB_EventDispatcherStopMode::Drain) || stopModeValue == static_cast<uint64_t>(GB_EventDispatcherStopMode::Discard);
}

uint64_t GB_EventDispatcher::GetCurrentTimestampMilliseconds()
{
    const std::chrono::system_clock::time_point nowTime = std::chrono::system_clock::now();
    const std::chrono::milliseconds milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(nowTime.time_since_epoch());
    return milliseconds.count() > 0 ? static_cast<uint64_t>(milliseconds.count()) : 0;
}

GB_Event GB_EventDispatcher::MakeEvent(const std::string& eventName, const GB_Variant& payload, const std::string& sourceName)
{
    return GB_Event(eventName, payload, sourceName);
}

GB_Event GB_EventDispatcher::PrepareEvent(const GB_Event& event)
{
    GB_Event preparedEvent(event);
    if (preparedEvent.timestampMilliseconds == 0)
    {
        preparedEvent.timestampMilliseconds = GetCurrentTimestampMilliseconds();
    }

    if (preparedEvent.sequenceId == 0)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        preparedEvent.sequenceId = TakeNextId(nextEventSequenceId);
    }

    return preparedEvent;
}

GB_SystemResult GB_EventDispatcher::ValidateEvent(const GB_Event& event, const std::string& operationName) const
{
    if (!event.IsValid())
    {
        return MakeInvalidEventNameResult(ResolveOperationName(operationName, u8"GB_EventDispatcher::ValidateEvent"));
    }

    return GB_SystemResult::Succeeded(ResolveOperationName(operationName, u8"GB_EventDispatcher::ValidateEvent"));
}

GB_SystemResult GB_EventDispatcher::DispatchPreparedEvent(const GB_Event& event, const bool countActiveDispatch)
{
    const std::string operationName = u8"GB_EventDispatcher::DispatchPreparedEvent";
    std::vector<SubscriptionSnapshot> subscriptionSnapshots;

    try
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (countActiveDispatch)
        {
            activeDispatchCount++;
        }

        subscriptionSnapshots = CopySubscriptionsForEventLocked(event.eventName);
    }
    catch (const std::bad_alloc&)
    {
        if (countActiveDispatch)
        {
            FinishActiveDispatch();
        }

        return MakeAllocationFailedResult(operationName);
    }
    catch (...)
    {
        if (countActiveDispatch)
        {
            FinishActiveDispatch();
        }

        return MakeInternalFailedResult(operationName);
    }

    const DispatchThreadScope dispatchThreadScope(this);
    for (size_t index = 0; index < subscriptionSnapshots.size(); index++)
    {
        const SubscriptionSnapshot& subscriptionSnapshot = subscriptionSnapshots[index];
        if (!subscriptionSnapshot.callback)
        {
            continue;
        }

        try
        {
            subscriptionSnapshot.callback(event);
        }
        catch (...)
        {
            ExceptionHandler localExceptionHandler;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                callbackExceptionCount++;
                localExceptionHandler = exceptionHandler;
            }

            if (localExceptionHandler)
            {
                try
                {
                    localExceptionHandler(event, subscriptionSnapshot.subscriptionToken, std::current_exception());
                }
                catch (...)
                {
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        dispatchedEventCount++;
    }

    if (countActiveDispatch)
    {
        FinishActiveDispatch();
    }

    return GB_SystemResult::Succeeded(operationName, u8"事件分发完成。");
}

void GB_EventDispatcher::WorkerLoop()
{
    for (;;)
    {
        GB_Event event;
        {
            std::unique_lock<std::mutex> lock(stateMutex);
            eventQueueCond.wait(lock, [this]()
                {
                    return isStopping || !eventQueue.empty();
                });

            if (eventQueue.empty())
            {
                if (isStopping)
                {
                    break;
                }

                continue;
            }

            event = std::move(eventQueue.front());
            eventQueue.pop_front();
            activeDispatchCount++;
        }

        DispatchPreparedEvent(event, false);
        FinishActiveDispatch();
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        isWorkerStarted = false;
        isStopping = false;
    }

    idleCond.notify_all();
}

void GB_EventDispatcher::FinishActiveDispatch()
{
    bool shouldNotifyIdle = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (activeDispatchCount > 0)
        {
            activeDispatchCount--;
        }

        shouldNotifyIdle = IsIdleLocked();
    }

    if (shouldNotifyIdle)
    {
        idleCond.notify_all();
    }
}

std::vector<GB_EventDispatcher::SubscriptionSnapshot> GB_EventDispatcher::CopySubscriptionsForEventLocked(const std::string& eventName) const
{
    std::vector<SubscriptionSnapshot> subscriptionSnapshots;
    subscriptionSnapshots.reserve(subscriptions.size());

    for (size_t index = 0; index < subscriptions.size(); index++)
    {
        const SubscriptionEntry& subscriptionEntry = subscriptions[index];
        if (!subscriptionEntry.matchAll && subscriptionEntry.eventName != eventName)
        {
            continue;
        }

        SubscriptionSnapshot subscriptionSnapshot;
        subscriptionSnapshot.subscriptionToken = subscriptionEntry.subscriptionToken;
        subscriptionSnapshot.callback = subscriptionEntry.callback;
        subscriptionSnapshots.push_back(subscriptionSnapshot);
    }

    return subscriptionSnapshots;
}

bool GB_EventDispatcher::IsIdleLocked() const
{
    return eventQueue.empty() && activeDispatchCount == 0;
}
