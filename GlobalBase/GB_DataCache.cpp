#include "GB_DataCache.h"

#include <algorithm>
#include <limits>
#include <typeinfo>

namespace
{
    bool WouldOverflowSizeAddition(const std::size_t leftValue, const std::size_t rightValue)
    {
        return rightValue > std::numeric_limits<std::size_t>::max() - leftValue;
    }

    bool ShouldEvictByBytes(const std::size_t currentBytes, const std::size_t incomingBytes, const std::size_t maxBytes)
    {
        if (WouldOverflowSizeAddition(currentBytes, incomingBytes))
        {
            return true;
        }

        if (maxBytes == 0)
        {
            return false;
        }

        if (currentBytes > maxBytes)
        {
            return true;
        }

        return incomingBytes > maxBytes - currentBytes;
    }

    bool ShouldEvictByCount(const std::size_t currentCount, const std::size_t incomingCount, const std::size_t maxCount)
    {
        if (maxCount == 0)
        {
            return false;
        }

        if (currentCount > maxCount)
        {
            return true;
        }

        return incomingCount > maxCount - currentCount;
    }

    template<typename TValue>
    bool TryEstimateExactValueBytes(const GB_Variant& value, std::size_t& valueBytes)
    {
        if (value.Is<TValue>())
        {
            valueBytes = sizeof(TValue);
            return true;
        }

        return false;
    }
}

GB_DataCache::GB_DataCache()
    : GB_DataCache(Options())
{
}

GB_DataCache::GB_DataCache(const Options& options)
    : options_(options), stats_(), currentBytes_(0), entries_(), orderList_(), freqToKeys_(), minFreq_(0), rng_(options.randomSeed), lock_()
{
}

GB_DataCache::~GB_DataCache()
{
}

bool GB_DataCache::Put(const std::string& key, GB_Variant value)
{
    const std::size_t valueBytes = EstimateValueBytes(value);
    return Put(key, std::move(value), valueBytes);
}

bool GB_DataCache::Put(const std::string& key, GB_Variant value, const std::size_t valueBytes)
{
    GB_WriteLockGuard lockGuard(lock_);

    if (options_.maxBytes != 0 && valueBytes > options_.maxBytes)
    {
        stats_.rejected++;
        return false;
    }

    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        if (!EnsureCapacityFor(valueBytes, 1, nullptr))
        {
            stats_.rejected++;
            return false;
        }

        Entry entry;
        entry.value = std::move(value);
        entry.bytes = valueBytes;

        bool hasInserted = false;
        std::unordered_map<std::string, Entry>::iterator insertedIt;
        try
        {
            const auto insertResult = entries_.insert(std::make_pair(key, std::move(entry)));
            insertedIt = insertResult.first;
            hasInserted = true;
            Entry& insertedEntry = insertedIt->second;
            OnInsert(insertedIt->first, insertedEntry);
        }
        catch (...)
        {
            if (hasInserted)
            {
                OnErase(insertedIt->first, insertedIt->second);
                entries_.erase(insertedIt);
            }

            stats_.rejected++;
            return false;
        }

        currentBytes_ += valueBytes;
        stats_.insertions++;
        return true;
    }

    Entry& entry = it->second;
    const std::size_t oldBytes = entry.bytes;

    if (!EnsureCapacityForReplacement(oldBytes, valueBytes, key))
    {
        stats_.rejected++;
        return false;
    }

    entry.value = std::move(value);
    entry.bytes = valueBytes;

    const std::size_t bytesWithoutEntry = currentBytes_ >= oldBytes ? currentBytes_ - oldBytes : 0;
    currentBytes_ = bytesWithoutEntry + valueBytes;

    if (options_.policy == Policy::Lru || options_.policy == Policy::Lfu)
    {
        OnAccess(key, entry);
    }

    stats_.updates++;
    return true;
}

bool GB_DataCache::TryGet(const std::string& key, GB_Variant& outValue)
{
    GB_WriteLockGuard lockGuard(lock_);

    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        stats_.misses++;
        return false;
    }

    Entry& entry = it->second;
    OnAccess(key, entry);

    outValue = entry.value;
    stats_.hits++;
    return true;
}

