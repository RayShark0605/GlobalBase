#include "GB_SpatialIndex.h"
#include "GB_Polygon.h"
#include "GB_Polyline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <memory>
#include <set>
#include <thread>
#include <utility>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4127)
#  pragma warning(disable: 4244)
#  pragma warning(disable: 4267)
#  pragma warning(disable: 4512)
#  pragma warning(disable: 4819)
#endif

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

namespace
{
    namespace bg = boost::geometry;
    namespace bgi = boost::geometry::index;

    typedef bg::model::point<double, 2, bg::cs::cartesian> BoostPoint;
    typedef bg::model::box<BoostPoint> BoostBox;
    typedef std::pair<BoostBox, std::size_t> TreeValue;

    bool IsFinite(double value)
    {
        return std::isfinite(value);
    }

    bool IsValidRange(const GB_Rectangle& range)
    {
        return range.IsValid() && IsFinite(range.minX) && IsFinite(range.minY) && IsFinite(range.maxX) && IsFinite(range.maxY);
    }

    BoostBox ToBoostBox(const GB_Rectangle& range)
    {
        return BoostBox(BoostPoint(range.minX, range.minY), BoostPoint(range.maxX, range.maxY));
    }

    GB_Rectangle MakeQueryRange(const GB_Rectangle& range, double tolerance)
    {
        if (!IsValidRange(range))
        {
            return GB_Rectangle::Invalid;
        }

        if (tolerance <= 0.0 || !std::isfinite(tolerance))
        {
            return range;
        }

        return range.Buffered(tolerance, tolerance);
    }

    bool MatchRelation(const GB_Rectangle& recordRange, const GB_Rectangle& queryRange, GB_SpatialIndex::QueryRelation relation, double tolerance)
    {
        if (!IsValidRange(recordRange) || !IsValidRange(queryRange))
        {
            return false;
        }

        switch (relation)
        {
        case GB_SpatialIndex::QueryRelation::Intersects:
            return recordRange.IsIntersects(queryRange, tolerance);
        case GB_SpatialIndex::QueryRelation::CoveredByQuery:
            return queryRange.IsContains(recordRange, tolerance);
        case GB_SpatialIndex::QueryRelation::ContainsQuery:
            return recordRange.IsContains(queryRange, tolerance);
        default:
            return false;
        }
    }

    std::size_t GetHardwareThreadCount()
    {
        const unsigned int hardwareThreadCount = std::thread::hardware_concurrency();
        if (hardwareThreadCount == 0)
        {
            return 1;
        }

        return static_cast<std::size_t>(hardwareThreadCount);
    }

    std::size_t GetBuildThreadCount(const GB_SpatialIndex::BuildOptions& options, std::size_t recordCount)
    {
        if (recordCount < options.parallelBuildThreshold)
        {
            return 1;
        }

        std::size_t threadCount = options.buildThreadCount;
        if (threadCount == 0)
        {
            threadCount = GetHardwareThreadCount();
        }

        if (threadCount < 1)
        {
            threadCount = 1;
        }

        if (threadCount > recordCount)
        {
            threadCount = recordCount;
        }

        return threadCount;
    }

    template<typename TValue, typename... TArgs>
    std::unique_ptr<TValue> MakeUnique(TArgs&&... args)
    {
        return std::unique_ptr<TValue>(new TValue(std::forward<TArgs>(args)...));
    }

    class LimitedIndexOutputIterator
    {
    public:
        typedef std::output_iterator_tag iterator_category;
        typedef void value_type;
        typedef void difference_type;
        typedef void pointer;
        typedef void reference;

        LimitedIndexOutputIterator(std::vector<std::size_t>& outputIndexes, std::size_t maxCount)
            : outputIndexes_(&outputIndexes), maxCount_(maxCount)
        {
        }

