#ifndef GLOBALBASE_SPATIAL_INDEX_H_H
#define GLOBALBASE_SPATIAL_INDEX_H_H

#include "../GlobalBasePort.h"
#include "../GB_BaseTypes.h"
#include "../GB_Math.h"
#include "../GB_Variant.h"
#include "GB_Rectangle.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

class GB_Polygon;
class GB_Polyline;

/**
 * @brief 二维空间索引树。
 *
 * @details
 * GB_SpatialIndex 面向“根据二维范围快速筛选对象”的工程场景：
 * - 每条记录以 GB_Rectangle 作为索引范围，以 uint64_t 作为稳定标识，以 GB_Variant 承载用户数据；
 * - 查询接口支持 GB_Rectangle、GB_Polygon、GB_Polyline，Polygon/Polyline 查询先使用轴对齐包围盒粗过滤，再基于真实几何做细过滤；
 * - Polygon 若本身是轴对齐矩形，会自动降级为矩形关系判断，避免不必要的逐边几何计算；
 * - 复杂 Polygon/Polyline 查询会为查询几何临时构建边索引，并对“记录范围完全包含查询包围盒”等常见场景做快速判定；
 * - Polygon 查询按面对象处理，少于 3 个有效顶点、显式首尾重复后仍退化、面积为 0 的输入会被视为无效查询；
 * - Polygon/Polyline 细过滤只判断“记录矩形范围”与查询几何的关系，不负责验证外部实体真实几何；
 * - Polyline 查询按线对象处理，所有有效顶点重合时按单点查询处理；
 * - 内部采用不可变快照（Copy-On-Write Snapshot）设计，查询线程只读取当前快照，不获取读写锁；
 * - 构建、插入、删除、清空属于写操作，会通过写互斥锁串行化，并在构建完成后一次性发布新快照；
 * - 适合千万级数据的批量构建和大量并发查询，少量动态写入可用 Insert/Remove，大量更新应优先重新批量 Build。
 *
 * 线程说明：
 * - 多个线程可以并发调用所有 const 查询接口；
 * - 多个线程可以与写接口并发调用，查询看到的是写入完成前或完成后的某一个完整快照；
 * - 写接口之间会串行执行；
 * - 调用方不要在 GB_Variant 中保存会被外部并发修改且没有自身同步保护的裸对象引用。
 */
class GLOBALBASE_PORT GB_SpatialIndex
{
public:
    /** @brief 内部 R 树节点分裂策略。 */
    enum class SplitAlgorithm : std::uint8_t
    {
        /** 插入代价较低，查询质量一般。 */
        Linear = 0,

        /** 插入和查询之间较均衡。 */
        Quadratic = 1,

        /** 默认策略，批量构建和查询质量通常更好。 */
        RStar = 2
    };

    /** @brief 节点容量档位。数值越大，树更矮但节点扫描成本更高。 */
    enum class NodeCapacity : std::uint8_t
    {
        Small8 = 8,
        Normal16 = 16,
        Large32 = 32,
        Huge64 = 64
    };

    /** @brief 空间关系查询模式。 */
    enum class QueryRelation : std::uint8_t
    {
        /** 记录范围与查询范围相交（含边界接触）。 */
        Intersects = 0,

        /** 记录范围完全位于查询范围内（含边界）。 */
        CoveredByQuery = 1,

        /** 记录范围完全包含查询范围（含边界）。 */
        ContainsQuery = 2
    };

    /** @brief 单条索引记录。 */
    struct GLOBALBASE_PORT Record
    {
        /** @brief 调用方维护的稳定标识。GB_SpatialIndex 不要求唯一，但 RemoveById 会删除所有同 id 记录。 */
        std::uint64_t id = 0;

        /** @brief 用于索引的二维范围。 */
        GB_Rectangle range;

        /** @brief 调用方数据。建议千万级数据优先只存 id，将大型对象放在外部数组或对象池中。 */
        GB_Variant value;

