#include "GB_LineSegment.h"
#include "GB_Vector2d.h"
#include "GB_Rectangle.h"
#include "GB_Matrix3x3.h"
#include "../GB_IO.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace
{
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

    static inline int ComparePointLexicographically(const GB_Point2d& a, const GB_Point2d& b)
    {
        if (a.x < b.x)
        {
            return -1;
        }
        if (a.x > b.x)
        {
            return 1;
        }
        if (a.y < b.y)
        {
            return -1;
        }
        if (a.y > b.y)
        {
            return 1;
        }
        return 0;
    }

    static inline long double CrossProductLongDouble(double firstVectorX, double firstVectorY, double secondVectorX, double secondVectorY)
    {
        return static_cast<long double>(firstVectorX) * static_cast<long double>(secondVectorY) - static_cast<long double>(firstVectorY) * static_cast<long double>(secondVectorX);
    }

    static inline long double DotProductLongDouble(double firstVectorX, double firstVectorY, double secondVectorX, double secondVectorY)
    {
        return static_cast<long double>(firstVectorX) * static_cast<long double>(secondVectorX) + static_cast<long double>(firstVectorY) * static_cast<long double>(secondVectorY);
    }

    static inline double VectorLength(double vectorX, double vectorY)
    {
        return std::hypot(vectorX, vectorY);
    }

    static inline bool TryGetSegmentVectorAndLengthSquared(const GB_LineSegment& segment, double& vectorX, double& vectorY, double& lengthSquared)
    {
        if (!segment.IsValid())
        {
            return false;
        }

        vectorX = segment.point2.x - segment.point1.x;
        vectorY = segment.point2.y - segment.point1.y;
        lengthSquared = vectorX * vectorX + vectorY * vectorY;
        return std::isfinite(lengthSquared);
    }

    static inline bool IsLengthSquaredDegenerate(double lengthSquared, double tolerance)
    {
        if (!std::isfinite(lengthSquared))
        {
            return false;
        }

        const double toleranceSquared = tolerance * tolerance;
        return std::isfinite(toleranceSquared) && lengthSquared <= toleranceSquared;
    }

    static inline bool IsCrossWithinDistanceTolerance(long double cross, double length, double tolerance)
    {
        if (!std::isfinite(cross) || !std::isfinite(length) || length < 0.0)
        {
            return false;
        }

        const long double leftValue = std::abs(cross);
        const long double rightValue = static_cast<long double>(tolerance) * static_cast<long double>(length);
        return leftValue <= rightValue;
    }

    static inline bool IsRelativeCrossWithinTolerance(long double cross, double firstLengthSquared, double secondLengthSquared, double tolerance)
    {
        if (!std::isfinite(cross) || !std::isfinite(firstLengthSquared) || !std::isfinite(secondLengthSquared) || firstLengthSquared <= 0.0 || secondLengthSquared <= 0.0)
        {
            return false;
        }

        const long double firstLength = std::sqrt(static_cast<long double>(firstLengthSquared));
        const long double secondLength = std::sqrt(static_cast<long double>(secondLengthSquared));
        const long double leftValue = std::abs(cross);
        const long double rightValue = static_cast<long double>(tolerance) * firstLength * secondLength;
        return leftValue <= rightValue;
    }

    static inline bool IsRelativeDotWithinTolerance(long double dot, double firstLengthSquared, double secondLengthSquared, double tolerance)
    {
        if (!std::isfinite(dot) || !std::isfinite(firstLengthSquared) || !std::isfinite(secondLengthSquared) || firstLengthSquared <= 0.0 || secondLengthSquared <= 0.0)
        {
            return false;
        }

        const long double firstLength = std::sqrt(static_cast<long double>(firstLengthSquared));
        const long double secondLength = std::sqrt(static_cast<long double>(secondLengthSquared));
        const long double leftValue = std::abs(dot);
        const long double rightValue = static_cast<long double>(tolerance) * firstLength * secondLength;
        return leftValue <= rightValue;
    }

    static inline double ToFiniteDoubleOrNan(long double value)
    {
        const long double maxValue = static_cast<long double>(std::numeric_limits<double>::max());
        if (!std::isfinite(value) || value > maxValue || value < -maxValue)
        {
            return GB_QuietNan;
        }

        return static_cast<double>(value);
    }

    static inline bool IsFinitePoint(double x, double y)
    {
        return std::isfinite(x) && std::isfinite(y);
    }

    static inline long double ClampUnitParameterLongDouble(long double parameter)
    {
        if (parameter < 0.0L)
        {
            return 0.0L;
        }
        if (parameter > 1.0L)
        {
            return 1.0L;
        }
        return parameter;
    }

    static inline bool IsParameterInRangeLongDouble(long double parameter, double tolerance)
    {
        const long double absTolerance = static_cast<long double>(tolerance);
        return parameter >= -absTolerance && parameter <= 1.0L + absTolerance;
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

    const double directionLengthSquared = direction.LengthSquared();
    if (!std::isfinite(directionLengthSquared) || directionLengthSquared <= 0.0)
    {
        Reset();
        return;
    }

    const double directionLength = std::sqrt(directionLengthSquared);
    const double scale = length / directionLength;
    Set(startPoint, startPoint + direction * scale);
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

    const double absTol = AbsTol(tolerance);
    return LengthSquared() <= absTol * absTol;
}

double GB_LineSegment::Length() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    return VectorLength(point2.x - point1.x, point2.y - point1.y);
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

    const double absTol = AbsTol(tolerance);
    double segmentVectorX = 0.0;
    double segmentVectorY = 0.0;
    double lengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, segmentVectorX, segmentVectorY, lengthSquared) || IsLengthSquaredDegenerate(lengthSquared, absTol))
    {
        return MakeNanVector();
    }

    const double length = VectorLength(segmentVectorX, segmentVectorY);
    if (!std::isfinite(length) || length <= absTol)
    {
        return MakeNanVector();
    }

    const double inverseLength = 1.0 / length;
    return GB_Vector2d(segmentVectorX * inverseLength, segmentVectorY * inverseLength);
}