        LimitedIndexOutputIterator& operator=(const TreeValue& value)
        {
            if (outputIndexes_ == nullptr)
            {
                return *this;
            }

            if (maxCount_ == 0 || outputIndexes_->size() < maxCount_)
            {
                outputIndexes_->push_back(value.second);
            }

            return *this;
        }

        LimitedIndexOutputIterator& operator*()
        {
            return *this;
        }

        LimitedIndexOutputIterator& operator++()
        {
            return *this;
        }

        LimitedIndexOutputIterator operator++(int)
        {
            return *this;
        }

    private:
        std::vector<std::size_t>* outputIndexes_ = nullptr;
        std::size_t maxCount_ = 0;
    };

    class ITreeWrapper
    {
    public:
        virtual ~ITreeWrapper()
        {
        }

        virtual void QueryIntersects(const BoostBox& queryBox, std::vector<std::size_t>& outIndexes, std::size_t maxCount) const = 0;
    };

    template<typename TParameters>
    class TreeWrapper : public ITreeWrapper
    {
    public:
        explicit TreeWrapper(const std::vector<TreeValue>& values, const TParameters& parameters)
            : tree_(values.begin(), values.end(), parameters)
        {
        }

        virtual void QueryIntersects(const BoostBox& queryBox, std::vector<std::size_t>& outIndexes, std::size_t maxCount) const override
        {
            LimitedIndexOutputIterator outputIterator(outIndexes, maxCount);
            tree_.query(bgi::intersects(queryBox), outputIterator);
        }

    private:
        bgi::rtree<TreeValue, TParameters> tree_;
    };

    std::vector<TreeValue> BuildTreeValues(const std::vector<GB_SpatialIndex::Record>& records, const GB_SpatialIndex::BuildOptions& options)
    {
        std::vector<TreeValue> values;
        values.resize(records.size());

        if (records.empty())
        {
            return values;
        }

        const std::size_t threadCount = GetBuildThreadCount(options, records.size());
        if (threadCount <= 1)
        {
            for (std::size_t i = 0; i < records.size(); i++)
            {
                values[i] = TreeValue(ToBoostBox(records[i].range), i);
            }
            return values;
        }

        std::vector<std::thread> threads;
        threads.reserve(threadCount);

        for (std::size_t threadIndex = 0; threadIndex < threadCount; threadIndex++)
        {
            const std::size_t beginIndex = records.size() * threadIndex / threadCount;
            const std::size_t endIndex = records.size() * (threadIndex + 1) / threadCount;

            threads.push_back(std::thread([beginIndex, endIndex, &records, &values]()
                {
                    for (std::size_t i = beginIndex; i < endIndex; i++)
                    {
                        values[i] = TreeValue(ToBoostBox(records[i].range), i);
                    }
                }));
        }

        for (std::size_t i = 0; i < threads.size(); i++)
        {
            threads[i].join();
        }

        return values;
    }

    template<typename TParameters>
    std::unique_ptr<ITreeWrapper> CreateTreeWithParameters(const std::vector<TreeValue>& values, const TParameters& parameters)
    {
        return MakeUnique<TreeWrapper<TParameters>>(values, parameters);
    }

    template<std::size_t MaxElements>
    std::unique_ptr<ITreeWrapper> CreateTreeWithCapacity(const std::vector<TreeValue>& values, GB_SpatialIndex::SplitAlgorithm splitAlgorithm)
    {
        constexpr std::size_t minElements = (MaxElements < 4) ? 1 : (MaxElements / 4);

        switch (splitAlgorithm)
        {
        case GB_SpatialIndex::SplitAlgorithm::Linear:
            return CreateTreeWithParameters(values, bgi::linear<MaxElements, minElements>());
        case GB_SpatialIndex::SplitAlgorithm::Quadratic:
            return CreateTreeWithParameters(values, bgi::quadratic<MaxElements, minElements>());
        case GB_SpatialIndex::SplitAlgorithm::RStar:
        default:
            return CreateTreeWithParameters(values, bgi::rstar<MaxElements, minElements>());
        }
    }

