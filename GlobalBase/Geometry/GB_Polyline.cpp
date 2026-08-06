#include "GB_Polyline.h"
#include "GB_LineSegment.h"
#include "GB_Matrix3x3.h"
#include "GB_Polygon.h"
#include "GB_Rectangle.h"
#include "GB_Vector2d.h"
#include "../GB_IO.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>

namespace
{
    constexpr static size_t BvhBuildSegmentThreshold = 64;
    constexpr static size_t BvhLeafSegmentCount = 16;
    constexpr static size_t InvalidBvhNodeIndex = std::numeric_limits<size_t>::max();

    static inline double AbsTol(double tolerance)
    {
        if (!std::isfinite(tolerance))
        {
            return 0.0;
        }
        return std::abs(tolerance);
    }

    static inline GB_Point2d MakeNanPoint()
    {
        return GB_Point2d(GB_QuietNan, GB_QuietNan);
    }

    static inline bool AreVerticesFinite(const std::vector<GB_Point2d>& vertices)
    {
        for (const GB_Point2d& vertex : vertices)
        {
            if (!vertex.IsValid())
            {
                return false;
            }
        }

        return true;
    }

    static inline bool AreVerticesValid(const std::vector<GB_Point2d>& vertices)
    {
        return vertices.size() >= 2 && AreVerticesFinite(vertices);
    }

    static inline double LengthCompareTolerance(double totalLength)
    {
        if (!std::isfinite(totalLength))
        {
            return std::numeric_limits<double>::epsilon() * 64.0;
        }

        return std::max(std::numeric_limits<double>::epsilon() * 64.0, std::abs(totalLength) * 1e-12);
    }

    static inline bool AreDistancesNearEqual(double distance1, double distance2, double tolerance)
    {
        if (!std::isfinite(distance1) || !std::isfinite(distance2))
        {
            return false;
        }

        return std::abs(distance1 - distance2) <= tolerance;
    }

    static inline bool IsBetterDistanceCandidate(double candidateDistanceSquared, size_t candidateSegmentIndex, double bestDistanceSquared, size_t bestSegmentIndex)
    {
        if (!std::isfinite(candidateDistanceSquared))
        {
            return false;
        }

        if (!std::isfinite(bestDistanceSquared))
        {
            return true;
        }

        if (candidateDistanceSquared < bestDistanceSquared)
        {
            return true;
        }

        const double distanceScale = std::max(1.0, std::max(std::abs(candidateDistanceSquared), std::abs(bestDistanceSquared)));
        const double compareTolerance = std::numeric_limits<double>::epsilon() * 64.0 * distanceScale;
        return std::abs(candidateDistanceSquared - bestDistanceSquared) <= compareTolerance && candidateSegmentIndex < bestSegmentIndex;
    }

    static inline double Clamp01(double value)
    {
        if (!std::isfinite(value))
        {
            return 0.0;
        }
        if (value < 0.0)
        {
            return 0.0;
        }
        if (value > 1.0)
        {
            return 1.0;
        }
        return value;
    }

    static inline double ClampDistance(double distance, double totalLength)
    {
        if (!std::isfinite(distance))
        {
            return 0.0;
        }
        if (distance < 0.0)
        {
            return 0.0;
        }
        if (distance > totalLength)
        {
            return totalLength;
        }
        return distance;
    }

    static inline GB_Rectangle SegmentBoundingRectangle(const GB_Point2d& point1, const GB_Point2d& point2)
    {
        return GB_Rectangle(point1, point2);
    }

    static inline double SquaredLengthFromComponents(double deltaX, double deltaY)
    {
        if (!std::isfinite(deltaX) || !std::isfinite(deltaY))
        {
            if (std::isnan(deltaX) || std::isnan(deltaY))
            {
                return GB_QuietNan;
            }

            return std::numeric_limits<double>::max();
        }

        const double scale = std::max(std::abs(deltaX), std::abs(deltaY));
        if (scale <= 0.0)
        {
            return 0.0;
        }

        const double scaledX = deltaX / scale;
        const double scaledY = deltaY / scale;
        const double scaledLengthSquared = scaledX * scaledX + scaledY * scaledY;
        if (!std::isfinite(scaledLengthSquared) || scaledLengthSquared <= 0.0)
        {
            return 0.0;
        }

        const double maxSafeScale = std::sqrt(std::numeric_limits<double>::max() / scaledLengthSquared);
        if (scale >= maxSafeScale)
        {
            return std::numeric_limits<double>::max();
        }

        return scale * scale * scaledLengthSquared;
    }

    static inline double RectangleDistanceToPointSquared(const GB_Rectangle& rect, const GB_Point2d& point)
    {
        if (!rect.IsValid() || !point.IsValid())
        {
            return GB_QuietNan;
        }

        double dx = 0.0;
        if (point.x < rect.minX)
        {
            dx = rect.minX - point.x;
        }
        else if (point.x > rect.maxX)
        {
            dx = point.x - rect.maxX;
        }

        double dy = 0.0;
        if (point.y < rect.minY)
        {
            dy = rect.minY - point.y;
        }
        else if (point.y > rect.maxY)
        {
            dy = point.y - rect.maxY;
        }

        return SquaredLengthFromComponents(dx, dy);
    }

    static inline bool RectangleContainsPointWithTolerance(const GB_Rectangle& rect, const GB_Point2d& point, double tolerance)
    {
        if (!rect.IsValid() || !point.IsValid())
        {
            return false;
        }

        const double absTolerance = AbsTol(tolerance);
        return point.x >= rect.minX - absTolerance && point.x <= rect.maxX + absTolerance && point.y >= rect.minY - absTolerance && point.y <= rect.maxY + absTolerance;
    }

    static inline double PointDistanceSquared(const GB_Point2d& point1, const GB_Point2d& point2)
    {
        if (!point1.IsValid() || !point2.IsValid())
        {
            return GB_QuietNan;
        }

        return SquaredLengthFromComponents(point1.x - point2.x, point1.y - point2.y);
    }

    static inline double PointDistance(const GB_Point2d& point1, const GB_Point2d& point2)
    {
        if (!point1.IsValid() || !point2.IsValid())
        {
            return GB_QuietNan;
        }

        const double deltaX = point1.x - point2.x;
        const double deltaY = point1.y - point2.y;
        if (!std::isfinite(deltaX) || !std::isfinite(deltaY))
        {
            if (std::isnan(deltaX) || std::isnan(deltaY))
            {
                return GB_QuietNan;
            }

            return std::numeric_limits<double>::max();
        }

        return std::hypot(deltaX, deltaY);
    }