        Record();
        Record(std::uint64_t id, const GB_Rectangle& range);
        Record(std::uint64_t id, const GB_Rectangle& range, const GB_Variant& value);
        Record(std::uint64_t id, const GB_Rectangle& range, GB_Variant&& value);

        bool IsValid() const;
    };

    /** @brief 查询结果。 */
    struct QueryResult
    {
        std::uint64_t id = 0;
        GB_Rectangle range;
        GB_Variant value;
    };

    /** @brief 批量构建选项。 */
    struct BuildOptions
    {
        BuildOptions();

        /**
         * @brief 分裂策略。默认 RStar。
         *
         * @note 当前实现对写操作采用“过滤记录 + 批量重建 R 树 + 发布快照”的方式。
         *       批量构建时仍由 Boost.Geometry R-tree 通过范围构造完成 packing；
         *       分裂策略主要影响后续节点参数与少量动态变更后的树形质量。
         *       当前实现使用 MaxElements/2 作为节点最小填充量，优先保证查询阶段的节点质量。
         */
        SplitAlgorithm splitAlgorithm;

        /** @brief 节点容量。默认 16，通常适合二维范围查询。 */
        NodeCapacity nodeCapacity;

        /** @brief 是否跳过无效范围记录。为 false 时只要存在无效记录，Build 失败且不改变旧索引。 */
        bool skipInvalidRecords;

        /** @brief 是否在构建后对记录数组做 shrink_to_fit。千万级数据且后续不频繁写入时可设为 true。 */
        bool shrinkRecordsToFit;

        /** @brief 用于并行准备 R 树输入值的线程数。0 表示根据硬件并发数自动选择。 */
        std::size_t buildThreadCount;

        /** @brief 记录数达到该阈值才启用并行准备。 */
        std::size_t parallelBuildThreshold;

        /**
         * @brief 少量追加记录时是否优先复制旧 R 树并增量插入新节点。
         *
         * @details
         * 该选项只影响 Insert/批量追加，不影响 Build。启用后，如果旧索引的节点容量和分裂策略与本次 options 一致，
         * 且本次追加数量不超过 incrementalInsertThreshold，则避免全量 bulk rebuild。
         * 对“偶发少量追加”更快；对“大批量导入”仍应使用 Build 或让本阈值较小以保持 packing 树质量。
         */
        bool preferIncrementalInsert;

        /** @brief 追加记录数不超过该阈值时才尝试增量插入。0 表示禁用增量插入。 */
        std::size_t incrementalInsertThreshold;
    };

    /** @brief 查询选项。 */
    struct QueryOptions
    {
        QueryOptions();

        /** @brief 容差。大于 0 时会扩大候选范围并用 GB_Rectangle 做二次关系判断；Polygon/Polyline 查询也会在细过滤中使用该容差。 */
        double tolerance;

        /** @brief 最多返回数量。0 表示不限制。 */
        std::size_t maxResults;

        /** @brief QueryRecords 是否拷贝 GB_Variant。仅需要 id 时应调用 QueryIds，或把该值设为 false。 */
        bool includeValue;
    };

    /** @brief 构建统计信息。 */
    struct BuildStatistics
    {
        BuildStatistics();

        bool succeeded;
        std::size_t inputRecordCount;
        std::size_t acceptedRecordCount;
        std::size_t skippedInvalidRecordCount;
    };

    typedef std::function<bool(const Record& record)> RecordFilter;

    /** @brief 查询结果访问器。返回 false 表示立即停止继续遍历。 */
    typedef std::function<bool(const QueryResult& result)> QueryResultVisitor;

    /** @brief 查询 id 访问器。返回 false 表示立即停止继续遍历。 */
    typedef std::function<bool(std::uint64_t id)> IdVisitor;

    GB_SpatialIndex();
    ~GB_SpatialIndex();

    GB_SpatialIndex(const GB_SpatialIndex& other);
    GB_SpatialIndex(GB_SpatialIndex&& other) noexcept;
    GB_SpatialIndex& operator=(const GB_SpatialIndex& other);
    GB_SpatialIndex& operator=(GB_SpatialIndex&& other) noexcept;