double GB_LineSegment::Angle() const
{
    if (!IsValid() || IsDegenerate())
    {
        return GB_QuietNan;
    }

    return ToVector().Angle();
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

    const double absTol = AbsTol(tolerance);
    return point1.IsNearEqual(other.point1, absTol) && point2.IsNearEqual(other.point2, absTol);
}

bool GB_LineSegment::IsSameLineSegment(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double absTol = AbsTol(tolerance);
    return (point1.IsNearEqual(other.point1, absTol) && point2.IsNearEqual(other.point2, absTol))
        || (point1.IsNearEqual(other.point2, absTol) && point2.IsNearEqual(other.point1, absTol));
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
    if (!IsValid() || !point.IsValid())
    {
        return GB_QuietNan;
    }

    double segmentVectorX = 0.0;
    double segmentVectorY = 0.0;
    double lengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, segmentVectorX, segmentVectorY, lengthSquared) || lengthSquared <= 0.0)
    {
        return GB_QuietNan;
    }

    const double pointVectorX = point.x - point1.x;
    const double pointVectorY = point.y - point1.y;
    const long double parameter = DotProductLongDouble(pointVectorX, pointVectorY, segmentVectorX, segmentVectorY) / static_cast<long double>(lengthSquared);
    return ToFiniteDoubleOrNan(parameter);
}