    static inline double PointToSegmentDistanceSquared(const GB_Point2d& point, const GB_Point2d& segmentStart, const GB_Point2d& segmentEnd, double& outSegmentParameter, GB_Point2d& outClosestPoint)
    {
        outSegmentParameter = GB_QuietNan;
        outClosestPoint = MakeNanPoint();

        if (!point.IsValid() || !segmentStart.IsValid() || !segmentEnd.IsValid())
        {
            return GB_QuietNan;
        }

        const double vectorX = segmentEnd.x - segmentStart.x;
        const double vectorY = segmentEnd.y - segmentStart.y;
        const double pointVectorX = point.x - segmentStart.x;
        const double pointVectorY = point.y - segmentStart.y;

        if (!std::isfinite(vectorX) || !std::isfinite(vectorY) || !std::isfinite(pointVectorX) || !std::isfinite(pointVectorY))
        {
            return GB_QuietNan;
        }

        const double scale = std::max(std::abs(vectorX), std::abs(vectorY));
        if (scale <= 0.0)
        {
            outSegmentParameter = 0.0;
            outClosestPoint = segmentStart;
            return PointDistanceSquared(point, segmentStart);
        }

        const double scaledVectorX = vectorX / scale;
        const double scaledVectorY = vectorY / scale;
        const double scaledPointVectorX = pointVectorX / scale;
        const double scaledPointVectorY = pointVectorY / scale;
        const double scaledLengthSquared = scaledVectorX * scaledVectorX + scaledVectorY * scaledVectorY;
        if (!std::isfinite(scaledLengthSquared) || scaledLengthSquared <= 0.0)
        {
            outSegmentParameter = 0.0;
            outClosestPoint = segmentStart;
            return PointDistanceSquared(point, segmentStart);
        }

        double parameter = (scaledPointVectorX * scaledVectorX + scaledPointVectorY * scaledVectorY) / scaledLengthSquared;
        parameter = Clamp01(parameter);

        outSegmentParameter = parameter;
        outClosestPoint = GB_Point2d(segmentStart.x + vectorX * parameter, segmentStart.y + vectorY * parameter);
        return PointDistanceSquared(point, outClosestPoint);
    }

    static inline double SquaredTolerance(double tolerance)
    {
        if (!std::isfinite(tolerance) || tolerance <= 0.0)
        {
            return 0.0;
        }

        const double maxSafeTolerance = std::sqrt(std::numeric_limits<double>::max());
        if (tolerance >= maxSafeTolerance)
        {
            return std::numeric_limits<double>::infinity();
        }

        return tolerance * tolerance;
    }

    static inline bool PointsAreNearEqual(const GB_Point2d& point1, const GB_Point2d& point2, double tolerance)
    {
        if (!point1.IsValid() || !point2.IsValid())
        {
            return false;
        }

        return PointDistanceSquared(point1, point2) <= SquaredTolerance(tolerance);
    }

    static inline bool TryParseSizeT(const std::string& text, size_t& value)
    {
        if (text.empty())
        {
            return false;
        }

        size_t result = 0;
        for (char ch : text)
        {
            if (ch < '0' || ch > '9')
            {
                return false;
            }

            const size_t digit = static_cast<size_t>(ch - '0');
            if (result > (std::numeric_limits<size_t>::max() - digit) / 10)
            {
                return false;
            }
            result = result * 10 + digit;
        }

        value = result;
        return true;
    }

    static inline bool TryParseDouble(const std::string& text, double& value)
    {
        std::istringstream iss(text);
        iss.imbue(std::locale::classic());

        double parsedValue = GB_QuietNan;
        if (!(iss >> parsedValue))
        {
            return false;
        }

        iss >> std::ws;
        if (!iss.eof() || !std::isfinite(parsedValue))
        {
            return false;
        }

        value = parsedValue;
        return true;
    }

    static bool ReadNextSerializedField(const std::string& body, size_t& offset, std::string& field)
    {
        if (offset > body.size())
        {
            return false;
        }

        const size_t nextSeparator = body.find('|', offset);
        if (nextSeparator == std::string::npos)
        {
            field = body.substr(offset);
            offset = body.size();
            return true;
        }

        field = body.substr(offset, nextSeparator - offset);
        offset = nextSeparator + 1;
        return true;
    }

    static inline std::string DoubleToString(double value)
    {
        std::ostringstream oss;
        oss.imbue(std::locale::classic());
        oss << std::setprecision(17) << value;
        return oss.str();
    }

    static GB_Point2d LerpPoint(const GB_Point2d& point1, const GB_Point2d& point2, double parameter)
    {
        return GB_Point2d(point1.x + (point2.x - point1.x) * parameter, point1.y + (point2.y - point1.y) * parameter);
    }

    static inline void EnsureAtLeastTwoVertices(std::vector<GB_Point2d>& vertices)
    {
        if (vertices.empty())
        {
            return;
        }

        if (vertices.size() == 1)
        {
            vertices.push_back(vertices.front());
        }
    }

    static std::vector<GB_Point2d> SimplifyOpenVerticesRdp(const std::vector<GB_Point2d>& inputVertices, double toleranceSquared)
    {
        if (inputVertices.size() <= 2 || toleranceSquared <= 0.0)
        {
            return inputVertices;
        }

        if (!std::isfinite(toleranceSquared))
        {
            std::vector<GB_Point2d> simplifiedVertices;
            simplifiedVertices.reserve(2);
            simplifiedVertices.push_back(inputVertices.front());
            simplifiedVertices.push_back(inputVertices.back());
            return simplifiedVertices;
        }

        std::vector<unsigned char> keepFlags(inputVertices.size(), 0);
        keepFlags.front() = 1;
        keepFlags.back() = 1;

        std::vector<std::pair<size_t, size_t>> ranges;
        ranges.reserve(64);
        ranges.emplace_back(0, inputVertices.size() - 1);

        while (!ranges.empty())
        {
            const std::pair<size_t, size_t> range = ranges.back();
            ranges.pop_back();

            const size_t firstIndex = range.first;
            const size_t lastIndex = range.second;
            if (lastIndex <= firstIndex + 1)
            {
                continue;
            }

            double maxDistanceSquared = -1.0;
            size_t maxIndex = firstIndex;
            for (size_t i = firstIndex + 1; i < lastIndex; i++)
            {
                double segmentParameter = GB_QuietNan;
                GB_Point2d closestPoint = MakeNanPoint();
                const double distanceSquared = PointToSegmentDistanceSquared(inputVertices[i], inputVertices[firstIndex], inputVertices[lastIndex], segmentParameter, closestPoint);
                if (std::isfinite(distanceSquared) && distanceSquared > maxDistanceSquared)
                {
                    maxDistanceSquared = distanceSquared;
                    maxIndex = i;
                }
            }

            if (maxDistanceSquared > toleranceSquared && maxIndex > firstIndex && maxIndex < lastIndex)
            {
                keepFlags[maxIndex] = 1;
                ranges.emplace_back(firstIndex, maxIndex);
                ranges.emplace_back(maxIndex, lastIndex);
            }
        }

        std::vector<GB_Point2d> simplifiedVertices;
        simplifiedVertices.reserve(inputVertices.size());
        for (size_t i = 0; i < inputVertices.size(); i++)
        {
            if (keepFlags[i] != 0)
            {
                simplifiedVertices.push_back(inputVertices[i]);
            }
        }

        EnsureAtLeastTwoVertices(simplifiedVertices);
        return simplifiedVertices;
    }

