#ifndef GLOBALBASE_DATA_CACHE_H_H
#define GLOBALBASE_DATA_CACHE_H_H

#include "GB_ReadWriteLock.h"
#include "GB_Variant.h"
#include "GlobalBasePort.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief 基于 GB_Variant 的通用数据缓存。
 *
 * 设计目标：
 * - 直接以 GB_Variant 作为缓存值类型，避免 shared_ptr<void> 带来的类型不透明问题。
 * - 支持 LRU、LFU、FIFO、Random 四种淘汰策略。
 * - 支持按缓存项数量和逻辑值字节数进行容量控制。
 * - 所有公开接口内部加锁，允许多个线程并发访问同一个缓存对象。
 * - 只读接口使用共享锁；写入、淘汰、命中统计和访问元数据刷新使用独占锁。
 *
 * 说明：
 * - 这里的字节数是“缓存预算字节数”，不是对象在堆上的严格内存占用。
 * - 若调用 Put(key, value)，缓存会根据 GB_Variant 的实际类型自动估算字节数。
 * - 若调用 Put(key, value, valueBytes)，则使用调用者指定的字节数，适合大型自定义对象。
 * - Get / TryGet 会更新命中统计，并在 LRU / LFU 策略下刷新访问元数据。
 * - Peek / TryPeek 只读取，不更新命中统计，也不刷新访问元数据。
 */
class GLOBALBASE_PORT GB_DataCache
{
public:
    /**
     * @brief 缓存淘汰策略。
     */
    enum class Policy
    {
        Lru,
        Lfu,
        Fifo,
        Random
    };

    /**
     * @brief 缓存配置。
     */
    struct Options
    {
        // 淘汰策略。
        Policy policy = Policy::Lru;

        // 最大逻辑值字节数，0 表示不限制。
        std::size_t maxBytes = 0;

        // 最大缓存项数量，0 表示不限制。
        std::size_t maxCount = 0;

        // Random 策略使用的随机种子。
        std::uint32_t randomSeed = 5489u;

        // 当无法精确估算 GB_Variant 中自定义类型的字节数时使用的兜底字节数。
        std::size_t unknownValueBytes = sizeof(GB_Variant);
    };

    /**
     * @brief 缓存统计信息。
     */
    struct Stats
    {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t insertions = 0;
        std::uint64_t updates = 0;
        std::uint64_t erases = 0;
        std::uint64_t clears = 0;
        std::uint64_t rejected = 0;
    };

public:
    /**
     * @brief 构造一个使用默认配置的缓存。
     */
    GB_DataCache();

    /**
     * @brief 构造一个使用指定配置的缓存。
     */
    explicit GB_DataCache(const Options& options);

    /**
     * @brief 析构函数。
     */
    ~GB_DataCache();

    GB_DataCache(const GB_DataCache& other) = delete;
    GB_DataCache& operator=(const GB_DataCache& other) = delete;
    GB_DataCache(GB_DataCache&& other) = delete;
    GB_DataCache& operator=(GB_DataCache&& other) = delete;

    /**
     * @brief 写入或更新一个缓存项，并自动估算缓存预算字节数。
     *
     * @param key 缓存键。
     * @param value 缓存值。
     * @return 成功返回 true；若超过单项容量上限或无法完成淘汰则返回 false。
     */
    bool Put(const std::string& key, GB_Variant value);

    /**
     * @brief 写入或更新一个缓存项，并使用调用者指定的缓存预算字节数。
     *
     * @param key 缓存键。
     * @param value 缓存值。
     * @param valueBytes 缓存预算字节数。
     * @return 成功返回 true；若超过单项容量上限或无法完成淘汰则返回 false。
     */
    bool Put(const std::string& key, GB_Variant value, std::size_t valueBytes);

    /**
     * @brief 尝试读取一个缓存项。
     *
     * 命中时会更新命中统计；LRU / LFU 策略下还会刷新访问元数据。
     *
     * @param key 缓存键。
     * @param outValue 输出缓存值。
     * @return 命中返回 true，否则返回 false。
     */
    bool TryGet(const std::string& key, GB_Variant& outValue);

    /**
     * @brief 读取一个缓存项。
     *
     * 未命中时返回空 GB_Variant。若需要区分“未命中”和“命中但值为空”，应使用 TryGet()。
     */
    GB_Variant Get(const std::string& key);

    /**
     * @brief 读取一个缓存项，未命中时返回指定默认值。
     */
    GB_Variant GetOrDefault(const std::string& key, const GB_Variant& defaultValue);

    /**
     * @brief 尝试只读查看一个缓存项。
     *
     * 不更新命中统计，也不刷新 LRU / LFU 访问元数据。
     */
    bool TryPeek(const std::string& key, GB_Variant& outValue) const;