    std::unique_ptr<ITreeWrapper> CreateTree(const std::vector<TreeValue>& values, const GB_SpatialIndex::BuildOptions& options)
    {
        switch (options.nodeCapacity)
        {
        case GB_SpatialIndex::NodeCapacity::Small8:
            return CreateTreeWithCapacity<8>(values, options.splitAlgorithm);
        case GB_SpatialIndex::NodeCapacity::Large32:
            return CreateTreeWithCapacity<32>(values, options.splitAlgorithm);
        case GB_SpatialIndex::NodeCapacity::Huge64:
            return CreateTreeWithCapacity<64>(values, options.splitAlgorithm);
        case GB_SpatialIndex::NodeCapacity::Normal16:
        default:
            return CreateTreeWithCapacity<16>(values, options.splitAlgorithm);
        }
    }

    std::vector<GB_SpatialIndex::Record> FilterRecords(const std::vector<GB_SpatialIndex::Record>& inputRecords, const GB_SpatialIndex::BuildOptions& options, GB_SpatialIndex::BuildStatistics* statistics)
    {
        std::vector<GB_SpatialIndex::Record> records;
        records.reserve(inputRecords.size());

        if (statistics != nullptr)
        {
            statistics->inputRecordCount = inputRecords.size();
        }

        for (std::size_t i = 0; i < inputRecords.size(); i++)
        {
            if (!inputRecords[i].IsValid())
            {
                if (statistics != nullptr)
                {
                    statistics->skippedInvalidRecordCount++;
                }

                if (!options.skipInvalidRecords)
                {
                    records.clear();
                    return records;
                }

                continue;
            }

            records.push_back(inputRecords[i]);
        }

        if (statistics != nullptr)
        {
            statistics->acceptedRecordCount = records.size();
        }

        return records;
    }

    std::vector<GB_SpatialIndex::Record> FilterRecords(std::vector<GB_SpatialIndex::Record>&& inputRecords, const GB_SpatialIndex::BuildOptions& options, GB_SpatialIndex::BuildStatistics* statistics)
    {
        std::vector<GB_SpatialIndex::Record> records;
        records.reserve(inputRecords.size());

        if (statistics != nullptr)
        {
            statistics->inputRecordCount = inputRecords.size();
        }

        for (std::size_t i = 0; i < inputRecords.size(); i++)
        {
            if (!inputRecords[i].IsValid())
            {
                if (statistics != nullptr)
                {
                    statistics->skippedInvalidRecordCount++;
                }

                if (!options.skipInvalidRecords)
                {
                    records.clear();
                    return records;
                }

                continue;
            }

            records.push_back(std::move(inputRecords[i]));
        }

        if (statistics != nullptr)
        {
            statistics->acceptedRecordCount = records.size();
        }

        return records;
    }

    thread_local std::vector<std::size_t> threadLocalCandidateIndexes;
}

struct GB_SpatialIndex::Impl
{
    std::vector<Record> records;
    std::unique_ptr<ITreeWrapper> tree;

    Impl()
    {
    }

    explicit Impl(std::vector<Record>&& inputRecords, const BuildOptions& options)
        : records(std::move(inputRecords))
    {
        if (records.empty())
        {
            return;
        }

        if (options.shrinkRecordsToFit)
        {
            records.shrink_to_fit();
        }

        const std::vector<TreeValue> values = BuildTreeValues(records, options);
        tree = CreateTree(values, options);
    }

    bool IsValid() const
    {
        return records.empty() || tree.get() != nullptr;
    }

    std::size_t Size() const
    {
        return records.size();
    }
};

GB_SpatialIndex::Record::Record()
{
}

GB_SpatialIndex::Record::Record(std::uint64_t id, const GB_Rectangle& range)
    : id(id), range(range)
{
}