    static GB_Point2d PointAtDistanceFromCache(const std::vector<GB_Point2d>& vertices, const std::vector<double>& cumulativeLengths, double totalLength, double distance)
    {
        if (vertices.size() < 2 || cumulativeLengths.size() != vertices.size())
        {
            return MakeNanPoint();
        }

        if (!std::isfinite(totalLength) || totalLength <= 0.0)
        {
            return vertices.front();
        }

        const double clampedDistance = ClampDistance(distance, totalLength);
        if (clampedDistance <= 0.0)
        {
            return vertices.front();
        }
        if (clampedDistance >= totalLength)
        {
            return vertices.back();
        }

        const auto upperIt = std::upper_bound(cumulativeLengths.begin(), cumulativeLengths.end(), clampedDistance);
        size_t segmentIndex = 0;
        if (upperIt == cumulativeLengths.begin())
        {
            segmentIndex = 0;
        }
        else
        {
            segmentIndex = static_cast<size_t>(upperIt - cumulativeLengths.begin() - 1);
        }

        if (segmentIndex + 1 >= vertices.size())
        {
            return vertices.back();
        }

        while (segmentIndex + 1 < cumulativeLengths.size() && cumulativeLengths[segmentIndex + 1] <= cumulativeLengths[segmentIndex])
        {
            segmentIndex++;
        }

        if (segmentIndex + 1 >= vertices.size())
        {
            return vertices.back();
        }

        const double segmentStartDistance = cumulativeLengths[segmentIndex];
        const double segmentEndDistance = cumulativeLengths[segmentIndex + 1];
        const double segmentLength = segmentEndDistance - segmentStartDistance;
        if (segmentLength <= 0.0 || !std::isfinite(segmentLength))
        {
            return vertices[segmentIndex];
        }

        const double segmentParameter = (clampedDistance - segmentStartDistance) / segmentLength;
        return LerpPoint(vertices[segmentIndex], vertices[segmentIndex + 1], segmentParameter);
    }
}

struct GB_Polyline::CacheData
{
    struct SegmentBox
    {
        GB_Rectangle box;
        size_t segmentIndex = 0;
    };

    struct BvhNode
    {
        GB_Rectangle box;
        size_t firstItemIndex = 0;
        size_t itemCount = 0;
        size_t leftChildIndex = InvalidBvhNodeIndex;
        size_t rightChildIndex = InvalidBvhNodeIndex;

        bool IsLeaf() const
        {
            return leftChildIndex == InvalidBvhNodeIndex && rightChildIndex == InvalidBvhNodeIndex;
        }
    };

    std::uint64_t version = 0;
    bool isValid = false;
    double totalLength = GB_QuietNan;
    GB_Rectangle boundingRectangle = GB_Rectangle::Invalid;
    std::vector<double> segmentLengths;
    std::vector<double> cumulativeLengths;
    mutable std::once_flag spatialIndexBuildOnce;
    mutable std::vector<SegmentBox> segmentBoxes;
    mutable std::vector<size_t> bvhItemSegmentIndices;
    mutable std::vector<BvhNode> bvhNodes;

    size_t BuildBvhNode(size_t firstItemIndex, size_t itemCount) const
    {
        const size_t nodeIndex = bvhNodes.size();
        bvhNodes.push_back(BvhNode());

        BvhNode node;
        node.firstItemIndex = firstItemIndex;
        node.itemCount = itemCount;

        bool hasBox = false;
        for (size_t i = 0; i < itemCount; i++)
        {
            const size_t itemIndex = firstItemIndex + i;
            if (itemIndex >= bvhItemSegmentIndices.size())
            {
                continue;
            }

            const size_t segmentBoxIndex = bvhItemSegmentIndices[itemIndex];
            if (segmentBoxIndex >= segmentBoxes.size())
            {
                continue;
            }

            const GB_Rectangle& segmentBox = segmentBoxes[segmentBoxIndex].box;
            if (!hasBox)
            {
                node.box = segmentBox;
                hasBox = true;
            }
            else
            {
                node.box.Expand(segmentBox);
            }
        }

        if (itemCount <= BvhLeafSegmentCount || !node.box.IsValid())
        {
            bvhNodes[nodeIndex] = node;
            return nodeIndex;
        }

        const double width = node.box.Width();
        const double height = node.box.Height();
        const bool splitByX = width >= height;
        const size_t middleItemIndex = firstItemIndex + itemCount / 2;

        std::nth_element(bvhItemSegmentIndices.begin() + firstItemIndex, bvhItemSegmentIndices.begin() + middleItemIndex, bvhItemSegmentIndices.begin() + firstItemIndex + itemCount,
            [this, splitByX](size_t leftSegmentIndex, size_t rightSegmentIndex)
            {
                const GB_Rectangle& leftBox = segmentBoxes[leftSegmentIndex].box;
                const GB_Rectangle& rightBox = segmentBoxes[rightSegmentIndex].box;
                const double leftCenter = splitByX ? (leftBox.minX + leftBox.maxX) * 0.5 : (leftBox.minY + leftBox.maxY) * 0.5;
                const double rightCenter = splitByX ? (rightBox.minX + rightBox.maxX) * 0.5 : (rightBox.minY + rightBox.maxY) * 0.5;
                if (leftCenter != rightCenter)
                {
                    return leftCenter < rightCenter;
                }
                return leftSegmentIndex < rightSegmentIndex;
            });

        const size_t leftCount = middleItemIndex - firstItemIndex;
        const size_t rightCount = itemCount - leftCount;
        if (leftCount == 0 || rightCount == 0)
        {
            bvhNodes[nodeIndex] = node;
            return nodeIndex;
        }

        node.leftChildIndex = BuildBvhNode(firstItemIndex, leftCount);
        node.rightChildIndex = BuildBvhNode(middleItemIndex, rightCount);
        bvhNodes[nodeIndex] = node;
        return nodeIndex;
    }

    void BuildBvh() const
    {
        bvhItemSegmentIndices.clear();
        bvhNodes.clear();

        if (segmentBoxes.size() < BvhBuildSegmentThreshold)
        {
            return;
        }

        bvhItemSegmentIndices.reserve(segmentBoxes.size());
        for (size_t i = 0; i < segmentBoxes.size(); i++)
        {
            bvhItemSegmentIndices.push_back(i);
        }

        const size_t nodeReserveCount = (segmentBoxes.size() > std::numeric_limits<size_t>::max() / 2) ? segmentBoxes.size() : segmentBoxes.size() * 2;
        bvhNodes.reserve(nodeReserveCount);
        BuildBvhNode(0, bvhItemSegmentIndices.size());
    }

    void BuildSpatialIndex(const std::vector<GB_Point2d>& inputVertices) const
    {
        segmentBoxes.clear();
        bvhItemSegmentIndices.clear();
        bvhNodes.clear();

        if (!isValid || inputVertices.size() < 2)
        {
            return;
        }

        const size_t numSegments = inputVertices.size() - 1;
        segmentBoxes.resize(numSegments);
        for (size_t i = 0; i < numSegments; i++)
        {
            segmentBoxes[i].box = SegmentBoundingRectangle(inputVertices[i], inputVertices[i + 1]);
            segmentBoxes[i].segmentIndex = i;
        }

        BuildBvh();
    }

    void EnsureSpatialIndex(const std::vector<GB_Point2d>& inputVertices) const
    {
        std::call_once(spatialIndexBuildOnce, [this, &inputVertices]()
            {
                BuildSpatialIndex(inputVertices);
            });
    }
};

const GB_Polyline GB_Polyline::Invalid = GB_Polyline();

GB_Polyline::GB_Polyline()
{
}

GB_Polyline::GB_Polyline(const std::vector<GB_Point2d>& vertices)
{
    SetVertices(vertices);
}

GB_Polyline::GB_Polyline(std::vector<GB_Point2d>&& vertices)
{
    SetVertices(std::move(vertices));
}

GB_Polyline::GB_Polyline(std::initializer_list<GB_Point2d> vertices)
{
    SetVertices(std::vector<GB_Point2d>(vertices));
}

GB_Polyline::GB_Polyline(const GB_LineSegment& segment)
{
    if (segment.IsValid())
    {
        vertices.push_back(segment.point1);
        vertices.push_back(segment.point2);
    }
}

GB_Polyline::~GB_Polyline()
{
}

GB_Polyline::GB_Polyline(const GB_Polyline& other) : vertices(other.vertices)
{
    InvalidateCaches();
}