GB_Point2d GB_LineSegment::ProjectPointOnLine(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return MakeNanPoint();
    }

    double segmentVectorX = 0.0;
    double segmentVectorY = 0.0;
    double lengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, segmentVectorX, segmentVectorY, lengthSquared) || lengthSquared <= 0.0)
    {
        return point1;
    }

    const double pointVectorX = point.x - point1.x;
    const double pointVectorY = point.y - point1.y;
    const long double parameter = DotProductLongDouble(pointVectorX, pointVectorY, segmentVectorX, segmentVectorY) / static_cast<long double>(lengthSquared);
    if (!std::isfinite(parameter))
    {
        return point1;
    }

    const double projectedX = ToFiniteDoubleOrNan(static_cast<long double>(point1.x) + static_cast<long double>(segmentVectorX) * parameter);
    const double projectedY = ToFiniteDoubleOrNan(static_cast<long double>(point1.y) + static_cast<long double>(segmentVectorY) * parameter);
    return IsFinitePoint(projectedX, projectedY) ? GB_Point2d(projectedX, projectedY) : MakeNanPoint();
}

GB_Point2d GB_LineSegment::ClosestPointTo(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return MakeNanPoint();
    }

    double segmentVectorX = 0.0;
    double segmentVectorY = 0.0;
    double lengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, segmentVectorX, segmentVectorY, lengthSquared) || lengthSquared <= 0.0)
    {
        return point1;
    }

    const double pointVectorX = point.x - point1.x;
    const double pointVectorY = point.y - point1.y;
    const long double parameter = DotProductLongDouble(pointVectorX, pointVectorY, segmentVectorX, segmentVectorY) / static_cast<long double>(lengthSquared);
    if (!std::isfinite(parameter))
    {
        return point1;
    }

    const long double clampedParameter = ClampUnitParameterLongDouble(parameter);
    const double closestX = ToFiniteDoubleOrNan(static_cast<long double>(point1.x) + static_cast<long double>(segmentVectorX) * clampedParameter);
    const double closestY = ToFiniteDoubleOrNan(static_cast<long double>(point1.y) + static_cast<long double>(segmentVectorY) * clampedParameter);
    return IsFinitePoint(closestX, closestY) ? GB_Point2d(closestX, closestY) : MakeNanPoint();
}

double GB_LineSegment::DistanceTo(const GB_Point2d& point) const
{
    return std::sqrt(DistanceToSquared(point));
}

double GB_LineSegment::DistanceToSquared(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return GB_QuietNan;
    }

    double segmentVectorX = 0.0;
    double segmentVectorY = 0.0;
    double lengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, segmentVectorX, segmentVectorY, lengthSquared) || lengthSquared <= 0.0)
    {
        return point1.DistanceToSquared(point);
    }

    const double pointVectorX = point.x - point1.x;
    const double pointVectorY = point.y - point1.y;
    const long double dot = DotProductLongDouble(pointVectorX, pointVectorY, segmentVectorX, segmentVectorY);
    if (!std::isfinite(dot))
    {
        return GB_QuietNan;
    }

    if (dot <= 0.0L)
    {
        return point1.DistanceToSquared(point);
    }

    if (dot >= static_cast<long double>(lengthSquared))
    {
        return point2.DistanceToSquared(point);
    }

    const long double cross = CrossProductLongDouble(segmentVectorX, segmentVectorY, pointVectorX, pointVectorY);
    if (!std::isfinite(cross))
    {
        return GB_QuietNan;
    }

    const long double distanceSquared = cross * cross / static_cast<long double>(lengthSquared);
    return ToFiniteDoubleOrNan(distanceSquared);
}

double GB_LineSegment::DistanceToLine(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return GB_QuietNan;
    }

    double segmentVectorX = 0.0;
    double segmentVectorY = 0.0;
    double lengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, segmentVectorX, segmentVectorY, lengthSquared) || lengthSquared <= 0.0)
    {
        return point1.DistanceTo(point);
    }

    const double pointVectorX = point.x - point1.x;
    const double pointVectorY = point.y - point1.y;
    const long double cross = CrossProductLongDouble(segmentVectorX, segmentVectorY, pointVectorX, pointVectorY);
    const double length = VectorLength(segmentVectorX, segmentVectorY);
    if (!std::isfinite(cross) || !std::isfinite(length) || length <= 0.0)
    {
        return GB_QuietNan;
    }

    return ToFiniteDoubleOrNan(std::abs(cross) / static_cast<long double>(length));
}

