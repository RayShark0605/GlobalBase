#include "GB_LineSegment.h"
#include "GB_Vector2d.h"
#include "GB_Rectangle.h"
#include "GB_Matrix3x3.h"
#include "../GB_IO.h"

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4819)
#endif
#include <boost/multiprecision/cpp_bin_float.hpp>
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace
{
    using HighPrecisionFloat = boost::multiprecision::cpp_bin_float_quad;

    static inline double AbsTol(double tolerance)
    {
        if (!std::isfinite(tolerance))
        {
            return 0;
        }
        return std::abs(tolerance);
    }

    static inline bool IsFinite4(double a, double b, double c, double d)
    {
        return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d);
    }

    static inline GB_Point2d MakeNanPoint()
    {
        return GB_Point2d(GB_QuietNan, GB_QuietNan);
    }

    static inline GB_Vector2d MakeNanVector()
    {
        return GB_Vector2d(GB_QuietNan, GB_QuietNan);
    }

    static inline int ComparePointLexicographically(const GB_Point2d& firstPoint, const GB_Point2d& secondPoint)
    {
        if (firstPoint.x < secondPoint.x)
        {
            return -1;
        }
        if (firstPoint.x > secondPoint.x)
        {
            return 1;
        }
        if (firstPoint.y < secondPoint.y)
        {
            return -1;
        }
        if (firstPoint.y > secondPoint.y)
        {
            return 1;
        }
        return 0;
    }

    static inline bool IsExactDegenerate(const GB_LineSegment& segment)
    {
        return segment.point1.x == segment.point2.x && segment.point1.y == segment.point2.y;
    }

    static bool TryGetUnitDirectionAndLength(const GB_LineSegment& segment, double& unitX, double& unitY, double& length)
    {
        unitX = GB_QuietNan;
        unitY = GB_QuietNan;
        length = GB_QuietNan;
        if (!segment.IsValid())
        {
            return false;
        }

        length = segment.point1.DistanceTo(segment.point2);
        if (std::isnan(length) || length == 0.0)
        {
            return false;
        }

        const double directDeltaX = segment.point2.x - segment.point1.x;
        const double directDeltaY = segment.point2.y - segment.point1.y;
        if (std::isfinite(directDeltaX) && std::isfinite(directDeltaY))
        {
            const double componentScale = std::max(std::abs(directDeltaX), std::abs(directDeltaY));
            if (componentScale == 0.0)
            {
                return false;
            }

            const double scaledDeltaX = directDeltaX / componentScale;
            const double scaledDeltaY = directDeltaY / componentScale;
            const double scaledLength = std::hypot(scaledDeltaX, scaledDeltaY);
            if (!std::isfinite(scaledLength) || scaledLength <= 0.0)
            {
                return false;
            }

            unitX = scaledDeltaX / scaledLength;
            unitY = scaledDeltaY / scaledLength;
            return std::isfinite(unitX) && std::isfinite(unitY);
        }

        const double coordinateScale = std::max(std::max(std::abs(segment.point1.x), std::abs(segment.point1.y)), std::max(std::abs(segment.point2.x), std::abs(segment.point2.y)));
        if (!std::isfinite(coordinateScale) || coordinateScale <= 0.0)
        {
            return false;
        }

        const double scaledDeltaX = segment.point2.x / coordinateScale - segment.point1.x / coordinateScale;
        const double scaledDeltaY = segment.point2.y / coordinateScale - segment.point1.y / coordinateScale;
        const double scaledLength = std::hypot(scaledDeltaX, scaledDeltaY);
        if (!std::isfinite(scaledLength) || scaledLength <= 0.0)
        {
            return false;
        }

        unitX = scaledDeltaX / scaledLength;
        unitY = scaledDeltaY / scaledLength;
        return std::isfinite(unitX) && std::isfinite(unitY);
    }

    static HighPrecisionFloat GetHighPrecisionSegmentLength(const GB_LineSegment& segment)
    {
        const HighPrecisionFloat deltaX = HighPrecisionFloat(segment.point2.x) - HighPrecisionFloat(segment.point1.x);
        const HighPrecisionFloat deltaY = HighPrecisionFloat(segment.point2.y) - HighPrecisionFloat(segment.point1.y);
        return boost::multiprecision::sqrt(deltaX * deltaX + deltaY * deltaY);
    }

    static bool TryGetParameterHighPrecision(const GB_LineSegment& segment, const GB_Point2d& point, HighPrecisionFloat& parameter)
    {
        if (!segment.IsValid() || !point.IsValid())
        {
            return false;
        }

        const HighPrecisionFloat segmentDeltaX = HighPrecisionFloat(segment.point2.x) - HighPrecisionFloat(segment.point1.x);
        const HighPrecisionFloat segmentDeltaY = HighPrecisionFloat(segment.point2.y) - HighPrecisionFloat(segment.point1.y);
        const HighPrecisionFloat denominator = segmentDeltaX * segmentDeltaX + segmentDeltaY * segmentDeltaY;
        if (denominator == 0)
        {
            return false;
        }

        const HighPrecisionFloat pointDeltaX = HighPrecisionFloat(point.x) - HighPrecisionFloat(segment.point1.x);
        const HighPrecisionFloat pointDeltaY = HighPrecisionFloat(point.y) - HighPrecisionFloat(segment.point1.y);
        parameter = (pointDeltaX * segmentDeltaX + pointDeltaY * segmentDeltaY) / denominator;
        return true;
    }

    static double HighPrecisionToDoubleOrInfinity(const HighPrecisionFloat& value)
    {
        const HighPrecisionFloat maxValue = HighPrecisionFloat((std::numeric_limits<double>::max)());
        if (value > maxValue)
        {
            return std::numeric_limits<double>::infinity();
        }
        if (value < -maxValue)
        {
            return -std::numeric_limits<double>::infinity();
        }
        return static_cast<double>(value);
    }

    static double NonNegativeHighPrecisionToDoubleOrInfinity(const HighPrecisionFloat& value)
    {
        if (value < 0)
        {
            return GB_QuietNan;
        }

        const HighPrecisionFloat maxValue = HighPrecisionFloat((std::numeric_limits<double>::max)());
        return value > maxValue ? std::numeric_limits<double>::infinity() : static_cast<double>(value);
    }

    static double HighPrecisionToFiniteDoubleOrNan(const HighPrecisionFloat& value)
    {
        const HighPrecisionFloat maxValue = HighPrecisionFloat((std::numeric_limits<double>::max)());
        if (value > maxValue || value < -maxValue)
        {
            return GB_QuietNan;
        }
        return static_cast<double>(value);
    }

    static bool TryProjectPointOnLineHighPrecision(const GB_LineSegment& segment, const GB_Point2d& point, GB_Point2d& projectedPoint)
    {
        HighPrecisionFloat parameter = 0;
        if (!TryGetParameterHighPrecision(segment, point, parameter))
        {
            return false;
        }

        const HighPrecisionFloat segmentDeltaX = HighPrecisionFloat(segment.point2.x) - HighPrecisionFloat(segment.point1.x);
        const HighPrecisionFloat segmentDeltaY = HighPrecisionFloat(segment.point2.y) - HighPrecisionFloat(segment.point1.y);
        const HighPrecisionFloat projectedX = HighPrecisionFloat(segment.point1.x) + segmentDeltaX * parameter;
        const HighPrecisionFloat projectedY = HighPrecisionFloat(segment.point1.y) + segmentDeltaY * parameter;
        const double resultX = HighPrecisionToFiniteDoubleOrNan(projectedX);
        const double resultY = HighPrecisionToFiniteDoubleOrNan(projectedY);
        if (!std::isfinite(resultX) || !std::isfinite(resultY))
        {
            return false;
        }

        projectedPoint.Set(resultX, resultY);
        return projectedPoint.IsValid();
    }

    static bool TryGetParameterFast(const GB_LineSegment& segment, const GB_Point2d& point, double& parameter)
    {
        parameter = GB_QuietNan;
        const double segmentDeltaX = segment.point2.x - segment.point1.x;
        const double segmentDeltaY = segment.point2.y - segment.point1.y;
        const double pointDeltaX = point.x - segment.point1.x;
        const double pointDeltaY = point.y - segment.point1.y;
        if (!std::isfinite(segmentDeltaX) || !std::isfinite(segmentDeltaY) || !std::isfinite(pointDeltaX) || !std::isfinite(pointDeltaY))
        {
            return false;
        }

        const double segmentScale = std::max(std::abs(segmentDeltaX), std::abs(segmentDeltaY));
        if (segmentScale <= 0.0)
        {
            return false;
        }

        const double normalizedSegmentX = segmentDeltaX / segmentScale;
        const double normalizedSegmentY = segmentDeltaY / segmentScale;
        const double scaledPointX = pointDeltaX / segmentScale;
        const double scaledPointY = pointDeltaY / segmentScale;
        if (!std::isfinite(scaledPointX) || !std::isfinite(scaledPointY))
        {
            return false;
        }

        const double denominator = normalizedSegmentX * normalizedSegmentX + normalizedSegmentY * normalizedSegmentY;
        const double numerator = scaledPointX * normalizedSegmentX + scaledPointY * normalizedSegmentY;
        if (!std::isfinite(denominator) || denominator <= 0.0 || !std::isfinite(numerator))
        {
            return false;
        }

        parameter = numerator / denominator;
        return std::isfinite(parameter);
    }

    static double DistanceToLineHighPrecision(const GB_LineSegment& segment, const GB_Point2d& point)
    {
        const HighPrecisionFloat segmentDeltaX = HighPrecisionFloat(segment.point2.x) - HighPrecisionFloat(segment.point1.x);
        const HighPrecisionFloat segmentDeltaY = HighPrecisionFloat(segment.point2.y) - HighPrecisionFloat(segment.point1.y);
        const HighPrecisionFloat length = boost::multiprecision::sqrt(segmentDeltaX * segmentDeltaX + segmentDeltaY * segmentDeltaY);
        if (length == 0)
        {
            return segment.point1.DistanceTo(point);
        }

        const HighPrecisionFloat pointDeltaX = HighPrecisionFloat(point.x) - HighPrecisionFloat(segment.point1.x);
        const HighPrecisionFloat pointDeltaY = HighPrecisionFloat(point.y) - HighPrecisionFloat(segment.point1.y);
        const HighPrecisionFloat cross = segmentDeltaX * pointDeltaY - segmentDeltaY * pointDeltaX;
        const HighPrecisionFloat distance = boost::multiprecision::abs(cross) / length;
        return NonNegativeHighPrecisionToDoubleOrInfinity(distance);
    }

    static int SideOfPointHighPrecision(const GB_LineSegment& segment, const GB_Point2d& point)
    {
        const HighPrecisionFloat segmentDeltaX = HighPrecisionFloat(segment.point2.x) - HighPrecisionFloat(segment.point1.x);
        const HighPrecisionFloat segmentDeltaY = HighPrecisionFloat(segment.point2.y) - HighPrecisionFloat(segment.point1.y);
        const HighPrecisionFloat pointDeltaX = HighPrecisionFloat(point.x) - HighPrecisionFloat(segment.point1.x);
        const HighPrecisionFloat pointDeltaY = HighPrecisionFloat(point.y) - HighPrecisionFloat(segment.point1.y);
        const HighPrecisionFloat cross = segmentDeltaX * pointDeltaY - segmentDeltaY * pointDeltaX;
        if (cross > 0)
        {
            return 1;
        }
        if (cross < 0)
        {
            return -1;
        }
        return 0;
    }

    static bool TryGetIntersectionParametersFast(const GB_LineSegment& firstSegment, const GB_LineSegment& secondSegment, double& firstParameter, double& secondParameter)
    {
        firstParameter = GB_QuietNan;
        secondParameter = GB_QuietNan;

        const double firstDeltaX = firstSegment.point2.x - firstSegment.point1.x;
        const double firstDeltaY = firstSegment.point2.y - firstSegment.point1.y;
        const double secondDeltaX = secondSegment.point2.x - secondSegment.point1.x;
        const double secondDeltaY = secondSegment.point2.y - secondSegment.point1.y;
        const double pointDeltaX = secondSegment.point1.x - firstSegment.point1.x;
        const double pointDeltaY = secondSegment.point1.y - firstSegment.point1.y;
        if (!std::isfinite(firstDeltaX) || !std::isfinite(firstDeltaY) || !std::isfinite(secondDeltaX) || !std::isfinite(secondDeltaY) || !std::isfinite(pointDeltaX) || !std::isfinite(pointDeltaY))
        {
            return false;
        }

        const double denominatorProduct1 = firstDeltaX * secondDeltaY;
        const double denominatorProduct2 = firstDeltaY * secondDeltaX;
        const double firstNumeratorProduct1 = pointDeltaX * secondDeltaY;
        const double firstNumeratorProduct2 = pointDeltaY * secondDeltaX;
        const double secondNumeratorProduct1 = pointDeltaX * firstDeltaY;
        const double secondNumeratorProduct2 = pointDeltaY * firstDeltaX;
        if (std::isfinite(denominatorProduct1) && std::isfinite(denominatorProduct2)
            && std::isfinite(firstNumeratorProduct1) && std::isfinite(firstNumeratorProduct2)
            && std::isfinite(secondNumeratorProduct1) && std::isfinite(secondNumeratorProduct2))
        {
            const double denominator = denominatorProduct1 - denominatorProduct2;
            const double firstNumerator = firstNumeratorProduct1 - firstNumeratorProduct2;
            const double secondNumerator = secondNumeratorProduct1 - secondNumeratorProduct2;
            if (std::isfinite(denominator) && denominator != 0.0 && std::isfinite(firstNumerator) && std::isfinite(secondNumerator))
            {
                firstParameter = firstNumerator / denominator;
                secondParameter = secondNumerator / denominator;
                if (std::isfinite(firstParameter) && std::isfinite(secondParameter))
                {
                    return true;
                }
            }
        }

        const double commonScale = std::max(std::max(std::max(std::abs(firstDeltaX), std::abs(firstDeltaY)), std::max(std::abs(secondDeltaX), std::abs(secondDeltaY))), std::max(std::abs(pointDeltaX), std::abs(pointDeltaY)));
        if (!std::isfinite(commonScale) || commonScale <= 0.0)
        {
            return false;
        }

        const double normalizedFirstDeltaX = firstDeltaX / commonScale;
        const double normalizedFirstDeltaY = firstDeltaY / commonScale;
        const double normalizedSecondDeltaX = secondDeltaX / commonScale;
        const double normalizedSecondDeltaY = secondDeltaY / commonScale;
        const double normalizedPointDeltaX = pointDeltaX / commonScale;
        const double normalizedPointDeltaY = pointDeltaY / commonScale;
        const double denominator = normalizedFirstDeltaX * normalizedSecondDeltaY - normalizedFirstDeltaY * normalizedSecondDeltaX;
        if (!std::isfinite(denominator) || denominator == 0.0)
        {
            return false;
        }

        firstParameter = (normalizedPointDeltaX * normalizedSecondDeltaY - normalizedPointDeltaY * normalizedSecondDeltaX) / denominator;
        secondParameter = (normalizedPointDeltaX * normalizedFirstDeltaY - normalizedPointDeltaY * normalizedFirstDeltaX) / denominator;
        return std::isfinite(firstParameter) && std::isfinite(secondParameter);
    }

    static bool TryGetIntersectionParametersHighPrecision(const GB_LineSegment& firstSegment, const GB_LineSegment& secondSegment, double& firstParameter, double& secondParameter)
    {
        const HighPrecisionFloat firstDeltaX = HighPrecisionFloat(firstSegment.point2.x) - HighPrecisionFloat(firstSegment.point1.x);
        const HighPrecisionFloat firstDeltaY = HighPrecisionFloat(firstSegment.point2.y) - HighPrecisionFloat(firstSegment.point1.y);
        const HighPrecisionFloat secondDeltaX = HighPrecisionFloat(secondSegment.point2.x) - HighPrecisionFloat(secondSegment.point1.x);
        const HighPrecisionFloat secondDeltaY = HighPrecisionFloat(secondSegment.point2.y) - HighPrecisionFloat(secondSegment.point1.y);
        const HighPrecisionFloat pointDeltaX = HighPrecisionFloat(secondSegment.point1.x) - HighPrecisionFloat(firstSegment.point1.x);
        const HighPrecisionFloat pointDeltaY = HighPrecisionFloat(secondSegment.point1.y) - HighPrecisionFloat(firstSegment.point1.y);
        const HighPrecisionFloat denominator = firstDeltaX * secondDeltaY - firstDeltaY * secondDeltaX;
        if (denominator == 0)
        {
            return false;
        }

        const HighPrecisionFloat firstParameterHigh = (pointDeltaX * secondDeltaY - pointDeltaY * secondDeltaX) / denominator;
        const HighPrecisionFloat secondParameterHigh = (pointDeltaX * firstDeltaY - pointDeltaY * firstDeltaX) / denominator;
        firstParameter = HighPrecisionToDoubleOrInfinity(firstParameterHigh);
        secondParameter = HighPrecisionToDoubleOrInfinity(secondParameterHigh);
        return !std::isnan(firstParameter) && !std::isnan(secondParameter);
    }

    static inline bool IsParameterInRange(double parameter, double tolerance)
    {
        return parameter >= -tolerance && parameter <= 1.0 + tolerance;
    }

    static inline double ClampUnitParameter(double parameter)
    {
        if (parameter < 0.0)
        {
            return 0.0;
        }
        if (parameter > 1.0)
        {
            return 1.0;
        }
        return parameter;
    }

    static inline bool DoBoundingRectanglesOverlap(const GB_LineSegment& firstSegment, const GB_LineSegment& secondSegment, double tolerance)
    {
        const double firstMinX = std::min(firstSegment.point1.x, firstSegment.point2.x);
        const double firstMaxX = std::max(firstSegment.point1.x, firstSegment.point2.x);
        const double firstMinY = std::min(firstSegment.point1.y, firstSegment.point2.y);
        const double firstMaxY = std::max(firstSegment.point1.y, firstSegment.point2.y);
        const double secondMinX = std::min(secondSegment.point1.x, secondSegment.point2.x);
        const double secondMaxX = std::max(secondSegment.point1.x, secondSegment.point2.x);
        const double secondMinY = std::min(secondSegment.point1.y, secondSegment.point2.y);
        const double secondMaxY = std::max(secondSegment.point1.y, secondSegment.point2.y);

        return !(firstMaxX + tolerance < secondMinX || secondMaxX + tolerance < firstMinX || firstMaxY + tolerance < secondMinY || secondMaxY + tolerance < firstMinY);
    }
}