    /** @brief 清空索引并发布空快照。 */
    void Clear();

    /** @brief 当前索引是否为空。 */
    bool IsEmpty() const;

    /** @brief 当前索引记录数。 */
    std::size_t Size() const;

    /** @brief 当前索引是否有可用快照。 */
    bool IsValid() const;

    /** @brief 拷贝记录并批量构建索引。 */
    bool Build(const std::vector<Record>& records, const BuildOptions& options = BuildOptions(), BuildStatistics* outStatistics = nullptr);

    /** @brief 移动记录并批量构建索引。适合千万级数据，避免额外拷贝 GB_Variant。 */
    bool Build(std::vector<Record>&& records, const BuildOptions& options = BuildOptions(), BuildStatistics* outStatistics = nullptr);

    /** @brief 插入单条记录。内部会基于旧记录重建新快照，适合少量写入，不适合海量逐条写入。 */
    bool Insert(const Record& record, const BuildOptions& options = BuildOptions());

    /** @brief 插入单条记录（移动版本）。 */
    bool Insert(Record&& record, const BuildOptions& options = BuildOptions());

    /** @brief 批量追加记录并重建新快照。 */
    bool Insert(const std::vector<Record>& records, const BuildOptions& options = BuildOptions(), BuildStatistics* outStatistics = nullptr);

    /** @brief 批量追加记录并重建新快照（移动版本）。 */
    bool Insert(std::vector<Record>&& records, const BuildOptions& options = BuildOptions(), BuildStatistics* outStatistics = nullptr);

    /** @brief 删除所有 id 等于指定值的记录。 */
    std::size_t RemoveById(std::uint64_t id, const BuildOptions& options = BuildOptions());

    /** @brief 删除所有 id 在 ids 中的记录。 */
    std::size_t RemoveByIds(const std::vector<std::uint64_t>& ids, const BuildOptions& options = BuildOptions());

    /** @brief 获取当前全部记录快照。会拷贝 GB_Variant，千万级数据慎用。 */
    std::vector<Record> GetRecords() const;

