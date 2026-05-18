#include "GB_ReadWriteLock.h"

#include <cassert>

bool GB_ReadWriteLock::CanLockSharedUnlocked() const
{
    return !writerActive_ && waitingWriters_ == 0;
}

bool GB_ReadWriteLock::CanLockUnlocked() const
{
    return !writerActive_ && activeReaders_ == 0;
}

void GB_ReadWriteLock::LockShared()
{
    std::unique_lock<std::mutex> lockGuard(mutex_);
    readersCondition_.wait(lockGuard, [this]() { return CanLockSharedUnlocked(); });
    activeReaders_++;
}

void GB_ReadWriteLock::UnlockShared()
{
    bool shouldNotifyWriter = false;
    bool shouldNotifyReaders = false;

    {
        std::unique_lock<std::mutex> lockGuard(mutex_);
        if (activeReaders_ == 0)
        {
            assert(false && "GB_ReadWriteLock::UnlockShared called without a shared lock.");
            return;
        }

        activeReaders_--;
        if (activeReaders_ == 0)
        {
            if (waitingWriters_ > 0)
            {
                shouldNotifyWriter = true;
            }
            else
            {
                shouldNotifyReaders = true;
            }
        }
    }

    if (shouldNotifyWriter)
    {
        writersCondition_.notify_one();
    }
    else if (shouldNotifyReaders)
    {
        readersCondition_.notify_all();
    }
}

void GB_ReadWriteLock::Lock()
{
    std::unique_lock<std::mutex> lockGuard(mutex_);

    waitingWriters_++;
    try
    {
        writersCondition_.wait(lockGuard, [this]() { return CanLockUnlocked(); });
    }
    catch (...)
    {
        waitingWriters_--;
        const bool shouldNotifyReaders = !writerActive_ && waitingWriters_ == 0;
        lockGuard.unlock();

        if (shouldNotifyReaders)
        {
            readersCondition_.notify_all();
        }

        throw;
    }

    waitingWriters_--;
    writerActive_ = true;
}

void GB_ReadWriteLock::Unlock()
{
    bool shouldNotifyWriter = false;
    bool shouldNotifyReaders = false;

    {
        std::unique_lock<std::mutex> lockGuard(mutex_);
        if (!writerActive_)
        {
            assert(false && "GB_ReadWriteLock::Unlock called without an exclusive lock.");
            return;
        }

        writerActive_ = false;
        if (waitingWriters_ > 0)
        {
            shouldNotifyWriter = true;
        }
        else
        {
            shouldNotifyReaders = true;
        }
    }

    if (shouldNotifyWriter)
    {
        writersCondition_.notify_one();
    }
    else if (shouldNotifyReaders)
    {
        readersCondition_.notify_all();
    }
}

bool GB_ReadWriteLock::TryLockShared()
{
    std::unique_lock<std::mutex> lockGuard(mutex_, std::try_to_lock);
    if (!lockGuard.owns_lock())
    {
        return false;
    }

    if (!CanLockSharedUnlocked())
    {
        return false;
    }

    activeReaders_++;
    return true;
}

bool GB_ReadWriteLock::TryLock()
{
    std::unique_lock<std::mutex> lockGuard(mutex_, std::try_to_lock);
    if (!lockGuard.owns_lock())
    {
        return false;
    }

    if (!CanLockUnlocked())
    {
        return false;
    }

    writerActive_ = true;
    return true;
}

GB_ReadLockGuard::GB_ReadLockGuard(GB_ReadWriteLock& lock)
    : lock_(&lock), ownsLock_(false)
{
    lock_->LockShared();
    ownsLock_ = true;
}

GB_ReadLockGuard::GB_ReadLockGuard(GB_ReadWriteLock& lock, GB_DeferLockTag)
    : lock_(&lock), ownsLock_(false)
{
}

