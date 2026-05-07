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
#include <sstream>
#include <utility>

namespace
{
    constexpr static size_t BvhBuildSegmentThreshold = 64;
    constexpr static size_t BvhLeafSegmentCount = 16;

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

    static inline bool IsFinitePoint(const GB_Point2d& point)
    {
        return point.IsValid();
    }

    static inline bool AreVerticesValid(const std::vector<GB_Point2d>& vertices)
    {
        if (vertices.size() < 2)
        {
            return false;
        }

        for (const GB_Point2d& vertex : vertices)
        {
            if (!vertex.IsValid())
            {
                return false;
            }
        }

        return true;
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

        return dx * dx + dy * dy;
    }

    static inline double PointDistanceSquared(const GB_Point2d& point1, const GB_Point2d& point2)
    {
        const double dx = point1.x - point2.x;
        const double dy = point1.y - point2.y;
        return dx * dx + dy * dy;
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
        const double lengthSquared = vectorX * vectorX + vectorY * vectorY;
        if (!std::isfinite(lengthSquared))
        {
            return GB_QuietNan;
        }

        if (lengthSquared <= 0.0)
        {
            outSegmentParameter = 0.0;
            outClosestPoint = segmentStart;
            return PointDistanceSquared(point, segmentStart);
        }

        const double pointVectorX = point.x - segmentStart.x;
        const double pointVectorY = point.y - segmentStart.y;
        double parameter = (pointVectorX * vectorX + pointVectorY * vectorY) / lengthSquared;
        parameter = Clamp01(parameter);

        outSegmentParameter = parameter;
        outClosestPoint = GB_Point2d(segmentStart.x + vectorX * parameter, segmentStart.y + vectorY * parameter);
        return PointDistanceSquared(point, outClosestPoint);
    }

