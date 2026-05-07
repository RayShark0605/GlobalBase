#include "GB_LineSegment.h"
#include "GB_Vector2d.h"
#include "GB_Rectangle.h"
#include "GB_Matrix3x3.h"
#include "../GB_IO.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
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

    static inline double Square(double value)
    {
        return value * value;
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

    static inline double CrossProduct(const GB_Vector2d& firstVector, const GB_Vector2d& secondVector)
    {
        return firstVector.x * secondVector.y - firstVector.y * secondVector.x;
    }

    static inline double DotProduct(const GB_Vector2d& firstVector, const GB_Vector2d& secondVector)
    {
        return firstVector.x * secondVector.x + firstVector.y * secondVector.y;
    }

    static inline bool IsParameterInRange(double parameter, double tolerance)
    {
        return parameter >= -tolerance && parameter <= 1.0 + tolerance;
    }

    static inline double ClampUnitParameter(double parameter)
    {
        return GB_Clamp(parameter, 0.0, 1.0);
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

    if (length <= GB_Epsilon)
    {
        Set(startPoint, startPoint);
        return;
    }

    if (!direction.IsValid())
    {
        Reset();
        return;
    }

    const double directionLength = direction.Length();
    if (!std::isfinite(directionLength) || directionLength <= GB_Epsilon)
    {
        Reset();
        return;
    }

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
    return std::sqrt(LengthSquared());
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
    const GB_Vector2d segmentVector = ToVector();
    const double length = segmentVector.Length();
    if (!std::isfinite(length) || length <= absTol)
    {
        return MakeNanVector();
    }

    return segmentVector / length;
}

double GB_LineSegment::Angle() const
{
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

    const GB_Vector2d segmentVector = ToVector();
    const double lengthSquared = segmentVector.LengthSquared();
    if (!std::isfinite(lengthSquared) || lengthSquared <= GB_Epsilon * GB_Epsilon)
    {
        return GB_QuietNan;
    }

    const GB_Vector2d pointVector = point - point1;
    return DotProduct(pointVector, segmentVector) / lengthSquared;
}

GB_Point2d GB_LineSegment::ProjectPointOnLine(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return MakeNanPoint();
    }

    const double parameter = ParameterAt(point);
    if (!std::isfinite(parameter))
    {
        return point1;
    }

    return PointAt(parameter);
}

GB_Point2d GB_LineSegment::ClosestPointTo(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return MakeNanPoint();
    }

    const double parameter = ParameterAt(point);
    if (!std::isfinite(parameter))
    {
        return point1;
    }

    return PointAt(ClampUnitParameter(parameter));
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

    const GB_Point2d closestPoint = ClosestPointTo(point);
    return closestPoint.DistanceToSquared(point);
}

double GB_LineSegment::DistanceToLine(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return GB_QuietNan;
    }

    const GB_Vector2d segmentVector = ToVector();
    const double lengthSquared = segmentVector.LengthSquared();
    if (!std::isfinite(lengthSquared) || lengthSquared <= GB_Epsilon * GB_Epsilon)
    {
        return point1.DistanceTo(point);
    }

    const GB_Vector2d pointVector = point - point1;
    const double cross = CrossProduct(segmentVector, pointVector);
    return std::abs(cross) / std::sqrt(lengthSquared);
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

    const double distanceToOtherPoint1 = DistanceTo(other.point1);
    const double distanceToOtherPoint2 = DistanceTo(other.point2);
    const double otherDistanceToPoint1 = other.DistanceTo(point1);
    const double otherDistanceToPoint2 = other.DistanceTo(point2);
    return std::min(std::min(distanceToOtherPoint1, distanceToOtherPoint2), std::min(otherDistanceToPoint1, otherDistanceToPoint2));
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
    *this = NormalizedEndpointOrder();
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

    point1.Rotate(angle, center);
    point2.Rotate(angle, center);
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

    point1 = center + (point1 - center) * scaleFactor;
    point2 = center + (point2 - center) * scaleFactor;
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

    const GB_Vector2d segmentVector = ToVector();
    const double length = segmentVector.Length();
    if (!std::isfinite(length))
    {
        Reset();
        return;
    }

    if (length <= GB_Epsilon)
    {
        if (std::abs(deltaAtPoint1) <= GB_Epsilon && std::abs(deltaAtPoint2) <= GB_Epsilon)
        {
            return;
        }

        Reset();
        return;
    }

    const double newLength = length + deltaAtPoint1 + deltaAtPoint2;
    if (newLength < -GB_Epsilon)
    {
        Reset();
        return;
    }

    const GB_Vector2d unit = segmentVector / length;
    const GB_Point2d newPoint1 = point1 - unit * deltaAtPoint1;
    const GB_Point2d newPoint2 = point2 + unit * deltaAtPoint2;

    if (std::abs(newLength) <= GB_Epsilon)
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
    const GB_Vector2d segmentVector = ToVector();
    const double lengthSquared = segmentVector.LengthSquared();
    if (!std::isfinite(lengthSquared))
    {
        return false;
    }

    if (lengthSquared <= absTol * absTol)
    {
        return point1.IsNearEqual(point, absTol);
    }

    const GB_Vector2d pointVector = point - point1;
    const double cross = CrossProduct(segmentVector, pointVector);
    if (!std::isfinite(cross) || Square(cross) > Square(absTol) * lengthSquared)
    {
        return false;
    }

    const double dot = DotProduct(pointVector, segmentVector);
    const double length = std::sqrt(lengthSquared);
    return dot >= -absTol * length && dot <= lengthSquared + absTol * length;
}

