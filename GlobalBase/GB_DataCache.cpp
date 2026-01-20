#include "GB_DataCache.h"

#include <algorithm>

GB_DataCache::GB_DataCache(const Options& options) : options_(options), stats_(), currentBytes_(0), entries_(), orderList_(),
    freqToKeys_(), minFreq_(0), rng_(options.randomSeed)
{
}

GB_DataCache::~GB_DataCache()
{
}

GB_DataCache::Policy GB_DataCache::GetPolicy() const
{
    return options_.policy;
}

size_t GB_DataCache::Size() const
{
    return entries_.size();
}

size_t GB_DataCache::GetCurrentBytes() const
{
    return currentBytes_;
}

size_t GB_DataCache::GetMaxBytes() const
{
    return options_.maxBytes;
}

void GB_DataCache::SetMaxBytes(size_t maxBytes)
{
    options_.maxBytes = maxBytes;
    EnsureCapacityFor(0, nullptr);
}

GB_DataCache::Stats GB_DataCache::GetStats() const
{
    return stats_;
}

void GB_DataCache::ResetStats()
{
    stats_ = Stats();
}

bool GB_DataCache::Contains(const std::string& key) const
{
    return entries_.find(key) != entries_.end();
}

bool GB_DataCache::TryGetValueBytes(const std::string& key, size_t& valueBytes) const
{
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return false;
    }

    valueBytes = it->second.bytes;
    return true;
}

bool GB_DataCache::PutRaw(const std::string& key, void* rawPtr, size_t valueBytes, const std::function<void(void*)>& deleter)
{
    if (rawPtr != nullptr && !deleter)
    {
        // 不允许：rawPtr 非空但未提供 deleter。
        return false;
    }

    std::shared_ptr<void> value;
    if (rawPtr == nullptr)
    {
        // 允许缓存一个空指针（通常 valueBytes 也应为 0）
        value = std::shared_ptr<void>();
    }
    else
    {
        // std::shared_ptr<void> 接管析构，由调用者提供 deleter
        value = std::shared_ptr<void>(rawPtr, deleter);
    }

    return Put(key, value, valueBytes);
}

bool GB_DataCache::Put(const std::string& key, const std::shared_ptr<void>& value, size_t valueBytes)
{
    if (options_.maxBytes != 0 && valueBytes > options_.maxBytes)
    {
        // 单条记录比缓存上限还大：拒绝
        return false;
    }

    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        // 新插入：先确保容量
        if (!EnsureCapacityFor(valueBytes, nullptr))
        {
            return false;
        }

        Entry entry;
        entry.value = value;
        entry.bytes = valueBytes;

        entries_.insert(std::make_pair(key, entry));
        Entry& inserted = entries_.find(key)->second;

        OnInsert(key, inserted);

        currentBytes_ += valueBytes;
        stats_.insertions++;
        return true;
    }
    else
    {
        // 更新已有 key：允许变更 bytes
        Entry& entry = it->second;

        if (options_.policy == Policy::Lru || options_.policy == Policy::Lfu)
        {
            // 把它当成一次“访问”，以避免它被当场淘汰
            OnAccess(key, entry);
        }

        const size_t oldBytes = entry.bytes;
        const size_t newBytes = valueBytes;

        if (options_.maxBytes != 0)
        {
            if (newBytes > oldBytes)
            {
                const size_t extraBytes = newBytes - oldBytes;
                if (!EnsureCapacityFor(extraBytes, &key))
                {
                    return false;
                }
            }
        }

        entry.value = value;
        entry.bytes = newBytes;

        if (newBytes > oldBytes)
        {
            currentBytes_ += (newBytes - oldBytes);
        }
        else
        {
            currentBytes_ -= (oldBytes - newBytes);
        }

        if (options_.policy == Policy::Fifo)
        {
            // FIFO 通常不因更新改变顺序；不 Touch
        }
        else if (options_.policy == Policy::Random)
        {
            // Random 不维护顺序；不 Touch
        }

        stats_.updates++;
        return true;
    }
}

std::shared_ptr<void> GB_DataCache::Peek(const std::string& key) const
{
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return std::shared_ptr<void>();
    }

    return it->second.value;
}

std::shared_ptr<void> GB_DataCache::Get(const std::string& key)
{
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        stats_.misses++;
        return std::shared_ptr<void>();
    }

    Entry& entry = it->second;
    OnAccess(key, entry);

    stats_.hits++;
    return entry.value;
}