const GB_LineSegment GB_LineSegment::Invalid = GB_LineSegment();

GB_LineSegment::GB_LineSegment()
{
}

GB_LineSegment::GB_LineSegment(const GB_Point2d& point1, const GB_Point2d& point2)
{
    Set(point1, point2);
}

GB_LineSegment::GB_LineSegment(double x1, double y1, double x2, double y2)
{
    Set(x1, y1, x2, y2);
}

GB_LineSegment::GB_LineSegment(const GB_Point2d& startPoint, const GB_Vector2d& direction, double length)
{
    if (!startPoint.IsValid() || !std::isfinite(length) || length < 0)
    {
        Reset();
        return;
    }

    if (length == 0.0)
    {
        Set(startPoint, startPoint);
        return;
    }

    if (!direction.IsValid())
    {
        Reset();
        return;
    }

    const GB_Vector2d unitDirection = direction.Normalized();
    if (!unitDirection.IsValid())
    {
        Reset();
        return;
    }

    Set(startPoint, startPoint + unitDirection * length);
}

GB_LineSegment::~GB_LineSegment()
{
}

const std::string& GB_LineSegment::GetClassType() const
{
    static const std::string classType = "GB_LineSegment";
    return classType;
}

uint64_t GB_LineSegment::GetClassTypeId() const
{
    static const uint64_t classTypeId = GB_GenerateClassTypeId(GetClassType());
    return classTypeId;
}

