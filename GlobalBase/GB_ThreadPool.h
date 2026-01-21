#ifndef GLOBALBASE_THREAD_POOL_H
#define GLOBALBASE_THREAD_POOL_H

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include "GlobalBasePort.h"

// "把 (function, tuple<args...>) 展开调用"的工具
namespace threadpool_detail
{
    // 生成 0..N-1 的索引序列
    template <size_t... Indices>
    struct IndexSequence
    {
    };

    // 生成 0..N-1 的索引序列
    template <size_t N, size_t... Indices>
    struct MakeIndexSequenceImpl : MakeIndexSequenceImpl<N - 1, N - 1, Indices...>
    {
    };

    template <size_t... Indices>
    struct MakeIndexSequenceImpl<0, Indices...>
    {
        using Type = IndexSequence<Indices...>;
    };

    template <size_t N>
    using MakeIndexSequence = typename MakeIndexSequenceImpl<N>::Type;

    // 针对 R 返回值的调用封装
    template <typename R>
    struct CallHelper
    {
        template <typename Func, typename Tuple, size_t... Indices>
        static R Call(Func& function, Tuple& argsTuple, IndexSequence<Indices...>)
        {
            return function(std::get<Indices>(argsTuple)...);
        }
    };

    // 针对 void 返回值的调用封装
    template <>
    struct CallHelper<void>
    {
        template <typename Func, typename Tuple, size_t... Indices>
        static void Call(Func& function, Tuple& argsTuple, IndexSequence<Indices...>)
        {
            function(std::get<Indices>(argsTuple)...);
        }
    };

    // 把函数和参数打包成一个可调用对象 operator()()
    template <typename R, typename F, typename... Args>
    class TaskBinder
    {
    public:
        explicit TaskBinder(F&& f, Args&&... args) : function(std::forward<F>(f)), argsTuple(std::forward<Args>(args)...)
        {
        }

        TaskBinder(TaskBinder&& other) noexcept : function(std::move(other.function)), argsTuple(std::move(other.argsTuple))
        {
        }

        TaskBinder& operator=(TaskBinder&& other) noexcept
        {
            if (this == &other)
            {
                return *this;
            }

            function = std::move(other.function);
            argsTuple = std::move(other.argsTuple);
            return *this;
        }

        TaskBinder(const TaskBinder&) = delete;
        TaskBinder& operator=(const TaskBinder&) = delete;

        R operator()()
        {
            return CallHelper<R>::Call(function, argsTuple, MakeIndexSequence<sizeof...(Args)>());
        }

    private:
        typename std::decay<F>::type function;
        std::tuple<typename std::decay<Args>::type...> argsTuple;
    };
}

#pragma warning(push)
#pragma warning(disable : 4251)

/*
    ThreadPool：线程池主体

    核心状态：
      - isAccepting：是否还接收新任务（Shutdown 后置 false）
      - isStopping ：是否已经发起停止请求（Shutdown 后置 true）
      - taskQueue  ：待执行任务队列
      - activeTaskCount：正在执行任务的数量（用于 WaitIdle 判断"真正空闲"）
*/
class GLOBALBASE_PORT GB_ThreadPool
{
public:
    enum class ShutdownMode
    {
        Drain,   // 不再接收新任务，把队列跑完后退出
        Discard  // 不再接收新任务，丢弃队列中未执行任务后退出
    };

    explicit GB_ThreadPool(size_t threadCount, size_t maxQueueSize = 0);
    ~GB_ThreadPool();

    GB_ThreadPool(const GB_ThreadPool&) = delete;
    GB_ThreadPool& operator=(const GB_ThreadPool&) = delete;
    GB_ThreadPool(GB_ThreadPool&&) = delete;
    GB_ThreadPool& operator=(GB_ThreadPool&&) = delete;

    size_t GetThreadCount() const;
    size_t GetMaxQueueSize() const;

    size_t GetPendingTaskCount() const;
    size_t GetActiveTaskCount() const;

    // 是否已经发起停止请求，并不代表线程池已经空闲或所有 worker 已经退出。
    // 若需要等待所有任务完成，请使用 WaitIdle()。
    bool IsShutdown() const;

    // Post 类型任务若抛出异常（没有 future 承接），默认会触发 std::terminate。
    using UnhandledExceptionHandler = void(*)(std::exception_ptr);
    void SetUnhandledExceptionHandler(UnhandledExceptionHandler handler);