GB_Polyline::GB_Polyline(GB_Polyline&& other) noexcept : vertices(std::move(other.vertices))
{
    InvalidateCaches();
    other.InvalidateCaches();
}

GB_Polyline& GB_Polyline::operator=(const GB_Polyline& other)
{
    if (this != &other)
    {
        vertices = other.vertices;
        InvalidateCaches();
    }
    return *this;
}

GB_Polyline& GB_Polyline::operator=(GB_Polyline&& other) noexcept
{
    if (this != &other)
    {
        vertices = std::move(other.vertices);
        InvalidateCaches();
        other.InvalidateCaches();
    }
    return *this;
}

const std::string& GB_Polyline::GetClassType() const
{
    static const std::string classType = "GB_Polyline";
    return classType;
}

uint64_t GB_Polyline::GetClassTypeId() const
{
    static const uint64_t classTypeId = GB_GenerateClassTypeId(GetClassType());
    return classTypeId;
}

void GB_Polyline::InvalidateCaches()
{
    cacheVersion.fetch_add(1, std::memory_order_release);
    std::atomic_store_explicit(&cache, std::shared_ptr<const CacheData>(), std::memory_order_release);
}

std::uint64_t GB_Polyline::GetCurrentCacheVersion() const
{
    return cacheVersion.load(std::memory_order_acquire);
}

std::shared_ptr<const GB_Polyline::CacheData> GB_Polyline::GetOrBuildCache() const
{
    const std::uint64_t currentVersion = GetCurrentCacheVersion();
    std::shared_ptr<const CacheData> cachedData = std::atomic_load_explicit(&cache, std::memory_order_acquire);
    if (cachedData && cachedData->version == currentVersion)
    {
        return cachedData;
    }

    std::shared_ptr<CacheData> newCache = std::make_shared<CacheData>();
    newCache->version = currentVersion;

    if (!AreVerticesValid(vertices))
    {
        cachedData = std::shared_ptr<const CacheData>(newCache);
        std::atomic_store_explicit(&cache, cachedData, std::memory_order_release);
        return cachedData;
    }

    newCache->isValid = true;

    const size_t numVertices = vertices.size();
    const size_t numSegments = numVertices - 1;

    newCache->segmentLengths.resize(numSegments, 0.0);
    newCache->cumulativeLengths.resize(numVertices, 0.0);
    newCache->totalLength = 0.0;
    newCache->boundingRectangle.SetFromPoint(vertices.front());

    for (size_t i = 0; i < numVertices; i++)
    {
        newCache->boundingRectangle.Expand(vertices[i]);
    }

    for (size_t i = 0; i < numSegments; i++)
    {
        const GB_Point2d& point1 = vertices[i];
        const GB_Point2d& point2 = vertices[i + 1];
        const double length = PointDistance(point1, point2);
        const double safeLength = std::isfinite(length) ? length : 0.0;

        newCache->segmentLengths[i] = safeLength;
        if (safeLength >= std::numeric_limits<double>::max() - newCache->totalLength)
        {
            newCache->totalLength = std::numeric_limits<double>::max();
        }
        else
        {
            newCache->totalLength += safeLength;
        }
        newCache->cumulativeLengths[i + 1] = newCache->totalLength;
    }

    cachedData = std::shared_ptr<const CacheData>(newCache);
    std::atomic_store_explicit(&cache, cachedData, std::memory_order_release);
    return cachedData;
}

void GB_Polyline::Clear()
{
    vertices.clear();
    InvalidateCaches();
}

void GB_Polyline::Reset()
{
    Clear();
}

bool GB_Polyline::SetVertices(const std::vector<GB_Point2d>& inputVertices)
{
    if (!AreVerticesValid(inputVertices))
    {
        Clear();
        return false;
    }

    vertices = inputVertices;
    InvalidateCaches();
    return true;
}

bool GB_Polyline::SetVertices(std::vector<GB_Point2d>&& inputVertices)
{
    if (!AreVerticesValid(inputVertices))
    {
        Clear();
        return false;
    }

    vertices = std::move(inputVertices);
    InvalidateCaches();
    return true;
}

void GB_Polyline::Reserve(size_t count)
{
    vertices.reserve(count);
}

bool GB_Polyline::AddVertex(const GB_Point2d& vertex)
{
    if (!vertex.IsValid())
    {
        return false;
    }

    vertices.push_back(vertex);
    InvalidateCaches();
    return true;
}

bool GB_Polyline::InsertVertex(size_t index, const GB_Point2d& vertex)
{
    if (!vertex.IsValid() || index > vertices.size())
    {
        return false;
    }

    vertices.insert(vertices.begin() + index, vertex);
    InvalidateCaches();
    return true;
}

bool GB_Polyline::SetVertex(size_t index, const GB_Point2d& vertex)
{
    if (!vertex.IsValid() || index >= vertices.size())
    {
        return false;
    }

    vertices[index] = vertex;
    InvalidateCaches();
    return true;
}

bool GB_Polyline::RemoveVertex(size_t index)
{
    if (index >= vertices.size())
    {
        return false;
    }

    vertices.erase(vertices.begin() + index);
    InvalidateCaches();
    return true;
}

bool GB_Polyline::IsEmpty() const
{
    return vertices.empty();
}

bool GB_Polyline::IsValid() const
{
    return AreVerticesValid(vertices);
}

bool GB_Polyline::IsDegenerate(double tolerance) const
{
    if (!IsValid())
    {
        return false;
    }

    const double absTolerance = AbsTol(tolerance);
    const double length = Length();
    return std::isfinite(length) && length <= absTolerance;
}

bool GB_Polyline::IsClosed(double tolerance) const
{
    if (!IsValid())
    {
        return false;
    }

    return vertices.front().IsNearEqual(vertices.back(), AbsTol(tolerance));
}

size_t GB_Polyline::GetNumVertices() const
{
    return vertices.size();
}

size_t GB_Polyline::GetNumSegments() const
{
    return (vertices.size() >= 2) ? (vertices.size() - 1) : 0;
}

const std::vector<GB_Point2d>& GB_Polyline::GetVertices() const
{
    return vertices;
}

GB_Point2d GB_Polyline::GetVertex(size_t index) const
{
    if (index >= vertices.size())
    {
        return MakeNanPoint();
    }

    return vertices[index];
}

bool GB_Polyline::TryGetVertex(size_t index, GB_Point2d& outVertex) const
{
    if (index >= vertices.size())
    {
        outVertex = MakeNanPoint();
        return false;
    }

    outVertex = vertices[index];
    return true;
}

GB_LineSegment GB_Polyline::GetSegment(size_t index) const
{
    if (index >= GetNumSegments())
    {
        return GB_LineSegment::Invalid;
    }

    return GB_LineSegment(vertices[index], vertices[index + 1]);
}

bool GB_Polyline::TryGetSegment(size_t index, GB_LineSegment& outSegment) const
{
    if (index >= GetNumSegments())
    {
        outSegment = GB_LineSegment::Invalid;
        return false;
    }

    outSegment.Set(vertices[index], vertices[index + 1]);
    return true;
}

std::vector<GB_LineSegment> GB_Polyline::GetSegments() const
{
    std::vector<GB_LineSegment> segments;
    const size_t numSegments = GetNumSegments();
    segments.reserve(numSegments);

    for (size_t i = 0; i < numSegments; i++)
    {
        segments.emplace_back(vertices[i], vertices[i + 1]);
    }

    return segments;
}