    /**
     * @brief 只读查看一个缓存项。
     *
     * 未命中时返回空 GB_Variant。若需要区分“未命中”和“命中但值为空”，应使用 TryPeek()。
     */
    GB_Variant Peek(const std::string& key) const;

    /**
     * @brief 只读查看一个缓存项，未命中时返回指定默认值。
     */
    GB_Variant PeekOrDefault(const std::string& key, const GB_Variant& defaultValue) const;

    /**
     * @brief 判断缓存中是否存在指定键。
     */
    bool Contains(const std::string& key) const;

    /**
     * @brief 删除指定缓存项。
     */
    bool Erase(const std::string& key);

    /**
     * @brief 删除所有缓存项。
     */
    void Clear();

    /**
     * @brief 取出指定缓存项并从缓存中删除。
     *
     * 该接口会把缓存项移动到输出参数中，适合取出大对象。
     */
    bool Take(const std::string& key, GB_Variant& outValue);

    /**
     * @brief 获取当前缓存项数量。
     */
    std::size_t Size() const;

    /**
     * @brief 判断当前缓存是否为空。
     */
    bool IsEmpty() const;

    /**
     * @brief 获取当前逻辑值字节数总和。
     */
    std::size_t GetCurrentBytes() const;

    /**
     * @brief 获取最大逻辑值字节数，0 表示不限制。
     */
    std::size_t GetMaxBytes() const;

    /**
     * @brief 设置最大逻辑值字节数，可能触发淘汰。
     */
    void SetMaxBytes(std::size_t maxBytes);

    /**
     * @brief 获取最大缓存项数量，0 表示不限制。
     */
    std::size_t GetMaxCount() const;

    /**
     * @brief 设置最大缓存项数量，可能触发淘汰。
     */
    void SetMaxCount(std::size_t maxCount);

    /**
     * @brief 获取当前缓存策略。
     */
    Policy GetPolicy() const;

    /**
     * @brief 获取当前统计信息。
     */
    Stats GetStats() const;

    /**
     * @brief 重置统计信息，不影响缓存内容。
     */
    void ResetStats();

    /**
     * @brief 预留底层哈希表容量。
     */
    void Reserve(std::size_t count);

    /**
     * @brief 获取当前所有键。
     *
     * 返回顺序不承诺与淘汰顺序一致。
     */
    std::vector<std::string> GetKeys() const;

    /**
     * @brief 获取指定缓存项的逻辑值字节数。
     */
    bool TryGetValueBytes(const std::string& key, std::size_t& valueBytes) const;

    /**
     * @brief 获取指定缓存项的类型名称。
     */
    bool TryGetTypeName(const std::string& key, std::string& typeName) const;

    /**
     * @brief 尝试按精确类型读取缓存项。
     *
     * 该接口调用 GB_Variant::AnyCast()，只做精确类型匹配，不做数值或字符串转换。
     */
    template<typename TValue>
    bool TryGetExact(const std::string& key, TValue& outValue)
    {
        GB_Variant value;
        if (!TryGet(key, value))
        {
            return false;
        }

        return value.AnyCast<TValue>(outValue);
    }

    /**
     * @brief 尝试按精确类型只读查看缓存项。
     *
     * 该接口不更新命中统计，也不刷新 LRU / LFU 访问元数据。
     */
    template<typename TValue>
    bool TryPeekExact(const std::string& key, TValue& outValue) const
    {
        GB_Variant value;
        if (!TryPeek(key, value))
        {
            return false;
        }

        return value.AnyCast<TValue>(outValue);
    }

private:
    struct Entry
    {
        GB_Variant value;
        std::size_t bytes = 0;

        std::list<std::string>::iterator orderIt;
        bool hasOrderIt = false;

        std::size_t freq = 0;
        std::list<std::string>::iterator freqIt;
        bool hasFreqIt = false;
    };

private:
    std::size_t EstimateValueBytes(const GB_Variant& value) const;
    bool EnsureCapacityFor(std::size_t incomingBytes, std::size_t incomingCount, const std::string* protectedKey);
    bool EnsureCapacityForReplacement(std::size_t oldBytes, std::size_t newBytes, const std::string& protectedKey);
    bool EvictOne(const std::string* protectedKey);
    void RemoveEntry(const std::string& key);

private:
    void OnInsert(const std::string& key, Entry& entry);
    void OnAccess(const std::string& key, Entry& entry);
    void OnErase(const std::string& key, Entry& entry);
    void OnClear();

    bool PickVictimKey(std::string& victimKey, const std::string* protectedKey);

private:
    Options options_;
    Stats stats_;

    std::size_t currentBytes_;
    std::unordered_map<std::string, Entry> entries_;

    std::list<std::string> orderList_;
    std::unordered_map<std::size_t, std::list<std::string>> freqToKeys_;
    std::size_t minFreq_;

    std::mt19937 rng_;
    mutable GB_ReadWriteLock lock_;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