GB_Variant GB_DataCache::Get(const std::string& key)
{
    GB_Variant value;
    TryGet(key, value);
    return value;
}

GB_Variant GB_DataCache::GetOrDefault(const std::string& key, const GB_Variant& defaultValue)
{
    GB_Variant value;
    if (!TryGet(key, value))
    {
        return defaultValue;
    }

    return value;
}

bool GB_DataCache::TryPeek(const std::string& key, GB_Variant& outValue) const
{
    GB_ReadLockGuard lockGuard(lock_);

    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return false;
    }

    outValue = it->second.value;
    return true;
}

GB_Variant GB_DataCache::Peek(const std::string& key) const
{
    GB_Variant value;
    TryPeek(key, value);
    return value;
}

GB_Variant GB_DataCache::PeekOrDefault(const std::string& key, const GB_Variant& defaultValue) const
{
    GB_Variant value;
    if (!TryPeek(key, value))
    {
        return defaultValue;
    }

    return value;
}

bool GB_DataCache::Contains(const std::string& key) const
{
    GB_ReadLockGuard lockGuard(lock_);
    return entries_.find(key) != entries_.end();
}

bool GB_DataCache::Erase(const std::string& key)
{
    GB_WriteLockGuard lockGuard(lock_);

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
    GB_WriteLockGuard lockGuard(lock_);

    const std::size_t eraseCount = entries_.size();
    OnClear();
    entries_.clear();
    currentBytes_ = 0;

    stats_.erases += static_cast<std::uint64_t>(eraseCount);
    stats_.clears++;
}

bool GB_DataCache::Take(const std::string& key, GB_Variant& outValue)
{
    GB_WriteLockGuard lockGuard(lock_);

    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return false;
    }

    Entry& entry = it->second;
    outValue = std::move(entry.value);

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

    stats_.erases++;
    return true;
}

std::size_t GB_DataCache::Size() const
{
    GB_ReadLockGuard lockGuard(lock_);
    return entries_.size();
}

bool GB_DataCache::IsEmpty() const
{
    GB_ReadLockGuard lockGuard(lock_);
    return entries_.empty();
}

std::size_t GB_DataCache::GetCurrentBytes() const
{
    GB_ReadLockGuard lockGuard(lock_);
    return currentBytes_;
}

std::size_t GB_DataCache::GetMaxBytes() const
{
    GB_ReadLockGuard lockGuard(lock_);
    return options_.maxBytes;
}

void GB_DataCache::SetMaxBytes(const std::size_t maxBytes)
{
    GB_WriteLockGuard lockGuard(lock_);

    options_.maxBytes = maxBytes;
    EnsureCapacityFor(0, 0, nullptr);
}

std::size_t GB_DataCache::GetMaxCount() const
{
    GB_ReadLockGuard lockGuard(lock_);
    return options_.maxCount;
}

void GB_DataCache::SetMaxCount(const std::size_t maxCount)
{
    GB_WriteLockGuard lockGuard(lock_);

    options_.maxCount = maxCount;
    EnsureCapacityFor(0, 0, nullptr);
}

GB_DataCache::Policy GB_DataCache::GetPolicy() const
{
    GB_ReadLockGuard lockGuard(lock_);
    return options_.policy;
}

GB_DataCache::Stats GB_DataCache::GetStats() const
{
    GB_ReadLockGuard lockGuard(lock_);
    return stats_;
}

void GB_DataCache::ResetStats()
{
    GB_WriteLockGuard lockGuard(lock_);
    stats_ = Stats();
}

void GB_DataCache::Reserve(const std::size_t count)
{
    GB_WriteLockGuard lockGuard(lock_);
    entries_.reserve(count);
}

std::vector<std::string> GB_DataCache::GetKeys() const
{
    GB_ReadLockGuard lockGuard(lock_);

    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
    {
        keys.push_back(it->first);
    }

    return keys;
}

bool GB_DataCache::TryGetValueBytes(const std::string& key, std::size_t& valueBytes) const
{
    GB_ReadLockGuard lockGuard(lock_);

    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return false;
    }

    valueBytes = it->second.bytes;
    return true;
}

