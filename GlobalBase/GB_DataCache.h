#ifndef GLOBALBASE_DATA_CACHE_LOCK_H_H
#define GLOBALBASE_DATA_CACHE_LOCK_H_H

#include "GlobalBasePort.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>

class GB_DataCache
{
public:
    enum class Policy
    {
        Lru,
        Lfu,
        Fifo,
        Random
    };

    struct Options
    {
        Policy policy = Policy::Lru;
        size_t maxBytes = 0;           // 0 表示不限制（不触发淘汰）
        uint32_t randomSeed = 5489u;   // mt19937 默认种子风格
    };

    struct Stats
    {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        uint64_t insertions = 0;
        uint64_t updates = 0;
        uint64_t erases = 0;
    };

public:
    explicit GB_DataCache(const Options& options);
    ~GB_DataCache();

public:
    // Put：直接放 shared_ptr<void>（类型擦除）
    // valueBytes：用于内存预算/淘汰判定（由调用者提供）
    bool Put(const std::string& key, const std::shared_ptr<void>& value, size_t valueBytes);

    // PutRaw：放裸指针，缓存内部用 shared_ptr<void> 接管析构
    // deleter：例如 [](void* p){ delete static_cast<MyType*>(p); }
    bool PutRaw(const std::string& key, void* rawPtr, size_t valueBytes, const std::function<void(void*)>& deleter);

    template<typename T>
    bool PutShared(const std::string& key, const std::shared_ptr<T>& value, size_t valueBytes);

    template<typename T>
    bool PutNew(const std::string& key, T* rawPtr, size_t valueBytes);

    template<typename T, typename Deleter>
    bool PutNew(const std::string& key, T* rawPtr, size_t valueBytes, Deleter deleter);

    template<typename T, typename Deleter>
    bool PutUnique(const std::string& key, std::unique_ptr<T, Deleter> uniquePtr, size_t valueBytes);

    // Get：命中则返回 shared_ptr<void>，并更新策略（LRU/LFU 会 Touch）
    std::shared_ptr<void> Get(const std::string& key);

    template<typename T>
    std::shared_ptr<T> GetAs(const std::string& key);

    // Peek：不更新策略，只读
    std::shared_ptr<void> Peek(const std::string& key) const;

    bool Contains(const std::string& key) const;

    // Erase / Clear
    bool Erase(const std::string& key);
    void Clear();

    // 统计与容量
    size_t Size() const;
    size_t GetCurrentBytes() const;
    size_t GetMaxBytes() const;
    void SetMaxBytes(size_t maxBytes);      // 可能触发淘汰
    Policy GetPolicy() const;

    Stats GetStats() const;
    void ResetStats();

    // 取某个 key 的记录字节数（若不存在返回 false）
    bool TryGetValueBytes(const std::string& key, size_t& valueBytes) const;

private:
    struct Entry
    {
        std::shared_ptr<void> value;
        size_t bytes = 0;

        // LRU / FIFO 元数据
        std::list<std::string>::iterator orderIt;
        bool hasOrderIt = false;

        // LFU 元数据
        size_t freq = 0;
        std::list<std::string>::iterator freqIt;
        bool hasFreqIt = false;
    };

private:
    bool EnsureCapacityFor(size_t incomingBytes, const std::string* protectedKey);
    bool EvictOne(const std::string* protectedKey);
    void RemoveEntry(const std::string& key);

private:
    // Policy hooks
    void OnInsert(const std::string& key, Entry& entry);
    void OnAccess(const std::string& key, Entry& entry);
    void OnErase(const std::string& key, Entry& entry);
    void OnClear();

    bool PickVictimKey(std::string& victimKey, const std::string* protectedKey);

private:
    Options options_;
    Stats stats_;

    size_t currentBytes_;

    std::unordered_map<std::string, Entry> entries_;

    // LRU / FIFO
    std::list<std::string> orderList_;

    // LFU：freq -> keys（keys list 内按“最近访问在前”组织，淘汰取 back）
    std::unordered_map<size_t, std::list<std::string>> freqToKeys_;
    size_t minFreq_;

    mutable std::mt19937 rng_;
};

template<typename T>
bool GB_DataCache::PutShared(const std::string& key, const std::shared_ptr<T>& value, size_t valueBytes)
{
    static_assert(!std::is_array<T>::value, "GB_DataCache::PutShared does not support array types. Use containers instead.");

    const std::shared_ptr<void> erasedValue = std::static_pointer_cast<void>(value);
    return Put(key, erasedValue, valueBytes);
}

template<typename T>
bool GB_DataCache::PutNew(const std::string& key, T* rawPtr, size_t valueBytes)
{
    static_assert(!std::is_array<T>::value, "GB_DataCache::PutNew does not support array types. Use containers instead.");

    if (rawPtr == nullptr)
    {
        return Put(key, std::shared_ptr<void>(), valueBytes);
    }

    const std::shared_ptr<T> typedValue(rawPtr);
    return PutShared<T>(key, typedValue, valueBytes);
}

template<typename T, typename Deleter>
bool GB_DataCache::PutNew(const std::string& key, T* rawPtr, size_t valueBytes, Deleter deleter)
{
    static_assert(!std::is_array<T>::value, "GB_DataCache::PutNew does not support array types. Use containers instead.");

    if (rawPtr == nullptr)
    {
        return Put(key, std::shared_ptr<void>(), valueBytes);
    }

    const std::shared_ptr<T> typedValue(rawPtr, std::move(deleter));
    return PutShared<T>(key, typedValue, valueBytes);
}

template<typename T, typename Deleter>
bool GB_DataCache::PutUnique(const std::string& key, std::unique_ptr<T, Deleter> uniquePtr, size_t valueBytes)
{
    static_assert(!std::is_array<T>::value, "GB_DataCache::PutUnique does not support array types. Use containers instead.");

    if (!uniquePtr)
    {
        return Put(key, std::shared_ptr<void>(), valueBytes);
    }

    const std::shared_ptr<T> typedValue(std::move(uniquePtr));
    return PutShared<T>(key, typedValue, valueBytes);
}

template<typename T>
std::shared_ptr<T> GB_DataCache::GetAs(const std::string& key)
{
    static_assert(!std::is_array<T>::value, "GB_DataCache::GetAs does not support array types.");

    const std::shared_ptr<void> erasedValue = Get(key);

    // 必须保证 key 对应的真实类型就是 T
    return std::static_pointer_cast<T>(erasedValue);
}


#endif