bool GB_DataCache::Erase(const std::string& key)
{
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return false;
    }

    RemoveEntry(key);
    stats_.erases++;
    return true;
}

void GB_DataCache::Clear()
{
    OnClear();
    entries_.clear();
    currentBytes_ = 0;
    stats_.erases += 0; // 不强行累加，避免误导；需要的话你可以自定义为 entries_.size()
}

void GB_DataCache::OnInsert(const std::string& key, Entry& entry)
{
    if (options_.policy == Policy::Lru)
    {
        orderList_.push_front(key);
        entry.orderIt = orderList_.begin();
        entry.hasOrderIt = true;
    }
    else if (options_.policy == Policy::Fifo)
    {
        orderList_.push_back(key);
        entry.orderIt = --orderList_.end();
        entry.hasOrderIt = true;
    }
    else if (options_.policy == Policy::Lfu)
    {
        entry.freq = 1;
        auto& bucket = freqToKeys_[1];
        bucket.push_front(key);
        entry.freqIt = bucket.begin();
        entry.hasFreqIt = true;
        minFreq_ = 1;
    }
    else
    {
        // Random：不需要元数据
    }
}

void GB_DataCache::OnAccess(const std::string& key, Entry& entry)
{
    if (options_.policy == Policy::Lru)
    {
        if (!entry.hasOrderIt)
        {
            // 理论不该发生
            orderList_.push_front(key);
            entry.orderIt = orderList_.begin();
            entry.hasOrderIt = true;
            return;
        }

        orderList_.splice(orderList_.begin(), orderList_, entry.orderIt);
        entry.orderIt = orderList_.begin();
    }
    else if (options_.policy == Policy::Lfu)
    {
        if (!entry.hasFreqIt)
        {
            entry.freq = 1;
            auto& bucket = freqToKeys_[1];
            bucket.push_front(key);
            entry.freqIt = bucket.begin();
            entry.hasFreqIt = true;
            minFreq_ = 1;
            return;
        }

        const size_t oldFreq = entry.freq;
        auto oldBucketIt = freqToKeys_.find(oldFreq);
        if (oldBucketIt != freqToKeys_.end())
        {
            oldBucketIt->second.erase(entry.freqIt);
            if (oldBucketIt->second.empty())
            {
                freqToKeys_.erase(oldBucketIt);
                if (minFreq_ == oldFreq)
                {
                    minFreq_ = oldFreq + 1;
                }
            }
        }

        entry.freq = oldFreq + 1;
        auto& newBucket = freqToKeys_[entry.freq];
        newBucket.push_front(key);
        entry.freqIt = newBucket.begin();
        entry.hasFreqIt = true;
    }
    else
    {
        // FIFO / Random：访问不影响元数据
    }
}

void GB_DataCache::OnErase(const std::string& key, Entry& entry)
{
    if (options_.policy == Policy::Lru || options_.policy == Policy::Fifo)
    {
        if (entry.hasOrderIt)
        {
            orderList_.erase(entry.orderIt);
            entry.hasOrderIt = false;
        }
    }
    else if (options_.policy == Policy::Lfu)
    {
        if (entry.hasFreqIt)
        {
            const size_t freq = entry.freq;
            auto bucketIt = freqToKeys_.find(freq);
            if (bucketIt != freqToKeys_.end())
            {
                bucketIt->second.erase(entry.freqIt);
                if (bucketIt->second.empty())
                {
                    freqToKeys_.erase(bucketIt);

                    if (minFreq_ == freq)
                    {
                        // 重新寻找 minFreq（代价 O(#freq buckets)，一般很小；如需严格 O(1)，可维护更复杂结构）
                        minFreq_ = 0;
                        for (auto it = freqToKeys_.begin(); it != freqToKeys_.end(); ++it)
                        {
                            if (minFreq_ == 0 || it->first < minFreq_)
                            {
                                minFreq_ = it->first;
                            }
                        }
                    }
                }
            }

            entry.hasFreqIt = false;
        }
    }
    else
    {
        // Random：不需要处理
    }
}

void GB_DataCache::OnClear()
{
    orderList_.clear();
    freqToKeys_.clear();
    minFreq_ = 0;
}

