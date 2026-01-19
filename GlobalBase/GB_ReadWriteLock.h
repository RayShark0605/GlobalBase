#ifndef GLOBALBASE_READ_WRITE_LOCK_H_H
#define GLOBALBASE_READ_WRITE_LOCK_H_H

#include "GlobalBasePort.h"
#include <condition_variable>
#include <chrono>
#include <mutex>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/*
	GB_ReadWriteLock（C++11 读写锁 / Reader-Writer Lock）

	目标：
	- 提供“多读并发、写独占”的同步原语，适用于读多写少的共享数据保护。
	- 仅依赖 C++11：std::mutex + std::condition_variable。

	策略：
	- 本实现采用“写优先（writer-preference）”策略：
		只要存在等待写者（waitingWriters_ > 0），新来的读者不会被放行。
	  这样可以避免“写者饥饿”（读者源源不断导致写者一直抢不到锁）。
	  代价是：在持续写压力下，读者可能出现饥饿（读优先/公平实现会更复杂）。

	限制：
	- 非递归：同一线程重复持有同一把锁（尤其写锁）再去加锁可能死锁。
	- 不支持“读锁原子升级为写锁”：如需升级必须释放读锁再抢写锁，并在抢到写锁后重检条件。
*/
class GLOBALBASE_PORT GB_ReadWriteLock
{
public:
	GB_ReadWriteLock() = default;
	~GB_ReadWriteLock() = default;
	GB_ReadWriteLock(const GB_ReadWriteLock&) = delete;
	GB_ReadWriteLock& operator=(const GB_ReadWriteLock&) = delete;

	// 读锁（共享锁）
	void LockShared();
	void UnlockShared();

	// 写锁（独占锁）
	void Lock();
	void Unlock();

	// 非阻塞尝试。立即返回，成功则持锁，失败则不持锁。
	bool TryLockShared();
	bool TryLock();

	// 带超时尝试。在 timeout 时间内尝试获取锁，超时返回 false。
	template <class Rep, class Period>
	bool TryLockSharedFor(const std::chrono::duration<Rep, Period>& timeout);

	// 带超时尝试。在 timeout 时间内尝试获取锁，超时返回 false。
	template <class Rep, class Period>
	bool TryLockFor(const std::chrono::duration<Rep, Period>& timeout);

private:
	// 写优先策略：
	std::mutex mutex_;
	std::condition_variable readersCondition_;
	std::condition_variable writersCondition_;

	int activeReaders_ = 0;
	int waitingWriters_ = 0;
	bool writerActive_ = false;
};

/*
	“标签类型（Tag Type）”用于区分 Guard 构造策略（编译期分派）。

	设计动机：
	- Guard 构造函数只有一个参数（GB_ReadWriteLock&），无法表达“延迟加锁 / try 加锁”等额外意图。
	- 通过不同的 tag 类型参与重载，调用者可以在构造时明确选择策略：
		1) 立即加锁（默认构造）
		2) 延迟加锁（DeferLock）
		3) 尝试加锁不阻塞（TryToLock）
*/
struct GLOBALBASE_PORT GB_DeferLockTag {};
struct GLOBALBASE_PORT GB_TryToLockTag {};

// 用法示例：GB_ReadLockGuard guard(rwLock, GB_DeferLock);
static const GB_DeferLockTag GB_DeferLock = GB_DeferLockTag();
static const GB_TryToLockTag GB_TryToLock = GB_TryToLockTag();

/*
	GB_ReadLockGuard：读锁（共享锁）RAII 封装
	- 持有期间保持“读锁”，析构时自动释放。
	- 可移动不可拷贝：便于在函数返回、容器移动等场景转移锁的所有权。
*/
class GLOBALBASE_PORT GB_ReadLockGuard
{
public:
	// 立即加锁（阻塞）
	explicit GB_ReadLockGuard(GB_ReadWriteLock& lock);

	// 延迟加锁（当前暂不加锁），之后必须调用 Lock() 或 TryLock() 来获得锁
	GB_ReadLockGuard(GB_ReadWriteLock& lock, GB_DeferLockTag);