GB_SpatialIndex::Record::Record(std::uint64_t id, const GB_Rectangle& range, const GB_Variant& value)
    : id(id), range(range), value(value)
{
}

GB_SpatialIndex::Record::Record(std::uint64_t id, const GB_Rectangle& range, GB_Variant&& value)
    : id(id), range(range), value(std::move(value))
{
}

bool GB_SpatialIndex::Record::IsValid() const
{
    return IsValidRange(range);
}

GB_SpatialIndex::GB_SpatialIndex()
    : impl_(std::make_shared<Impl>())
{
}

GB_SpatialIndex::~GB_SpatialIndex()
{
}

GB_SpatialIndex::GB_SpatialIndex(const GB_SpatialIndex& other)
{
    StoreSnapshot(other.LoadSnapshot());
}

GB_SpatialIndex::GB_SpatialIndex(GB_SpatialIndex&& other) noexcept
{
    std::lock_guard<std::mutex> otherLockGuard(other.writeMutex_);
    StoreSnapshot(other.LoadSnapshot());
    other.StoreSnapshot(std::make_shared<Impl>());
}

GB_SpatialIndex& GB_SpatialIndex::operator=(const GB_SpatialIndex& other)
{
    if (this == &other)
    {
        return *this;
    }

    std::shared_ptr<const Impl> otherSnapshot = other.LoadSnapshot();
    std::lock_guard<std::mutex> lockGuard(writeMutex_);
    StoreSnapshot(otherSnapshot);
    return *this;
}

GB_SpatialIndex& GB_SpatialIndex::operator=(GB_SpatialIndex&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    std::unique_lock<std::mutex> lockGuard(writeMutex_, std::defer_lock);
    std::unique_lock<std::mutex> otherLockGuard(other.writeMutex_, std::defer_lock);
    std::lock(lockGuard, otherLockGuard);

    StoreSnapshot(other.LoadSnapshot());
    other.StoreSnapshot(std::make_shared<Impl>());
    return *this;
}

std::shared_ptr<const GB_SpatialIndex::Impl> GB_SpatialIndex::LoadSnapshot() const
{
    return std::atomic_load_explicit(&impl_, std::memory_order_acquire);
}

void GB_SpatialIndex::StoreSnapshot(const std::shared_ptr<const Impl>& impl)
{
    std::atomic_store_explicit(&impl_, impl, std::memory_order_release);
}

void GB_SpatialIndex::Clear()
{
    std::lock_guard<std::mutex> lockGuard(writeMutex_);
    StoreSnapshot(std::make_shared<Impl>());
}

bool GB_SpatialIndex::IsEmpty() const
{
    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    return snapshot == nullptr || snapshot->records.empty();
}

std::size_t GB_SpatialIndex::Size() const
{
    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    if (snapshot == nullptr)
    {
        return 0;
    }

    return snapshot->Size();
}

bool GB_SpatialIndex::IsValid() const
{
    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    return snapshot != nullptr && snapshot->IsValid();
}

bool GB_SpatialIndex::Build(const std::vector<Record>& records, const BuildOptions& options, BuildStatistics* outStatistics)
{
    std::lock_guard<std::mutex> lockGuard(writeMutex_);

    BuildStatistics statistics;
    std::vector<Record> filteredRecords = FilterRecords(records, options, &statistics);

    if (!options.skipInvalidRecords && statistics.skippedInvalidRecordCount > 0)
    {
        statistics.succeeded = false;
        if (outStatistics != nullptr)
        {
            *outStatistics = statistics;
        }
        return false;
    }

    std::shared_ptr<const Impl> newImpl = std::make_shared<Impl>(std::move(filteredRecords), options);
    if (newImpl == nullptr || !newImpl->IsValid())
    {
        statistics.succeeded = false;
        if (outStatistics != nullptr)
        {
            *outStatistics = statistics;
        }
        return false;
    }

    StoreSnapshot(newImpl);

    statistics.succeeded = true;
    if (outStatistics != nullptr)
    {
        *outStatistics = statistics;
    }

    return true;
}