bool GB_DataCache::TryGetTypeName(const std::string& key, std::string& typeName) const
{
    GB_ReadLockGuard lockGuard(lock_);

    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return false;
    }

    typeName = it->second.value.TypeName();
    return true;
}

std::size_t GB_DataCache::EstimateValueBytes(const GB_Variant& value) const
{
    if (value.IsEmpty())
    {
        return 0;
    }

    if (const std::string* const stringValue = value.AnyCast<std::string>())
    {
        return stringValue->size();
    }

    if (const GB_ByteBuffer* const binaryValue = value.AnyCast<GB_ByteBuffer>())
    {
        return binaryValue->size();
    }

    std::size_t valueBytes = 0;
    if (TryEstimateExactValueBytes<bool>(value, valueBytes) ||
        TryEstimateExactValueBytes<char>(value, valueBytes) ||
        TryEstimateExactValueBytes<signed char>(value, valueBytes) ||
        TryEstimateExactValueBytes<unsigned char>(value, valueBytes) ||
        TryEstimateExactValueBytes<short>(value, valueBytes) ||
        TryEstimateExactValueBytes<unsigned short>(value, valueBytes) ||
        TryEstimateExactValueBytes<int>(value, valueBytes) ||
        TryEstimateExactValueBytes<unsigned int>(value, valueBytes) ||
        TryEstimateExactValueBytes<long>(value, valueBytes) ||
        TryEstimateExactValueBytes<unsigned long>(value, valueBytes) ||
        TryEstimateExactValueBytes<long long>(value, valueBytes) ||
        TryEstimateExactValueBytes<unsigned long long>(value, valueBytes) ||
        TryEstimateExactValueBytes<float>(value, valueBytes) ||
        TryEstimateExactValueBytes<double>(value, valueBytes) ||
        TryEstimateExactValueBytes<long double>(value, valueBytes))
    {
        return valueBytes;
    }

    GB_ByteBuffer serializedValue;
    if (value.Serialize(serializedValue))
    {
        return serializedValue.size();
    }

    return options_.unknownValueBytes;
}

bool GB_DataCache::EnsureCapacityFor(const std::size_t incomingBytes, const std::size_t incomingCount, const std::string* protectedKey)
{
    while (ShouldEvictByBytes(currentBytes_, incomingBytes, options_.maxBytes) ||
        ShouldEvictByCount(entries_.size(), incomingCount, options_.maxCount))
    {
        if (!EvictOne(protectedKey))
        {
            return false;
        }
    }

    return true;
}

bool GB_DataCache::EnsureCapacityForReplacement(const std::size_t oldBytes, const std::size_t newBytes, const std::string& protectedKey)
{
    while (true)
    {
        const std::size_t bytesWithoutProtectedEntry = currentBytes_ >= oldBytes ? currentBytes_ - oldBytes : 0;
        const bool shouldEvictByBytes = ShouldEvictByBytes(bytesWithoutProtectedEntry, newBytes, options_.maxBytes);
        const bool shouldEvictByCount = ShouldEvictByCount(entries_.size(), 0, options_.maxCount);
        if (!shouldEvictByBytes && !shouldEvictByCount)
        {
            return true;
        }

        if (!EvictOne(&protectedKey))
        {
            return false;
        }
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
        const auto insertResult = freqToKeys_.insert(std::make_pair(static_cast<std::size_t>(1), std::list<std::string>()));
        try
        {
            insertResult.first->second.push_front(key);
        }
        catch (...)
        {
            if (insertResult.second && insertResult.first->second.empty())
            {
                freqToKeys_.erase(insertResult.first);
            }

            entry.freq = 0;
            throw;
        }

        entry.freqIt = insertResult.first->second.begin();
        entry.hasFreqIt = true;
        minFreq_ = 1;
    }
}