    void Shutdown(ShutdownMode mode = ShutdownMode::Drain);

    void WaitIdle();

    // 带超时的 WaitIdle
    template <typename Rep, typename Period>
    bool WaitIdleFor(const std::chrono::duration<Rep, Period>& timeout);

    // 提交一个“有返回值/可捕获异常”的任务
    template <class F, class... Args>
    auto Enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>;

    // 非阻塞提交
    template <class F, class... Args>
    auto TryEnqueue(F&& f, Args&&... args) -> std::pair<bool, std::future<typename std::result_of<F(Args...)>::type>>;

    // 带超时的提交
    template <class Rep, class Period, class F, class... Args>
    auto EnqueueFor(const std::chrono::duration<Rep, Period>& timeout, F&& f, Args&&... args) -> std::pair<bool, std::future<typename std::result_of<F(Args...)>::type>>;

    // Post：不关心返回值的提交（不创建 future/shared state），阻塞；停止接收则抛异常
    template <class F, class... Args>
    void Post(F&& f, Args&&... args);

    // TryPost：非阻塞 fire-and-forget
    template <class F, class... Args>
    bool TryPost(F&& f, Args&&... args);

    // PostFor：带超时的 Post
    template <class Rep, class Period, class F, class... Args>
    bool PostFor(const std::chrono::duration<Rep, Period>& timeout, F&& f, Args&&... args);

private:
    // 一个"只可移动"的 type-erasure 任务包装器，类似于不可拷贝，只能 move 的 std::function<void()>
    class MoveOnlyTask
    {
    private:
        struct ITask
        {
            virtual ~ITask() {}
            virtual void Run() = 0;
        };

        template <typename Callable>
        struct TaskModel : ITask
        {
            explicit TaskModel(Callable&& callable) : callable(std::move(callable))
            {
            }

            void Run() override
            {
                callable();
            }

            Callable callable;
        };

    public:
        MoveOnlyTask()
        {
        }

        template <typename Callable>
        explicit MoveOnlyTask(Callable&& callable) : taskImpl(new TaskModel<typename std::decay<Callable>::type>(std::forward<Callable>(callable)))
        {
        }

        MoveOnlyTask(MoveOnlyTask&& other) noexcept : taskImpl(std::move(other.taskImpl))
        {
        }

        MoveOnlyTask& operator=(MoveOnlyTask&& other) noexcept
        {
            if (this == &other)
            {
                return *this;
            }

            taskImpl = std::move(other.taskImpl);
            return *this;
        }

        MoveOnlyTask(const MoveOnlyTask&) = delete;
        MoveOnlyTask& operator=(const MoveOnlyTask&) = delete;

        void operator()()
        {
            if (taskImpl)
            {
                taskImpl->Run();
            }
        }

        explicit operator bool() const
        {
            return static_cast<bool>(taskImpl);
        }

    private:
        std::unique_ptr<ITask> taskImpl;
    };

private:
    void EnqueueTaskBlocking(MoveOnlyTask&& task);
    bool EnqueueTaskNonBlocking(MoveOnlyTask&& task);
    bool EnqueueTaskUntil(const std::chrono::steady_clock::time_point& deadline, MoveOnlyTask&& task);

    bool WaitIdleUntil(const std::chrono::steady_clock::time_point& deadline);

    // 统一的任务执行处理逻辑。
    // 说明：
    // - worker 线程从队列取出任务后执行，会运行这里。
    // - “caller-runs”策略下（worker 线程内递归提交且队列满）直接内联执行的任务，也必须运行这里，
    //   否则 WaitIdle() 可能错误地提前返回。
    void RunTaskAndFinalize(MoveOnlyTask&& task);

    void WorkerLoop();
    void Join();

private:
    std::vector<std::thread> workers;

    mutable std::mutex queueMutex;
    std::condition_variable notEmptyCond;
    std::condition_variable notFullCond;
    std::condition_variable idleCond;

    std::deque<MoveOnlyTask> taskQueue;

    const size_t maxQueueSize; // 0 = 无界
    bool isAccepting;
    bool isStopping;

    size_t activeTaskCount;

    std::atomic<UnhandledExceptionHandler> unhandledExceptionHandler;

    static GB_ThreadPool*& GetTlsWorkerOwner();
};