void GB_LineSegment::Reset()
{
    point1 = MakeNanPoint();
    point2 = MakeNanPoint();
}

void GB_LineSegment::Set(const GB_Point2d& point1, const GB_Point2d& point2)
{
    if (!point1.IsValid() || !point2.IsValid())
    {
        Reset();
        return;
    }

    this->point1 = point1;
    this->point2 = point2;
}

void GB_LineSegment::Set(double x1, double y1, double x2, double y2)
{
    if (!IsFinite4(x1, y1, x2, y2))
    {
        Reset();
        return;
    }

    point1.Set(x1, y1);
    point2.Set(x2, y2);
}

bool GB_LineSegment::IsValid() const
{
    return point1.IsValid() && point2.IsValid();
}

bool GB_LineSegment::IsDegenerate(double tolerance) const
{
    if (!IsValid())
    {
        return false;
    }

    const double length = Length();
    return !std::isnan(length) && length <= AbsTol(tolerance);
}

double GB_LineSegment::Length() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    return point1.DistanceTo(point2);
}

double GB_LineSegment::LengthSquared() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    return point1.DistanceToSquared(point2);
}

GB_Point2d GB_LineSegment::MidPoint() const
{
    if (!IsValid())
    {
        return MakeNanPoint();
    }

    return GB_Point2d::MidPoint(point1, point2);
}