    static inline bool PointsAreNearEqual(const GB_Point2d& point1, const GB_Point2d& point2, double tolerance)
    {
        if (!point1.IsValid() || !point2.IsValid())
        {
            return false;
        }

        const double toleranceSquared = tolerance * tolerance;
        return PointDistanceSquared(point1, point2) <= toleranceSquared;
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

    static GB_Point2d PointAtDistanceFromCache(const std::vector<GB_Point2d>& vertices, const std::vector<double>& cumulativeLengths, double totalLength, double distance)
    {
        if (!AreVerticesValid(vertices))
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
        int leftChildIndex = -1;
        int rightChildIndex = -1;

        bool IsLeaf() const
        {
            return leftChildIndex < 0 && rightChildIndex < 0;
        }
    };

    std::uint64_t version = 0;
    double totalLength = GB_QuietNan;
    GB_Rectangle boundingRectangle = GB_Rectangle::Invalid;
    std::vector<double> segmentLengths;
    std::vector<double> cumulativeLengths;
    std::vector<SegmentBox> segmentBoxes;
    std::vector<size_t> bvhItemSegmentIndices;
    std::vector<BvhNode> bvhNodes;

    int BuildBvhNode(size_t firstItemIndex, size_t itemCount)
    {
        const int nodeIndex = static_cast<int>(bvhNodes.size());
        bvhNodes.push_back(BvhNode());

        BvhNode node;
        node.firstItemIndex = firstItemIndex;
        node.itemCount = itemCount;

        bool hasBox = false;
        for (size_t i = 0; i < itemCount; i++)
        {
            const size_t itemIndex = firstItemIndex + i;
            const size_t segmentIndex = bvhItemSegmentIndices[itemIndex];
            const GB_Rectangle& segmentBox = segmentBoxes[segmentIndex].box;
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

        std::sort(bvhItemSegmentIndices.begin() + firstItemIndex, bvhItemSegmentIndices.begin() + firstItemIndex + itemCount,
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

    void BuildBvh()
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

        bvhNodes.reserve(segmentBoxes.size() * 2);
        BuildBvhNode(0, bvhItemSegmentIndices.size());
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

    const size_t numVertices = vertices.size();
    const size_t numSegments = numVertices - 1;

    newCache->segmentLengths.resize(numSegments, 0.0);
    newCache->cumulativeLengths.resize(numVertices, 0.0);
    newCache->segmentBoxes.resize(numSegments);
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
        const double length = point1.DistanceTo(point2);
        const double safeLength = std::isfinite(length) ? length : 0.0;

        newCache->segmentLengths[i] = safeLength;
        newCache->totalLength += safeLength;
        newCache->cumulativeLengths[i + 1] = newCache->totalLength;

        newCache->segmentBoxes[i].box = SegmentBoundingRectangle(point1, point2);
        newCache->segmentBoxes[i].segmentIndex = i;
    }

    newCache->BuildBvh();

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
    if (index + 1 >= vertices.size())
    {
        return GB_LineSegment::Invalid;
    }

    return GB_LineSegment(vertices[index], vertices[index + 1]);
}

bool GB_Polyline::TryGetSegment(size_t index, GB_LineSegment& outSegment) const
{
    if (index + 1 >= vertices.size())
    {
        outSegment = GB_LineSegment::Invalid;
        return false;
    }

    outSegment.Set(vertices[index], vertices[index + 1]);
    return outSegment.IsValid();
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
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    return cachedData ? cachedData->totalLength : GB_QuietNan;
}

GB_Rectangle GB_Polyline::BoundingRectangle() const
{
    if (!IsValid())
    {
        return GB_Rectangle::Invalid;
    }

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    return cachedData ? cachedData->boundingRectangle : GB_Rectangle::Invalid;
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

    const double distanceSquared = DistanceToSquared(point);
    const double absTolerance = AbsTol(tolerance);
    return std::isfinite(distanceSquared) && distanceSquared <= absTolerance * absTolerance;
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
    return result.succeeded ? (result.distance * result.distance) : GB_QuietNan;
}

GB_Polyline::ClosestPointResult GB_Polyline::GetClosestPointResult(const GB_Point2d& point) const
{
    ClosestPointResult result;

    if (!IsValid() || !point.IsValid())
    {
        return result;
    }

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    if (!cachedData || cachedData->segmentBoxes.empty())
    {
        return result;
    }

    double bestDistanceSquared = std::numeric_limits<double>::infinity();
    double bestSegmentParameter = GB_QuietNan;
    GB_Point2d bestClosestPoint = MakeNanPoint();
    size_t bestSegmentIndex = 0;

    const auto evaluateSegment = [&](size_t segmentIndex)
        {
            if (segmentIndex + 1 >= vertices.size())
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

            if (distanceSquared < bestDistanceSquared)
            {
                bestDistanceSquared = distanceSquared;
                bestSegmentParameter = segmentParameter;
                bestClosestPoint = closestPoint;
                bestSegmentIndex = segmentIndex;
            }
        };

    if (!cachedData->bvhNodes.empty())
    {
        std::vector<int> nodeStack;
        nodeStack.reserve(64);
        nodeStack.push_back(0);

        while (!nodeStack.empty())
        {
            const int nodeIndex = nodeStack.back();
            nodeStack.pop_back();

            if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= cachedData->bvhNodes.size())
            {
                continue;
            }

            const CacheData::BvhNode& node = cachedData->bvhNodes[static_cast<size_t>(nodeIndex)];
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
                    evaluateSegment(cachedData->bvhItemSegmentIndices[itemIndex]);
                }
            }
            else
            {
                const int leftChildIndex = node.leftChildIndex;
                const int rightChildIndex = node.rightChildIndex;

                double leftDistanceSquared = std::numeric_limits<double>::infinity();
                double rightDistanceSquared = std::numeric_limits<double>::infinity();

                if (leftChildIndex >= 0 && static_cast<size_t>(leftChildIndex) < cachedData->bvhNodes.size())
                {
                    leftDistanceSquared = RectangleDistanceToPointSquared(cachedData->bvhNodes[static_cast<size_t>(leftChildIndex)].box, point);
                }
                if (rightChildIndex >= 0 && static_cast<size_t>(rightChildIndex) < cachedData->bvhNodes.size())
                {
                    rightDistanceSquared = RectangleDistanceToPointSquared(cachedData->bvhNodes[static_cast<size_t>(rightChildIndex)].box, point);
                }

                if (leftDistanceSquared < rightDistanceSquared)
                {
                    if (rightDistanceSquared <= bestDistanceSquared)
                    {
                        nodeStack.push_back(rightChildIndex);
                    }
                    if (leftDistanceSquared <= bestDistanceSquared)
                    {
                        nodeStack.push_back(leftChildIndex);
                    }
                }
                else
                {
                    if (leftDistanceSquared <= bestDistanceSquared)
                    {
                        nodeStack.push_back(leftChildIndex);
                    }
                    if (rightDistanceSquared <= bestDistanceSquared)
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
    if (!IsValid() || !std::isfinite(distance))
    {
        return MakeNanPoint();
    }

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    if (!cachedData)
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
    if (!IsValid() || !std::isfinite(t))
    {
        return MakeNanPoint();
    }

    const double length = Length();
    if (!std::isfinite(length))
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

    return PointAtDistance(length * parameter, true);
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

    if (!AreVerticesValid(vertices))
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

    const double toleranceSquared = absTolerance * absTolerance;
    std::vector<unsigned char> keepFlags(vertices.size(), 0);
    keepFlags.front() = 1;
    keepFlags.back() = 1;

    std::vector<std::pair<size_t, size_t>> ranges;
    ranges.reserve(64);
    ranges.emplace_back(0, vertices.size() - 1);

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
            const double distanceSquared = PointToSegmentDistanceSquared(vertices[i], vertices[firstIndex], vertices[lastIndex], segmentParameter, closestPoint);
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
    simplifiedVertices.reserve(vertices.size());
    for (size_t i = 0; i < vertices.size(); i++)
    {
        if (keepFlags[i] != 0)
        {
            simplifiedVertices.push_back(vertices[i]);
        }
    }

    EnsureAtLeastTwoVertices(simplifiedVertices);
    return GB_Polyline(std::move(simplifiedVertices));
}

std::pair<GB_Polyline, GB_Polyline> GB_Polyline::SplitAt(double t) const
{
    if (!IsValid())
    {
        return std::make_pair(GB_Polyline::Invalid, GB_Polyline::Invalid);
    }

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    if (!cachedData)
    {
        return std::make_pair(GB_Polyline::Invalid, GB_Polyline::Invalid);
    }

    const double parameter = Clamp01(t);
    const double totalLength = cachedData->totalLength;

    if (!std::isfinite(totalLength) || totalLength <= 0.0)
    {
        if (parameter <= 0.0)
        {
            std::vector<GB_Point2d> firstVertices = { vertices.front(), vertices.front() };
            return std::make_pair(GB_Polyline(std::move(firstVertices)), *this);
        }
        if (parameter >= 1.0)
        {
            std::vector<GB_Point2d> secondVertices = { vertices.back(), vertices.back() };
            return std::make_pair(*this, GB_Polyline(std::move(secondVertices)));
        }

        std::vector<GB_Point2d> firstVertices = { vertices.front(), vertices.front() };
        return std::make_pair(GB_Polyline(std::move(firstVertices)), *this);
    }

    const double targetDistance = totalLength * parameter;

    if (targetDistance <= 0.0)
    {
        std::vector<GB_Point2d> firstVertices = { vertices.front(), vertices.front() };
        return std::make_pair(GB_Polyline(std::move(firstVertices)), *this);
    }

    if (targetDistance >= totalLength)
    {
        std::vector<GB_Point2d> secondVertices = { vertices.back(), vertices.back() };
        return std::make_pair(*this, GB_Polyline(std::move(secondVertices)));
    }

    const std::vector<double>& cumulativeLengths = cachedData->cumulativeLengths;
    for (size_t i = 0; i < cumulativeLengths.size(); i++)
    {
        if (cumulativeLengths[i] == targetDistance)
        {
            std::vector<GB_Point2d> firstVertices(vertices.begin(), vertices.begin() + i + 1);
            std::vector<GB_Point2d> secondVertices(vertices.begin() + i, vertices.end());
            EnsureAtLeastTwoVertices(firstVertices);
            EnsureAtLeastTwoVertices(secondVertices);
            return std::make_pair(GB_Polyline(std::move(firstVertices)), GB_Polyline(std::move(secondVertices)));
        }
    }

    size_t segmentIndex = 0;
    bool foundSegment = false;
    for (size_t i = 0; i + 1 < cumulativeLengths.size(); i++)
    {
        if (cumulativeLengths[i] < targetDistance && targetDistance < cumulativeLengths[i + 1])
        {
            segmentIndex = i;
            foundSegment = true;
            break;
        }
    }

    if (!foundSegment || segmentIndex + 1 >= vertices.size())
    {
        std::vector<GB_Point2d> secondVertices = { vertices.back(), vertices.back() };
        return std::make_pair(*this, GB_Polyline(std::move(secondVertices)));
    }

    const double segmentStartDistance = cumulativeLengths[segmentIndex];
    const double segmentEndDistance = cumulativeLengths[segmentIndex + 1];
    const double segmentLength = segmentEndDistance - segmentStartDistance;
    const double segmentParameter = (targetDistance - segmentStartDistance) / segmentLength;
    const GB_Point2d splitPoint = LerpPoint(vertices[segmentIndex], vertices[segmentIndex + 1], segmentParameter);

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
    if (!IsValid())
    {
        return GB_Polyline::Invalid;
    }

    if (!std::isfinite(t1) || !std::isfinite(t2))
    {
        return GB_Polyline::Invalid;
    }

    const bool needReverse = t1 > t2;
    const double startParameter = needReverse ? Clamp01(t2) : Clamp01(t1);
    const double endParameter = needReverse ? Clamp01(t1) : Clamp01(t2);

    const std::shared_ptr<const CacheData> cachedData = GetOrBuildCache();
    if (!cachedData)
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
    const GB_Point2d startPoint = PointAtDistanceFromCache(vertices, cachedData->cumulativeLengths, totalLength, startDistance);
    const GB_Point2d endPoint = PointAtDistanceFromCache(vertices, cachedData->cumulativeLengths, totalLength, endDistance);

    std::vector<GB_Point2d> resultVertices;
    resultVertices.reserve(vertices.size());
    resultVertices.push_back(startPoint);

    for (size_t i = 1; i + 1 < vertices.size(); i++)
    {
        const double vertexDistance = cachedData->cumulativeLengths[i];
        if (vertexDistance > startDistance && vertexDistance < endDistance)
        {
            resultVertices.push_back(vertices[i]);
        }
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
    buffer.reserve(24 + vertices.size() * 16);

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
    Clear();

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

    std::vector<GB_Point2d> parsedVertices;
    parsedVertices.reserve(numVertices);

    for (size_t i = 0; i < numVertices; i++)
    {
        std::string xText;
        std::string yText;
        if (!ReadNextSerializedField(body, offset, xText) || !ReadNextSerializedField(body, offset, yText))
        {
            Clear();
            return false;
        }

        double x = GB_QuietNan;
        double y = GB_QuietNan;
        if (!TryParseDouble(xText, x) || !TryParseDouble(yText, y))
        {
            Clear();
            return false;
        }

        parsedVertices.emplace_back(x, y);
    }

    if (offset != body.size())
    {
        Clear();
        return false;
    }

    return SetVertices(std::move(parsedVertices));
}

bool GB_Polyline::Deserialize(const GB_ByteBuffer& data)
{
    Clear();

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

    if (magicNumber != GB_ClassMagicNumber || classTypeId != GetClassTypeId() || payloadVersion != expectedPayloadVersion)
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
            Clear();
            return false;
        }
        parsedVertices.emplace_back(x, y);
    }

    return SetVertices(std::move(parsedVertices));
}