bool GB_SpatialIndex::Build(std::vector<Record>&& records, const BuildOptions& options, BuildStatistics* outStatistics)
{
    std::lock_guard<std::mutex> lockGuard(writeMutex_);

    BuildStatistics statistics;
    std::vector<Record> filteredRecords = FilterRecords(std::move(records), options, &statistics);

    if (!options.skipInvalidRecords && statistics.skippedInvalidRecordCount > 0)
    {
        statistics.succeeded = false;
        if (outStatistics != nullptr)
        {
            *outStatistics = statistics;
        }
        return false;
    }

    std::shared_ptr<const Impl> newImpl = std::make_shared<Impl>(std::move(filteredRecords), options);
    if (newImpl == nullptr || !newImpl->IsValid())
    {
        statistics.succeeded = false;
        if (outStatistics != nullptr)
        {
            *outStatistics = statistics;
        }
        return false;
    }

    StoreSnapshot(newImpl);

    statistics.succeeded = true;
    if (outStatistics != nullptr)
    {
        *outStatistics = statistics;
    }

    return true;
}

bool GB_SpatialIndex::Insert(const Record& record, const BuildOptions& options)
{
    if (!record.IsValid())
    {
        return false;
    }

    std::lock_guard<std::mutex> lockGuard(writeMutex_);

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    std::vector<Record> records;
    if (snapshot != nullptr)
    {
        records = snapshot->records;
    }

    records.push_back(record);
    StoreSnapshot(std::make_shared<Impl>(std::move(records), options));
    return true;
}

bool GB_SpatialIndex::Insert(Record&& record, const BuildOptions& options)
{
    if (!record.IsValid())
    {
        return false;
    }

    std::lock_guard<std::mutex> lockGuard(writeMutex_);

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    std::vector<Record> records;
    if (snapshot != nullptr)
    {
        records = snapshot->records;
    }

    records.push_back(std::move(record));
    StoreSnapshot(std::make_shared<Impl>(std::move(records), options));
    return true;
}

bool GB_SpatialIndex::Insert(const std::vector<Record>& inputRecords, const BuildOptions& options, BuildStatistics* outStatistics)
{
    std::lock_guard<std::mutex> lockGuard(writeMutex_);

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    std::vector<Record> records;
    if (snapshot != nullptr)
    {
        records = snapshot->records;
    }

    records.reserve(records.size() + inputRecords.size());
    records.insert(records.end(), inputRecords.begin(), inputRecords.end());

    BuildStatistics statistics;
    std::vector<Record> filteredRecords = FilterRecords(std::move(records), options, &statistics);
    if (!options.skipInvalidRecords && statistics.skippedInvalidRecordCount > 0)
    {
        statistics.succeeded = false;
        if (outStatistics != nullptr)
        {
            *outStatistics = statistics;
        }
        return false;
    }

    StoreSnapshot(std::make_shared<Impl>(std::move(filteredRecords), options));

    statistics.succeeded = true;
    if (outStatistics != nullptr)
    {
        *outStatistics = statistics;
    }

    return true;
}

bool GB_SpatialIndex::Insert(std::vector<Record>&& inputRecords, const BuildOptions& options, BuildStatistics* outStatistics)
{
    std::lock_guard<std::mutex> lockGuard(writeMutex_);

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    std::vector<Record> records;
    if (snapshot != nullptr)
    {
        records = snapshot->records;
    }

    records.reserve(records.size() + inputRecords.size());
    for (std::size_t i = 0; i < inputRecords.size(); i++)
    {
        records.push_back(std::move(inputRecords[i]));
    }

    BuildStatistics statistics;
    std::vector<Record> filteredRecords = FilterRecords(std::move(records), options, &statistics);
    if (!options.skipInvalidRecords && statistics.skippedInvalidRecordCount > 0)
    {
        statistics.succeeded = false;
        if (outStatistics != nullptr)
        {
            *outStatistics = statistics;
        }
        return false;
    }

    StoreSnapshot(std::make_shared<Impl>(std::move(filteredRecords), options));

    statistics.succeeded = true;
    if (outStatistics != nullptr)
    {
        *outStatistics = statistics;
    }

    return true;
}

