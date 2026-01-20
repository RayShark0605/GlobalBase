#include "GB_ThreadPool.h"

#include <exception>
#include <stdexcept>

/*
    实现要点：
      - 所有共享状态都受 queueMutex 保护：taskQueue / isAccepting / isStopping / activeTaskCount
      - 条件变量一律使用 predicate 版本 wait()/wait_until()，以正确处理"伪唤醒"（spurious wakeup）。
      - WorkerLoop 只在 isStopping && taskQueue.empty() 时退出：
          * Drain：先跑完队列再退
          * Discard：Shutdown 时清队列，然后自然退出
      - RunTaskAndFinalize 统一做：
          * 执行任务
          * 捕获无人接收异常（Post）
          * activeTaskCount 退账
          * 必要时唤醒 idleCond
*/

/*
    构造线程池：
      - 创建 threadCount 个 worker 线程，统一跑 WorkerLoop()
      - threadCount==0 视为非法参数

    异常安全：
      如果在创建线程的过程中抛异常，构造函数会被中断，
      且析构函数不会被调用。已创建的 std::thread 若仍 joinable，
      在其析构时会触发 std::terminate。
      因此这里 catch(...) 后必须主动 Shutdown + Join 再 rethrow。
*/
GB_ThreadPool::GB_ThreadPool(size_t threadCount, size_t maxQueueSize) : maxQueueSize(maxQueueSize), isAccepting(true),
isStopping(false), activeTaskCount(0), unhandledExceptionHandler(nullptr)
{
    if (threadCount == 0)
    {
        throw std::invalid_argument("threadCount must be > 0");
    }

    workers.reserve(threadCount);
    try
    {
        for (size_t i = 0; i < threadCount; i++)
        {
            workers.emplace_back(&GB_ThreadPool::WorkerLoop, this);
        }
    }
    catch (...)
    {
        // 重要：构造函数抛异常时，析构函数不会被调用。
        // 已创建的 std::thread 若在析构时仍 joinable，会触发 std::terminate。
        // 因此这里必须主动停止并 join。
        Shutdown(ShutdownMode::Discard);
        Join();
        throw;
    }
}

/*
    析构：默认 Drain + Join。

    注意：请不要在本线程池的 worker 线程中析构 ThreadPool。
    因为 Join() 不能 join 自己，否则会抛 std::system_error，
    在析构场景下通常会导致 std::terminate。
*/
GB_ThreadPool::~GB_ThreadPool()
{
    Shutdown(ShutdownMode::Drain);
    Join();
}

size_t GB_ThreadPool::GetThreadCount() const
{
    return workers.size();
}

size_t GB_ThreadPool::GetMaxQueueSize() const
{
    return maxQueueSize;
}

size_t GB_ThreadPool::GetPendingTaskCount() const
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return taskQueue.size();
}

size_t GB_ThreadPool::GetActiveTaskCount() const
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return activeTaskCount;
}

bool GB_ThreadPool::IsShutdown() const
{
    std::lock_guard<std::mutex> lock(queueMutex);
    return isStopping;
}

void GB_ThreadPool::SetUnhandledExceptionHandler(UnhandledExceptionHandler handler)
{
    // handler 指针本身不依赖其它共享状态，但这里用 release/acquire 让可见性更明确。
    unhandledExceptionHandler.store(handler, std::memory_order_release);
}

/*
    发起停止请求：
      - isAccepting=false：禁止新任务进入
      - isStopping=true ：通知 worker 可以退出
      - Discard 模式下会清空未执行任务

    之后 notify_all()：
      - 唤醒等待 notEmpty 的 worker（让它们检查 isStopping）
      - 唤醒等待 notFull 的提交者（让它们及时失败/抛异常）
      - 唤醒等待 idleCond 的 WaitIdle
*/
void GB_ThreadPool::Shutdown(ShutdownMode mode)
{
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (isStopping)
        {
            return;
        }

        isAccepting = false;
        isStopping = true;

        if (mode == ShutdownMode::Discard)
        {
            taskQueue.clear();
        }
    }

    notEmptyCond.notify_all();
    notFullCond.notify_all();
    idleCond.notify_all();
}

void GB_ThreadPool::WaitIdle()
{
    std::unique_lock<std::mutex> lock(queueMutex);
    idleCond.wait(lock, [&](){
        return taskQueue.empty() && activeTaskCount == 0;
    });
}

bool GB_ThreadPool::WaitIdleUntil(const std::chrono::steady_clock::time_point& deadline)
{
    std::unique_lock<std::mutex> lock(queueMutex);
    return idleCond.wait_until(lock, deadline, [&]() {
        return taskQueue.empty() && activeTaskCount == 0;
    });
}

/*
    阻塞提交一个 MoveOnlyTask。

    关键点：
      - 若 isAccepting==false，说明已 Shutdown，直接抛异常。
      - 有界队列满时：
          * 如果当前线程是本池 worker：使用 caller-runs 内联执行，避免死锁。
          * 否则等待 notFullCond，直到队列可用或池停止接收。
      - 成功入队后 notify_one(notEmptyCond) 唤醒一个 worker。
*/
void GB_ThreadPool::EnqueueTaskBlocking(MoveOnlyTask&& task)
{
    {
        std::unique_lock<std::mutex> lock(queueMutex);

        if (!isAccepting)
        {
            throw std::runtime_error("Enqueue on stopped GB_ThreadPool");
        }

        if (maxQueueSize > 0)
        {
            // 重要：若任务在 worker 线程内递归提交，并且队列是有界的，阻塞等待 notFull 可能导致死锁。
            // 这里采用“caller-runs”策略：当发现当前线程就是本线程池 worker 且队列已满时，直接在当前线程执行任务。
            if (GetTlsWorkerOwner() == this && taskQueue.size() >= maxQueueSize)
            {
                // 关键：caller-runs 也必须纳入 activeTaskCount 记账，否则 WaitIdle() 可能提前返回。
                activeTaskCount++;
                lock.unlock();
                RunTaskAndFinalize(std::move(task));
                return;
            }

            notFullCond.wait(lock, [&]() {
                return !isAccepting || taskQueue.size() < maxQueueSize;
            });

            if (!isAccepting)
            {
                throw std::runtime_error("Enqueue on stopped GB_ThreadPool");
            }
        }

        taskQueue.emplace_back(std::move(task));
    }

    notEmptyCond.notify_one();
}