double GB_Polyline::Length() const
{
    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    return (cachedData && cachedData->isValid) ? cachedData->totalLength : GB_QuietNan;
}

GB_Rectangle GB_Polyline::BoundingRectangle() const
{
    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    return (cachedData && cachedData->isValid) ? cachedData->boundingRectangle : GB_Rectangle::Invalid;
}

bool GB_Polyline::HasDuplicateAdjacentVertices(double tolerance) const
{
    if (vertices.size() < 2)
    {
        return false;
    }

    const double absTolerance = AbsTol(tolerance);
    for (size_t i = 1; i < vertices.size(); i++)
    {
        if (PointsAreNearEqual(vertices[i - 1], vertices[i], absTolerance))
        {
            return true;
        }
    }

    return false;
}

bool GB_Polyline::HasZeroLengthSegments(double tolerance) const
{
    return HasDuplicateAdjacentVertices(tolerance);
}

bool GB_Polyline::IsContains(const GB_Point2d& point, double tolerance) const
{
    if (!point.IsValid())
    {
        return false;
    }

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    if (!cachedData || !cachedData->isValid)
    {
        return false;
    }

    const double absTolerance = AbsTol(tolerance);
    if (!RectangleContainsPointWithTolerance(cachedData->boundingRectangle, point, absTolerance))
    {
        return false;
    }

    const double distanceSquared = DistanceToSquared(point);
    return std::isfinite(distanceSquared) && distanceSquared <= SquaredTolerance(absTolerance);
}

GB_Point2d GB_Polyline::ClosestPointTo(const GB_Point2d& point) const
{
    const ClosestPointResult result = GetClosestPointResult(point);
    return result.succeeded ? result.closestPoint : MakeNanPoint();
}

double GB_Polyline::DistanceTo(const GB_Point2d& point) const
{
    const double distanceSquared = DistanceToSquared(point);
    return std::isfinite(distanceSquared) ? std::sqrt(distanceSquared) : GB_QuietNan;
}

double GB_Polyline::DistanceToSquared(const GB_Point2d& point) const
{
    const ClosestPointResult result = GetClosestPointResult(point);
    return (result.succeeded && result.closestPoint.IsValid() && point.IsValid()) ? PointDistanceSquared(point, result.closestPoint) : GB_QuietNan;
}

GB_Polyline::ClosestPointResult GB_Polyline::GetClosestPointResult(const GB_Point2d& point) const
{
    ClosestPointResult result;

    if (!point.IsValid())
    {
        return result;
    }

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    if (!cachedData || !cachedData->isValid)
    {
        return result;
    }

    cachedData->EnsureSpatialIndex(vertices);
    if (cachedData->segmentBoxes.empty())
    {
        return result;
    }

    const size_t numSegments = GetNumSegments();
    double bestDistanceSquared = std::numeric_limits<double>::infinity();
    double bestSegmentParameter = GB_QuietNan;
    GB_Point2d bestClosestPoint = MakeNanPoint();
    size_t bestSegmentIndex = 0;

    const auto evaluateSegment = [&](size_t segmentIndex)
        {
            if (segmentIndex >= numSegments)
            {
                return;
            }

            double segmentParameter = GB_QuietNan;
            GB_Point2d closestPoint = MakeNanPoint();
            const double distanceSquared = PointToSegmentDistanceSquared(point, vertices[segmentIndex], vertices[segmentIndex + 1], segmentParameter, closestPoint);
            if (!std::isfinite(distanceSquared))
            {
                return;
            }

            if (IsBetterDistanceCandidate(distanceSquared, segmentIndex, bestDistanceSquared, bestSegmentIndex))
            {
                bestDistanceSquared = distanceSquared;
                bestSegmentParameter = segmentParameter;
                bestClosestPoint = closestPoint;
                bestSegmentIndex = segmentIndex;
            }
        };

    if (!cachedData->bvhNodes.empty())
    {
        std::vector<size_t> nodeStack;
        nodeStack.reserve(64);
        nodeStack.push_back(0);

        while (!nodeStack.empty())
        {
            const size_t nodeIndex = nodeStack.back();
            nodeStack.pop_back();

            if (nodeIndex >= cachedData->bvhNodes.size())
            {
                continue;
            }

            const CacheData::BvhNode& node = cachedData->bvhNodes[nodeIndex];
            const double nodeDistanceSquared = RectangleDistanceToPointSquared(node.box, point);
            if (!std::isfinite(nodeDistanceSquared) || nodeDistanceSquared > bestDistanceSquared)
            {
                continue;
            }

            if (node.IsLeaf())
            {
                for (size_t i = 0; i < node.itemCount; i++)
                {
                    const size_t itemIndex = node.firstItemIndex + i;
                    if (itemIndex >= cachedData->bvhItemSegmentIndices.size())
                    {
                        continue;
                    }
                    const size_t segmentBoxIndex = cachedData->bvhItemSegmentIndices[itemIndex];
                    if (segmentBoxIndex >= cachedData->segmentBoxes.size())
                    {
                        continue;
                    }

                    const double boxDistanceSquared = RectangleDistanceToPointSquared(cachedData->segmentBoxes[segmentBoxIndex].box, point);
                    if (std::isfinite(boxDistanceSquared) && boxDistanceSquared <= bestDistanceSquared)
                    {
                        evaluateSegment(cachedData->segmentBoxes[segmentBoxIndex].segmentIndex);
                    }
                }
            }
            else
            {
                const size_t leftChildIndex = node.leftChildIndex;
                const size_t rightChildIndex = node.rightChildIndex;

                double leftDistanceSquared = std::numeric_limits<double>::infinity();
                double rightDistanceSquared = std::numeric_limits<double>::infinity();

                if (leftChildIndex < cachedData->bvhNodes.size())
                {
                    leftDistanceSquared = RectangleDistanceToPointSquared(cachedData->bvhNodes[leftChildIndex].box, point);
                }
                if (rightChildIndex < cachedData->bvhNodes.size())
                {
                    rightDistanceSquared = RectangleDistanceToPointSquared(cachedData->bvhNodes[rightChildIndex].box, point);
                }

                if (leftDistanceSquared < rightDistanceSquared)
                {
                    if (rightChildIndex < cachedData->bvhNodes.size() && rightDistanceSquared <= bestDistanceSquared)
                    {
                        nodeStack.push_back(rightChildIndex);
                    }
                    if (leftChildIndex < cachedData->bvhNodes.size() && leftDistanceSquared <= bestDistanceSquared)
                    {
                        nodeStack.push_back(leftChildIndex);
                    }
                }
                else
                {
                    if (leftChildIndex < cachedData->bvhNodes.size() && leftDistanceSquared <= bestDistanceSquared)
                    {
                        nodeStack.push_back(leftChildIndex);
                    }
                    if (rightChildIndex < cachedData->bvhNodes.size() && rightDistanceSquared <= bestDistanceSquared)
                    {
                        nodeStack.push_back(rightChildIndex);
                    }
                }
            }
        }
    }
    else
    {
        for (size_t i = 0; i < cachedData->segmentBoxes.size(); i++)
        {
            const double boxDistanceSquared = RectangleDistanceToPointSquared(cachedData->segmentBoxes[i].box, point);
            if (std::isfinite(boxDistanceSquared) && boxDistanceSquared <= bestDistanceSquared)
            {
                evaluateSegment(cachedData->segmentBoxes[i].segmentIndex);
            }
        }
    }

    if (!std::isfinite(bestDistanceSquared) || !bestClosestPoint.IsValid())
    {
        return result;
    }

    result.succeeded = true;
    result.segmentIndex = bestSegmentIndex;
    result.segmentParameter = bestSegmentParameter;
    result.distance = std::sqrt(bestDistanceSquared);
    result.closestPoint = bestClosestPoint;

    const double totalLength = cachedData->totalLength;
    if (std::isfinite(totalLength) && totalLength > 0.0 && bestSegmentIndex < cachedData->segmentLengths.size() && bestSegmentIndex < cachedData->cumulativeLengths.size())
    {
        const double segmentLength = cachedData->segmentLengths[bestSegmentIndex];
        const double distanceAtPoint = cachedData->cumulativeLengths[bestSegmentIndex] + segmentLength * bestSegmentParameter;
        result.parameter = distanceAtPoint / totalLength;
    }
    else
    {
        result.parameter = 0.0;
    }

    return result;
}