void GB_DataCache::OnAccess(const std::string& key, Entry& entry)
{
    if (options_.policy == Policy::Lru)
    {
        if (!entry.hasOrderIt)
        {
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
            try
            {
                entry.freq = 1;
                const auto insertResult = freqToKeys_.insert(std::make_pair(static_cast<std::size_t>(1), std::list<std::string>()));
                try
                {
                    insertResult.first->second.push_front(key);
                }
                catch (...)
                {
                    if (insertResult.second && insertResult.first->second.empty())
                    {
                        freqToKeys_.erase(insertResult.first);
                    }

                    throw;
                }

                entry.freqIt = insertResult.first->second.begin();
                entry.hasFreqIt = true;
                minFreq_ = 1;
            }
            catch (...)
            {
                entry.freq = 0;
            }

            return;
        }

        const std::size_t oldFreq = entry.freq;
        auto oldBucketIt = freqToKeys_.find(oldFreq);
        if (oldBucketIt == freqToKeys_.end())
        {
            entry.hasFreqIt = false;
            entry.freq = 0;
            OnAccess(key, entry);
            return;
        }

        if (oldFreq == std::numeric_limits<std::size_t>::max())
        {
            oldBucketIt->second.splice(oldBucketIt->second.begin(), oldBucketIt->second, entry.freqIt);
            entry.freqIt = oldBucketIt->second.begin();
            return;
        }

        const std::size_t newFreq = oldFreq + 1;
        std::list<std::string>* const oldBucket = &oldBucketIt->second;
        std::list<std::string>* newBucket = nullptr;
        std::list<std::string>::iterator newFreqIt;
        bool hasCreatedNewBucket = false;
        try
        {
            const auto insertResult = freqToKeys_.insert(std::make_pair(newFreq, std::list<std::string>()));
            newBucket = &insertResult.first->second;
            hasCreatedNewBucket = insertResult.second;
            newBucket->push_front(key);
            newFreqIt = newBucket->begin();
        }
        catch (...)
        {
            if (hasCreatedNewBucket && newBucket != nullptr && newBucket->empty())
            {
                freqToKeys_.erase(newFreq);
            }

            return;
        }

        oldBucket->erase(entry.freqIt);

        if (oldBucket->empty())
        {
            freqToKeys_.erase(oldFreq);
            if (minFreq_ == oldFreq)
            {
                minFreq_ = newFreq;
            }
        }

        entry.freq = newFreq;
        entry.freqIt = newFreqIt;
        entry.hasFreqIt = true;
    }
}

void GB_DataCache::OnErase(const std::string& key, Entry& entry)
{
    (void)key;

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
            const std::size_t freq = entry.freq;
            auto bucketIt = freqToKeys_.find(freq);
            if (bucketIt != freqToKeys_.end())
            {
                bucketIt->second.erase(entry.freqIt);
                if (bucketIt->second.empty())
                {
                    freqToKeys_.erase(bucketIt);

                    if (minFreq_ == freq)
                    {
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

    if (options_.policy == Policy::Fifo)
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

    if (options_.policy == Policy::Lfu)
    {
        if (freqToKeys_.empty())
        {
            return false;
        }

        auto tryPickFromBucket = [&](const std::list<std::string>& bucket, std::string& pickedKey) -> bool
            {
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

        if (minFreq_ != 0)
        {
            const auto bucketIt = freqToKeys_.find(minFreq_);
            if (bucketIt != freqToKeys_.end() && tryPickFromBucket(bucketIt->second, victimKey))
            {
                return true;
            }
        }

        std::size_t bestFreq = 0;
        std::string bestKey;
        for (auto it = freqToKeys_.begin(); it != freqToKeys_.end(); ++it)
        {
            std::string candidateKey;
            if (!tryPickFromBucket(it->second, candidateKey))
            {
                continue;
            }

            if (bestFreq == 0 || it->first < bestFreq)
            {
                bestFreq = it->first;
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

    if (entries_.size() == 1 && protectedKey != nullptr && entries_.find(*protectedKey) != entries_.end())
    {
        return false;
    }

    std::uniform_int_distribution<std::size_t> dist(0, entries_.size() - 1);
    for (int index = 0; index < 8; index++)
    {
        const std::size_t offset = dist(rng_);
        auto it = entries_.begin();
        for (std::size_t step = 0; step < offset; step++)
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