template <typename Rep, typename Period>
bool GB_ThreadPool::WaitIdleFor(const std::chrono::duration<Rep, Period>& timeout)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    return WaitIdleUntil(deadline);
}

template <class F, class... Args>
auto GB_ThreadPool::Enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>
{
#if __cplusplus >= 201703L
    using ReturnType = typename std::invoke_result<F, Args...>::type;
#else
    using ReturnType = typename std::result_of<F(Args...)>::type;
#endif

    threadpool_detail::TaskBinder<ReturnType, F, Args...> binder(std::forward<F>(f), std::forward<Args>(args)...);
    std::packaged_task<ReturnType()> packagedTask(std::move(binder));
    std::future<ReturnType> future = packagedTask.get_future();

    EnqueueTaskBlocking(MoveOnlyTask(std::move(packagedTask)));
    return future;
}

template <class F, class... Args>
auto GB_ThreadPool::TryEnqueue(F&& f, Args&&... args) -> std::pair<bool, std::future<typename std::result_of<F(Args...)>::type>>
{
#if __cplusplus >= 201703L
    using ReturnType = typename std::invoke_result<F, Args...>::type;
#else
    using ReturnType = typename std::result_of<F(Args...)>::type;
#endif

    threadpool_detail::TaskBinder<ReturnType, F, Args...> binder(std::forward<F>(f), std::forward<Args>(args)...);
    std::packaged_task<ReturnType()> packagedTask(std::move(binder));
    std::future<ReturnType> future = packagedTask.get_future();

    const bool ok = EnqueueTaskNonBlocking(MoveOnlyTask(std::move(packagedTask)));
    if (!ok)
    {
        return std::make_pair(false, std::future<ReturnType>());
    }

    return std::make_pair(true, std::move(future));
}

template <class Rep, class Period, class F, class... Args>
auto GB_ThreadPool::EnqueueFor(const std::chrono::duration<Rep, Period>& timeout, F&& f, Args&&... args) -> std::pair<bool, std::future<typename std::result_of<F(Args...)>::type>>
{
#if __cplusplus >= 201703L
    using ReturnType = typename std::invoke_result<F, Args...>::type;
#else
    using ReturnType = typename std::result_of<F(Args...)>::type;
#endif

    threadpool_detail::TaskBinder<ReturnType, F, Args...> binder(std::forward<F>(f), std::forward<Args>(args)...);
    std::packaged_task<ReturnType()> packagedTask(std::move(binder));
    std::future<ReturnType> future = packagedTask.get_future();

    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    const bool ok = EnqueueTaskUntil(deadline, MoveOnlyTask(std::move(packagedTask)));
    if (!ok)
    {
        return std::make_pair(false, std::future<ReturnType>());
    }

    return std::make_pair(true, std::move(future));
}

template <class F, class... Args>
void GB_ThreadPool::Post(F&& f, Args&&... args)
{
    threadpool_detail::TaskBinder<void, F, Args...> binder(std::forward<F>(f), std::forward<Args>(args)...);
    EnqueueTaskBlocking(MoveOnlyTask(std::move(binder)));
}

template <class F, class... Args>
bool GB_ThreadPool::TryPost(F&& f, Args&&... args)
{
    threadpool_detail::TaskBinder<void, F, Args...> binder(std::forward<F>(f), std::forward<Args>(args)...);
    return EnqueueTaskNonBlocking(MoveOnlyTask(std::move(binder)));
}

template <class Rep, class Period, class F, class... Args>
bool GB_ThreadPool::PostFor(const std::chrono::duration<Rep, Period>& timeout, F&& f, Args&&... args)
{
    threadpool_detail::TaskBinder<void, F, Args...> binder(std::forward<F>(f), std::forward<Args>(args)...);
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    return EnqueueTaskUntil(deadline, MoveOnlyTask(std::move(binder)));
}

#pragma warning(pop)

#endif