std::size_t GB_SpatialIndex::RemoveById(std::uint64_t id, const BuildOptions& options)
{
    std::lock_guard<std::mutex> lockGuard(writeMutex_);

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    if (snapshot == nullptr || snapshot->records.empty())
    {
        return 0;
    }

    std::vector<Record> records;
    records.reserve(snapshot->records.size());

    std::size_t removedCount = 0;
    for (std::size_t i = 0; i < snapshot->records.size(); i++)
    {
        if (snapshot->records[i].id == id)
        {
            removedCount++;
            continue;
        }

        records.push_back(snapshot->records[i]);
    }

    if (removedCount == 0)
    {
        return 0;
    }

    StoreSnapshot(std::make_shared<Impl>(std::move(records), options));
    return removedCount;
}

std::size_t GB_SpatialIndex::RemoveByIds(const std::vector<std::uint64_t>& ids, const BuildOptions& options)
{
    if (ids.empty())
    {
        return 0;
    }

    std::set<std::uint64_t> idSet(ids.begin(), ids.end());
    std::lock_guard<std::mutex> lockGuard(writeMutex_);

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    if (snapshot == nullptr || snapshot->records.empty())
    {
        return 0;
    }

    std::vector<Record> records;
    records.reserve(snapshot->records.size());

    std::size_t removedCount = 0;
    for (std::size_t i = 0; i < snapshot->records.size(); i++)
    {
        if (idSet.find(snapshot->records[i].id) != idSet.end())
        {
            removedCount++;
            continue;
        }

        records.push_back(snapshot->records[i]);
    }

    if (removedCount == 0)
    {
        return 0;
    }

    StoreSnapshot(std::make_shared<Impl>(std::move(records), options));
    return removedCount;
}

std::vector<GB_SpatialIndex::Record> GB_SpatialIndex::GetRecords() const
{
    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    if (snapshot == nullptr)
    {
        return std::vector<Record>();
    }

    return snapshot->records;
}

std::vector<GB_SpatialIndex::QueryResult> GB_SpatialIndex::QueryRecords(const GB_Rectangle& range, QueryRelation relation, const QueryOptions& options) const
{
    return QueryRecords(range, RecordFilter(), relation, options);
}

std::vector<GB_SpatialIndex::QueryResult> GB_SpatialIndex::QueryRecords(const GB_Polygon& polygon, QueryRelation relation, const QueryOptions& options) const
{
    return QueryRecords(polygon.GetBoundingBox(), relation, options);
}

std::vector<GB_SpatialIndex::QueryResult> GB_SpatialIndex::QueryRecords(const GB_Polyline& polyline, QueryRelation relation, const QueryOptions& options) const
{
    return QueryRecords(polyline.BoundingRectangle(), relation, options);
}

std::vector<std::uint64_t> GB_SpatialIndex::QueryIds(const GB_Rectangle& range, QueryRelation relation, const QueryOptions& options) const
{
    return QueryIds(range, RecordFilter(), relation, options);
}

std::vector<std::uint64_t> GB_SpatialIndex::QueryIds(const GB_Polygon& polygon, QueryRelation relation, const QueryOptions& options) const
{
    return QueryIds(polygon.GetBoundingBox(), relation, options);
}

std::vector<std::uint64_t> GB_SpatialIndex::QueryIds(const GB_Polyline& polyline, QueryRelation relation, const QueryOptions& options) const
{
    return QueryIds(polyline.BoundingRectangle(), relation, options);
}