GB_Point2d GB_Polyline::PointAtDistance(double distance, bool clampToRange) const
{
    if (!std::isfinite(distance))
    {
        return MakeNanPoint();
    }

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    if (!cachedData || !cachedData->isValid)
    {
        return MakeNanPoint();
    }

    const double totalLength = cachedData->totalLength;
    if (!std::isfinite(totalLength))
    {
        return MakeNanPoint();
    }

    double targetDistance = distance;
    if (clampToRange)
    {
        targetDistance = ClampDistance(distance, totalLength);
    }
    else if (targetDistance < 0.0 || targetDistance > totalLength)
    {
        return MakeNanPoint();
    }

    return PointAtDistanceFromCache(vertices, cachedData->cumulativeLengths, totalLength, targetDistance);
}

GB_Point2d GB_Polyline::PointAtNormalizedLength(double t, bool clampToRange) const
{
    if (!std::isfinite(t))
    {
        return MakeNanPoint();
    }

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    if (!cachedData || !cachedData->isValid || !std::isfinite(cachedData->totalLength))
    {
        return MakeNanPoint();
    }

    double parameter = t;
    if (clampToRange)
    {
        parameter = Clamp01(t);
    }
    else if (parameter < 0.0 || parameter > 1.0)
    {
        return MakeNanPoint();
    }

    return PointAtDistanceFromCache(vertices, cachedData->cumulativeLengths, cachedData->totalLength, cachedData->totalLength * parameter);
}

GB_Polyline GB_Polyline::Reversed() const
{
    GB_Polyline result(*this);
    result.Reverse();
    return result;
}

void GB_Polyline::Reverse()
{
    std::reverse(vertices.begin(), vertices.end());
    InvalidateCaches();
}

GB_Polyline GB_Polyline::Offsetted(double deltaX, double deltaY) const
{
    GB_Polyline result(*this);
    result.Offset(deltaX, deltaY);
    return result;
}

GB_Polyline GB_Polyline::Offsetted(const GB_Vector2d& translation) const
{
    GB_Polyline result(*this);
    result.Offset(translation);
    return result;
}

void GB_Polyline::Offset(double deltaX, double deltaY)
{
    if (!std::isfinite(deltaX) || !std::isfinite(deltaY))
    {
        Reset();
        return;
    }

    for (GB_Point2d& vertex : vertices)
    {
        vertex.Offset(deltaX, deltaY);
    }

    if (!AreVerticesFinite(vertices))
    {
        Reset();
        return;
    }

    InvalidateCaches();
}

void GB_Polyline::Offset(const GB_Vector2d& translation)
{
    if (!translation.IsValid())
    {
        Reset();
        return;
    }

    Offset(translation.x, translation.y);
}

GB_Polyline GB_Polyline::Rotated(double angle, const GB_Point2d& center) const
{
    GB_Polyline result(*this);
    result.Rotate(angle, center);
    return result;
}

void GB_Polyline::Rotate(double angle, const GB_Point2d& center)
{
    if (!std::isfinite(angle) || !center.IsValid())
    {
        Reset();
        return;
    }

    for (GB_Point2d& vertex : vertices)
    {
        vertex.Rotate(angle, center);
    }

    if (!AreVerticesFinite(vertices))
    {
        Reset();
        return;
    }

    InvalidateCaches();
}

GB_Polyline GB_Polyline::Scaled(double scaleFactor, const GB_Point2d& center) const
{
    return Scaled(scaleFactor, scaleFactor, center);
}

GB_Polyline GB_Polyline::Scaled(double scaleX, double scaleY, const GB_Point2d& center) const
{
    GB_Polyline result(*this);
    result.Scale(scaleX, scaleY, center);
    return result;
}

void GB_Polyline::Scale(double scaleFactor, const GB_Point2d& center)
{
    Scale(scaleFactor, scaleFactor, center);
}

void GB_Polyline::Scale(double scaleX, double scaleY, const GB_Point2d& center)
{
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || !center.IsValid())
    {
        Reset();
        return;
    }

    for (GB_Point2d& vertex : vertices)
    {
        vertex.x = center.x + (vertex.x - center.x) * scaleX;
        vertex.y = center.y + (vertex.y - center.y) * scaleY;
    }

    if (!AreVerticesFinite(vertices))
    {
        Reset();
        return;
    }

    InvalidateCaches();
}

GB_Polyline GB_Polyline::Transformed(const GB_Matrix3x3& mat) const
{
    GB_Polyline result(*this);
    result.Transform(mat);
    return result;
}

void GB_Polyline::Transform(const GB_Matrix3x3& mat)
{
    if (!mat.IsValid())
    {
        Reset();
        return;
    }

    for (GB_Point2d& vertex : vertices)
    {
        vertex.Transform(mat);
    }

    if (!AreVerticesFinite(vertices))
    {
        Reset();
        return;
    }

    InvalidateCaches();
}

GB_Polyline GB_Polyline::RemovedDuplicateAdjacentVertices(double tolerance) const
{
    GB_Polyline result(*this);
    result.RemoveDuplicateAdjacentVertices(tolerance);
    return result;
}

void GB_Polyline::RemoveDuplicateAdjacentVertices(double tolerance)
{
    if (vertices.size() < 2)
    {
        return;
    }

    const double absTolerance = AbsTol(tolerance);
    std::vector<GB_Point2d> filteredVertices;
    filteredVertices.reserve(vertices.size());
    filteredVertices.push_back(vertices.front());

    for (size_t i = 1; i < vertices.size(); i++)
    {
        if (!PointsAreNearEqual(filteredVertices.back(), vertices[i], absTolerance))
        {
            filteredVertices.push_back(vertices[i]);
        }
    }

    EnsureAtLeastTwoVertices(filteredVertices);
    vertices.swap(filteredVertices);
    InvalidateCaches();
}

GB_Polyline GB_Polyline::Simplified(double tolerance) const
{
    if (!IsValid())
    {
        return GB_Polyline::Invalid;
    }

    const double absTolerance = AbsTol(tolerance);
    if (vertices.size() <= 2 || absTolerance <= 0.0)
    {
        return *this;
    }

    const double toleranceSquared = SquaredTolerance(absTolerance);
    const bool hasExplicitClosingVertex = vertices.size() > 3 && PointsAreNearEqual(vertices.front(), vertices.back(), GB_Epsilon);
    if (hasExplicitClosingVertex)
    {
        std::vector<GB_Point2d> openVertices(vertices.begin(), vertices.end() - 1);
        std::vector<GB_Point2d> simplifiedVertices = SimplifyOpenVerticesRdp(openVertices, toleranceSquared);
        EnsureAtLeastTwoVertices(simplifiedVertices);
        simplifiedVertices.push_back(simplifiedVertices.front());
        return GB_Polyline(std::move(simplifiedVertices));
    }

    std::vector<GB_Point2d> simplifiedVertices = SimplifyOpenVerticesRdp(vertices, toleranceSquared);
    EnsureAtLeastTwoVertices(simplifiedVertices);
    return GB_Polyline(std::move(simplifiedVertices));
}