double GB_LineSegment::DistanceTo(const GB_LineSegment& other) const
{
    if (!IsValid() || !other.IsValid())
    {
        return GB_QuietNan;
    }

    if (IsIntersects(other))
    {
        return 0;
    }

    double minDistanceSquared = std::numeric_limits<double>::infinity();
    bool hasValidDistanceSquared = false;
    const double distanceSquaredValues[4] =
    {
        DistanceToSquared(other.point1),
        DistanceToSquared(other.point2),
        other.DistanceToSquared(point1),
        other.DistanceToSquared(point2)
    };

    for (int i = 0; i < 4; i++)
    {
        if (!std::isnan(distanceSquaredValues[i]))
        {
            if (!hasValidDistanceSquared || distanceSquaredValues[i] < minDistanceSquared)
            {
                minDistanceSquared = distanceSquaredValues[i];
            }
            hasValidDistanceSquared = true;
        }
    }

    return hasValidDistanceSquared ? std::sqrt(minDistanceSquared) : GB_QuietNan;
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

    if (ComparePointLexicographically(point2, point1) < 0)
    {
        return GB_LineSegment(point2, point1);
    }

    return *this;
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

    double segmentVectorX = 0.0;
    double segmentVectorY = 0.0;
    double lengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, segmentVectorX, segmentVectorY, lengthSquared) || lengthSquared <= 0.0)
    {
        if (std::abs(deltaAtPoint1) <= GB_Epsilon && std::abs(deltaAtPoint2) <= GB_Epsilon)
        {
            return;
        }

        Reset();
        return;
    }

    const double length = VectorLength(segmentVectorX, segmentVectorY);
    if (!std::isfinite(length) || length <= 0.0)
    {
        Reset();
        return;
    }

    const double newLength = length + deltaAtPoint1 + deltaAtPoint2;
    const double lengthTolerance = GB_Epsilon * std::max(1.0, length);
    if (newLength < -lengthTolerance)
    {
        Reset();
        return;
    }

    const double inverseLength = 1.0 / length;
    const GB_Vector2d unit(segmentVectorX * inverseLength, segmentVectorY * inverseLength);
    const GB_Point2d newPoint1 = point1 - unit * deltaAtPoint1;
    const GB_Point2d newPoint2 = point2 + unit * deltaAtPoint2;

    if (std::abs(newLength) <= lengthTolerance)
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

    const double absTol = AbsTol(tolerance);
    double segmentVectorX = 0.0;
    double segmentVectorY = 0.0;
    double lengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, segmentVectorX, segmentVectorY, lengthSquared))
    {
        return false;
    }

    const long double toleranceSquared = static_cast<long double>(absTol) * static_cast<long double>(absTol);
    if (IsLengthSquaredDegenerate(lengthSquared, absTol))
    {
        const double distanceSquared = point1.DistanceToSquared(point);
        return std::isfinite(distanceSquared) && static_cast<long double>(distanceSquared) <= toleranceSquared;
    }

    const double pointVectorX = point.x - point1.x;
    const double pointVectorY = point.y - point1.y;
    const long double dot = DotProductLongDouble(pointVectorX, pointVectorY, segmentVectorX, segmentVectorY);
    if (!std::isfinite(dot))
    {
        return false;
    }

    if (dot <= 0.0L)
    {
        const double distanceSquared = point1.DistanceToSquared(point);
        return std::isfinite(distanceSquared) && static_cast<long double>(distanceSquared) <= toleranceSquared;
    }

    if (dot >= static_cast<long double>(lengthSquared))
    {
        const double distanceSquared = point2.DistanceToSquared(point);
        return std::isfinite(distanceSquared) && static_cast<long double>(distanceSquared) <= toleranceSquared;
    }

    const long double cross = CrossProductLongDouble(segmentVectorX, segmentVectorY, pointVectorX, pointVectorY);
    if (!std::isfinite(cross))
    {
        return false;
    }

    const long double distanceSquared = cross * cross / static_cast<long double>(lengthSquared);
    return distanceSquared <= toleranceSquared;
}