GB_Vector2d GB_LineSegment::ToVector() const
{
    if (!IsValid())
    {
        return MakeNanVector();
    }

    return point2 - point1;
}

GB_Vector2d GB_LineSegment::UnitDirectionVector(double tolerance) const
{
    if (!IsValid())
    {
        return MakeNanVector();
    }

    const double absTolerance = AbsTol(tolerance);
    const double length = Length();
    if (std::isnan(length) || length <= absTolerance)
    {
        return MakeNanVector();
    }

    double unitX = 0.0;
    double unitY = 0.0;
    double ignoredLength = 0.0;
    if (!TryGetUnitDirectionAndLength(*this, unitX, unitY, ignoredLength))
    {
        return MakeNanVector();
    }

    return GB_Vector2d(unitX, unitY);
}

double GB_LineSegment::Angle() const
{
    if (!IsValid() || IsDegenerate())
    {
        return GB_QuietNan;
    }

    const GB_Vector2d unitDirection = UnitDirectionVector(0.0);
    return unitDirection.IsValid() ? unitDirection.Angle() : GB_QuietNan;
}

GB_Rectangle GB_LineSegment::BoundingRectangle() const
{
    if (!IsValid())
    {
        return GB_Rectangle::Invalid;
    }

    return GB_Rectangle(point1, point2);
}

bool GB_LineSegment::operator==(const GB_LineSegment& other) const
{
    return point1 == other.point1 && point2 == other.point2;
}

bool GB_LineSegment::operator!=(const GB_LineSegment& other) const
{
    return !(*this == other);
}

bool GB_LineSegment::IsNearEqual(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double absTolerance = AbsTol(tolerance);
    return point1.IsNearEqual(other.point1, absTolerance) && point2.IsNearEqual(other.point2, absTolerance);
}

bool GB_LineSegment::IsSameLineSegment(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double absTolerance = AbsTol(tolerance);
    return (point1.IsNearEqual(other.point1, absTolerance) && point2.IsNearEqual(other.point2, absTolerance))
        || (point1.IsNearEqual(other.point2, absTolerance) && point2.IsNearEqual(other.point1, absTolerance));
}

GB_Point2d GB_LineSegment::PointAt(double t) const
{
    if (!IsValid() || !std::isfinite(t))
    {
        return MakeNanPoint();
    }

    return GB_Point2d::Lerp(point1, point2, t);
}

double GB_LineSegment::ParameterAt(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid() || IsExactDegenerate(*this))
    {
        return GB_QuietNan;
    }

    double parameter = GB_QuietNan;
    if (TryGetParameterFast(*this, point, parameter))
    {
        return parameter;
    }

    HighPrecisionFloat highPrecisionParameter = 0;
    return TryGetParameterHighPrecision(*this, point, highPrecisionParameter) ? HighPrecisionToDoubleOrInfinity(highPrecisionParameter) : GB_QuietNan;
}