GB_ReadLockGuard::GB_ReadLockGuard(GB_ReadWriteLock& lock, GB_TryToLockTag)
    : lock_(&lock), ownsLock_(false)
{
    ownsLock_ = lock_->TryLockShared();
}

GB_ReadLockGuard::~GB_ReadLockGuard()
{
    if (lock_ != nullptr && ownsLock_)
    {
        lock_->UnlockShared();
    }
}

GB_ReadLockGuard::GB_ReadLockGuard(GB_ReadLockGuard&& other) noexcept
    : lock_(other.lock_), ownsLock_(other.ownsLock_)
{
    other.ResetNoUnlock();
}

GB_ReadLockGuard& GB_ReadLockGuard::operator=(GB_ReadLockGuard&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    if (lock_ != nullptr && ownsLock_)
    {
        lock_->UnlockShared();
    }

    lock_ = other.lock_;
    ownsLock_ = other.ownsLock_;
    other.ResetNoUnlock();

    return *this;
}

void GB_ReadLockGuard::Lock()
{
    if (lock_ == nullptr || ownsLock_)
    {
        return;
    }

    lock_->LockShared();
    ownsLock_ = true;
}

bool GB_ReadLockGuard::TryLock()
{
    if (lock_ == nullptr || ownsLock_)
    {
        return ownsLock_;
    }

    ownsLock_ = lock_->TryLockShared();
    return ownsLock_;
}

void GB_ReadLockGuard::Unlock()
{
    if (lock_ == nullptr || !ownsLock_)
    {
        return;
    }

    lock_->UnlockShared();
    ownsLock_ = false;
}

bool GB_ReadLockGuard::OwnsLock() const
{
    return ownsLock_;
}

void GB_ReadLockGuard::ResetNoUnlock()
{
    lock_ = nullptr;
    ownsLock_ = false;
}

GB_WriteLockGuard::GB_WriteLockGuard(GB_ReadWriteLock& lock)
    : lock_(&lock), ownsLock_(false)
{
    lock_->Lock();
    ownsLock_ = true;
}

GB_WriteLockGuard::GB_WriteLockGuard(GB_ReadWriteLock& lock, GB_DeferLockTag)
    : lock_(&lock), ownsLock_(false)
{
}

GB_WriteLockGuard::GB_WriteLockGuard(GB_ReadWriteLock& lock, GB_TryToLockTag)
    : lock_(&lock), ownsLock_(false)
{
    ownsLock_ = lock_->TryLock();
}

GB_WriteLockGuard::~GB_WriteLockGuard()
{
    if (lock_ != nullptr && ownsLock_)
    {
        lock_->Unlock();
    }
}

GB_WriteLockGuard::GB_WriteLockGuard(GB_WriteLockGuard&& other) noexcept
    : lock_(other.lock_), ownsLock_(other.ownsLock_)
{
    other.ResetNoUnlock();
}

GB_WriteLockGuard& GB_WriteLockGuard::operator=(GB_WriteLockGuard&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    if (lock_ != nullptr && ownsLock_)
    {
        lock_->Unlock();
    }

    lock_ = other.lock_;
    ownsLock_ = other.ownsLock_;
    other.ResetNoUnlock();

    return *this;
}

void GB_WriteLockGuard::Lock()
{
    if (lock_ == nullptr || ownsLock_)
    {
        return;
    }

    lock_->Lock();
    ownsLock_ = true;
}

bool GB_WriteLockGuard::TryLock()
{
    if (lock_ == nullptr || ownsLock_)
    {
        return ownsLock_;
    }

    ownsLock_ = lock_->TryLock();
    return ownsLock_;
}

void GB_WriteLockGuard::Unlock()
{
    if (lock_ == nullptr || !ownsLock_)
    {
        return;
    }

    lock_->Unlock();
    ownsLock_ = false;
}

bool GB_WriteLockGuard::OwnsLock() const
{
    return ownsLock_;
}

void GB_WriteLockGuard::ResetNoUnlock()
{
    lock_ = nullptr;
    ownsLock_ = false;
}