int GB_LineSegment::SideOfPoint(const GB_Point2d& point, double tolerance) const
{
    if (!IsValid() || !point.IsValid())
    {
        return 0;
    }

    const double absTol = AbsTol(tolerance);
    const GB_Vector2d segmentVector = ToVector();
    const double lengthSquared = segmentVector.LengthSquared();
    if (!std::isfinite(lengthSquared) || lengthSquared <= absTol * absTol)
    {
        return 0;
    }

    const double cross = CrossProduct(segmentVector, point - point1);
    const double threshold = absTol * std::sqrt(lengthSquared);
    if (cross > threshold)
    {
        return 1;
    }
    if (cross < -threshold)
    {
        return -1;
    }
    return 0;
}

bool GB_LineSegment::IsParallelTo(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    return ToVector().IsParallelTo(other.ToVector(), AbsTol(tolerance));
}

bool GB_LineSegment::IsPerpendicularTo(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    return ToVector().IsPerpendicularTo(other.ToVector(), AbsTol(tolerance));
}

bool GB_LineSegment::IsCollinearWith(const GB_LineSegment& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double absTol = AbsTol(tolerance);
    if (IsDegenerate(absTol) || other.IsDegenerate(absTol))
    {
        return IsDegenerate(absTol) && other.IsDegenerate(absTol) && point1.IsNearEqual(other.point1, absTol);
    }

    return IsParallelTo(other, absTol) && DistanceToLine(other.point1) <= absTol && DistanceToLine(other.point2) <= absTol;
}

double GB_LineSegment::AngleBetween(const GB_LineSegment& other) const
{
    if (!IsValid() || !other.IsValid())
    {
        return GB_QuietNan;
    }

    double angle = ToVector().AngleBetween(other.ToVector());
    if (!std::isfinite(angle))
    {
        return GB_QuietNan;
    }

    if (angle > GB_HalfPi)
    {
        angle = GB_Pi - angle;
    }
    return angle;
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

    const GB_Vector2d thisVector = ToVector();
    const GB_Vector2d otherVector = other.ToVector();
    const GB_Vector2d point1ToOtherPoint1 = other.point1 - point1;
    const double thisLengthSquared = thisVector.LengthSquared();
    const double otherLengthSquared = otherVector.LengthSquared();
    const double thisLength = std::sqrt(thisLengthSquared);
    const double otherLength = std::sqrt(otherLengthSquared);
    const double denominator = CrossProduct(thisVector, otherVector);
    const double denominatorThreshold = absTol * thisLength * otherLength;

    if (std::abs(denominator) <= denominatorThreshold)
    {
        if (DistanceToLine(other.point1) > absTol || DistanceToLine(other.point2) > absTol)
        {
            return 0;
        }

        const double parameterTolerance = (thisLength > GB_Epsilon) ? (absTol / thisLength) : absTol;
        double otherPoint1Parameter = ParameterAt(other.point1);
        double otherPoint2Parameter = ParameterAt(other.point2);
        if (!std::isfinite(otherPoint1Parameter) || !std::isfinite(otherPoint2Parameter))
        {
            return 0;
        }

        if (otherPoint1Parameter > otherPoint2Parameter)
        {
            std::swap(otherPoint1Parameter, otherPoint2Parameter);
        }

        const double overlapStart = std::max(0.0, otherPoint1Parameter);
        const double overlapEnd = std::min(1.0, otherPoint2Parameter);
        if (overlapStart > overlapEnd + parameterTolerance)
        {
            return 0;
        }

        if (std::abs(overlapEnd - overlapStart) <= parameterTolerance)
        {
            outIntersection = PointAt(ClampUnitParameter(0.5 * (overlapStart + overlapEnd)));
            return 1;
        }

        outOverlap.Set(PointAt(ClampUnitParameter(overlapStart)), PointAt(ClampUnitParameter(overlapEnd)));
        return outOverlap.IsValid() ? 2 : 0;
    }

    const double thisParameter = CrossProduct(point1ToOtherPoint1, otherVector) / denominator;
    const double otherParameter = CrossProduct(point1ToOtherPoint1, thisVector) / denominator;
    const double thisParameterTolerance = absTol / thisLength;
    const double otherParameterTolerance = absTol / otherLength;

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

    if (data.size() < minSize)
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

    if (magic != GB_ClassMagicNumber || typeId != GetClassTypeId() || payloadVersion != expectedPayloadVersion)
    {
        Reset();
        return false;
    }

    Set(parsedX1, parsedY1, parsedX2, parsedY2);
    return IsValid();
}