GB_Point2d GB_LineSegment::ProjectPointOnLine(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return MakeNanPoint();
    }

    if (IsExactDegenerate(*this))
    {
        return point1;
    }

    const double parameter = ParameterAt(point);
    if (std::isfinite(parameter))
    {
        const GB_Point2d projectedPoint = PointAt(parameter);
        if (projectedPoint.IsValid())
        {
            return projectedPoint;
        }
    }

    GB_Point2d projectedPoint;
    return TryProjectPointOnLineHighPrecision(*this, point, projectedPoint) ? projectedPoint : MakeNanPoint();
}

GB_Point2d GB_LineSegment::ClosestPointTo(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return MakeNanPoint();
    }

    if (IsExactDegenerate(*this))
    {
        return point1;
    }

    const double parameter = ParameterAt(point);
    if (std::isnan(parameter) || parameter <= 0.0)
    {
        return point1;
    }
    if (parameter >= 1.0)
    {
        return point2;
    }

    return PointAt(parameter);
}

double GB_LineSegment::DistanceTo(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return GB_QuietNan;
    }

    const GB_Point2d closestPoint = ClosestPointTo(point);
    return closestPoint.IsValid() ? closestPoint.DistanceTo(point) : GB_QuietNan;
}

double GB_LineSegment::DistanceToSquared(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return GB_QuietNan;
    }

    const GB_Point2d closestPoint = ClosestPointTo(point);
    return closestPoint.IsValid() ? closestPoint.DistanceToSquared(point) : GB_QuietNan;
}

double GB_LineSegment::DistanceToLine(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return GB_QuietNan;
    }

    if (IsExactDegenerate(*this))
    {
        return point1.DistanceTo(point);
    }

    double unitX = 0.0;
    double unitY = 0.0;
    double length = 0.0;
    if (!TryGetUnitDirectionAndLength(*this, unitX, unitY, length))
    {
        return GB_QuietNan;
    }

    const double pointDeltaX = point.x - point1.x;
    const double pointDeltaY = point.y - point1.y;
    if (std::isfinite(pointDeltaX) && std::isfinite(pointDeltaY))
    {
        const double pointScale = std::max(std::abs(pointDeltaX), std::abs(pointDeltaY));
        if (pointScale == 0.0)
        {
            return 0.0;
        }

        if (unitX == 0.0)
        {
            return std::abs(pointDeltaX);
        }
        if (unitY == 0.0)
        {
            return std::abs(pointDeltaY);
        }

        const double normalizedPointX = pointDeltaX / pointScale;
        const double normalizedPointY = pointDeltaY / pointScale;
        const double normalizedCross = unitX * normalizedPointY - unitY * normalizedPointX;
        const double absoluteNormalizedCross = std::abs(normalizedCross);
        if (std::isfinite(absoluteNormalizedCross) && absoluteNormalizedCross > std::numeric_limits<double>::epsilon() * 64.0)
        {
            if (absoluteNormalizedCross > std::numeric_limits<double>::max() / pointScale)
            {
                return std::numeric_limits<double>::infinity();
            }
            return pointScale * absoluteNormalizedCross;
        }
    }

    return DistanceToLineHighPrecision(*this, point);
}

double GB_LineSegment::DistanceTo(const GB_LineSegment& other) const
{
    if (!IsValid() || !other.IsValid())
    {
        return GB_QuietNan;
    }

    if (IsIntersects(other))
    {
        return 0.0;
    }

    const double distanceValues[4] =
    {
        DistanceTo(other.point1),
        DistanceTo(other.point2),
        other.DistanceTo(point1),
        other.DistanceTo(point2)
    };

    double minimumDistance = std::numeric_limits<double>::infinity();
    bool hasDistance = false;
    for (int valueIndex = 0; valueIndex < 4; valueIndex++)
    {
        if (std::isnan(distanceValues[valueIndex]))
        {
            continue;
        }

        minimumDistance = std::min(minimumDistance, distanceValues[valueIndex]);
        hasDistance = true;
    }

    return hasDistance ? minimumDistance : GB_QuietNan;
}

GB_LineSegment GB_LineSegment::Reversed() const
{
    if (!IsValid())
    {
        return GB_LineSegment::Invalid;
    }

    return GB_LineSegment(point2, point1);
}

void GB_LineSegment::Reverse()
{
    if (!IsValid())
    {
        Reset();
        return;
    }

    std::swap(point1, point2);
}

GB_LineSegment GB_LineSegment::NormalizedEndpointOrder() const
{
    if (!IsValid())
    {
        return GB_LineSegment::Invalid;
    }

    return ComparePointLexicographically(point2, point1) < 0 ? GB_LineSegment(point2, point1) : *this;
}

void GB_LineSegment::NormalizeEndpointOrder()
{
    if (!IsValid())
    {
        Reset();
        return;
    }

    if (ComparePointLexicographically(point2, point1) < 0)
    {
        std::swap(point1, point2);
    }
}

GB_LineSegment GB_LineSegment::Offsetted(double deltaX, double deltaY) const
{
    GB_LineSegment result = *this;
    result.Offset(deltaX, deltaY);
    return result;
}

GB_LineSegment GB_LineSegment::Offsetted(const GB_Vector2d& translation) const
{
    GB_LineSegment result = *this;
    result.Offset(translation);
    return result;
}

void GB_LineSegment::Offset(double deltaX, double deltaY)
{
    if (!IsValid() || !std::isfinite(deltaX) || !std::isfinite(deltaY))
    {
        Reset();
        return;
    }

    point1.Offset(deltaX, deltaY);
    point2.Offset(deltaX, deltaY);
    if (!point1.IsValid() || !point2.IsValid())
    {
        Reset();
    }
}

void GB_LineSegment::Offset(const GB_Vector2d& translation)
{
    if (!translation.IsValid())
    {
        Reset();
        return;
    }

    Offset(translation.x, translation.y);
}

GB_LineSegment GB_LineSegment::Rotated(double angle, const GB_Point2d& center) const
{
    GB_LineSegment result = *this;
    result.Rotate(angle, center);
    return result;
}