std::pair<GB_Polyline, GB_Polyline> GB_Polyline::SplitAt(double t) const
{
    if (!std::isfinite(t))
    {
        return std::make_pair(GB_Polyline::Invalid, GB_Polyline::Invalid);
    }

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    if (!cachedData || !cachedData->isValid)
    {
        return std::make_pair(GB_Polyline::Invalid, GB_Polyline::Invalid);
    }

    const double parameter = Clamp01(t);
    const double totalLength = cachedData->totalLength;

    if (!std::isfinite(totalLength) || totalLength <= 0.0)
    {
        if (parameter >= 1.0)
        {
            std::vector<GB_Point2d> secondVertices = { vertices.back(), vertices.back() };
            return std::make_pair(*this, GB_Polyline(std::move(secondVertices)));
        }

        std::vector<GB_Point2d> firstVertices = { vertices.front(), vertices.front() };
        return std::make_pair(GB_Polyline(std::move(firstVertices)), *this);
    }

    const double targetDistance = totalLength * parameter;
    const double lengthTolerance = LengthCompareTolerance(totalLength);

    if (targetDistance <= lengthTolerance)
    {
        std::vector<GB_Point2d> firstVertices = { vertices.front(), vertices.front() };
        return std::make_pair(GB_Polyline(std::move(firstVertices)), *this);
    }

    if (targetDistance >= totalLength - lengthTolerance)
    {
        std::vector<GB_Point2d> secondVertices = { vertices.back(), vertices.back() };
        return std::make_pair(*this, GB_Polyline(std::move(secondVertices)));
    }

    const std::vector<double>& cumulativeLengths = cachedData->cumulativeLengths;
    const auto lowerIt = std::lower_bound(cumulativeLengths.begin(), cumulativeLengths.end(), targetDistance);
    if (lowerIt != cumulativeLengths.end() && AreDistancesNearEqual(*lowerIt, targetDistance, lengthTolerance))
    {
        const size_t vertexIndex = static_cast<size_t>(lowerIt - cumulativeLengths.begin());
        std::vector<GB_Point2d> firstVertices(vertices.begin(), vertices.begin() + vertexIndex + 1);
        std::vector<GB_Point2d> secondVertices(vertices.begin() + vertexIndex, vertices.end());
        EnsureAtLeastTwoVertices(firstVertices);
        EnsureAtLeastTwoVertices(secondVertices);
        return std::make_pair(GB_Polyline(std::move(firstVertices)), GB_Polyline(std::move(secondVertices)));
    }

    if (lowerIt != cumulativeLengths.begin())
    {
        const auto previousIt = lowerIt - 1;
        if (AreDistancesNearEqual(*previousIt, targetDistance, lengthTolerance))
        {
            const size_t vertexIndex = static_cast<size_t>(previousIt - cumulativeLengths.begin());
            std::vector<GB_Point2d> firstVertices(vertices.begin(), vertices.begin() + vertexIndex + 1);
            std::vector<GB_Point2d> secondVertices(vertices.begin() + vertexIndex, vertices.end());
            EnsureAtLeastTwoVertices(firstVertices);
            EnsureAtLeastTwoVertices(secondVertices);
            return std::make_pair(GB_Polyline(std::move(firstVertices)), GB_Polyline(std::move(secondVertices)));
        }
    }

    size_t segmentIndex = 0;
    if (lowerIt == cumulativeLengths.begin())
    {
        segmentIndex = 0;
    }
    else
    {
        segmentIndex = static_cast<size_t>(lowerIt - cumulativeLengths.begin() - 1);
    }

    while (segmentIndex + 1 < cumulativeLengths.size() && cumulativeLengths[segmentIndex + 1] <= cumulativeLengths[segmentIndex] + lengthTolerance)
    {
        segmentIndex++;
    }

    if (segmentIndex + 1 >= vertices.size())
    {
        std::vector<GB_Point2d> secondVertices = { vertices.back(), vertices.back() };
        return std::make_pair(*this, GB_Polyline(std::move(secondVertices)));
    }

    const double segmentStartDistance = cumulativeLengths[segmentIndex];
    const double segmentEndDistance = cumulativeLengths[segmentIndex + 1];
    const double segmentLength = segmentEndDistance - segmentStartDistance;
    if (!std::isfinite(segmentLength) || segmentLength <= lengthTolerance)
    {
        std::vector<GB_Point2d> firstVertices(vertices.begin(), vertices.begin() + segmentIndex + 1);
        std::vector<GB_Point2d> secondVertices(vertices.begin() + segmentIndex, vertices.end());
        EnsureAtLeastTwoVertices(firstVertices);
        EnsureAtLeastTwoVertices(secondVertices);
        return std::make_pair(GB_Polyline(std::move(firstVertices)), GB_Polyline(std::move(secondVertices)));
    }

    const double segmentParameter = (targetDistance - segmentStartDistance) / segmentLength;
    const GB_Point2d splitPoint = LerpPoint(vertices[segmentIndex], vertices[segmentIndex + 1], Clamp01(segmentParameter));

    std::vector<GB_Point2d> firstVertices;
    firstVertices.reserve(segmentIndex + 2);
    firstVertices.insert(firstVertices.end(), vertices.begin(), vertices.begin() + segmentIndex + 1);
    firstVertices.push_back(splitPoint);

    std::vector<GB_Point2d> secondVertices;
    secondVertices.reserve(vertices.size() - segmentIndex);
    secondVertices.push_back(splitPoint);
    secondVertices.insert(secondVertices.end(), vertices.begin() + segmentIndex + 1, vertices.end());

    EnsureAtLeastTwoVertices(firstVertices);
    EnsureAtLeastTwoVertices(secondVertices);
    return std::make_pair(GB_Polyline(std::move(firstVertices)), GB_Polyline(std::move(secondVertices)));
}

GB_Polyline GB_Polyline::SubPolyline(double t1, double t2) const
{
    if (!std::isfinite(t1) || !std::isfinite(t2))
    {
        return GB_Polyline::Invalid;
    }

    const bool needReverse = t1 > t2;
    const double startParameter = needReverse ? Clamp01(t2) : Clamp01(t1);
    const double endParameter = needReverse ? Clamp01(t1) : Clamp01(t2);

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    if (!cachedData || !cachedData->isValid)
    {
        return GB_Polyline::Invalid;
    }

    const double totalLength = cachedData->totalLength;
    if (!std::isfinite(totalLength) || totalLength <= 0.0)
    {
        std::vector<GB_Point2d> resultVertices = { vertices.front(), vertices.front() };
        GB_Polyline result(std::move(resultVertices));
        if (needReverse)
        {
            result.Reverse();
        }
        return result;
    }

    const double startDistance = totalLength * startParameter;
    const double endDistance = totalLength * endParameter;
    const double lengthTolerance = LengthCompareTolerance(totalLength);
    const GB_Point2d startPoint = PointAtDistanceFromCache(vertices, cachedData->cumulativeLengths, totalLength, startDistance);
    const GB_Point2d endPoint = PointAtDistanceFromCache(vertices, cachedData->cumulativeLengths, totalLength, endDistance);

    std::vector<GB_Point2d> resultVertices;
    resultVertices.reserve(vertices.size());
    resultVertices.push_back(startPoint);

    const std::vector<double>& cumulativeLengths = cachedData->cumulativeLengths;
    auto firstInteriorIt = std::upper_bound(cumulativeLengths.begin() + 1, cumulativeLengths.end(), startDistance + lengthTolerance);
    size_t vertexIndex = static_cast<size_t>(firstInteriorIt - cumulativeLengths.begin());
    while (vertexIndex + 1 < vertices.size() && cumulativeLengths[vertexIndex] < endDistance - lengthTolerance)
    {
        resultVertices.push_back(vertices[vertexIndex]);
        vertexIndex++;
    }

    resultVertices.push_back(endPoint);
    EnsureAtLeastTwoVertices(resultVertices);

    GB_Polyline result(std::move(resultVertices));
    if (needReverse)
    {
        result.Reverse();
    }
    return result;
}