    /** @brief 基于矩形范围查询记录。 */
    std::vector<QueryResult> QueryRecords(const GB_Rectangle& range, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于多边形真实几何查询记录。内部先按包围盒粗过滤，再做多边形细过滤。 */
    std::vector<QueryResult> QueryRecords(const GB_Polygon& polygon, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于多段线真实几何查询记录。内部先按包围盒粗过滤，再做多段线细过滤。 */
    std::vector<QueryResult> QueryRecords(const GB_Polyline& polyline, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于矩形范围查询 id。 */
    std::vector<std::uint64_t> QueryIds(const GB_Rectangle& range, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于多边形真实几何查询 id。内部先按包围盒粗过滤，再做多边形细过滤。 */
    std::vector<std::uint64_t> QueryIds(const GB_Polygon& polygon, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于多段线真实几何查询 id。内部先按包围盒粗过滤，再做多段线细过滤。 */
    std::vector<std::uint64_t> QueryIds(const GB_Polyline& polyline, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于矩形范围查询，并使用调用方过滤器做二次过滤。 */
    std::vector<QueryResult> QueryRecords(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于矩形范围查询 id，并使用调用方过滤器做二次过滤。 */
    std::vector<std::uint64_t> QueryIds(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于矩形范围流式访问记录，避免构造大结果数组。visitor 返回 false 时立即停止。 */
    std::size_t ForEachRecord(const GB_Rectangle& range, const QueryResultVisitor& visitor, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于多边形真实几何流式访问记录，避免构造大结果数组。visitor 返回 false 时立即停止。 */
    std::size_t ForEachRecord(const GB_Polygon& polygon, const QueryResultVisitor& visitor, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于多段线真实几何流式访问记录，避免构造大结果数组。visitor 返回 false 时立即停止。 */
    std::size_t ForEachRecord(const GB_Polyline& polyline, const QueryResultVisitor& visitor, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于矩形范围流式访问记录，并使用调用方过滤器做二次过滤。visitor 返回 false 时立即停止。 */
    std::size_t ForEachRecord(const GB_Rectangle& range, const RecordFilter& filter, const QueryResultVisitor& visitor, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于矩形范围流式访问 id，避免构造大结果数组。visitor 返回 false 时立即停止。 */
    std::size_t ForEachId(const GB_Rectangle& range, const IdVisitor& visitor, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于多边形真实几何流式访问 id，避免构造大结果数组。visitor 返回 false 时立即停止。 */
    std::size_t ForEachId(const GB_Polygon& polygon, const IdVisitor& visitor, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于多段线真实几何流式访问 id，避免构造大结果数组。visitor 返回 false 时立即停止。 */
    std::size_t ForEachId(const GB_Polyline& polyline, const IdVisitor& visitor, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于矩形范围流式访问 id，并使用调用方过滤器做二次过滤。visitor 返回 false 时立即停止。 */
    std::size_t ForEachId(const GB_Rectangle& range, const RecordFilter& filter, const IdVisitor& visitor, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于矩形范围统计命中数量。受 relation、filter、tolerance 和 maxResults 约束。 */
    std::size_t QueryCount(const GB_Rectangle& range, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于多边形真实几何统计命中数量。受 relation、tolerance 和 maxResults 约束。 */
    std::size_t QueryCount(const GB_Polygon& polygon, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于多段线真实几何统计命中数量。受 relation、tolerance 和 maxResults 约束。 */
    std::size_t QueryCount(const GB_Polyline& polyline, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 基于矩形范围统计命中数量，并使用调用方过滤器做二次过滤。受 relation、filter、tolerance 和 maxResults 约束。 */
    std::size_t QueryCount(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation = QueryRelation::Intersects, const QueryOptions& options = QueryOptions()) const;

    /** @brief 便捷构造记录。 */
    static Record MakeRecord(std::uint64_t id, const GB_Rectangle& range);

    /** @brief 便捷构造记录。 */
    static Record MakeRecord(std::uint64_t id, const GB_Rectangle& range, const GB_Variant& value);

    /** @brief 便捷构造记录。 */
    static Record MakeRecord(std::uint64_t id, const GB_Rectangle& range, GB_Variant&& value);

    /** @brief 便捷构造记录。 */
    template<typename TValue>
    static Record MakeRecord(std::uint64_t id, const GB_Rectangle& range, TValue&& value)
    {
        return Record(id, range, GB_Variant(std::forward<TValue>(value)));
    }

    /** @brief 便捷插入任意可被 GB_Variant 承载的数据。 */
    template<typename TValue>
    bool Insert(std::uint64_t id, const GB_Rectangle& range, TValue&& value, const BuildOptions& options = BuildOptions())
    {
        return Insert(MakeRecord(id, range, std::forward<TValue>(value)), options);
    }

private:
    struct Impl;

    std::shared_ptr<const Impl> LoadSnapshot() const;
    void StoreSnapshot(const std::shared_ptr<const Impl>& impl);
    static std::shared_ptr<const Impl> CreateImplByAppendingRecords(const std::shared_ptr<const Impl>& snapshot, std::vector<Record>&& newRecords, const BuildOptions& options);

    std::vector<QueryResult> QueryRecordsInternal(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation, const QueryOptions& options, bool filterHandlesRelation) const;
    std::vector<std::uint64_t> QueryIdsInternal(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation, const QueryOptions& options, bool filterHandlesRelation) const;
    std::size_t ForEachRecordInternal(const GB_Rectangle& range, const RecordFilter& filter, const QueryResultVisitor& visitor, QueryRelation relation, const QueryOptions& options, bool filterHandlesRelation) const;
    std::size_t ForEachIdInternal(const GB_Rectangle& range, const RecordFilter& filter, const IdVisitor& visitor, QueryRelation relation, const QueryOptions& options, bool filterHandlesRelation) const;

private:
    mutable std::mutex writeMutex_;
    std::shared_ptr<const Impl> impl_;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