void GB_LineSegment::Rotate(double angle, const GB_Point2d& center)
{
    if (!IsValid() || !center.IsValid() || !std::isfinite(angle))
    {
        Reset();
        return;
    }

    const double cosAngle = std::cos(angle);
    const double sinAngle = std::sin(angle);
    if (!std::isfinite(cosAngle) || !std::isfinite(sinAngle))
    {
        Reset();
        return;
    }

    const double point1DeltaX = point1.x - center.x;
    const double point1DeltaY = point1.y - center.y;
    const double point2DeltaX = point2.x - center.x;
    const double point2DeltaY = point2.y - center.y;

    const GB_Point2d newPoint1(center.x + point1DeltaX * cosAngle - point1DeltaY * sinAngle, center.y + point1DeltaX * sinAngle + point1DeltaY * cosAngle);
    const GB_Point2d newPoint2(center.x + point2DeltaX * cosAngle - point2DeltaY * sinAngle, center.y + point2DeltaX * sinAngle + point2DeltaY * cosAngle);
    Set(newPoint1, newPoint2);
}

GB_LineSegment GB_LineSegment::Scaled(double scaleFactor, const GB_Point2d& center) const
{
    GB_LineSegment result = *this;
    result.Scale(scaleFactor, center);
    return result;
}

void GB_LineSegment::Scale(double scaleFactor, const GB_Point2d& center)
{
    if (!IsValid() || !center.IsValid() || !std::isfinite(scaleFactor))
    {
        Reset();
        return;
    }

    const GB_Point2d newPoint1 = center + (point1 - center) * scaleFactor;
    const GB_Point2d newPoint2 = center + (point2 - center) * scaleFactor;
    Set(newPoint1, newPoint2);
}

GB_LineSegment GB_LineSegment::Transformed(const GB_Matrix3x3& mat) const
{
    GB_LineSegment result = *this;
    result.Transform(mat);
    return result;
}

void GB_LineSegment::Transform(const GB_Matrix3x3& mat)
{
    if (!IsValid() || !mat.IsValid())
    {
        Reset();
        return;
    }

    Set(point1.Transformed(mat), point2.Transformed(mat));
}

GB_LineSegment GB_LineSegment::Extended(double delta) const
{
    return Extended(delta, delta);
}

GB_LineSegment GB_LineSegment::Extended(double deltaAtPoint1, double deltaAtPoint2) const
{
    GB_LineSegment result = *this;
    result.Extend(deltaAtPoint1, deltaAtPoint2);
    return result;
}

void GB_LineSegment::Extend(double delta)
{
    Extend(delta, delta);
}

void GB_LineSegment::Extend(double deltaAtPoint1, double deltaAtPoint2)
{
    if (!IsValid() || !std::isfinite(deltaAtPoint1) || !std::isfinite(deltaAtPoint2))
    {
        Reset();
        return;
    }

    if (IsExactDegenerate(*this))
    {
        if (std::abs(deltaAtPoint1) <= GB_Epsilon && std::abs(deltaAtPoint2) <= GB_Epsilon)
        {
            return;
        }

        Reset();
        return;
    }

    double unitX = 0.0;
    double unitY = 0.0;
    double ignoredLength = 0.0;
    if (!TryGetUnitDirectionAndLength(*this, unitX, unitY, ignoredLength))
    {
        Reset();
        return;
    }

    bool shouldCollapse = false;
    bool checkedLength = false;
    const double currentLength = Length();
    if (std::isfinite(currentLength))
    {
        const double totalDelta = deltaAtPoint1 + deltaAtPoint2;
        if (std::isfinite(totalDelta))
        {
            const double newLength = currentLength + totalDelta;
            if (std::isfinite(newLength))
            {
                const double lengthTolerance = GB_Epsilon * std::max(1.0, currentLength);
                if (newLength < -lengthTolerance)
                {
                    Reset();
                    return;
                }

                shouldCollapse = std::abs(newLength) <= lengthTolerance;
                checkedLength = true;
            }
        }
    }

    if (!checkedLength)
    {
        const HighPrecisionFloat highPrecisionLength = GetHighPrecisionSegmentLength(*this);
        const HighPrecisionFloat newLength = highPrecisionLength + HighPrecisionFloat(deltaAtPoint1) + HighPrecisionFloat(deltaAtPoint2);
        const HighPrecisionFloat lengthTolerance = HighPrecisionFloat(GB_Epsilon) * (highPrecisionLength > 1 ? highPrecisionLength : HighPrecisionFloat(1));
        if (newLength < -lengthTolerance)
        {
            Reset();
            return;
        }

        shouldCollapse = boost::multiprecision::abs(newLength) <= lengthTolerance;
    }

    const GB_Vector2d unitDirection(unitX, unitY);
    const GB_Point2d newPoint1 = point1 - unitDirection * deltaAtPoint1;
    const GB_Point2d newPoint2 = point2 + unitDirection * deltaAtPoint2;
    if (!newPoint1.IsValid() || !newPoint2.IsValid())
    {
        Reset();
        return;
    }

    if (shouldCollapse)
    {
        const GB_Point2d collapsePoint = GB_Point2d::MidPoint(newPoint1, newPoint2);
        Set(collapsePoint, collapsePoint);
        return;
    }

    Set(newPoint1, newPoint2);
}

bool GB_LineSegment::IsContains(const GB_Point2d& point, double tolerance) const
{
    if (!IsValid() || !point.IsValid())
    {
        return false;
    }

    const double distance = DistanceTo(point);
    return !std::isnan(distance) && distance <= AbsTol(tolerance);
}

int GB_LineSegment::SideOfPoint(const GB_Point2d& point, double tolerance) const
{
    if (!IsValid() || !point.IsValid() || IsDegenerate(AbsTol(tolerance)))
    {
        return 0;
    }

    const double absTolerance = AbsTol(tolerance);
    const double distance = DistanceToLine(point);
    if (std::isnan(distance) || distance <= absTolerance)
    {
        return 0;
    }

    double unitX = 0.0;
    double unitY = 0.0;
    double ignoredLength = 0.0;
    if (!TryGetUnitDirectionAndLength(*this, unitX, unitY, ignoredLength))
    {
        return 0;
    }

    const double pointDeltaX = point.x - point1.x;
    const double pointDeltaY = point.y - point1.y;
    if (std::isfinite(pointDeltaX) && std::isfinite(pointDeltaY))
    {
        const double pointScale = std::max(std::abs(pointDeltaX), std::abs(pointDeltaY));
        if (pointScale > 0.0)
        {
            const double normalizedCross = unitX * (pointDeltaY / pointScale) - unitY * (pointDeltaX / pointScale);
            if (std::isfinite(normalizedCross) && std::abs(normalizedCross) > std::numeric_limits<double>::epsilon() * 64.0)
            {
                return normalizedCross > 0.0 ? 1 : -1;
            }
        }
    }

    return SideOfPointHighPrecision(*this, point);
}