int GB_LineSegment::SideOfPoint(const GB_Point2d& point, double tolerance) const
{
    if (!IsValid() || !point.IsValid())
    {
        return 0;
    }

    const double absTol = AbsTol(tolerance);
    double segmentVectorX = 0.0;
    double segmentVectorY = 0.0;
    double lengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, segmentVectorX, segmentVectorY, lengthSquared) || IsLengthSquaredDegenerate(lengthSquared, absTol))
    {
        return 0;
    }

    const double length = VectorLength(segmentVectorX, segmentVectorY);
    const long double cross = CrossProductLongDouble(segmentVectorX, segmentVectorY, point.x - point1.x, point.y - point1.y);
    if (!std::isfinite(cross) || !std::isfinite(length) || IsCrossWithinDistanceTolerance(cross, length, absTol))
    {
        return 0;
    }

    return cross > 0.0L ? 1 : -1;
}

bool GB_LineSegment::IsParallelTo(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double absTol = AbsTol(tolerance);
    double thisVectorX = 0.0;
    double thisVectorY = 0.0;
    double thisLengthSquared = 0.0;
    double otherVectorX = 0.0;
    double otherVectorY = 0.0;
    double otherLengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, thisVectorX, thisVectorY, thisLengthSquared)
        || !TryGetSegmentVectorAndLengthSquared(other, otherVectorX, otherVectorY, otherLengthSquared)
        || IsLengthSquaredDegenerate(thisLengthSquared, absTol)
        || IsLengthSquaredDegenerate(otherLengthSquared, absTol))
    {
        return false;
    }

    const long double cross = CrossProductLongDouble(thisVectorX, thisVectorY, otherVectorX, otherVectorY);
    return IsRelativeCrossWithinTolerance(cross, thisLengthSquared, otherLengthSquared, absTol);
}

bool GB_LineSegment::IsPerpendicularTo(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double absTol = AbsTol(tolerance);
    double thisVectorX = 0.0;
    double thisVectorY = 0.0;
    double thisLengthSquared = 0.0;
    double otherVectorX = 0.0;
    double otherVectorY = 0.0;
    double otherLengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, thisVectorX, thisVectorY, thisLengthSquared)
        || !TryGetSegmentVectorAndLengthSquared(other, otherVectorX, otherVectorY, otherLengthSquared)
        || IsLengthSquaredDegenerate(thisLengthSquared, absTol)
        || IsLengthSquaredDegenerate(otherLengthSquared, absTol))
    {
        return false;
    }

    const long double dot = DotProductLongDouble(thisVectorX, thisVectorY, otherVectorX, otherVectorY);
    return IsRelativeDotWithinTolerance(dot, thisLengthSquared, otherLengthSquared, absTol);
}

bool GB_LineSegment::IsCollinearWith(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double absTol = AbsTol(tolerance);
    const bool thisDegenerate = IsDegenerate(absTol);
    const bool otherDegenerate = other.IsDegenerate(absTol);

    if (thisDegenerate && otherDegenerate)
    {
        return point1.IsNearEqual(other.point1, absTol);
    }

    if (thisDegenerate)
    {
        return other.DistanceToLine(point1) <= absTol;
    }

    if (otherDegenerate)
    {
        return DistanceToLine(other.point1) <= absTol;
    }

    return IsParallelTo(other, absTol) && DistanceToLine(other.point1) <= absTol && DistanceToLine(other.point2) <= absTol;
}