// Demo 1：Enqueue 返回 future，拿结果 + 等全部完成
/*
static int ComputeSomething(int inputValue)
{
    if (inputValue < 0)
    {
        throw std::runtime_error("inputValue must be >= 0");
    }
    return inputValue * inputValue;
}
int main()
{
    GB_ThreadPool threadPool(4, 0); // 4 个 worker；0 表示无界队列

    std::vector<std::future<int>> futures;
    futures.reserve(10);
    for (int i = 0; i < 10; i++)
    {
        futures.emplace_back(threadPool.Enqueue(ComputeSomething, i));
    }

    long long totalSum = 0;
    for (std::future<int>& future : futures)
    {
        totalSum += future.get(); // 这里会等任务完成，并取回返回值/异常
    }

    std::cout << "totalSum = " << totalSum << std::endl;

    // 可选：显式等到“线程池完全空闲”
    threadPool.WaitIdle();

    return 0; // 析构会默认 Drain + Join
}

*/

// Demo 2：Post + 异常捕获
/*
static void OnUnhandledTaskException(std::exception_ptr exceptionPtr)
{
    try
    {
        if (exceptionPtr)
        {
            std::rethrow_exception(exceptionPtr);
        }
    }
    catch (const std::exception& exception)
    {
        std::cout << "[GB_ThreadPool] Unhandled exception: " << exception.what() << std::endl;
    }
    catch (...)
    {
        std::cout << "[GB_ThreadPool] Unhandled unknown exception." << std::endl;
    }
}
static void DoWork(int taskId)
{
    std::cout << "Task " << taskId << " running..." << std::endl;
    if (taskId == 3)
    {
        throw std::runtime_error("boom");
    }
}

int main()
{
    GB_ThreadPool threadPool(4, 0);
    threadPool.SetUnhandledExceptionHandler(&OnUnhandledTaskException);

    for (int i = 0; i < 8; i++)
    {
        threadPool.Post(DoWork, i);
    }

    threadPool.WaitIdle(); // 等全部任务执行结束（队列空且 active==0）

    return 0;
}

*/

// Demo 3：有界队列 + TryEnqueue/TryPost
/*
static int HeavyJob(int inputValue)
{
    // 重计算...
    return inputValue + 100;
}

int main()
{
    GB_ThreadPool threadPool(4, 64); // 队列最多 64 个等待任务

    std::vector<std::future<int>> futures;
    futures.reserve(1000);

    for (int i = 0; i < 1000; i++)
    {
        const auto result = threadPool.TryEnqueue(HeavyJob, i);
        const bool ok = result.first;
        if (ok)
        {
            futures.emplace_back(std::move(result.second));
        }
        else
        {
            // 失败处理示例：
            // 1) 丢弃
            // 2) 或者在当前线程降级同步执行
            // const int fallback = HeavyJob(i);
        }
    }

    long long sum = 0;
    for (auto& future : futures)
    {
        sum += future.get();
    }

    std::cout << "sum = " << sum << std::endl;
    return 0;
}
*/

// Demo 4：EnqueueFor/PostFor（带超时提交）+ WaitIdleFor（带超时等待空闲）
/*
static void ShortJob(int jobId)
{
    (void)jobId;
}

int main()
{
    using namespace std::chrono;

    GB_ThreadPool threadPool(4, 32);

    // 最多等 2ms 入队
    const auto enqueueResult = threadPool.EnqueueFor(milliseconds(2), []()
    {
        return 42;
    });

    if (enqueueResult.first)
    {
        const int value = enqueueResult.second.get();
        std::cout << "value = " << value << std::endl;
    }
    else
    {
        std::cout << "enqueue timeout or pool not accepting." << std::endl;
    }

    // fire-and-forget 版本
    const bool postOk = threadPool.PostFor(milliseconds(2), ShortJob, 7);
    std::cout << "postOk = " << (postOk ? "true" : "false") << std::endl;

    // 最多等 100ms 变空闲
    const bool idleOk = threadPool.WaitIdleFor(milliseconds(100));
    std::cout << "idleOk = " << (idleOk ? "true" : "false") << std::endl;

    return 0;
}
*/

// Demo 5：显式 Shutdown
/*
static void Job()
{
    // ...
}

int main()
{
    GB_ThreadPool threadPool(4, 0);

    for (int i = 0; i < 100; i++)
    {
        threadPool.Post(Job);
    }

    // 方案 A：优雅停机
    threadPool.Shutdown(GB_ThreadPool::ShutdownMode::Drain);
    threadPool.WaitIdle(); // 可选：等任务全跑完
    // 析构时也会 Join

    // 方案 B：快速停机（丢弃队列中未执行任务）
    // threadPool.Shutdown(GB_ThreadPool::ShutdownMode::Discard);

    return 0;
}
*/