bool GB_LineSegment::IsParallelTo(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double absTolerance = AbsTol(tolerance);
    if (IsDegenerate(absTolerance) || other.IsDegenerate(absTolerance))
    {
        return false;
    }

    const GB_Vector2d firstUnit = UnitDirectionVector(0.0);
    const GB_Vector2d secondUnit = other.UnitDirectionVector(0.0);
    if (!firstUnit.IsValid() || !secondUnit.IsValid())
    {
        return false;
    }

    const double cross = firstUnit.x * secondUnit.y - firstUnit.y * secondUnit.x;
    return std::isfinite(cross) && std::abs(cross) <= absTolerance;
}

bool GB_LineSegment::IsPerpendicularTo(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double absTolerance = AbsTol(tolerance);
    if (IsDegenerate(absTolerance) || other.IsDegenerate(absTolerance))
    {
        return false;
    }

    const GB_Vector2d firstUnit = UnitDirectionVector(0.0);
    const GB_Vector2d secondUnit = other.UnitDirectionVector(0.0);
    if (!firstUnit.IsValid() || !secondUnit.IsValid())
    {
        return false;
    }

    const double dot = firstUnit.x * secondUnit.x + firstUnit.y * secondUnit.y;
    return std::isfinite(dot) && std::abs(dot) <= absTolerance;
}

bool GB_LineSegment::IsCollinearWith(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double absTolerance = AbsTol(tolerance);
    const bool thisDegenerate = IsDegenerate(absTolerance);
    const bool otherDegenerate = other.IsDegenerate(absTolerance);

    if (thisDegenerate && otherDegenerate)
    {
        return point1.IsNearEqual(other.point1, absTolerance);
    }
    if (thisDegenerate)
    {
        const double distance = other.DistanceToLine(point1);
        return !std::isnan(distance) && distance <= absTolerance;
    }
    if (otherDegenerate)
    {
        const double distance = DistanceToLine(other.point1);
        return !std::isnan(distance) && distance <= absTolerance;
    }

    if (!IsParallelTo(other, absTolerance))
    {
        return false;
    }

    const double firstDistance = DistanceToLine(other.point1);
    const double secondDistance = DistanceToLine(other.point2);
    return !std::isnan(firstDistance) && !std::isnan(secondDistance) && firstDistance <= absTolerance && secondDistance <= absTolerance;
}

double GB_LineSegment::AngleBetween(const GB_LineSegment& other) const
{
    if (!IsValid() || !other.IsValid() || IsDegenerate() || other.IsDegenerate())
    {
        return GB_QuietNan;
    }

    const GB_Vector2d firstUnit = UnitDirectionVector(0.0);
    const GB_Vector2d secondUnit = other.UnitDirectionVector(0.0);
    if (!firstUnit.IsValid() || !secondUnit.IsValid())
    {
        return GB_QuietNan;
    }

    double cosineValue = std::abs(firstUnit.x * secondUnit.x + firstUnit.y * secondUnit.y);
    if (!std::isfinite(cosineValue))
    {
        return GB_QuietNan;
    }
    cosineValue = std::min(1.0, cosineValue);
    return std::acos(cosineValue);
}

bool GB_LineSegment::IsIntersects(const GB_LineSegment& other, double tolerance) const
{
    GB_Point2d intersection;
    GB_LineSegment overlap;
    return Intersect(other, intersection, overlap, tolerance) != 0;
}

int GB_LineSegment::Intersect(const GB_LineSegment& other, GB_Point2d& outIntersection, GB_LineSegment& outOverlap, double tolerance) const
{
    outIntersection = MakeNanPoint();
    outOverlap.Reset();

    if (!IsValid() || !other.IsValid())
    {
        return 0;
    }

    const double absTolerance = AbsTol(tolerance);
    if (!DoBoundingRectanglesOverlap(*this, other, absTolerance))
    {
        return 0;
    }

    const bool thisDegenerate = IsDegenerate(absTolerance);
    const bool otherDegenerate = other.IsDegenerate(absTolerance);
    if (thisDegenerate && otherDegenerate)
    {
        if (point1.IsNearEqual(other.point1, absTolerance))
        {
            outIntersection = point1;
            return 1;
        }
        return 0;
    }
    if (thisDegenerate)
    {
        if (other.IsContains(point1, absTolerance))
        {
            outIntersection = point1;
            return 1;
        }
        return 0;
    }
    if (otherDegenerate)
    {
        if (IsContains(other.point1, absTolerance))
        {
            outIntersection = other.point1;
            return 1;
        }
        return 0;
    }

    const GB_Vector2d thisUnit = UnitDirectionVector(0.0);
    const GB_Vector2d otherUnit = other.UnitDirectionVector(0.0);
    if (!thisUnit.IsValid() || !otherUnit.IsValid())
    {
        return 0;
    }

    const double relativeCross = thisUnit.x * otherUnit.y - thisUnit.y * otherUnit.x;
    if (!std::isfinite(relativeCross))
    {
        return 0;
    }

    if (std::abs(relativeCross) <= absTolerance)
    {
        const double firstDistance = DistanceToLine(other.point1);
        const double secondDistance = DistanceToLine(other.point2);
        const bool isCollinear = !std::isnan(firstDistance) && !std::isnan(secondDistance) && firstDistance <= absTolerance && secondDistance <= absTolerance;
        if (!isCollinear)
        {
            return 0;
        }

        double otherPoint1Parameter = ParameterAt(other.point1);
        double otherPoint2Parameter = ParameterAt(other.point2);
        if (std::isnan(otherPoint1Parameter) || std::isnan(otherPoint2Parameter))
        {
            return 0;
        }
        if (otherPoint1Parameter > otherPoint2Parameter)
        {
            std::swap(otherPoint1Parameter, otherPoint2Parameter);
        }

        const double thisLength = Length();
        const double parameterTolerance = std::isfinite(thisLength) && thisLength > 0.0 ? absTolerance / thisLength : 0.0;
        const double overlapStart = std::max(0.0, otherPoint1Parameter);
        const double overlapEnd = std::min(1.0, otherPoint2Parameter);
        if (overlapStart > overlapEnd + parameterTolerance)
        {
            return 0;
        }

        if (std::abs(overlapEnd - overlapStart) <= parameterTolerance)
        {
            const double intersectionParameter = ClampUnitParameter((overlapStart + overlapEnd) * 0.5);
            outIntersection = PointAt(intersectionParameter);
            return outIntersection.IsValid() ? 1 : 0;
        }

        outOverlap.Set(PointAt(ClampUnitParameter(overlapStart)), PointAt(ClampUnitParameter(overlapEnd)));
        return outOverlap.IsValid() ? 2 : 0;
    }

    double thisParameter = GB_QuietNan;
    double otherParameter = GB_QuietNan;
    const bool preferHighPrecision = std::abs(relativeCross) <= std::numeric_limits<double>::epsilon() * 64.0;
    const bool parameterSucceeded = preferHighPrecision
        ? TryGetIntersectionParametersHighPrecision(*this, other, thisParameter, otherParameter)
        : (TryGetIntersectionParametersFast(*this, other, thisParameter, otherParameter) || TryGetIntersectionParametersHighPrecision(*this, other, thisParameter, otherParameter));
    if (!parameterSucceeded)
    {
        return 0;
    }

    const double thisLength = Length();
    const double otherLength = other.Length();
    const double thisParameterTolerance = std::isfinite(thisLength) && thisLength > 0.0 ? absTolerance / thisLength : 0.0;
    const double otherParameterTolerance = std::isfinite(otherLength) && otherLength > 0.0 ? absTolerance / otherLength : 0.0;
    if (!IsParameterInRange(thisParameter, thisParameterTolerance) || !IsParameterInRange(otherParameter, otherParameterTolerance))
    {
        return 0;
    }

    outIntersection = PointAt(ClampUnitParameter(thisParameter));
    return outIntersection.IsValid() ? 1 : 0;
}