bool GB_ThreadPool::EnqueueTaskNonBlocking(MoveOnlyTask&& task)
{
    {
        std::lock_guard<std::mutex> lock(queueMutex);

        if (!isAccepting)
        {
            return false;
        }

        if (maxQueueSize > 0 && taskQueue.size() >= maxQueueSize)
        {
            return false;
        }

        taskQueue.emplace_back(std::move(task));
    }

    notEmptyCond.notify_one();
    return true;
}

bool GB_ThreadPool::EnqueueTaskUntil(const std::chrono::steady_clock::time_point& deadline, MoveOnlyTask&& task)
{
    {
        std::unique_lock<std::mutex> lock(queueMutex);

        if (!isAccepting)
        {
            return false;
        }

        if (maxQueueSize > 0)
        {
            if (GetTlsWorkerOwner() == this && taskQueue.size() >= maxQueueSize)
            {
                activeTaskCount++;
                lock.unlock();
                RunTaskAndFinalize(std::move(task));
                return true;
            }

            const bool ok = notFullCond.wait_until(lock, deadline, [&]() {
                return !isAccepting || taskQueue.size() < maxQueueSize;
            });

            if (!ok || !isAccepting)
            {
                return false;
            }
        }

        taskQueue.emplace_back(std::move(task));
    }

    notEmptyCond.notify_one();
    return true;
}

/*
    统一的"执行 + 退账 + 唤醒"逻辑。

    为什么要集中在这里？
      - worker 从队列取任务后会调用这里
      - caller-runs 内联执行任务也会调用这里
    这样才能确保 activeTaskCount 和 idleCond 的语义一致，否则 WaitIdle() 会出错。

    异常处理：
      - 对 packaged_task：异常会被写入 future（shared state），通常不会被这里 catch 到。
      - 对 Post 任务：若抛异常，这里捕获并交给 UnhandledExceptionHandler；
        没有 handler 则 std::terminate。
*/
void GB_ThreadPool::RunTaskAndFinalize(MoveOnlyTask&& task)
{
    std::exception_ptr unhandledException;
    try
    {
        // Enqueue 返回 future 的任务通常是 packaged_task：它会把异常存入 shared state，而不是直接抛出。
        // Post 类型任务（直接入队 binder）若抛异常，这里会捕获并交由回调处理。
        task();
    }
    catch (...)
    {
        unhandledException = std::current_exception();
    }

    bool shouldNotifyIdle = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex);

        activeTaskCount--;
        if (taskQueue.empty() && activeTaskCount == 0)
        {
            shouldNotifyIdle = true;
        }
    }

    if (shouldNotifyIdle)
    {
        idleCond.notify_all();
    }

    if (unhandledException)
    {
        UnhandledExceptionHandler handler = unhandledExceptionHandler.load(std::memory_order_acquire);
        if (handler)
        {
            try
            {
                handler(unhandledException);
            }
            catch (...)
            {
                std::terminate();
            }
        }
        else
        {
            std::terminate();
        }
    }
}

/*
    worker 主循环：
      1) 等待 notEmptyCond（队列非空）或 isStopping
      2) 若 isStopping && 队列为空：退出线程
      3) 从队列取出一个任务，activeTaskCount++
      4) 若有界队列：notify_one(notFullCond) 唤醒可能阻塞的提交者
      5) 执行任务并在 RunTaskAndFinalize 里 activeTaskCount--

    tlsWorkerOwner：线程局部指针，用于判断"当前线程是否本池 worker"。
    这主要服务于 caller-runs 策略。
*/
void GB_ThreadPool::WorkerLoop()
{
    GetTlsWorkerOwner() = this;

    for (;;)
    {
        MoveOnlyTask task;

        {
            std::unique_lock<std::mutex> lock(queueMutex);

            notEmptyCond.wait(lock, [&]() {
                return isStopping || !taskQueue.empty();
            });

            if (isStopping && taskQueue.empty())
            {
                break;
            }

            task = std::move(taskQueue.front());
            taskQueue.pop_front();

            activeTaskCount++;
        }

        if (maxQueueSize > 0)
        {
            notFullCond.notify_one();
        }

        RunTaskAndFinalize(std::move(task));
    }

    GetTlsWorkerOwner() = nullptr;
}

/*
    Join 所有 worker 线程。

    注意：
      - Join() 必须在 Shutdown() 之后调用，否则 worker 可能一直阻塞在 notEmptyCond。
      - 不能在 worker 自己的线程上下文里 join 自己。
       （std::thread::join 在这种情况下会报 "resource deadlock avoided"。）
*/
void GB_ThreadPool::Join()
{
    for (size_t i = 0; i < workers.size(); i++)
    {
        std::thread& worker = workers[i];
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

GB_ThreadPool*& GB_ThreadPool::GetTlsWorkerOwner()
{
    thread_local GB_ThreadPool* tlsWorkerOwner = nullptr;
    return tlsWorkerOwner;
}
