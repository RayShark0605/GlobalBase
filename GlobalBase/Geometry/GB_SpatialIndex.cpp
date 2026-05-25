#include "GB_SpatialIndex.h"
#include "GB_Polygon.h"
#include "GB_Polyline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <thread>
#include <unordered_set>
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
    typedef std::pair<BoostBox, std::size_t> SegmentTreeValue;
    typedef bgi::rtree<SegmentTreeValue, bgi::rstar<16, 8>> SegmentTree;

    const std::size_t MinSegmentTreeSize = 64;

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

    double ClampLongDoubleToFiniteDouble(long double value)
    {
        const long double maxValue = static_cast<long double>(std::numeric_limits<double>::max());
        if (value > maxValue)
        {
            return std::numeric_limits<double>::max();
        }
        if (value < -maxValue)
        {
            return -std::numeric_limits<double>::max();
        }
        return static_cast<double>(value);
    }

    GB_Rectangle MakeQueryRange(const GB_Rectangle& range, double tolerance)
    {
        if (!IsValidRange(range))
        {
            return GB_Rectangle::Invalid;
        }

        if (!std::isfinite(tolerance))
        {
            return range;
        }

        const double absTolerance = std::abs(tolerance);
        if (absTolerance <= 0.0)
        {
            return range;
        }

        const long double expandedMinX = static_cast<long double>(range.minX) - static_cast<long double>(absTolerance);
        const long double expandedMinY = static_cast<long double>(range.minY) - static_cast<long double>(absTolerance);
        const long double expandedMaxX = static_cast<long double>(range.maxX) + static_cast<long double>(absTolerance);
        const long double expandedMaxY = static_cast<long double>(range.maxY) + static_cast<long double>(absTolerance);

        const GB_Rectangle queryRange(ClampLongDoubleToFiniteDouble(expandedMinX), ClampLongDoubleToFiniteDouble(expandedMinY), ClampLongDoubleToFiniteDouble(expandedMaxX), ClampLongDoubleToFiniteDouble(expandedMaxY));
        return IsValidRange(queryRange) ? queryRange : GB_Rectangle::Invalid;
    }

    GB_Rectangle MakeInnerQueryRange(const GB_Rectangle& range, double tolerance)
    {
        if (!IsValidRange(range))
        {
            return GB_Rectangle::Invalid;
        }

        if (!std::isfinite(tolerance))
        {
            return range;
        }

        const double absTolerance = std::abs(tolerance);
        if (absTolerance <= 0.0)
        {
            return range;
        }

        const double minX = range.minX + absTolerance;
        const double minY = range.minY + absTolerance;
        const double maxX = range.maxX - absTolerance;
        const double maxY = range.maxY - absTolerance;
        if (!std::isfinite(minX) || !std::isfinite(minY) || !std::isfinite(maxX) || !std::isfinite(maxY) || minX > maxX || minY > maxY)
        {
            return GB_Rectangle::Invalid;
        }

        return GB_Rectangle(minX, minY, maxX, maxY);
    }

    double AbsTolerance(double tolerance)
    {
        if (!std::isfinite(tolerance))
        {
            return 0.0;
        }

        return std::abs(tolerance);
    }

    double ClampDouble(double value, double minValue, double maxValue)
    {
        if (value < minValue)
        {
            return minValue;
        }
        if (value > maxValue)
        {
            return maxValue;
        }
        return value;
    }

    bool IsValidPoint(const GB_Point2d& point)
    {
        return point.IsValid() && IsFinite(point.x) && IsFinite(point.y);
    }

    struct Segment2d
    {
        GB_Point2d point1;
        GB_Point2d point2;
        GB_Rectangle range;

        Segment2d()
        {
        }

        Segment2d(const GB_Point2d& point1, const GB_Point2d& point2)
            : point1(point1), point2(point2), range(point1, point2)
        {
        }

        bool IsValid() const
        {
            return IsValidPoint(point1) && IsValidPoint(point2) && IsValidRange(range);
        }
    };

    std::unique_ptr<SegmentTree> CreateSegmentTree(const std::vector<Segment2d>& segments)
    {
        if (segments.size() < MinSegmentTreeSize)
        {
            return std::unique_ptr<SegmentTree>();
        }

        std::vector<SegmentTreeValue> values;
        values.reserve(segments.size());
        for (std::size_t i = 0; i < segments.size(); i++)
        {
            if (segments[i].IsValid())
            {
                values.emplace_back(ToBoostBox(segments[i].range), i);
            }
        }

        if (values.empty())
        {
            return std::unique_ptr<SegmentTree>();
        }

        return std::unique_ptr<SegmentTree>(new SegmentTree(values.begin(), values.end()));
    }

    template<typename TVisitor>
    void VisitSegmentTree(const SegmentTree& tree, const GB_Rectangle& range, double tolerance, const TVisitor& visitor)
    {
        const GB_Rectangle queryRange = MakeQueryRange(range, tolerance);
        if (!IsValidRange(queryRange))
        {
            return;
        }

        const BoostBox queryBox = ToBoostBox(queryRange);
        for (auto iterator = tree.qbegin(bgi::intersects(queryBox)); iterator != tree.qend(); iterator++)
        {
            if (!visitor(iterator->second))
            {
                break;
            }
        }
    }

    bool ArePointsExactlyEqual(const GB_Point2d& point1, const GB_Point2d& point2)
    {
        return IsValidPoint(point1) && IsValidPoint(point2) && point1.x == point2.x && point1.y == point2.y;
    }

    GB_Rectangle ComputeBoundingBox(const std::vector<GB_Point2d>& vertices)
    {
        GB_Rectangle boundingBox = GB_Rectangle::Invalid;
        for (std::size_t i = 0; i < vertices.size(); i++)
        {
            if (!IsValidPoint(vertices[i]))
            {
                return GB_Rectangle::Invalid;
            }

            boundingBox.Expand(vertices[i]);
        }

        return boundingBox;
    }

    double ComputeSignedArea(const std::vector<GB_Point2d>& vertices)
    {
        if (vertices.size() < 3)
        {
            return 0.0;
        }

        long double area2 = 0.0L;
        for (std::size_t i = 0; i < vertices.size(); i++)
        {
            const std::size_t nextIndex = (i + 1) % vertices.size();
            area2 += static_cast<long double>(vertices[i].x) * static_cast<long double>(vertices[nextIndex].y);
            area2 -= static_cast<long double>(vertices[nextIndex].x) * static_cast<long double>(vertices[i].y);
        }

        const long double area = area2 * 0.5L;
        if (!std::isfinite(area))
        {
            return std::numeric_limits<double>::quiet_NaN();
        }

        return ClampLongDoubleToFiniteDouble(area);
    }

    std::vector<GB_Point2d> NormalizePolygonVertices(std::vector<GB_Point2d>&& vertices)
    {
        std::vector<GB_Point2d> normalizedVertices;
        normalizedVertices.reserve(vertices.size());

        for (std::size_t i = 0; i < vertices.size(); i++)
        {
            if (!IsValidPoint(vertices[i]))
            {
                normalizedVertices.clear();
                return normalizedVertices;
            }

            if (!normalizedVertices.empty() && ArePointsExactlyEqual(normalizedVertices.back(), vertices[i]))
            {
                continue;
            }

            normalizedVertices.push_back(vertices[i]);
        }

        while (normalizedVertices.size() > 1 && ArePointsExactlyEqual(normalizedVertices.front(), normalizedVertices.back()))
        {
            normalizedVertices.pop_back();
        }

        return normalizedVertices;
    }

    std::vector<GB_Point2d> NormalizePolylineVertices(std::vector<GB_Point2d>&& vertices)
    {
        std::vector<GB_Point2d> normalizedVertices;
        normalizedVertices.reserve(vertices.size());

        for (std::size_t i = 0; i < vertices.size(); i++)
        {
            if (!IsValidPoint(vertices[i]))
            {
                normalizedVertices.clear();
                return normalizedVertices;
            }

            if (!normalizedVertices.empty() && ArePointsExactlyEqual(normalizedVertices.back(), vertices[i]))
            {
                continue;
            }

            normalizedVertices.push_back(vertices[i]);
        }

        return normalizedVertices;
    }

    struct PreparedPolygon
    {
        bool isValid = false;
        bool isAxisAlignedRectangle = false;
        GB_Rectangle boundingBox;
        GB_Rectangle axisAlignedRectangle;
        std::vector<GB_Point2d> vertices;
        std::vector<Segment2d> edges;
        std::unique_ptr<SegmentTree> edgeTree;
    };

    struct PreparedPolyline
    {
        bool isValid = false;
        GB_Rectangle boundingBox;
        std::vector<GB_Point2d> vertices;
        std::vector<Segment2d> segments;
        std::unique_ptr<SegmentTree> segmentTree;
    };

    double SquaredLengthFromComponents(double deltaX, double deltaY)
    {
        if (std::isnan(deltaX) || std::isnan(deltaY))
        {
            return std::numeric_limits<double>::quiet_NaN();
        }

        if (!std::isfinite(deltaX) || !std::isfinite(deltaY))
        {
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
        if (!std::isfinite(scaledLengthSquared))
        {
            return std::numeric_limits<double>::max();
        }

        const double sqrtMax = std::sqrt(std::numeric_limits<double>::max());
        if (scale > sqrtMax / std::sqrt(std::max(scaledLengthSquared, std::numeric_limits<double>::min())))
        {
            return std::numeric_limits<double>::max();
        }

        return scale * scale * scaledLengthSquared;
    }

    double PointDistanceSquared(const GB_Point2d& point1, const GB_Point2d& point2)
    {
        if (!IsValidPoint(point1) || !IsValidPoint(point2))
        {
            return std::numeric_limits<double>::quiet_NaN();
        }

        return SquaredLengthFromComponents(point1.x - point2.x, point1.y - point2.y);
    }

    double SegmentLengthSquared(const Segment2d& segment)
    {
        return PointDistanceSquared(segment.point1, segment.point2);
    }

    double CrossProduct(const GB_Point2d& origin, const GB_Point2d& point1, const GB_Point2d& point2)
    {
        const long double vector1X = static_cast<long double>(point1.x) - static_cast<long double>(origin.x);
        const long double vector1Y = static_cast<long double>(point1.y) - static_cast<long double>(origin.y);
        const long double vector2X = static_cast<long double>(point2.x) - static_cast<long double>(origin.x);
        const long double vector2Y = static_cast<long double>(point2.y) - static_cast<long double>(origin.y);
        return ClampLongDoubleToFiniteDouble(vector1X * vector2Y - vector1Y * vector2X);
    }

    double DotProduct(const GB_Point2d& origin, const GB_Point2d& point1, const GB_Point2d& point2)
    {
        const long double vector1X = static_cast<long double>(point1.x) - static_cast<long double>(origin.x);
        const long double vector1Y = static_cast<long double>(point1.y) - static_cast<long double>(origin.y);
        const long double vector2X = static_cast<long double>(point2.x) - static_cast<long double>(origin.x);
        const long double vector2Y = static_cast<long double>(point2.y) - static_cast<long double>(origin.y);
        return ClampLongDoubleToFiniteDouble(vector1X * vector2X + vector1Y * vector2Y);
    }

    double GetParameterTolerance(const Segment2d& segment, double tolerance)
    {
        const double lengthSquared = SegmentLengthSquared(segment);
        if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0)
        {
            return 1e-12;
        }

        const double length = std::sqrt(lengthSquared);
        const double absTolerance = AbsTolerance(tolerance);
        return std::max(1e-12, absTolerance / std::max(length, 1.0));
    }

    int OrientationSign(const GB_Point2d& point1, const GB_Point2d& point2, const GB_Point2d& point3, double tolerance)
    {
        if (!IsValidPoint(point1) || !IsValidPoint(point2) || !IsValidPoint(point3))
        {
            return 0;
        }

        const double crossValue = CrossProduct(point1, point2, point3);
        const double lengthSquared = PointDistanceSquared(point1, point2);
        if (!std::isfinite(crossValue) || !std::isfinite(lengthSquared))
        {
            return 0;
        }

        const double lineLength = std::sqrt(lengthSquared);
        const double areaTolerance = AbsTolerance(tolerance) * std::max(lineLength, 1.0);
        if (std::abs(crossValue) <= areaTolerance)
        {
            return 0;
        }

        return crossValue > 0.0 ? 1 : -1;
    }

    bool RangesOverlap(double minValue1, double maxValue1, double minValue2, double maxValue2, double tolerance)
    {
        const double absTolerance = AbsTolerance(tolerance);
        if (minValue1 > maxValue1)
        {
            std::swap(minValue1, maxValue1);
        }
        if (minValue2 > maxValue2)
        {
            std::swap(minValue2, maxValue2);
        }

        return maxValue1 >= minValue2 - absTolerance && maxValue2 >= minValue1 - absTolerance;
    }

    bool IsPointOnSegment(const GB_Point2d& point, const Segment2d& segment, double tolerance)
    {
        if (!IsValidPoint(point) || !segment.IsValid())
        {
            return false;
        }

        const double absTolerance = AbsTolerance(tolerance);
        const double lengthSquared = SegmentLengthSquared(segment);
        if (!std::isfinite(lengthSquared))
        {
            return false;
        }

        if (lengthSquared <= absTolerance * absTolerance)
        {
            const double distanceSquared = PointDistanceSquared(point, segment.point1);
            return std::isfinite(distanceSquared) && distanceSquared <= absTolerance * absTolerance;
        }

        if (!RangesOverlap(point.x, point.x, segment.point1.x, segment.point2.x, absTolerance) || !RangesOverlap(point.y, point.y, segment.point1.y, segment.point2.y, absTolerance))
        {
            return false;
        }

        const double segmentLength = std::sqrt(lengthSquared);
        const double scaledTolerance = absTolerance * std::max(segmentLength, 1.0);
        const double crossValue = std::abs(CrossProduct(segment.point1, segment.point2, point));
        if (crossValue > scaledTolerance)
        {
            return false;
        }

        const double dot1 = DotProduct(segment.point1, segment.point2, point);
        const double dot2 = DotProduct(segment.point2, segment.point1, point);
        return dot1 >= -scaledTolerance && dot2 >= -scaledTolerance;
    }

    bool IsPointOnInfiniteLine(const GB_Point2d& point, const Segment2d& line, double tolerance)
    {
        if (!IsValidPoint(point) || !line.IsValid())
        {
            return false;
        }

        const double lengthSquared = SegmentLengthSquared(line);
        if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0)
        {
            return IsPointOnSegment(point, line, tolerance);
        }

        const double crossValue = std::abs(CrossProduct(line.point1, line.point2, point));
        return crossValue <= AbsTolerance(tolerance) * std::max(std::sqrt(lengthSquared), 1.0);
    }

    double ParameterOnSegmentLine(const GB_Point2d& point, const Segment2d& segment)
    {
        const double dx = segment.point2.x - segment.point1.x;
        const double dy = segment.point2.y - segment.point1.y;
        if (std::abs(dx) >= std::abs(dy))
        {
            if (std::abs(dx) <= std::numeric_limits<double>::epsilon())
            {
                return 0.0;
            }
            return (point.x - segment.point1.x) / dx;
        }

        if (std::abs(dy) <= std::numeric_limits<double>::epsilon())
        {
            return 0.0;
        }
        return (point.y - segment.point1.y) / dy;
    }

    bool SegmentsIntersect(const Segment2d& segment1, const Segment2d& segment2, double tolerance)
    {
        if (!segment1.IsValid() || !segment2.IsValid())
        {
            return false;
        }

        if (!segment1.range.IsIntersects(segment2.range, tolerance))
        {
            return false;
        }

        if (!RangesOverlap(segment1.point1.x, segment1.point2.x, segment2.point1.x, segment2.point2.x, tolerance) || !RangesOverlap(segment1.point1.y, segment1.point2.y, segment2.point1.y, segment2.point2.y, tolerance))
        {
            return false;
        }

        if (IsPointOnSegment(segment1.point1, segment2, tolerance) || IsPointOnSegment(segment1.point2, segment2, tolerance) || IsPointOnSegment(segment2.point1, segment1, tolerance) || IsPointOnSegment(segment2.point2, segment1, tolerance))
        {
            return true;
        }

        const int orientation1 = OrientationSign(segment1.point1, segment1.point2, segment2.point1, tolerance);
        const int orientation2 = OrientationSign(segment1.point1, segment1.point2, segment2.point2, tolerance);
        const int orientation3 = OrientationSign(segment2.point1, segment2.point2, segment1.point1, tolerance);
        const int orientation4 = OrientationSign(segment2.point1, segment2.point2, segment1.point2, tolerance);

        return orientation1 * orientation2 < 0 && orientation3 * orientation4 < 0;
    }

    std::array<GB_Point2d, 4> GetRectangleCorners(const GB_Rectangle& range)
    {
        std::array<GB_Point2d, 4> corners =
        {
            GB_Point2d(range.minX, range.minY),
            GB_Point2d(range.maxX, range.minY),
            GB_Point2d(range.maxX, range.maxY),
            GB_Point2d(range.minX, range.maxY)
        };
        return corners;
    }

    std::array<Segment2d, 4> GetRectangleEdges(const GB_Rectangle& range)
    {
        const std::array<GB_Point2d, 4> corners = GetRectangleCorners(range);
        std::array<Segment2d, 4> edges =
        {
            Segment2d(corners[0], corners[1]),
            Segment2d(corners[1], corners[2]),
            Segment2d(corners[2], corners[3]),
            Segment2d(corners[3], corners[0])
        };
        return edges;
    }

    bool IsPointStrictlyInsideRectangle(const GB_Rectangle& range, const GB_Point2d& point, double tolerance)
    {
        if (!IsValidRange(range) || !IsValidPoint(point))
        {
            return false;
        }

        const double absTolerance = AbsTolerance(tolerance);
        return point.x > range.minX + absTolerance && point.x < range.maxX - absTolerance && point.y > range.minY + absTolerance && point.y < range.maxY - absTolerance;
    }

    bool SegmentIntersectsRectangle(const Segment2d& segment, const GB_Rectangle& range, double tolerance)
    {
        if (!segment.IsValid() || !IsValidRange(range) || !segment.range.IsIntersects(range, tolerance))
        {
            return false;
        }

        if (range.IsContains(segment.range, tolerance))
        {
            return true;
        }

        if (range.IsContains(segment.point1, tolerance) || range.IsContains(segment.point2, tolerance))
        {
            return true;
        }

        const std::array<Segment2d, 4> edges = GetRectangleEdges(range);
        for (std::size_t i = 0; i < edges.size(); i++)
        {
            if (SegmentsIntersect(segment, edges[i], tolerance))
            {
                return true;
            }
        }

        return false;
    }

    bool ClipSegmentToRectangle(const Segment2d& segment, const GB_Rectangle& range, double& outBeginParameter, double& outEndParameter)
    {
        outBeginParameter = 0.0;
        outEndParameter = 1.0;

        if (!segment.IsValid() || !IsValidRange(range) || !segment.range.IsIntersects(range, 0.0))
        {
            return false;
        }

        const double dx = segment.point2.x - segment.point1.x;
        const double dy = segment.point2.y - segment.point1.y;

        const auto clip = [&outBeginParameter, &outEndParameter](double denominator, double numerator) -> bool
            {
                if (std::abs(denominator) <= std::numeric_limits<double>::epsilon())
                {
                    return numerator >= 0.0;
                }

                const double ratio = numerator / denominator;
                if (denominator < 0.0)
                {
                    if (ratio > outEndParameter)
                    {
                        return false;
                    }
                    if (ratio > outBeginParameter)
                    {
                        outBeginParameter = ratio;
                    }
                }
                else
                {
                    if (ratio < outBeginParameter)
                    {
                        return false;
                    }
                    if (ratio < outEndParameter)
                    {
                        outEndParameter = ratio;
                    }
                }

                return true;
            };

        if (!clip(-dx, segment.point1.x - range.minX))
        {
            return false;
        }
        if (!clip(dx, range.maxX - segment.point1.x))
        {
            return false;
        }
        if (!clip(-dy, segment.point1.y - range.minY))
        {
            return false;
        }
        if (!clip(dy, range.maxY - segment.point1.y))
        {
            return false;
        }

        return outBeginParameter <= outEndParameter;
    }

    GB_Point2d PointAtParameter(const Segment2d& segment, double parameter)
    {
        return GB_Point2d(segment.point1.x + (segment.point2.x - segment.point1.x) * parameter, segment.point1.y + (segment.point2.y - segment.point1.y) * parameter);
    }

    bool SegmentIntersectsRectangleInterior(const Segment2d& segment, const GB_Rectangle& range, double tolerance)
    {
        if (!segment.IsValid() || !IsValidRange(range) || !segment.range.IsIntersects(range, tolerance))
        {
            return false;
        }

        if (IsPointStrictlyInsideRectangle(range, segment.point1, tolerance) || IsPointStrictlyInsideRectangle(range, segment.point2, tolerance))
        {
            return true;
        }

        double beginParameter = 0.0;
        double endParameter = 1.0;
        if (!ClipSegmentToRectangle(segment, range, beginParameter, endParameter))
        {
            return false;
        }

        const double parameterTolerance = GetParameterTolerance(segment, tolerance);
        if (endParameter - beginParameter <= parameterTolerance)
        {
            return false;
        }

        const double middleParameter = 0.5 * (beginParameter + endParameter);
        const GB_Point2d middlePoint = PointAtParameter(segment, middleParameter);
        return IsPointStrictlyInsideRectangle(range, middlePoint, tolerance);
    }

    void AddUniqueParameter(std::vector<double>& parameters, double parameter)
    {
        if (!std::isfinite(parameter))
        {
            return;
        }

        parameters.push_back(ClampDouble(parameter, 0.0, 1.0));
    }

    void AppendIntersectionParametersOnFirstSegment(const Segment2d& segment1, const Segment2d& segment2, std::vector<double>& parameters, double tolerance)
    {
        if (!segment1.range.IsIntersects(segment2.range, tolerance) || !SegmentsIntersect(segment1, segment2, tolerance))
        {
            return;
        }

        if (IsPointOnSegment(segment2.point1, segment1, tolerance))
        {
            AddUniqueParameter(parameters, ParameterOnSegmentLine(segment2.point1, segment1));
        }
        if (IsPointOnSegment(segment2.point2, segment1, tolerance))
        {
            AddUniqueParameter(parameters, ParameterOnSegmentLine(segment2.point2, segment1));
        }
        if (IsPointOnSegment(segment1.point1, segment2, tolerance))
        {
            AddUniqueParameter(parameters, 0.0);
        }
        if (IsPointOnSegment(segment1.point2, segment2, tolerance))
        {
            AddUniqueParameter(parameters, 1.0);
        }

        const double dx1 = segment1.point2.x - segment1.point1.x;
        const double dy1 = segment1.point2.y - segment1.point1.y;
        const double dx2 = segment2.point2.x - segment2.point1.x;
        const double dy2 = segment2.point2.y - segment2.point1.y;
        const double denominator = dx1 * dy2 - dy1 * dx2;
        if (std::abs(denominator) <= std::numeric_limits<double>::epsilon())
        {
            return;
        }

        const double dx3 = segment2.point1.x - segment1.point1.x;
        const double dy3 = segment2.point1.y - segment1.point1.y;
        const double parameter = (dx3 * dy2 - dy3 * dx2) / denominator;
        AddUniqueParameter(parameters, parameter);
    }

    void SortAndUniqueParameters(std::vector<double>& parameters, double parameterTolerance)
    {
        if (parameters.empty())
        {
            return;
        }

        for (std::size_t i = 0; i < parameters.size(); i++)
        {
            parameters[i] = ClampDouble(parameters[i], 0.0, 1.0);
        }

        std::sort(parameters.begin(), parameters.end());

        std::size_t writeIndex = 1;
        for (std::size_t readIndex = 1; readIndex < parameters.size(); readIndex++)
        {
            if (std::abs(parameters[readIndex] - parameters[writeIndex - 1]) > parameterTolerance)
            {
                parameters[writeIndex] = parameters[readIndex];
                writeIndex++;
            }
        }

        parameters.resize(writeIndex);
    }

    enum class PointPolygonLocation : std::uint8_t
    {
        Outside = 0,
        OnBoundary = 1,
        Inside = 2
    };

    PointPolygonLocation ClassifyPointInPolygon(const PreparedPolygon& polygon, const GB_Point2d& point, double tolerance)
    {
        if (!polygon.isValid || !IsValidPoint(point))
        {
            return PointPolygonLocation::Outside;
        }

        if (!polygon.boundingBox.IsContains(point, tolerance))
        {
            return PointPolygonLocation::Outside;
        }

        bool inside = false;
        for (std::size_t i = 0; i < polygon.edges.size(); i++)
        {
            const Segment2d& edge = polygon.edges[i];
            if (!edge.IsValid())
            {
                continue;
            }

            if (edge.range.IsContains(point, tolerance) && IsPointOnSegment(point, edge, tolerance))
            {
                return PointPolygonLocation::OnBoundary;
            }

            const bool crossY = (edge.point1.y > point.y) != (edge.point2.y > point.y);
            if (!crossY)
            {
                continue;
            }

            const double denominator = edge.point2.y - edge.point1.y;
            if (std::abs(denominator) <= std::numeric_limits<double>::epsilon())
            {
                continue;
            }

            const double intersectX = edge.point1.x + (point.y - edge.point1.y) * (edge.point2.x - edge.point1.x) / denominator;
            if (intersectX > point.x)
            {
                inside = !inside;
            }
        }

        return inside ? PointPolygonLocation::Inside : PointPolygonLocation::Outside;
    }

    bool IsPointInPolygonOrOnBoundary(const PreparedPolygon& polygon, const GB_Point2d& point, double tolerance)
    {
        return ClassifyPointInPolygon(polygon, point, tolerance) != PointPolygonLocation::Outside;
    }

    PreparedPolygon PreparePolygon(const GB_Polygon& polygon)
    {
        PreparedPolygon preparedPolygon;
        if (!polygon.IsValid())
        {
            return preparedPolygon;
        }

        GB_Rectangle axisAlignedRectangle;
        if (polygon.TryGetAxisAlignedRectangle(axisAlignedRectangle) && IsValidRange(axisAlignedRectangle))
        {
            preparedPolygon.isValid = true;
            preparedPolygon.isAxisAlignedRectangle = true;
            preparedPolygon.boundingBox = axisAlignedRectangle;
            preparedPolygon.axisAlignedRectangle = axisAlignedRectangle;
            return preparedPolygon;
        }

        preparedPolygon.vertices = NormalizePolygonVertices(polygon.GetVerticesAsDouble());
        if (preparedPolygon.vertices.size() < 3)
        {
            preparedPolygon.vertices.clear();
            return preparedPolygon;
        }

        const double signedArea = ComputeSignedArea(preparedPolygon.vertices);
        if (!std::isfinite(signedArea) || signedArea == 0.0)
        {
            preparedPolygon.vertices.clear();
            return preparedPolygon;
        }

        preparedPolygon.boundingBox = ComputeBoundingBox(preparedPolygon.vertices);
        if (!IsValidRange(preparedPolygon.boundingBox))
        {
            preparedPolygon.vertices.clear();
            return preparedPolygon;
        }

        preparedPolygon.edges.reserve(preparedPolygon.vertices.size());
        for (std::size_t i = 0; i < preparedPolygon.vertices.size(); i++)
        {
            const std::size_t nextIndex = (i + 1) % preparedPolygon.vertices.size();
            const Segment2d edge(preparedPolygon.vertices[i], preparedPolygon.vertices[nextIndex]);
            if (edge.IsValid())
            {
                preparedPolygon.edges.push_back(edge);
            }
        }

        if (preparedPolygon.edges.size() < 3)
        {
            preparedPolygon.vertices.clear();
            preparedPolygon.edges.clear();
            return preparedPolygon;
        }

        preparedPolygon.edgeTree = CreateSegmentTree(preparedPolygon.edges);
        preparedPolygon.isValid = true;
        return preparedPolygon;
    }

    PreparedPolyline PreparePolyline(const GB_Polyline& polyline)
    {
        PreparedPolyline preparedPolyline;
        if (!polyline.IsValid())
        {
            return preparedPolyline;
        }

        std::vector<GB_Point2d> polylineVertices = polyline.GetVertices();
        preparedPolyline.vertices = NormalizePolylineVertices(std::move(polylineVertices));
        if (preparedPolyline.vertices.empty())
        {
            return preparedPolyline;
        }

        if (preparedPolyline.vertices.size() == 1)
        {
            preparedPolyline.boundingBox = ComputeBoundingBox(preparedPolyline.vertices);
            if (!IsValidRange(preparedPolyline.boundingBox))
            {
                preparedPolyline.vertices.clear();
                return preparedPolyline;
            }

            preparedPolyline.segments.push_back(Segment2d(preparedPolyline.vertices[0], preparedPolyline.vertices[0]));
            preparedPolyline.isValid = true;
            return preparedPolyline;
        }

        preparedPolyline.boundingBox = ComputeBoundingBox(preparedPolyline.vertices);
        if (!IsValidRange(preparedPolyline.boundingBox))
        {
            preparedPolyline.vertices.clear();
            return preparedPolyline;
        }

        preparedPolyline.segments.reserve(preparedPolyline.vertices.size() - 1);
        for (std::size_t i = 0; i + 1 < preparedPolyline.vertices.size(); i++)
        {
            const Segment2d segment(preparedPolyline.vertices[i], preparedPolyline.vertices[i + 1]);
            if (segment.IsValid())
            {
                preparedPolyline.segments.push_back(segment);
            }
        }

        if (preparedPolyline.segments.empty())
        {
            preparedPolyline.vertices.clear();
            return preparedPolyline;
        }

        preparedPolyline.segmentTree = CreateSegmentTree(preparedPolyline.segments);
        preparedPolyline.isValid = true;
        return preparedPolyline;
    }

    bool RectangleIntersectsPolygon(const GB_Rectangle& range, const PreparedPolygon& polygon, double tolerance)
    {
        if (!IsValidRange(range) || !polygon.isValid || !range.IsIntersects(polygon.boundingBox, tolerance))
        {
            return false;
        }

        if (range.IsContains(polygon.boundingBox, tolerance))
        {
            return true;
        }

        const std::array<GB_Point2d, 4> corners = GetRectangleCorners(range);
        for (std::size_t i = 0; i < corners.size(); i++)
        {
            if (IsPointInPolygonOrOnBoundary(polygon, corners[i], tolerance))
            {
                return true;
            }
        }

        bool foundIntersection = false;
        const auto checkEdge = [&polygon, &range, tolerance, &foundIntersection](std::size_t edgeIndex) -> bool
            {
                if (edgeIndex >= polygon.edges.size())
                {
                    return true;
                }

                const Segment2d& edge = polygon.edges[edgeIndex];
                if (!edge.range.IsIntersects(range, tolerance))
                {
                    return true;
                }

                if (SegmentIntersectsRectangle(edge, range, tolerance))
                {
                    foundIntersection = true;
                    return false;
                }

                return true;
            };

        if (polygon.edgeTree.get() != nullptr)
        {
            VisitSegmentTree(*polygon.edgeTree, range, tolerance, checkEdge);
            return foundIntersection;
        }

        for (std::size_t i = 0; i < polygon.edges.size(); i++)
        {
            if (!checkEdge(i))
            {
                return true;
            }
        }

        return false;
    }

    bool SegmentCoveredByPolygon(const Segment2d& segment, const PreparedPolygon& polygon, double tolerance)
    {
        if (!segment.IsValid() || !polygon.isValid)
        {
            return false;
        }

        if (!IsPointInPolygonOrOnBoundary(polygon, segment.point1, tolerance) || !IsPointInPolygonOrOnBoundary(polygon, segment.point2, tolerance))
        {
            return false;
        }

        const double lengthSquared = SegmentLengthSquared(segment);
        if (!std::isfinite(lengthSquared))
        {
            return false;
        }
        if (lengthSquared <= AbsTolerance(tolerance) * AbsTolerance(tolerance))
        {
            return true;
        }

        std::vector<double> parameters;
        parameters.reserve(std::min<std::size_t>(polygon.edges.size() + 2, 64));
        parameters.push_back(0.0);
        parameters.push_back(1.0);

        const auto appendParameters = [&segment, &polygon, &parameters, tolerance](std::size_t edgeIndex) -> bool
            {
                if (edgeIndex < polygon.edges.size())
                {
                    AppendIntersectionParametersOnFirstSegment(segment, polygon.edges[edgeIndex], parameters, tolerance);
                }
                return true;
            };

        if (polygon.edgeTree.get() != nullptr)
        {
            VisitSegmentTree(*polygon.edgeTree, segment.range, tolerance, appendParameters);
        }
        else
        {
            for (std::size_t i = 0; i < polygon.edges.size(); i++)
            {
                appendParameters(i);
            }
        }

        const double parameterTolerance = GetParameterTolerance(segment, tolerance);
        SortAndUniqueParameters(parameters, parameterTolerance);

        for (std::size_t i = 0; i + 1 < parameters.size(); i++)
        {
            if (parameters[i + 1] - parameters[i] <= parameterTolerance)
            {
                continue;
            }

            const double middleParameter = 0.5 * (parameters[i] + parameters[i + 1]);
            const GB_Point2d middlePoint = PointAtParameter(segment, middleParameter);
            if (!IsPointInPolygonOrOnBoundary(polygon, middlePoint, tolerance))
            {
                return false;
            }
        }

        return true;
    }

    bool RectangleCoveredByPolygon(const GB_Rectangle& range, const PreparedPolygon& polygon, double tolerance)
    {
        if (!IsValidRange(range) || !polygon.isValid || !polygon.boundingBox.IsContains(range, tolerance))
        {
            return false;
        }

        const double absTolerance = AbsTolerance(tolerance);
        const double width = range.Width();
        const double height = range.Height();
        if (!std::isfinite(width) || !std::isfinite(height))
        {
            return false;
        }

        if (width <= absTolerance && height <= absTolerance)
        {
            return IsPointInPolygonOrOnBoundary(polygon, range.Center(), tolerance);
        }

        if (width <= absTolerance)
        {
            return SegmentCoveredByPolygon(Segment2d(GB_Point2d(range.minX, range.minY), GB_Point2d(range.minX, range.maxY)), polygon, tolerance);
        }

        if (height <= absTolerance)
        {
            return SegmentCoveredByPolygon(Segment2d(GB_Point2d(range.minX, range.minY), GB_Point2d(range.maxX, range.minY)), polygon, tolerance);
        }

        const std::array<GB_Point2d, 4> corners = GetRectangleCorners(range);
        for (std::size_t i = 0; i < corners.size(); i++)
        {
            if (!IsPointInPolygonOrOnBoundary(polygon, corners[i], tolerance))
            {
                return false;
            }
        }

        bool hasInteriorIntersection = false;
        const auto checkInteriorIntersection = [&polygon, &range, tolerance, &hasInteriorIntersection](std::size_t edgeIndex) -> bool
            {
                if (edgeIndex >= polygon.edges.size())
                {
                    return true;
                }

                const Segment2d& edge = polygon.edges[edgeIndex];
                if (!edge.range.IsIntersects(range, tolerance))
                {
                    return true;
                }

                if (SegmentIntersectsRectangleInterior(edge, range, tolerance))
                {
                    hasInteriorIntersection = true;
                    return false;
                }

                return true;
            };

        if (polygon.edgeTree.get() != nullptr)
        {
            VisitSegmentTree(*polygon.edgeTree, range, tolerance, checkInteriorIntersection);
        }
        else
        {
            for (std::size_t i = 0; i < polygon.edges.size(); i++)
            {
                if (!checkInteriorIntersection(i))
                {
                    break;
                }
            }
        }

        return !hasInteriorIntersection;
    }

    bool RectangleContainsPolygon(const GB_Rectangle& range, const PreparedPolygon& polygon, double tolerance)
    {
        if (!IsValidRange(range) || !polygon.isValid || !range.IsContains(polygon.boundingBox, tolerance))
        {
            return false;
        }

        return true;
    }

    bool RectangleIntersectsPolyline(const GB_Rectangle& range, const PreparedPolyline& polyline, double tolerance)
    {
        if (!IsValidRange(range) || !polyline.isValid || !range.IsIntersects(polyline.boundingBox, tolerance))
        {
            return false;
        }

        if (range.IsContains(polyline.boundingBox, tolerance))
        {
            return true;
        }

        bool foundIntersection = false;
        const auto checkSegment = [&polyline, &range, tolerance, &foundIntersection](std::size_t segmentIndex) -> bool
            {
                if (segmentIndex >= polyline.segments.size())
                {
                    return true;
                }

                const Segment2d& currentSegment = polyline.segments[segmentIndex];
                if (!currentSegment.range.IsIntersects(range, tolerance))
                {
                    return true;
                }

                if (SegmentIntersectsRectangle(currentSegment, range, tolerance))
                {
                    foundIntersection = true;
                    return false;
                }

                return true;
            };

        if (polyline.segmentTree.get() != nullptr)
        {
            VisitSegmentTree(*polyline.segmentTree, range, tolerance, checkSegment);
            return foundIntersection;
        }

        for (std::size_t i = 0; i < polyline.segments.size(); i++)
        {
            if (!checkSegment(i))
            {
                return true;
            }
        }

        return false;
    }

    bool SegmentCoveredByPolyline(const Segment2d& segment, const PreparedPolyline& polyline, double tolerance)
    {
        if (!segment.IsValid() || !polyline.isValid)
        {
            return false;
        }

        const double absTolerance = AbsTolerance(tolerance);
        const double lengthSquared = SegmentLengthSquared(segment);
        if (!std::isfinite(lengthSquared))
        {
            return false;
        }

        if (lengthSquared <= absTolerance * absTolerance)
        {
            bool foundPoint = false;
            const auto checkPointSegment = [&segment, &polyline, tolerance, &foundPoint](std::size_t segmentIndex) -> bool
                {
                    if (segmentIndex < polyline.segments.size() && IsPointOnSegment(segment.point1, polyline.segments[segmentIndex], tolerance))
                    {
                        foundPoint = true;
                        return false;
                    }
                    return true;
                };

            if (polyline.segmentTree.get() != nullptr)
            {
                VisitSegmentTree(*polyline.segmentTree, segment.range, tolerance, checkPointSegment);
                return foundPoint;
            }

            for (std::size_t i = 0; i < polyline.segments.size(); i++)
            {
                if (!checkPointSegment(i))
                {
                    return true;
                }
            }
            return false;
        }

        std::vector<std::pair<double, double>> intervals;
        intervals.reserve(std::min<std::size_t>(polyline.segments.size(), 64));

        const auto appendInterval = [&segment, &polyline, &intervals, tolerance](std::size_t segmentIndex) -> bool
            {
                if (segmentIndex >= polyline.segments.size())
                {
                    return true;
                }

                const Segment2d& polylineSegment = polyline.segments[segmentIndex];
                if (!polylineSegment.IsValid() || !polylineSegment.range.IsIntersects(segment.range, tolerance))
                {
                    return true;
                }

                if (!IsPointOnInfiniteLine(polylineSegment.point1, segment, tolerance) || !IsPointOnInfiniteLine(polylineSegment.point2, segment, tolerance))
                {
                    return true;
                }

                double beginParameter = ParameterOnSegmentLine(polylineSegment.point1, segment);
                double endParameter = ParameterOnSegmentLine(polylineSegment.point2, segment);
                if (beginParameter > endParameter)
                {
                    std::swap(beginParameter, endParameter);
                }

                beginParameter = std::max(0.0, beginParameter);
                endParameter = std::min(1.0, endParameter);
                if (beginParameter <= endParameter)
                {
                    intervals.push_back(std::make_pair(beginParameter, endParameter));
                }

                return true;
            };

        if (polyline.segmentTree.get() != nullptr)
        {
            VisitSegmentTree(*polyline.segmentTree, segment.range, tolerance, appendInterval);
        }
        else
        {
            for (std::size_t i = 0; i < polyline.segments.size(); i++)
            {
                appendInterval(i);
            }
        }

        if (intervals.empty())
        {
            return false;
        }

        std::sort(intervals.begin(), intervals.end());
        const double parameterTolerance = GetParameterTolerance(segment, tolerance);
        double coveredEnd = 0.0;
        bool hasStarted = false;

        for (std::size_t i = 0; i < intervals.size(); i++)
        {
            const double beginParameter = ClampDouble(intervals[i].first, 0.0, 1.0);
            const double endParameter = ClampDouble(intervals[i].second, 0.0, 1.0);
            if (endParameter < beginParameter)
            {
                continue;
            }

            if (!hasStarted)
            {
                if (beginParameter > parameterTolerance)
                {
                    return false;
                }
                coveredEnd = endParameter;
                hasStarted = true;
            }
            else
            {
                if (beginParameter > coveredEnd + parameterTolerance)
                {
                    return false;
                }
                if (endParameter > coveredEnd)
                {
                    coveredEnd = endParameter;
                }
            }

            if (coveredEnd >= 1.0 - parameterTolerance)
            {
                return true;
            }
        }

        return hasStarted && coveredEnd >= 1.0 - parameterTolerance;
    }

    bool RectangleCoveredByPolyline(const GB_Rectangle& range, const PreparedPolyline& polyline, double tolerance)
    {
        if (!IsValidRange(range) || !polyline.isValid || !polyline.boundingBox.IsContains(range, tolerance))
        {
            return false;
        }

        const double absTolerance = AbsTolerance(tolerance);
        const double width = range.Width();
        const double height = range.Height();
        if (!std::isfinite(width) || !std::isfinite(height))
        {
            return false;
        }

        if (width <= absTolerance && height <= absTolerance)
        {
            bool foundPoint = false;
            const GB_Point2d center = range.Center();
            const auto checkSegment = [&polyline, &range, &center, tolerance, &foundPoint](std::size_t segmentIndex) -> bool
                {
                    if (segmentIndex >= polyline.segments.size())
                    {
                        return true;
                    }

                    const Segment2d& currentSegment = polyline.segments[segmentIndex];
                    if (!currentSegment.range.IsIntersects(range, tolerance))
                    {
                        return true;
                    }

                    if (IsPointOnSegment(center, currentSegment, tolerance))
                    {
                        foundPoint = true;
                        return false;
                    }

                    return true;
                };

            if (polyline.segmentTree.get() != nullptr)
            {
                VisitSegmentTree(*polyline.segmentTree, range, tolerance, checkSegment);
                return foundPoint;
            }

            for (std::size_t i = 0; i < polyline.segments.size(); i++)
            {
                if (!checkSegment(i))
                {
                    return true;
                }
            }
            return false;
        }

        if (width <= absTolerance)
        {
            return SegmentCoveredByPolyline(Segment2d(GB_Point2d(range.minX, range.minY), GB_Point2d(range.minX, range.maxY)), polyline, tolerance);
        }

        if (height <= absTolerance)
        {
            return SegmentCoveredByPolyline(Segment2d(GB_Point2d(range.minX, range.minY), GB_Point2d(range.maxX, range.minY)), polyline, tolerance);
        }

        return false;
    }

    bool RectangleContainsPolyline(const GB_Rectangle& range, const PreparedPolyline& polyline, double tolerance)
    {
        if (!IsValidRange(range) || !polyline.isValid || !range.IsContains(polyline.boundingBox, tolerance))
        {
            return false;
        }

        return true;
    }

    bool MatchPolygonRelation(const GB_Rectangle& recordRange, const PreparedPolygon& polygon, GB_SpatialIndex::QueryRelation relation, double tolerance)
    {
        if (polygon.isAxisAlignedRectangle)
        {
            switch (relation)
            {
            case GB_SpatialIndex::QueryRelation::Intersects:
                return recordRange.IsIntersects(polygon.axisAlignedRectangle, tolerance);
            case GB_SpatialIndex::QueryRelation::CoveredByQuery:
                return polygon.axisAlignedRectangle.IsContains(recordRange, tolerance);
            case GB_SpatialIndex::QueryRelation::ContainsQuery:
                return recordRange.IsContains(polygon.axisAlignedRectangle, tolerance);
            default:
                return false;
            }
        }

        switch (relation)
        {
        case GB_SpatialIndex::QueryRelation::Intersects:
            return RectangleIntersectsPolygon(recordRange, polygon, tolerance);
        case GB_SpatialIndex::QueryRelation::CoveredByQuery:
            return RectangleCoveredByPolygon(recordRange, polygon, tolerance);
        case GB_SpatialIndex::QueryRelation::ContainsQuery:
            return RectangleContainsPolygon(recordRange, polygon, tolerance);
        default:
            return false;
        }
    }

    bool MatchPolylineRelation(const GB_Rectangle& recordRange, const PreparedPolyline& polyline, GB_SpatialIndex::QueryRelation relation, double tolerance)
    {
        switch (relation)
        {
        case GB_SpatialIndex::QueryRelation::Intersects:
            return RectangleIntersectsPolyline(recordRange, polyline, tolerance);
        case GB_SpatialIndex::QueryRelation::CoveredByQuery:
            return RectangleCoveredByPolyline(recordRange, polyline, tolerance);
        case GB_SpatialIndex::QueryRelation::ContainsQuery:
            return RectangleContainsPolyline(recordRange, polyline, tolerance);
        default:
            return false;
        }
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

    std::size_t GetMaxReasonableThreadCount();

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

        const std::size_t maxReasonableThreadCount = GetMaxReasonableThreadCount();
        if (threadCount > maxReasonableThreadCount)
        {
            threadCount = maxReasonableThreadCount;
        }

        return threadCount;
    }

    template<typename TValue, typename... TArgs>
    std::unique_ptr<TValue> MakeUnique(TArgs&&... args)
    {
        return std::unique_ptr<TValue>(new TValue(std::forward<TArgs>(args)...));
    }

    std::size_t GetMaxReasonableThreadCount()
    {
        const std::size_t hardwareThreadCount = GetHardwareThreadCount();
        return std::max<std::size_t>(1, hardwareThreadCount * 2);
    }

    class ThreadJoiner
    {
    public:
        explicit ThreadJoiner(std::vector<std::thread>& threads)
            : threads_(threads)
        {
        }

        ~ThreadJoiner()
        {
            for (std::size_t i = 0; i < threads_.size(); i++)
            {
                if (threads_[i].joinable())
                {
                    threads_[i].join();
                }
            }
        }

    private:
        std::vector<std::thread>& threads_;
    };

    enum class TreeQueryMode : std::uint8_t
    {
        Intersects = 0,
        CoveredBy = 1,
        Covers = 2
    };

    typedef std::function<bool(std::size_t recordIndex)> TreeIndexFilter;
    typedef std::function<bool(std::size_t recordIndex)> TreeIndexConsumer;

    class ITreeWrapper
    {
    public:
        virtual ~ITreeWrapper()
        {
        }

        virtual void ForEach(const BoostBox& queryBox, TreeQueryMode queryMode, const TreeIndexFilter& filter, const TreeIndexConsumer& consumer) const = 0;
        virtual std::unique_ptr<ITreeWrapper> CloneWithInsertedValues(const std::vector<TreeValue>& values) const = 0;
    };

    template<typename TParameters>
    class TreeWrapper : public ITreeWrapper
    {
    public:
        explicit TreeWrapper(const std::vector<TreeValue>& values, const TParameters& parameters)
            : tree_(values.begin(), values.end(), parameters)
        {
        }

        virtual void ForEach(const BoostBox& queryBox, TreeQueryMode queryMode, const TreeIndexFilter& filter, const TreeIndexConsumer& consumer) const override
        {
            if (!consumer)
            {
                return;
            }

            switch (queryMode)
            {
            case TreeQueryMode::CoveredBy:
                ForEachByPredicate(bgi::covered_by(queryBox), filter, consumer);
                return;
            case TreeQueryMode::Covers:
                ForEachByPredicate(bgi::covers(queryBox), filter, consumer);
                return;
            case TreeQueryMode::Intersects:
            default:
                ForEachByPredicate(bgi::intersects(queryBox), filter, consumer);
                return;
            }
        }

        virtual std::unique_ptr<ITreeWrapper> CloneWithInsertedValues(const std::vector<TreeValue>& values) const override
        {
            std::unique_ptr<TreeWrapper<TParameters>> result(new TreeWrapper<TParameters>(*this));
            for (std::size_t i = 0; i < values.size(); i++)
            {
                result->tree_.insert(values[i]);
            }
            return std::unique_ptr<ITreeWrapper>(result.release());
        }

    private:
        template<typename TPredicate>
        void ForEachByPredicate(const TPredicate& predicate, const TreeIndexFilter& filter, const TreeIndexConsumer& consumer) const
        {
            if (filter)
            {
                const auto combinedPredicate = predicate && bgi::satisfies([&filter](const TreeValue& value) -> bool
                    {
                        return filter(value.second);
                    });
                ForEachByIterator(combinedPredicate, consumer);
                return;
            }

            ForEachByIterator(predicate, consumer);
        }

        template<typename TPredicate>
        void ForEachByIterator(const TPredicate& predicate, const TreeIndexConsumer& consumer) const
        {
            for (auto iterator = tree_.qbegin(predicate); iterator != tree_.qend(); iterator++)
            {
                if (!consumer(iterator->second))
                {
                    break;
                }
            }
        }

    private:
        bgi::rtree<TreeValue, TParameters> tree_;
    };

    struct CandidateTreeQuery
    {
        GB_Rectangle range;
        TreeQueryMode queryMode = TreeQueryMode::Intersects;
    };

    CandidateTreeQuery MakeCandidateTreeQuery(const GB_Rectangle& range, GB_SpatialIndex::QueryRelation relation, double tolerance)
    {
        CandidateTreeQuery query;
        switch (relation)
        {
        case GB_SpatialIndex::QueryRelation::CoveredByQuery:
            query.range = MakeQueryRange(range, tolerance);
            query.queryMode = TreeQueryMode::CoveredBy;
            return query;
        case GB_SpatialIndex::QueryRelation::ContainsQuery:
            // ContainsQuery 的公开语义是“含边界的包含”。
            // tolerance > 0 时，记录只需要覆盖查询范围向内收缩后的核心区域即可成为候选；
            // 对点、线或容差过大的退化场景，退回到相交粗过滤以避免漏查。
            if (AbsTolerance(tolerance) > 0.0)
            {
                const GB_Rectangle innerRange = MakeInnerQueryRange(range, tolerance);
                if (IsValidRange(innerRange))
                {
                    query.range = innerRange;
                    query.queryMode = TreeQueryMode::Covers;
                    return query;
                }

                query.range = MakeQueryRange(range, tolerance);
                query.queryMode = TreeQueryMode::Intersects;
                return query;
            }

            query.range = range;
            query.queryMode = TreeQueryMode::Covers;
            return query;
        case GB_SpatialIndex::QueryRelation::Intersects:
            query.range = MakeQueryRange(range, tolerance);
            query.queryMode = TreeQueryMode::Intersects;
            return query;
        default:
            query.range = GB_Rectangle::Invalid;
            query.queryMode = TreeQueryMode::Intersects;
            return query;
        }
    }

    std::vector<TreeValue> BuildTreeValues(const std::vector<GB_SpatialIndex::Record>& records, const GB_SpatialIndex::BuildOptions& options, std::size_t beginIndex, std::size_t endIndex)
    {
        std::vector<TreeValue> values;
        if (records.empty() || beginIndex >= endIndex || beginIndex >= records.size())
        {
            return values;
        }

        if (endIndex > records.size())
        {
            endIndex = records.size();
        }

        const std::size_t valueCount = endIndex - beginIndex;
        const std::size_t threadCount = GetBuildThreadCount(options, valueCount);
        if (threadCount <= 1)
        {
            values.reserve(valueCount);
            for (std::size_t i = beginIndex; i < endIndex; i++)
            {
                values.emplace_back(ToBoostBox(records[i].range), i);
            }
            return values;
        }

        values.resize(valueCount);

        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        ThreadJoiner threadJoiner(threads);

        for (std::size_t threadIndex = 0; threadIndex < threadCount; threadIndex++)
        {
            const std::size_t localBeginIndex = valueCount * threadIndex / threadCount;
            const std::size_t localEndIndex = valueCount * (threadIndex + 1) / threadCount;

            threads.push_back(std::thread([beginIndex, localBeginIndex, localEndIndex, &records, &values]()
                {
                    for (std::size_t localIndex = localBeginIndex; localIndex < localEndIndex; localIndex++)
                    {
                        const std::size_t recordIndex = beginIndex + localIndex;
                        values[localIndex] = TreeValue(ToBoostBox(records[recordIndex].range), recordIndex);
                    }
                }));
        }

        for (std::size_t i = 0; i < threads.size(); i++)
        {
            threads[i].join();
        }

        return values;
    }

    std::vector<TreeValue> BuildTreeValues(const std::vector<GB_SpatialIndex::Record>& records, const GB_SpatialIndex::BuildOptions& options)
    {
        return BuildTreeValues(records, options, 0, records.size());
    }

    template<typename TParameters>
    std::unique_ptr<ITreeWrapper> CreateTreeWithParameters(const std::vector<TreeValue>& values, const TParameters& parameters)
    {
        return MakeUnique<TreeWrapper<TParameters>>(values, parameters);
    }

    template<std::size_t MaxElements>
    std::unique_ptr<ITreeWrapper> CreateTreeWithCapacity(const std::vector<TreeValue>& values, GB_SpatialIndex::SplitAlgorithm splitAlgorithm)
    {
        constexpr std::size_t minElements = (MaxElements < 4) ? 1 : (MaxElements / 2);

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
        if (statistics != nullptr)
        {
            statistics->inputRecordCount = inputRecords.size();
        }

        if (!options.skipInvalidRecords)
        {
            std::size_t invalidRecordCount = 0;
            for (std::size_t i = 0; i < inputRecords.size(); i++)
            {
                if (!inputRecords[i].IsValid())
                {
                    invalidRecordCount++;
                }
            }

            if (statistics != nullptr)
            {
                statistics->skippedInvalidRecordCount = invalidRecordCount;
                statistics->acceptedRecordCount = invalidRecordCount == 0 ? inputRecords.size() : 0;
            }

            if (invalidRecordCount != 0)
            {
                return std::vector<GB_SpatialIndex::Record>();
            }

            return inputRecords;
        }

        std::vector<GB_SpatialIndex::Record> records;
        records.reserve(inputRecords.size());

        for (std::size_t i = 0; i < inputRecords.size(); i++)
        {
            if (!inputRecords[i].IsValid())
            {
                if (statistics != nullptr)
                {
                    statistics->skippedInvalidRecordCount++;
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
        if (statistics != nullptr)
        {
            statistics->inputRecordCount = inputRecords.size();
        }

        if (!options.skipInvalidRecords)
        {
            std::size_t invalidRecordCount = 0;
            for (std::size_t i = 0; i < inputRecords.size(); i++)
            {
                if (!inputRecords[i].IsValid())
                {
                    invalidRecordCount++;
                }
            }

            if (statistics != nullptr)
            {
                statistics->skippedInvalidRecordCount = invalidRecordCount;
                statistics->acceptedRecordCount = invalidRecordCount == 0 ? inputRecords.size() : 0;
            }

            if (invalidRecordCount != 0)
            {
                return std::vector<GB_SpatialIndex::Record>();
            }

            return std::move(inputRecords);
        }

        std::size_t writeIndex = 0;
        for (std::size_t readIndex = 0; readIndex < inputRecords.size(); readIndex++)
        {
            if (!inputRecords[readIndex].IsValid())
            {
                if (statistics != nullptr)
                {
                    statistics->skippedInvalidRecordCount++;
                }
                continue;
            }

            if (writeIndex != readIndex)
            {
                inputRecords[writeIndex] = std::move(inputRecords[readIndex]);
            }
            writeIndex++;
        }

        inputRecords.resize(writeIndex);

        if (statistics != nullptr)
        {
            statistics->acceptedRecordCount = inputRecords.size();
        }

        return std::move(inputRecords);
    }

}

struct GB_SpatialIndex::Impl
{
    std::vector<Record> records;
    std::unique_ptr<ITreeWrapper> tree;
    SplitAlgorithm splitAlgorithm;
    NodeCapacity nodeCapacity;

    Impl()
        : splitAlgorithm(SplitAlgorithm::RStar), nodeCapacity(NodeCapacity::Normal16)
    {
    }

    explicit Impl(std::vector<Record>&& inputRecords, const BuildOptions& options)
        : records(std::move(inputRecords)), splitAlgorithm(options.splitAlgorithm), nodeCapacity(options.nodeCapacity)
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

    Impl(std::vector<Record>&& inputRecords, std::unique_ptr<ITreeWrapper>&& inputTree, const BuildOptions& options)
        : records(std::move(inputRecords)), tree(std::move(inputTree)), splitAlgorithm(options.splitAlgorithm), nodeCapacity(options.nodeCapacity)
    {
        if (options.shrinkRecordsToFit)
        {
            records.shrink_to_fit();
        }
    }

    bool HasSameTreeOptions(const BuildOptions& options) const
    {
        return splitAlgorithm == options.splitAlgorithm && nodeCapacity == options.nodeCapacity;
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

GB_SpatialIndex::BuildOptions::BuildOptions()
    : splitAlgorithm(SplitAlgorithm::RStar),
    nodeCapacity(NodeCapacity::Normal16),
    skipInvalidRecords(true),
    shrinkRecordsToFit(false),
    buildThreadCount(0),
    parallelBuildThreshold(200000),
    preferIncrementalInsert(true),
    incrementalInsertThreshold(1024)
{
}

GB_SpatialIndex::QueryOptions::QueryOptions()
    : tolerance(0.0),
    maxResults(0),
    includeValue(true)
{
}

GB_SpatialIndex::BuildStatistics::BuildStatistics()
    : succeeded(false),
    inputRecordCount(0),
    acceptedRecordCount(0),
    skippedInvalidRecordCount(0)
{
}

GB_SpatialIndex::Record::Record()
    : id(0)
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

std::shared_ptr<const GB_SpatialIndex::Impl> GB_SpatialIndex::CreateImplByAppendingRecords(const std::shared_ptr<const Impl>& snapshot, std::vector<Record>&& newRecords, const BuildOptions& options)
{
    const std::size_t oldRecordCount = snapshot != nullptr ? snapshot->records.size() : 0;

    std::vector<Record> records;
    records.reserve(oldRecordCount + newRecords.size());
    if (snapshot != nullptr)
    {
        records.insert(records.end(), snapshot->records.begin(), snapshot->records.end());
    }

    for (std::size_t i = 0; i < newRecords.size(); i++)
    {
        records.push_back(std::move(newRecords[i]));
    }

    const bool shouldUseIncrementalInsert = options.preferIncrementalInsert && options.incrementalInsertThreshold != 0 && newRecords.size() <= options.incrementalInsertThreshold && snapshot != nullptr && !snapshot->records.empty() && snapshot->tree.get() != nullptr && snapshot->HasSameTreeOptions(options);
    if (shouldUseIncrementalInsert)
    {
        const std::vector<TreeValue> appendedValues = BuildTreeValues(records, options, oldRecordCount, records.size());
        std::unique_ptr<ITreeWrapper> newTree = snapshot->tree->CloneWithInsertedValues(appendedValues);
        return std::make_shared<Impl>(std::move(records), std::move(newTree), options);
    }

    return std::make_shared<Impl>(std::move(records), options);
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
    BuildStatistics statistics;

    try
    {
        std::lock_guard<std::mutex> lockGuard(writeMutex_);

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
    catch (...)
    {
        statistics.succeeded = false;
        if (outStatistics != nullptr)
        {
            *outStatistics = statistics;
        }
        return false;
    }
}

bool GB_SpatialIndex::Build(std::vector<Record>&& records, const BuildOptions& options, BuildStatistics* outStatistics)
{
    BuildStatistics statistics;

    try
    {
        std::lock_guard<std::mutex> lockGuard(writeMutex_);

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
    catch (...)
    {
        statistics.succeeded = false;
        if (outStatistics != nullptr)
        {
            *outStatistics = statistics;
        }
        return false;
    }
}

bool GB_SpatialIndex::Insert(const Record& record, const BuildOptions& options)
{
    if (!record.IsValid())
    {
        return false;
    }

    try
    {
        std::lock_guard<std::mutex> lockGuard(writeMutex_);

        const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
        std::vector<Record> newRecords;
        newRecords.reserve(1);
        newRecords.push_back(record);
        std::shared_ptr<const Impl> newImpl = CreateImplByAppendingRecords(snapshot, std::move(newRecords), options);
        if (newImpl == nullptr || !newImpl->IsValid())
        {
            return false;
        }

        StoreSnapshot(newImpl);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool GB_SpatialIndex::Insert(Record&& record, const BuildOptions& options)
{
    if (!record.IsValid())
    {
        return false;
    }

    try
    {
        std::lock_guard<std::mutex> lockGuard(writeMutex_);

        const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
        std::vector<Record> newRecords;
        newRecords.reserve(1);
        newRecords.push_back(std::move(record));
        std::shared_ptr<const Impl> newImpl = CreateImplByAppendingRecords(snapshot, std::move(newRecords), options);
        if (newImpl == nullptr || !newImpl->IsValid())
        {
            return false;
        }

        StoreSnapshot(newImpl);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool GB_SpatialIndex::Insert(const std::vector<Record>& inputRecords, const BuildOptions& options, BuildStatistics* outStatistics)
{
    BuildStatistics statistics;

    try
    {
        std::vector<Record> newRecords = FilterRecords(inputRecords, options, &statistics);
        if (!options.skipInvalidRecords && statistics.skippedInvalidRecordCount > 0)
        {
            statistics.succeeded = false;
            if (outStatistics != nullptr)
            {
                *outStatistics = statistics;
            }
            return false;
        }

        if (newRecords.empty())
        {
            statistics.succeeded = true;
            if (outStatistics != nullptr)
            {
                *outStatistics = statistics;
            }
            return true;
        }

        std::lock_guard<std::mutex> lockGuard(writeMutex_);

        const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
        std::shared_ptr<const Impl> newImpl = CreateImplByAppendingRecords(snapshot, std::move(newRecords), options);
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
    catch (...)
    {
        statistics.succeeded = false;
        if (outStatistics != nullptr)
        {
            *outStatistics = statistics;
        }
        return false;
    }
}

bool GB_SpatialIndex::Insert(std::vector<Record>&& inputRecords, const BuildOptions& options, BuildStatistics* outStatistics)
{
    BuildStatistics statistics;

    try
    {
        std::vector<Record> newRecords = FilterRecords(std::move(inputRecords), options, &statistics);
        if (!options.skipInvalidRecords && statistics.skippedInvalidRecordCount > 0)
        {
            statistics.succeeded = false;
            if (outStatistics != nullptr)
            {
                *outStatistics = statistics;
            }
            return false;
        }

        if (newRecords.empty())
        {
            statistics.succeeded = true;
            if (outStatistics != nullptr)
            {
                *outStatistics = statistics;
            }
            return true;
        }

        std::lock_guard<std::mutex> lockGuard(writeMutex_);

        const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
        std::shared_ptr<const Impl> newImpl = CreateImplByAppendingRecords(snapshot, std::move(newRecords), options);
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
    catch (...)
    {
        statistics.succeeded = false;
        if (outStatistics != nullptr)
        {
            *outStatistics = statistics;
        }
        return false;
    }
}

std::size_t GB_SpatialIndex::RemoveById(std::uint64_t id, const BuildOptions& options)
{
    try
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

        std::shared_ptr<const Impl> newImpl = std::make_shared<Impl>(std::move(records), options);
        if (newImpl == nullptr || !newImpl->IsValid())
        {
            return 0;
        }

        StoreSnapshot(newImpl);
        return removedCount;
    }
    catch (...)
    {
        return 0;
    }
}

std::size_t GB_SpatialIndex::RemoveByIds(const std::vector<std::uint64_t>& ids, const BuildOptions& options)
{
    if (ids.empty())
    {
        return 0;
    }

    if (ids.size() == 1)
    {
        return RemoveById(ids.front(), options);
    }

    try
    {
        std::unordered_set<std::uint64_t> idSet;
        idSet.max_load_factor(0.7f);
        idSet.reserve(ids.size());
        idSet.insert(ids.begin(), ids.end());

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

        std::shared_ptr<const Impl> newImpl = std::make_shared<Impl>(std::move(records), options);
        if (newImpl == nullptr || !newImpl->IsValid())
        {
            return 0;
        }

        StoreSnapshot(newImpl);
        return removedCount;
    }
    catch (...)
    {
        return 0;
    }
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
    return QueryRecordsInternal(range, RecordFilter(), relation, options, false);
}

std::vector<GB_SpatialIndex::QueryResult> GB_SpatialIndex::QueryRecords(const GB_Polygon& polygon, QueryRelation relation, const QueryOptions& options) const
{
    const PreparedPolygon preparedPolygon = PreparePolygon(polygon);
    if (!preparedPolygon.isValid)
    {
        return std::vector<QueryResult>();
    }

    const double tolerance = options.tolerance;
    const RecordFilter filter = [&preparedPolygon, relation, tolerance](const Record& record) -> bool
        {
            return MatchPolygonRelation(record.range, preparedPolygon, relation, tolerance);
        };

    return QueryRecordsInternal(preparedPolygon.boundingBox, filter, relation, options, true);
}

std::vector<GB_SpatialIndex::QueryResult> GB_SpatialIndex::QueryRecords(const GB_Polyline& polyline, QueryRelation relation, const QueryOptions& options) const
{
    const PreparedPolyline preparedPolyline = PreparePolyline(polyline);
    if (!preparedPolyline.isValid)
    {
        return std::vector<QueryResult>();
    }

    const double tolerance = options.tolerance;
    const RecordFilter filter = [&preparedPolyline, relation, tolerance](const Record& record) -> bool
        {
            return MatchPolylineRelation(record.range, preparedPolyline, relation, tolerance);
        };

    return QueryRecordsInternal(preparedPolyline.boundingBox, filter, relation, options, true);
}

std::vector<std::uint64_t> GB_SpatialIndex::QueryIds(const GB_Rectangle& range, QueryRelation relation, const QueryOptions& options) const
{
    return QueryIdsInternal(range, RecordFilter(), relation, options, false);
}

std::vector<std::uint64_t> GB_SpatialIndex::QueryIds(const GB_Polygon& polygon, QueryRelation relation, const QueryOptions& options) const
{
    const PreparedPolygon preparedPolygon = PreparePolygon(polygon);
    if (!preparedPolygon.isValid)
    {
        return std::vector<std::uint64_t>();
    }

    const double tolerance = options.tolerance;
    const RecordFilter filter = [&preparedPolygon, relation, tolerance](const Record& record) -> bool
        {
            return MatchPolygonRelation(record.range, preparedPolygon, relation, tolerance);
        };

    return QueryIdsInternal(preparedPolygon.boundingBox, filter, relation, options, true);
}

std::vector<std::uint64_t> GB_SpatialIndex::QueryIds(const GB_Polyline& polyline, QueryRelation relation, const QueryOptions& options) const
{
    const PreparedPolyline preparedPolyline = PreparePolyline(polyline);
    if (!preparedPolyline.isValid)
    {
        return std::vector<std::uint64_t>();
    }

    const double tolerance = options.tolerance;
    const RecordFilter filter = [&preparedPolyline, relation, tolerance](const Record& record) -> bool
        {
            return MatchPolylineRelation(record.range, preparedPolyline, relation, tolerance);
        };

    return QueryIdsInternal(preparedPolyline.boundingBox, filter, relation, options, true);
}

std::vector<GB_SpatialIndex::QueryResult> GB_SpatialIndex::QueryRecords(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation, const QueryOptions& options) const
{
    return QueryRecordsInternal(range, filter, relation, options, false);
}

std::vector<GB_SpatialIndex::QueryResult> GB_SpatialIndex::QueryRecordsInternal(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation, const QueryOptions& options, bool filterHandlesRelation) const
{
    std::vector<QueryResult> results;

    const CandidateTreeQuery candidateQuery = MakeCandidateTreeQuery(range, relation, options.tolerance);
    if (!IsValidRange(candidateQuery.range))
    {
        return results;
    }

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    if (snapshot == nullptr || snapshot->tree.get() == nullptr || snapshot->records.empty())
    {
        return results;
    }

    if (options.maxResults != 0)
    {
        results.reserve(std::min(options.maxResults, snapshot->records.size()));
    }
    else
    {
        results.reserve(std::min<std::size_t>(snapshot->records.size(), 256));
    }

    const double tolerance = options.tolerance;
    const bool hasFilter = static_cast<bool>(filter);
    const bool needsRelationCheck = !filterHandlesRelation && AbsTolerance(tolerance) > 0.0;
    TreeIndexFilter indexFilter;

    if (hasFilter || needsRelationCheck)
    {
        indexFilter = [&snapshot, &range, relation, tolerance, &filter, filterHandlesRelation](std::size_t recordIndex) -> bool
            {
                if (recordIndex >= snapshot->records.size())
                {
                    return false;
                }

                const Record& record = snapshot->records[recordIndex];
                if (!filterHandlesRelation && !MatchRelation(record.range, range, relation, tolerance))
                {
                    return false;
                }

                return !filter || filter(record);
            };
    }

    const bool includeValue = options.includeValue;
    const std::size_t maxResults = options.maxResults;
    const TreeIndexConsumer consumer = [&snapshot, &results, includeValue, maxResults](std::size_t recordIndex) -> bool
        {
            if (recordIndex >= snapshot->records.size())
            {
                return true;
            }

            const Record& record = snapshot->records[recordIndex];
            QueryResult result;
            result.id = record.id;
            result.range = record.range;
            if (includeValue)
            {
                result.value = record.value;
            }

            results.push_back(std::move(result));
            return maxResults == 0 || results.size() < maxResults;
        };

    snapshot->tree->ForEach(ToBoostBox(candidateQuery.range), candidateQuery.queryMode, indexFilter, consumer);

    return results;
}

std::vector<std::uint64_t> GB_SpatialIndex::QueryIds(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation, const QueryOptions& options) const
{
    return QueryIdsInternal(range, filter, relation, options, false);
}

std::vector<std::uint64_t> GB_SpatialIndex::QueryIdsInternal(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation, const QueryOptions& options, bool filterHandlesRelation) const
{
    std::vector<std::uint64_t> results;

    const CandidateTreeQuery candidateQuery = MakeCandidateTreeQuery(range, relation, options.tolerance);
    if (!IsValidRange(candidateQuery.range))
    {
        return results;
    }

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    if (snapshot == nullptr || snapshot->tree.get() == nullptr || snapshot->records.empty())
    {
        return results;
    }

    if (options.maxResults != 0)
    {
        results.reserve(std::min(options.maxResults, snapshot->records.size()));
    }
    else
    {
        results.reserve(std::min<std::size_t>(snapshot->records.size(), 256));
    }

    const double tolerance = options.tolerance;
    const bool hasFilter = static_cast<bool>(filter);
    const bool needsRelationCheck = !filterHandlesRelation && AbsTolerance(tolerance) > 0.0;
    TreeIndexFilter indexFilter;

    if (hasFilter || needsRelationCheck)
    {
        indexFilter = [&snapshot, &range, relation, tolerance, &filter, filterHandlesRelation](std::size_t recordIndex) -> bool
            {
                if (recordIndex >= snapshot->records.size())
                {
                    return false;
                }

                const Record& record = snapshot->records[recordIndex];
                if (!filterHandlesRelation && !MatchRelation(record.range, range, relation, tolerance))
                {
                    return false;
                }

                return !filter || filter(record);
            };
    }

    const std::size_t maxResults = options.maxResults;
    const TreeIndexConsumer consumer = [&snapshot, &results, maxResults](std::size_t recordIndex) -> bool
        {
            if (recordIndex >= snapshot->records.size())
            {
                return true;
            }

            results.push_back(snapshot->records[recordIndex].id);
            return maxResults == 0 || results.size() < maxResults;
        };

    snapshot->tree->ForEach(ToBoostBox(candidateQuery.range), candidateQuery.queryMode, indexFilter, consumer);

    return results;
}


std::size_t GB_SpatialIndex::ForEachRecord(const GB_Rectangle& range, const QueryResultVisitor& visitor, QueryRelation relation, const QueryOptions& options) const
{
    return ForEachRecordInternal(range, RecordFilter(), visitor, relation, options, false);
}

std::size_t GB_SpatialIndex::ForEachRecord(const GB_Polygon& polygon, const QueryResultVisitor& visitor, QueryRelation relation, const QueryOptions& options) const
{
    const PreparedPolygon preparedPolygon = PreparePolygon(polygon);
    if (!preparedPolygon.isValid)
    {
        return 0;
    }

    const double tolerance = options.tolerance;
    const RecordFilter filter = [&preparedPolygon, relation, tolerance](const Record& record) -> bool
        {
            return MatchPolygonRelation(record.range, preparedPolygon, relation, tolerance);
        };

    return ForEachRecordInternal(preparedPolygon.boundingBox, filter, visitor, relation, options, true);
}

std::size_t GB_SpatialIndex::ForEachRecord(const GB_Polyline& polyline, const QueryResultVisitor& visitor, QueryRelation relation, const QueryOptions& options) const
{
    const PreparedPolyline preparedPolyline = PreparePolyline(polyline);
    if (!preparedPolyline.isValid)
    {
        return 0;
    }

    const double tolerance = options.tolerance;
    const RecordFilter filter = [&preparedPolyline, relation, tolerance](const Record& record) -> bool
        {
            return MatchPolylineRelation(record.range, preparedPolyline, relation, tolerance);
        };

    return ForEachRecordInternal(preparedPolyline.boundingBox, filter, visitor, relation, options, true);
}

std::size_t GB_SpatialIndex::ForEachRecord(const GB_Rectangle& range, const RecordFilter& filter, const QueryResultVisitor& visitor, QueryRelation relation, const QueryOptions& options) const
{
    return ForEachRecordInternal(range, filter, visitor, relation, options, false);
}

std::size_t GB_SpatialIndex::ForEachId(const GB_Rectangle& range, const IdVisitor& visitor, QueryRelation relation, const QueryOptions& options) const
{
    return ForEachIdInternal(range, RecordFilter(), visitor, relation, options, false);
}

std::size_t GB_SpatialIndex::ForEachId(const GB_Polygon& polygon, const IdVisitor& visitor, QueryRelation relation, const QueryOptions& options) const
{
    const PreparedPolygon preparedPolygon = PreparePolygon(polygon);
    if (!preparedPolygon.isValid)
    {
        return 0;
    }

    const double tolerance = options.tolerance;
    const RecordFilter filter = [&preparedPolygon, relation, tolerance](const Record& record) -> bool
        {
            return MatchPolygonRelation(record.range, preparedPolygon, relation, tolerance);
        };

    return ForEachIdInternal(preparedPolygon.boundingBox, filter, visitor, relation, options, true);
}

std::size_t GB_SpatialIndex::ForEachId(const GB_Polyline& polyline, const IdVisitor& visitor, QueryRelation relation, const QueryOptions& options) const
{
    const PreparedPolyline preparedPolyline = PreparePolyline(polyline);
    if (!preparedPolyline.isValid)
    {
        return 0;
    }

    const double tolerance = options.tolerance;
    const RecordFilter filter = [&preparedPolyline, relation, tolerance](const Record& record) -> bool
        {
            return MatchPolylineRelation(record.range, preparedPolyline, relation, tolerance);
        };

    return ForEachIdInternal(preparedPolyline.boundingBox, filter, visitor, relation, options, true);
}

std::size_t GB_SpatialIndex::ForEachId(const GB_Rectangle& range, const RecordFilter& filter, const IdVisitor& visitor, QueryRelation relation, const QueryOptions& options) const
{
    return ForEachIdInternal(range, filter, visitor, relation, options, false);
}

std::size_t GB_SpatialIndex::QueryCount(const GB_Rectangle& range, QueryRelation relation, const QueryOptions& options) const
{
    return QueryCount(range, RecordFilter(), relation, options);
}

std::size_t GB_SpatialIndex::QueryCount(const GB_Polygon& polygon, QueryRelation relation, const QueryOptions& options) const
{
    std::size_t count = 0;
    QueryOptions countOptions = options;
    countOptions.includeValue = false;
    ForEachId(polygon, [&count](std::uint64_t) -> bool
        {
            count++;
            return true;
        }, relation, countOptions);
    return count;
}

std::size_t GB_SpatialIndex::QueryCount(const GB_Polyline& polyline, QueryRelation relation, const QueryOptions& options) const
{
    std::size_t count = 0;
    QueryOptions countOptions = options;
    countOptions.includeValue = false;
    ForEachId(polyline, [&count](std::uint64_t) -> bool
        {
            count++;
            return true;
        }, relation, countOptions);
    return count;
}

std::size_t GB_SpatialIndex::QueryCount(const GB_Rectangle& range, const RecordFilter& filter, QueryRelation relation, const QueryOptions& options) const
{
    std::size_t count = 0;
    QueryOptions countOptions = options;
    countOptions.includeValue = false;
    ForEachId(range, filter, [&count](std::uint64_t) -> bool
        {
            count++;
            return true;
        }, relation, countOptions);
    return count;
}

std::size_t GB_SpatialIndex::ForEachRecordInternal(const GB_Rectangle& range, const RecordFilter& filter, const QueryResultVisitor& visitor, QueryRelation relation, const QueryOptions& options, bool filterHandlesRelation) const
{
    if (!visitor)
    {
        return 0;
    }

    const CandidateTreeQuery candidateQuery = MakeCandidateTreeQuery(range, relation, options.tolerance);
    if (!IsValidRange(candidateQuery.range))
    {
        return 0;
    }

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    if (snapshot == nullptr || snapshot->tree.get() == nullptr || snapshot->records.empty())
    {
        return 0;
    }

    const double tolerance = options.tolerance;
    const bool hasFilter = static_cast<bool>(filter);
    const bool needsRelationCheck = !filterHandlesRelation && AbsTolerance(tolerance) > 0.0;
    TreeIndexFilter indexFilter;

    if (hasFilter || needsRelationCheck)
    {
        indexFilter = [&snapshot, &range, relation, tolerance, &filter, filterHandlesRelation](std::size_t recordIndex) -> bool
            {
                if (recordIndex >= snapshot->records.size())
                {
                    return false;
                }

                const Record& record = snapshot->records[recordIndex];
                if (!filterHandlesRelation && !MatchRelation(record.range, range, relation, tolerance))
                {
                    return false;
                }

                return !filter || filter(record);
            };
    }

    const bool includeValue = options.includeValue;
    const std::size_t maxResults = options.maxResults;
    std::size_t visitedCount = 0;
    const TreeIndexConsumer consumer = [&snapshot, &visitor, includeValue, maxResults, &visitedCount](std::size_t recordIndex) -> bool
        {
            if (recordIndex >= snapshot->records.size())
            {
                return true;
            }

            const Record& record = snapshot->records[recordIndex];
            QueryResult result;
            result.id = record.id;
            result.range = record.range;
            if (includeValue)
            {
                result.value = record.value;
            }

            visitedCount++;
            if (!visitor(result))
            {
                return false;
            }

            return maxResults == 0 || visitedCount < maxResults;
        };

    snapshot->tree->ForEach(ToBoostBox(candidateQuery.range), candidateQuery.queryMode, indexFilter, consumer);
    return visitedCount;
}

std::size_t GB_SpatialIndex::ForEachIdInternal(const GB_Rectangle& range, const RecordFilter& filter, const IdVisitor& visitor, QueryRelation relation, const QueryOptions& options, bool filterHandlesRelation) const
{
    if (!visitor)
    {
        return 0;
    }

    const CandidateTreeQuery candidateQuery = MakeCandidateTreeQuery(range, relation, options.tolerance);
    if (!IsValidRange(candidateQuery.range))
    {
        return 0;
    }

    const std::shared_ptr<const Impl> snapshot = LoadSnapshot();
    if (snapshot == nullptr || snapshot->tree.get() == nullptr || snapshot->records.empty())
    {
        return 0;
    }

    const double tolerance = options.tolerance;
    const bool hasFilter = static_cast<bool>(filter);
    const bool needsRelationCheck = !filterHandlesRelation && AbsTolerance(tolerance) > 0.0;
    TreeIndexFilter indexFilter;

    if (hasFilter || needsRelationCheck)
    {
        indexFilter = [&snapshot, &range, relation, tolerance, &filter, filterHandlesRelation](std::size_t recordIndex) -> bool
            {
                if (recordIndex >= snapshot->records.size())
                {
                    return false;
                }

                const Record& record = snapshot->records[recordIndex];
                if (!filterHandlesRelation && !MatchRelation(record.range, range, relation, tolerance))
                {
                    return false;
                }

                return !filter || filter(record);
            };
    }

    const std::size_t maxResults = options.maxResults;
    std::size_t visitedCount = 0;
    const TreeIndexConsumer consumer = [&snapshot, &visitor, maxResults, &visitedCount](std::size_t recordIndex) -> bool
        {
            if (recordIndex >= snapshot->records.size())
            {
                return true;
            }

            visitedCount++;
            if (!visitor(snapshot->records[recordIndex].id))
            {
                return false;
            }

            return maxResults == 0 || visitedCount < maxResults;
        };

    snapshot->tree->ForEach(ToBoostBox(candidateQuery.range), candidateQuery.queryMode, indexFilter, consumer);
    return visitedCount;
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