std::string GB_LineSegment::SerializeToString() const
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << "(" << GetClassType() << " " << std::setprecision(17) << point1.x << "," << point1.y << "," << point2.x << "," << point2.y << ")";
    return oss.str();
}

GB_ByteBuffer GB_LineSegment::SerializeToBinary() const
{
    constexpr static uint16_t payloadVersion = 1;

    GB_ByteBuffer buffer;
    buffer.reserve(48);

    GB_ByteBufferIO::AppendUInt32LE(buffer, GB_ClassMagicNumber);
    GB_ByteBufferIO::AppendUInt64LE(buffer, GetClassTypeId());
    GB_ByteBufferIO::AppendUInt16LE(buffer, payloadVersion);
    GB_ByteBufferIO::AppendUInt16LE(buffer, 0);

    GB_ByteBufferIO::AppendDoubleLE(buffer, point1.x);
    GB_ByteBufferIO::AppendDoubleLE(buffer, point1.y);
    GB_ByteBufferIO::AppendDoubleLE(buffer, point2.x);
    GB_ByteBufferIO::AppendDoubleLE(buffer, point2.y);

    return buffer;
}

bool GB_LineSegment::Deserialize(const std::string& data)
{
    std::istringstream iss(data);
    iss.imbue(std::locale::classic());

    char leftParen = 0;
    std::string type;
    char comma1 = 0;
    char comma2 = 0;
    char comma3 = 0;
    char rightParen = 0;

    double parsedX1 = GB_QuietNan;
    double parsedY1 = GB_QuietNan;
    double parsedX2 = GB_QuietNan;
    double parsedY2 = GB_QuietNan;

    if (!(iss >> leftParen >> type >> parsedX1 >> comma1 >> parsedY1 >> comma2 >> parsedX2 >> comma3 >> parsedY2 >> rightParen))
    {
        return false;
    }

    iss >> std::ws;
    if (!iss.eof() || leftParen != '(' || rightParen != ')' || comma1 != ',' || comma2 != ',' || comma3 != ',' || type != GetClassType())
    {
        return false;
    }

    GB_LineSegment parsedSegment;
    parsedSegment.Set(parsedX1, parsedY1, parsedX2, parsedY2);
    if (!parsedSegment.IsValid())
    {
        return false;
    }

    *this = parsedSegment;
    return true;
}

bool GB_LineSegment::Deserialize(const GB_ByteBuffer& data)
{
    constexpr static uint16_t expectedPayloadVersion = 1;
    constexpr static size_t expectedSize = 48;

    if (data.size() != expectedSize)
    {
        return false;
    }

    size_t offset = 0;
    uint32_t magic = 0;
    uint64_t typeId = 0;
    uint16_t payloadVersion = 0;
    uint16_t reserved = 0;

    double parsedX1 = GB_QuietNan;
    double parsedY1 = GB_QuietNan;
    double parsedX2 = GB_QuietNan;
    double parsedY2 = GB_QuietNan;

    if (!GB_ByteBufferIO::ReadUInt32LE(data, offset, magic)
        || !GB_ByteBufferIO::ReadUInt64LE(data, offset, typeId)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, payloadVersion)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, reserved)
        || !GB_ByteBufferIO::ReadDoubleLE(data, offset, parsedX1)
        || !GB_ByteBufferIO::ReadDoubleLE(data, offset, parsedY1)
        || !GB_ByteBufferIO::ReadDoubleLE(data, offset, parsedX2)
        || !GB_ByteBufferIO::ReadDoubleLE(data, offset, parsedY2))
    {
        return false;
    }

    if (magic != GB_ClassMagicNumber || typeId != GetClassTypeId() || payloadVersion != expectedPayloadVersion || reserved != 0 || offset != data.size())
    {
        return false;
    }

    GB_LineSegment parsedSegment;
    parsedSegment.Set(parsedX1, parsedY1, parsedX2, parsedY2);
    if (!parsedSegment.IsValid())
    {
        return false;
    }

    *this = parsedSegment;
    return true;
}