double GB_LineSegment::AngleBetween(const GB_LineSegment& other) const
{
    if (!IsValid() || !other.IsValid())
    {
        return GB_QuietNan;
    }

    double thisVectorX = 0.0;
    double thisVectorY = 0.0;
    double thisLengthSquared = 0.0;
    double otherVectorX = 0.0;
    double otherVectorY = 0.0;
    double otherLengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, thisVectorX, thisVectorY, thisLengthSquared)
        || !TryGetSegmentVectorAndLengthSquared(other, otherVectorX, otherVectorY, otherLengthSquared)
        || IsLengthSquaredDegenerate(thisLengthSquared, GB_Epsilon)
        || IsLengthSquaredDegenerate(otherLengthSquared, GB_Epsilon))
    {
        return GB_QuietNan;
    }

    const long double denominator = std::sqrt(static_cast<long double>(thisLengthSquared)) * std::sqrt(static_cast<long double>(otherLengthSquared));
    if (!std::isfinite(denominator) || denominator <= 0.0L)
    {
        return GB_QuietNan;
    }

    const long double dot = DotProductLongDouble(thisVectorX, thisVectorY, otherVectorX, otherVectorY);
    if (!std::isfinite(dot))
    {
        return GB_QuietNan;
    }

    long double cosineValue = std::abs(dot / denominator);
    if (cosineValue > 1.0L)
    {
        cosineValue = 1.0L;
    }

    const long double angle = std::acos(cosineValue);
    return ToFiniteDoubleOrNan(angle);
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

    const double absTol = AbsTol(tolerance);
    if (!DoBoundingRectanglesOverlap(*this, other, absTol))
    {
        return 0;
    }

    const bool thisDegenerate = IsDegenerate(absTol);
    const bool otherDegenerate = other.IsDegenerate(absTol);

    if (thisDegenerate && otherDegenerate)
    {
        if (point1.IsNearEqual(other.point1, absTol))
        {
            outIntersection = point1;
            return 1;
        }
        return 0;
    }

    if (thisDegenerate)
    {
        if (other.IsContains(point1, absTol))
        {
            outIntersection = point1;
            return 1;
        }
        return 0;
    }

    if (otherDegenerate)
    {
        if (IsContains(other.point1, absTol))
        {
            outIntersection = other.point1;
            return 1;
        }
        return 0;
    }

    double thisVectorX = 0.0;
    double thisVectorY = 0.0;
    double thisLengthSquared = 0.0;
    double otherVectorX = 0.0;
    double otherVectorY = 0.0;
    double otherLengthSquared = 0.0;
    if (!TryGetSegmentVectorAndLengthSquared(*this, thisVectorX, thisVectorY, thisLengthSquared)
        || !TryGetSegmentVectorAndLengthSquared(other, otherVectorX, otherVectorY, otherLengthSquared)
        || thisLengthSquared <= 0.0
        || otherLengthSquared <= 0.0)
    {
        return 0;
    }

    const double thisLength = VectorLength(thisVectorX, thisVectorY);
    const double otherLength = VectorLength(otherVectorX, otherVectorY);
    if (!std::isfinite(thisLength) || !std::isfinite(otherLength) || thisLength <= 0.0 || otherLength <= 0.0)
    {
        return 0;
    }

    const double pointDeltaX = other.point1.x - point1.x;
    const double pointDeltaY = other.point1.y - point1.y;
    const long double denominator = CrossProductLongDouble(thisVectorX, thisVectorY, otherVectorX, otherVectorY);
    const long double denominatorAbs = std::abs(denominator);
    const long double relativeScale = static_cast<long double>(thisLength) * static_cast<long double>(otherLength);
    const long double parallelThreshold = static_cast<long double>(absTol) * relativeScale;
    const long double numericThreshold = static_cast<long double>(std::numeric_limits<double>::epsilon()) * relativeScale * 64.0L;

    if (denominatorAbs <= parallelThreshold)
    {
        const long double otherPoint1LineCross = CrossProductLongDouble(thisVectorX, thisVectorY, other.point1.x - point1.x, other.point1.y - point1.y);
        const long double otherPoint2LineCross = CrossProductLongDouble(thisVectorX, thisVectorY, other.point2.x - point1.x, other.point2.y - point1.y);
        const bool isCollinear = IsCrossWithinDistanceTolerance(otherPoint1LineCross, thisLength, absTol)
            && IsCrossWithinDistanceTolerance(otherPoint2LineCross, thisLength, absTol);

        if (isCollinear)
        {
            const double parameterTolerance = thisLength > 0.0 ? absTol / thisLength : 0.0;
            const long double inverseThisLengthSquared = 1.0L / static_cast<long double>(thisLengthSquared);
            long double otherPoint1Parameter = DotProductLongDouble(other.point1.x - point1.x, other.point1.y - point1.y, thisVectorX, thisVectorY) * inverseThisLengthSquared;
            long double otherPoint2Parameter = DotProductLongDouble(other.point2.x - point1.x, other.point2.y - point1.y, thisVectorX, thisVectorY) * inverseThisLengthSquared;
            if (!std::isfinite(otherPoint1Parameter) || !std::isfinite(otherPoint2Parameter))
            {
                return 0;
            }

            if (otherPoint1Parameter > otherPoint2Parameter)
            {
                std::swap(otherPoint1Parameter, otherPoint2Parameter);
            }

            const long double overlapStart = std::max(0.0L, otherPoint1Parameter);
            const long double overlapEnd = std::min(1.0L, otherPoint2Parameter);
            if (overlapStart > overlapEnd + static_cast<long double>(parameterTolerance))
            {
                return 0;
            }

            if (std::abs(overlapEnd - overlapStart) <= static_cast<long double>(parameterTolerance))
            {
                const double intersectionParameter = ToFiniteDoubleOrNan(ClampUnitParameterLongDouble(0.5L * (overlapStart + overlapEnd)));
                outIntersection = PointAt(intersectionParameter);
                return outIntersection.IsValid() ? 1 : 0;
            }

            const double overlapStartParameter = ToFiniteDoubleOrNan(ClampUnitParameterLongDouble(overlapStart));
            const double overlapEndParameter = ToFiniteDoubleOrNan(ClampUnitParameterLongDouble(overlapEnd));
            outOverlap.Set(PointAt(overlapStartParameter), PointAt(overlapEndParameter));
            return outOverlap.IsValid() ? 2 : 0;
        }

        if (denominatorAbs <= numericThreshold)
        {
            return 0;
        }
    }

    const long double thisParameterLong = CrossProductLongDouble(pointDeltaX, pointDeltaY, otherVectorX, otherVectorY) / denominator;
    const long double otherParameterLong = CrossProductLongDouble(pointDeltaX, pointDeltaY, thisVectorX, thisVectorY) / denominator;
    if (!std::isfinite(thisParameterLong) || !std::isfinite(otherParameterLong))
    {
        return 0;
    }

    const double thisParameterTolerance = absTol / thisLength;
    const double otherParameterTolerance = absTol / otherLength;

    if (!IsParameterInRangeLongDouble(thisParameterLong, thisParameterTolerance) || !IsParameterInRangeLongDouble(otherParameterLong, otherParameterTolerance))
    {
        return 0;
    }

    const double thisParameter = ToFiniteDoubleOrNan(ClampUnitParameterLongDouble(thisParameterLong));
    outIntersection = PointAt(thisParameter);
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
        Reset();
        return false;
    }

    iss >> std::ws;
    if (!iss.eof())
    {
        Reset();
        return false;
    }

    if (leftParen != '(' || rightParen != ')' || comma1 != ',' || comma2 != ',' || comma3 != ',' || type != GetClassType())
    {
        Reset();
        return false;
    }

    Set(parsedX1, parsedY1, parsedX2, parsedY2);
    return IsValid();
}

bool GB_LineSegment::Deserialize(const GB_ByteBuffer& data)
{
    constexpr static uint16_t expectedPayloadVersion = 1;
    constexpr static size_t minSize = 48;

    if (data.size() != minSize)
    {
        Reset();
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
        Reset();
        return false;
    }

    if (magic != GB_ClassMagicNumber || typeId != GetClassTypeId() || payloadVersion != expectedPayloadVersion || reserved != 0 || offset != data.size())
    {
        Reset();
        return false;
    }

    Set(parsedX1, parsedY1, parsedX2, parsedY2);
    return IsValid();
}