	// 尝试加锁（不阻塞），成功则持锁，失败则不持锁。用 OwnsLock() 判断是否成功持锁
	GB_ReadLockGuard(GB_ReadWriteLock& lock, GB_TryToLockTag);

	~GB_ReadLockGuard();

	GB_ReadLockGuard(const GB_ReadLockGuard&) = delete;
	GB_ReadLockGuard& operator=(const GB_ReadLockGuard&) = delete;

	GB_ReadLockGuard(GB_ReadLockGuard&& other) noexcept;
	GB_ReadLockGuard& operator=(GB_ReadLockGuard&& other) noexcept;

	// 显式加锁（阻塞）；只有在 defer/try 构造且当前未持锁时才有意义
	void Lock();

	// 显式 try 加锁（不阻塞），返回是否持锁成功
	bool TryLock();

	// 显式解锁；只有 ownsLock_ == true 时才会真正 UnlockShared()
	void Unlock();

	// 查询当前是否持有读锁
	bool OwnsLock() const;

private:
	void ResetNoUnlock();

private:
	GB_ReadWriteLock* lock_ = nullptr;
	bool ownsLock_ = false;
};

/*
	GB_WriteLockGuard：写锁（独占锁）RAII 封装
	- 持有期间保持“写锁”，析构时自动释放。
*/
class GLOBALBASE_PORT GB_WriteLockGuard
{
public:
	// 立即加锁（阻塞）
	explicit GB_WriteLockGuard(GB_ReadWriteLock& lock);

	// 延迟加锁（当前暂不加锁），之后必须调用 Lock() 或 TryLock() 来获得锁
	GB_WriteLockGuard(GB_ReadWriteLock& lock, GB_DeferLockTag);

	// 尝试加锁（不阻塞），成功则持锁，失败则不持锁。用 OwnsLock() 判断是否成功持锁
	GB_WriteLockGuard(GB_ReadWriteLock& lock, GB_TryToLockTag);

	~GB_WriteLockGuard();

	GB_WriteLockGuard(const GB_WriteLockGuard&) = delete;
	GB_WriteLockGuard& operator=(const GB_WriteLockGuard&) = delete;

	GB_WriteLockGuard(GB_WriteLockGuard&& other) noexcept;
	GB_WriteLockGuard& operator=(GB_WriteLockGuard&& other) noexcept;

	// 显式加锁（阻塞）；只有在 defer/try 构造且当前未持锁时才有意义
	void Lock();

	// 显式 try 加锁（不阻塞），返回是否持锁成功
	bool TryLock();

	// 显式解锁；只有 ownsLock_ == true 时才会真正 Unlock()
	void Unlock();

	// 查询当前是否持有写锁
	bool OwnsLock() const;

private:
	void ResetNoUnlock();

private:
	GB_ReadWriteLock* lock_ = nullptr;
	bool ownsLock_ = false;
};

template <class Rep, class Period>
bool GB_ReadWriteLock::TryLockSharedFor(const std::chrono::duration<Rep, Period>& timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	std::unique_lock<std::mutex> lockGuard(mutex_);

	while (writerActive_ || waitingWriters_ > 0)
	{
		if (readersCondition_.wait_until(lockGuard, deadline) == std::cv_status::timeout)
		{
			return false;
		}
	}

	activeReaders_++;
	return true;
}

template <class Rep, class Period>
bool GB_ReadWriteLock::TryLockFor(const std::chrono::duration<Rep, Period>& timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	std::unique_lock<std::mutex> lockGuard(mutex_);

	waitingWriters_++;
	while (writerActive_ || activeReaders_ > 0)
	{
		if (writersCondition_.wait_until(lockGuard, deadline) == std::cv_status::timeout)
		{
			waitingWriters_--;

			// 如果没有写者在排队了，放行读者
			if (!writerActive_ && waitingWriters_ == 0)
			{
				readersCondition_.notify_all();
			}

			return false;
		}
	}

	waitingWriters_--;
	writerActive_ = true;
	return true;
}

#ifdef _MSC_VER
#  pragma warning(pop)
#endif


#endif