bool GB_DataCache::PickVictimKey(std::string& victimKey, const std::string* protectedKey)
{
    if (entries_.empty())
    {
        return false;
    }

    if (options_.policy == Policy::Lru)
    {
        for (auto it = orderList_.rbegin(); it != orderList_.rend(); ++it)
        {
            if (protectedKey == nullptr || *it != *protectedKey)
            {
                victimKey = *it;
                return true;
            }
        }
        return false;
    }
    else if (options_.policy == Policy::Fifo)
    {
        for (auto it = orderList_.begin(); it != orderList_.end(); ++it)
        {
            if (protectedKey == nullptr || *it != *protectedKey)
            {
                victimKey = *it;
                return true;
            }
        }
        return false;
    }
    else if (options_.policy == Policy::Lfu)
    {
        if (freqToKeys_.empty())
        {
            return false;
        }

        auto TryPickFromBucket = [&](const std::list<std::string>& bucket, std::string& pickedKey) -> bool {
            for (auto it = bucket.rbegin(); it != bucket.rend(); ++it)
            {
                if (protectedKey == nullptr || *it != *protectedKey)
                {
                    pickedKey = *it;
                    return true;
                }
            }
            return false;
        };

        // 先尝试 minFreq_（快路径）
        if (minFreq_ != 0)
        {
            const auto bucketIt = freqToKeys_.find(minFreq_);
            if (bucketIt != freqToKeys_.end())
            {
                if (TryPickFromBucket(bucketIt->second, victimKey))
                {
                    return true;
                }
            }
        }

        // 如果 minFreq_ 桶里没有可淘汰对象（例如只有 protectedKey），
        // 则扫描所有频次桶，挑“频次最小且存在可淘汰 key”的桶。
        size_t bestFreq = 0;
        std::string bestKey;

        for (const auto& pair : freqToKeys_)
        {
            const size_t freq = pair.first;
            const auto& bucket = pair.second;

            std::string candidateKey;
            if (!TryPickFromBucket(bucket, candidateKey))
            {
                continue;
            }

            if (bestFreq == 0 || freq < bestFreq)
            {
                bestFreq = freq;
                bestKey = candidateKey;
            }
        }

        if (bestFreq == 0)
        {
            return false;
        }

        victimKey = bestKey;
        return true;
    }
    else
    {
        // Random
        if (entries_.size() == 1 && protectedKey != nullptr)
        {
            if (entries_.find(*protectedKey) != entries_.end())
            {
                return false;
            }
        }

        std::uniform_int_distribution<size_t> dist(0, entries_.size() - 1);

        for (int i = 0; i < 8; i++)
        {
            const size_t offset = dist(rng_);
            auto it = entries_.begin();
            for (size_t step = 0; step < offset; step++)
            {
                ++it;
                if (it == entries_.end())
                {
                    it = entries_.begin();
                }
            }

            if (protectedKey == nullptr || it->first != *protectedKey)
            {
                victimKey = it->first;
                return true;
            }
        }

        // 兜底：线性找第一个非 protected
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
        {
            if (protectedKey == nullptr || it->first != *protectedKey)
            {
                victimKey = it->first;
                return true;
            }
        }

        return false;
    }
}

bool GB_DataCache::EvictOne(const std::string* protectedKey)
{
    std::string victimKey;
    if (!PickVictimKey(victimKey, protectedKey))
    {
        return false;
    }

    RemoveEntry(victimKey);
    stats_.evictions++;
    return true;
}

bool GB_DataCache::EnsureCapacityFor(size_t incomingBytes, const std::string* protectedKey)
{
    if (options_.maxBytes == 0)
    {
        return true;
    }

    if (incomingBytes > options_.maxBytes)
    {
        return false;
    }

    while (currentBytes_ + incomingBytes > options_.maxBytes)
    {
        if (!EvictOne(protectedKey))
        {
            return false;
        }
    }

    return true;
}

void GB_DataCache::RemoveEntry(const std::string& key)
{
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return;
    }

    Entry& entry = it->second;
    OnErase(key, entry);

    if (currentBytes_ >= entry.bytes)
    {
        currentBytes_ -= entry.bytes;
    }
    else
    {
        currentBytes_ = 0;
    }

    entries_.erase(it);

    if (options_.policy == Policy::Lfu && entries_.empty())
    {
        minFreq_ = 0;
        freqToKeys_.clear();
    }
}