std::vector<GB_SpatialIndex::QueryResult> GB_SpatialIndex::QueryRecords(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation, const QueryOptions& options) const
{
    std::vector<QueryResult> results;

    const GB_Rectangle queryRange = MakeQueryRange(range, options.tolerance);
    if (!IsValidRange(queryRange))
    {
        return results;
    }

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    if (snapshot == nullptr || snapshot->tree.get() == nullptr || snapshot->records.empty())
    {
        return results;
    }

    std::vector<std::size_t> localCandidateIndexes;
    std::vector<std::size_t>& candidateIndexes = options.useThreadLocalCache ? threadLocalCandidateIndexes : localCandidateIndexes;
    candidateIndexes.clear();

    snapshot->tree->QueryIntersects(ToBoostBox(queryRange), candidateIndexes, 0);

    results.reserve(candidateIndexes.size());
    for (std::size_t i = 0; i < candidateIndexes.size(); i++)
    {
        if (options.maxResults != 0 && results.size() >= options.maxResults)
        {
            break;
        }

        const std::size_t recordIndex = candidateIndexes[i];
        if (recordIndex >= snapshot->records.size())
        {
            continue;
        }

        const Record& record = snapshot->records[recordIndex];
        if (!MatchRelation(record.range, range, relation, options.tolerance))
        {
            continue;
        }

        if (filter && !filter(record))
        {
            continue;
        }

        QueryResult result;
        result.id = record.id;
        result.range = record.range;
        if (options.includeValue)
        {
            result.value = record.value;
        }

        results.push_back(std::move(result));
    }

    if (options.useThreadLocalCache && candidateIndexes.capacity() > options.maxThreadLocalCacheCapacity)
    {
        std::vector<std::size_t>().swap(candidateIndexes);
    }

    return results;
}

std::vector<std::uint64_t> GB_SpatialIndex::QueryIds(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation, const QueryOptions& options) const
{
    std::vector<std::uint64_t> results;

    const GB_Rectangle queryRange = MakeQueryRange(range, options.tolerance);
    if (!IsValidRange(queryRange))
    {
        return results;
    }

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    if (snapshot == nullptr || snapshot->tree.get() == nullptr || snapshot->records.empty())
    {
        return results;
    }

    std::vector<std::size_t> localCandidateIndexes;
    std::vector<std::size_t>& candidateIndexes = options.useThreadLocalCache ? threadLocalCandidateIndexes : localCandidateIndexes;
    candidateIndexes.clear();

    snapshot->tree->QueryIntersects(ToBoostBox(queryRange), candidateIndexes, 0);

    results.reserve(candidateIndexes.size());
    for (std::size_t i = 0; i < candidateIndexes.size(); i++)
    {
        if (options.maxResults != 0 && results.size() >= options.maxResults)
        {
            break;
        }

        const std::size_t recordIndex = candidateIndexes[i];
        if (recordIndex >= snapshot->records.size())
        {
            continue;
        }

        const Record& record = snapshot->records[recordIndex];
        if (!MatchRelation(record.range, range, relation, options.tolerance))
        {
            continue;
        }

        if (filter && !filter(record))
        {
            continue;
        }

        results.push_back(record.id);
    }

    if (options.useThreadLocalCache && candidateIndexes.capacity() > options.maxThreadLocalCacheCapacity)
    {
        std::vector<std::size_t>().swap(candidateIndexes);
    }

    return results;
}

GB_SpatialIndex::Record GB_SpatialIndex::MakeRecord(std::uint64_t id, const GB_Rectangle& range)
{
    return Record(id, range);
}

GB_SpatialIndex::Record GB_SpatialIndex::MakeRecord(std::uint64_t id, const GB_Rectangle& range, const GB_Variant& value)
{
    return Record(id, range, value);
}

GB_SpatialIndex::Record GB_SpatialIndex::MakeRecord(std::uint64_t id, const GB_Rectangle& range, GB_Variant&& value)
{
    return Record(id, range, std::move(value));
}