bool GB_Polyline::TryToPolygon(GB_Polygon& outPolygon, bool removeDuplicatedClosingVertex) const
{
    outPolygon = GB_Polygon::Invalid;

    if (!IsValid() || !IsClosed())
    {
        return false;
    }

    std::vector<GB_Point2d> polygonVertices = vertices;
    if (removeDuplicatedClosingVertex && polygonVertices.size() > 2 && polygonVertices.front().IsNearEqual(polygonVertices.back()))
    {
        polygonVertices.pop_back();
    }

    if (polygonVertices.size() < 2)
    {
        return false;
    }

    GB_Polygon polygon(polygonVertices);
    if (!polygon.IsValid())
    {
        return false;
    }

    outPolygon = std::move(polygon);
    return true;
}

GB_Polygon GB_Polyline::ToPolygon(bool removeDuplicatedClosingVertex) const
{
    GB_Polygon polygon;
    if (!TryToPolygon(polygon, removeDuplicatedClosingVertex))
    {
        return GB_Polygon::Invalid;
    }
    return polygon;
}

bool GB_Polyline::operator==(const GB_Polyline& other) const
{
    return vertices == other.vertices;
}

bool GB_Polyline::operator!=(const GB_Polyline& other) const
{
    return !(*this == other);
}

bool GB_Polyline::IsNearEqual(const GB_Polyline& other, double tolerance) const
{
    if (vertices.size() != other.vertices.size())
    {
        return false;
    }

    const double absTolerance = AbsTol(tolerance);
    for (size_t i = 0; i < vertices.size(); i++)
    {
        if (!vertices[i].IsNearEqual(other.vertices[i], absTolerance))
        {
            return false;
        }
    }

    return true;
}

std::string GB_Polyline::SerializeToString() const
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << "(" << GetClassType() << "|" << vertices.size();

    for (const GB_Point2d& vertex : vertices)
    {
        oss << "|" << DoubleToString(vertex.x) << "|" << DoubleToString(vertex.y);
    }

    oss << ")";
    return oss.str();
}

GB_ByteBuffer GB_Polyline::SerializeToBinary() const
{
    constexpr static uint16_t payloadVersion = 1;

    GB_ByteBuffer buffer;
    if (vertices.size() <= (std::numeric_limits<size_t>::max() - 24) / 16)
    {
        buffer.reserve(24 + vertices.size() * 16);
    }

    GB_ByteBufferIO::AppendUInt32LE(buffer, GB_ClassMagicNumber);
    GB_ByteBufferIO::AppendUInt64LE(buffer, GetClassTypeId());
    GB_ByteBufferIO::AppendUInt16LE(buffer, payloadVersion);
    GB_ByteBufferIO::AppendUInt16LE(buffer, 0);
    GB_ByteBufferIO::AppendUInt64LE(buffer, static_cast<uint64_t>(vertices.size()));

    for (const GB_Point2d& vertex : vertices)
    {
        GB_ByteBufferIO::AppendDoubleLE(buffer, vertex.x);
        GB_ByteBufferIO::AppendDoubleLE(buffer, vertex.y);
    }

    return buffer;
}

bool GB_Polyline::Deserialize(const std::string& data)
{
    if (data.size() < 2 || data.front() != '(' || data.back() != ')')
    {
        return false;
    }

    const std::string body = data.substr(1, data.size() - 2);
    size_t offset = 0;
    std::string field;

    if (!ReadNextSerializedField(body, offset, field) || field != GetClassType())
    {
        return false;
    }

    if (!ReadNextSerializedField(body, offset, field))
    {
        return false;
    }

    size_t numVertices = 0;
    if (!TryParseSizeT(field, numVertices))
    {
        return false;
    }

    if (numVertices > (body.size() + 1) / 4)
    {
        return false;
    }

    std::vector<GB_Point2d> parsedVertices;
    parsedVertices.reserve(numVertices);

    for (size_t i = 0; i < numVertices; i++)
    {
        std::string xText;
        std::string yText;
        if (!ReadNextSerializedField(body, offset, xText) || !ReadNextSerializedField(body, offset, yText))
        {
            return false;
        }

        double x = GB_QuietNan;
        double y = GB_QuietNan;
        if (!TryParseDouble(xText, x) || !TryParseDouble(yText, y) || !std::isfinite(x) || !std::isfinite(y))
        {
            return false;
        }

        parsedVertices.emplace_back(x, y);
    }

    if (offset != body.size() || (!body.empty() && body.back() == '|'))
    {
        return false;
    }

    vertices = std::move(parsedVertices);
    InvalidateCaches();
    return true;
}

bool GB_Polyline::Deserialize(const GB_ByteBuffer& data)
{
    constexpr static uint16_t expectedPayloadVersion = 1;
    constexpr static size_t headerSize = 24;

    if (data.size() < headerSize)
    {
        return false;
    }

    size_t offset = 0;
    uint32_t magicNumber = 0;
    uint64_t classTypeId = 0;
    uint16_t payloadVersion = 0;
    uint16_t reserved = 0;
    uint64_t numVertices64 = 0;

    if (!GB_ByteBufferIO::ReadUInt32LE(data, offset, magicNumber)
        || !GB_ByteBufferIO::ReadUInt64LE(data, offset, classTypeId)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, payloadVersion)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, reserved)
        || !GB_ByteBufferIO::ReadUInt64LE(data, offset, numVertices64))
    {
        return false;
    }

    if (magicNumber != GB_ClassMagicNumber || classTypeId != GetClassTypeId() || payloadVersion != expectedPayloadVersion || reserved != 0)
    {
        return false;
    }

    if (numVertices64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    {
        return false;
    }

    const size_t numVertices = static_cast<size_t>(numVertices64);
    if (numVertices > (std::numeric_limits<size_t>::max() - headerSize) / 16)
    {
        return false;
    }

    if (data.size() != headerSize + numVertices * 16)
    {
        return false;
    }

    std::vector<GB_Point2d> parsedVertices;
    parsedVertices.reserve(numVertices);

    for (size_t i = 0; i < numVertices; i++)
    {
        double x = GB_QuietNan;
        double y = GB_QuietNan;
        if (!GB_ByteBufferIO::ReadDoubleLE(data, offset, x) || !GB_ByteBufferIO::ReadDoubleLE(data, offset, y))
        {
            return false;
        }
        if (!std::isfinite(x) || !std::isfinite(y))
        {
            return false;
        }
        parsedVertices.emplace_back(x, y);
    }

    if (offset != data.size())
    {
        return false;
    }

    vertices = std::move(parsedVertices);
    InvalidateCaches();
    return true;
}
