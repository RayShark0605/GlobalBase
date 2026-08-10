#include "GB_Point2d.h"
#include "GB_Vector2d.h"
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

#include <assert.h>
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

    static bool TryGetAbsoluteTolerance(double tolerance, double& absoluteTolerance)
    {
        if (std::isnan(tolerance))
        {
            absoluteTolerance = 0.0;
            return false;
        }

        absoluteTolerance = std::abs(tolerance);
        return true;
    }

    static double SquareLengthOrInfinity(double x, double y)
    {
        if (!std::isfinite(x) || !std::isfinite(y))
        {
            return GB_QuietNan;
        }

        const double absoluteX = std::abs(x);
        const double absoluteY = std::abs(y);
        const double maxComponent = std::max(absoluteX, absoluteY);
        if (maxComponent == 0.0)
        {
            return 0.0;
        }

        const double normalizedX = x / maxComponent;
        const double normalizedY = y / maxComponent;
        const double normalizedSquareLength = normalizedX * normalizedX + normalizedY * normalizedY;
        const double maxSafeScale = std::sqrt(std::numeric_limits<double>::max() / normalizedSquareLength);
        if (maxComponent > maxSafeScale)
        {
            return std::numeric_limits<double>::infinity();
        }

        return maxComponent * maxComponent * normalizedSquareLength;
    }

    static double AbsoluteDifferenceOrInfinity(double firstValue, double secondValue)
    {
        if (!std::isfinite(firstValue) || !std::isfinite(secondValue))
        {
            return GB_QuietNan;
        }

        if (std::signbit(firstValue) != std::signbit(secondValue))
        {
            const double absoluteFirstValue = std::abs(firstValue);
            const double absoluteSecondValue = std::abs(secondValue);
            if (absoluteFirstValue > std::numeric_limits<double>::max() - absoluteSecondValue)
            {
                return std::numeric_limits<double>::infinity();
            }

            return absoluteFirstValue + absoluteSecondValue;
        }

        return std::abs(firstValue - secondValue);
    }

    static double DistanceOrInfinity(double firstX, double firstY, double secondX, double secondY)
    {
        const double deltaX = AbsoluteDifferenceOrInfinity(firstX, secondX);
        const double deltaY = AbsoluteDifferenceOrInfinity(firstY, secondY);
        if (std::isnan(deltaX) || std::isnan(deltaY))
        {
            return GB_QuietNan;
        }

        return std::hypot(deltaX, deltaY);
    }

    static double DistanceSquaredOrInfinity(double firstX, double firstY, double secondX, double secondY)
    {
        const double deltaX = AbsoluteDifferenceOrInfinity(firstX, secondX);
        const double deltaY = AbsoluteDifferenceOrInfinity(firstY, secondY);
        if (std::isnan(deltaX) || std::isnan(deltaY))
        {
            return GB_QuietNan;
        }

        if (std::isinf(deltaX) || std::isinf(deltaY))
        {
            return std::numeric_limits<double>::infinity();
        }

        return SquareLengthOrInfinity(deltaX, deltaY);
    }

    static double RobustMidpoint(double firstValue, double secondValue)
    {
        return firstValue * 0.5 + secondValue * 0.5;
    }

    static bool TryHighPrecisionToFiniteDouble(const HighPrecisionFloat& value, double& result)
    {
        const HighPrecisionFloat maxDouble = HighPrecisionFloat((std::numeric_limits<double>::max)());
        if (value > maxDouble || value < -maxDouble)
        {
            result = GB_QuietNan;
            return false;
        }

        result = static_cast<double>(value);
        return std::isfinite(result);
    }

    static double RobustLerp(double firstValue, double secondValue, double t)
    {
        if (t >= 0.0 && t <= 1.0)
        {
            return firstValue * (1.0 - t) + secondValue * t;
        }

        const double directResult = firstValue + (secondValue - firstValue) * t;
        if (std::isfinite(directResult))
        {
            return directResult;
        }

        const HighPrecisionFloat firstValueHigh = HighPrecisionFloat(firstValue);
        const HighPrecisionFloat secondValueHigh = HighPrecisionFloat(secondValue);
        const HighPrecisionFloat tHigh = HighPrecisionFloat(t);
        const HighPrecisionFloat resultHigh = firstValueHigh + (secondValueHigh - firstValueHigh) * tHigh;
        double result = GB_QuietNan;
        return TryHighPrecisionToFiniteDouble(resultHigh, result) ? result : GB_QuietNan;
    }

    static bool TryRotatePointHighPrecision(const GB_Point2d& point, const GB_Point2d& center, double cosAngle, double sinAngle, GB_Point2d& rotatedPoint)
    {
        const HighPrecisionFloat pointX = HighPrecisionFloat(point.x);
        const HighPrecisionFloat pointY = HighPrecisionFloat(point.y);
        const HighPrecisionFloat centerX = HighPrecisionFloat(center.x);
        const HighPrecisionFloat centerY = HighPrecisionFloat(center.y);
        const HighPrecisionFloat cosAngleHigh = HighPrecisionFloat(cosAngle);
        const HighPrecisionFloat sinAngleHigh = HighPrecisionFloat(sinAngle);
        const HighPrecisionFloat localX = pointX - centerX;
        const HighPrecisionFloat localY = pointY - centerY;
        const HighPrecisionFloat rotatedXHigh = centerX + localX * cosAngleHigh - localY * sinAngleHigh;
        const HighPrecisionFloat rotatedYHigh = centerY + localX * sinAngleHigh + localY * cosAngleHigh;

        double rotatedX = GB_QuietNan;
        double rotatedY = GB_QuietNan;
        if (!TryHighPrecisionToFiniteDouble(rotatedXHigh, rotatedX) || !TryHighPrecisionToFiniteDouble(rotatedYHigh, rotatedY))
        {
            rotatedPoint = GB_Point2d();
            return false;
        }

        rotatedPoint.Set(rotatedX, rotatedY);
        return true;
    }
}

const GB_Point2d GB_Point2d::Origin(0, 0);

GB_Point2d::GB_Point2d()
{
}

GB_Point2d::GB_Point2d(double x, double y)
    : x(x)
    , y(y)
{
}

GB_Point2d::GB_Point2d(const GB_Vector2d& vec)
    : x(vec.x)
    , y(vec.y)
{
}

GB_Point2d::~GB_Point2d()
{
}

const std::string& GB_Point2d::GetClassType() const
{
    static const std::string classType = "GB_Point2d";
    return classType;
}

uint64_t GB_Point2d::GetClassTypeId() const
{
    static const uint64_t classTypeId = GB_GenerateClassTypeId(GetClassType()); // 17680236665691764099
    return classTypeId;
}

void GB_Point2d::Set(double x, double y)
{
    this->x = x;
    this->y = y;
}

bool GB_Point2d::IsValid() const
{
    return std::isfinite(x) && std::isfinite(y);
}

bool GB_Point2d::IsOrigin(double tolerance) const
{
    if (!IsValid())
    {
        return false;
    }

    double absoluteTolerance = 0.0;
    if (!TryGetAbsoluteTolerance(tolerance, absoluteTolerance))
    {
        return false;
    }

    return std::hypot(x, y) <= absoluteTolerance;
}

GB_Point2d GB_Point2d::operator*(double scalar) const
{
    if (!std::isfinite(scalar))
    {
        return GB_Point2d();
    }

    return GB_Point2d(x * scalar, y * scalar);
}

GB_Point2d& GB_Point2d::operator*=(double scalar)
{
    if (!std::isfinite(scalar))
    {
        x = GB_QuietNan;
        y = GB_QuietNan;
        return *this;
    }

    x *= scalar;
    y *= scalar;
    return *this;
}

GB_Point2d GB_Point2d::operator/(double scalar) const
{
    if (!std::isfinite(scalar) || scalar == 0.0)
    {
        return GB_Point2d();
    }

    return GB_Point2d(x / scalar, y / scalar);
}

GB_Point2d& GB_Point2d::operator/=(double scalar)
{
    if (!std::isfinite(scalar) || scalar == 0.0)
    {
        x = GB_QuietNan;
        y = GB_QuietNan;
        return *this;
    }

    x /= scalar;
    y /= scalar;
    return *this;
}

GB_Point2d GB_Point2d::operator+(const GB_Vector2d& vec) const
{
    return GB_Point2d(x + vec.x, y + vec.y);
}

GB_Point2d& GB_Point2d::operator+=(const GB_Vector2d& vec)
{
    x += vec.x;
    y += vec.y;
    return *this;
}

GB_Point2d GB_Point2d::operator-(const GB_Vector2d& vec) const
{
    return GB_Point2d(x - vec.x, y - vec.y);
}

GB_Point2d& GB_Point2d::operator-=(const GB_Vector2d& vec)
{
    x -= vec.x;
    y -= vec.y;
    return *this;
}

GB_Vector2d GB_Point2d::operator-(const GB_Point2d& other) const
{
    return GB_Vector2d(x - other.x, y - other.y);
}

bool GB_Point2d::operator==(const GB_Point2d& other) const
{
    return x == other.x && y == other.y;
}

bool GB_Point2d::operator!=(const GB_Point2d& other) const
{
    return !(*this == other);
}

double& GB_Point2d::operator[](size_t index)
{
    assert(index < 2);
    return index == 0 ? x : y;
}

const double& GB_Point2d::operator[](size_t index) const
{
    assert(index < 2);
    return index == 0 ? x : y;
}

GB_Vector2d GB_Point2d::ToVector2d() const
{
    return GB_Vector2d(x, y);
}

double GB_Point2d::DistanceTo(const GB_Point2d& other) const
{
    if (!IsValid() || !other.IsValid())
    {
        return GB_QuietNan;
    }

    return DistanceOrInfinity(x, y, other.x, other.y);
}

double GB_Point2d::DistanceToSquared(const GB_Point2d& other) const
{
    if (!IsValid() || !other.IsValid())
    {
        return GB_QuietNan;
    }

    return DistanceSquaredOrInfinity(x, y, other.x, other.y);
}

double GB_Point2d::DistanceToOrigin() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    return std::hypot(x, y);
}

double GB_Point2d::DistanceToOriginSquared() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    return SquareLengthOrInfinity(x, y);
}

bool GB_Point2d::IsNearEqual(const GB_Point2d& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    double absoluteTolerance = 0.0;
    if (!TryGetAbsoluteTolerance(tolerance, absoluteTolerance))
    {
        return false;
    }

    return DistanceOrInfinity(x, y, other.x, other.y) <= absoluteTolerance;
}

GB_Point2d GB_Point2d::Transformed(const GB_Matrix3x3& mat) const
{
    if (!IsValid() || !mat.IsValid())
    {
        return GB_Point2d();
    }

    return mat.TransformPoint(*this);
}

void GB_Point2d::Transform(const GB_Matrix3x3& mat)
{
    *this = Transformed(mat);
}

GB_Point2d GB_Point2d::Rotated(double angle, const GB_Point2d& center) const
{
    if (!IsValid() || !center.IsValid() || !std::isfinite(angle))
    {
        return GB_Point2d();
    }

    const double cosAngle = std::cos(angle);
    const double sinAngle = std::sin(angle);
    if (cosAngle == 1.0 && sinAngle == 0.0)
    {
        return *this;
    }

    const double localX = x - center.x;
    const double localY = y - center.y;
    const double rotatedX = localX * cosAngle - localY * sinAngle;
    const double rotatedY = localX * sinAngle + localY * cosAngle;
    const GB_Point2d result(center.x + rotatedX, center.y + rotatedY);
    if (result.IsValid())
    {
        return result;
    }

    GB_Point2d robustResult;
    return TryRotatePointHighPrecision(*this, center, cosAngle, sinAngle, robustResult) ? robustResult : GB_Point2d();
}

void GB_Point2d::Rotate(double angle, const GB_Point2d& center)
{
    *this = Rotated(angle, center);
}

GB_Point2d GB_Point2d::Offsetted(double deltaX, double deltaY) const
{
    if (!IsValid() || !std::isfinite(deltaX) || !std::isfinite(deltaY))
    {
        return GB_Point2d();
    }

    const GB_Point2d result(x + deltaX, y + deltaY);
    return result.IsValid() ? result : GB_Point2d();
}

void GB_Point2d::Offset(double deltaX, double deltaY)
{
    *this = Offsetted(deltaX, deltaY);
}

GB_Point2d GB_Point2d::MidPoint(const GB_Point2d& a, const GB_Point2d& b)
{
    if (!a.IsValid() || !b.IsValid())
    {
        return GB_Point2d();
    }

    return GB_Point2d(RobustMidpoint(a.x, b.x), RobustMidpoint(a.y, b.y));
}

GB_Point2d GB_Point2d::MidPointTo(const GB_Point2d& other) const
{
    return MidPoint(*this, other);
}

GB_Point2d GB_Point2d::Lerp(const GB_Point2d& a, const GB_Point2d& b, double t)
{
    if (!a.IsValid() || !b.IsValid() || !std::isfinite(t))
    {
        return GB_Point2d();
    }

    const GB_Point2d result(RobustLerp(a.x, b.x, t), RobustLerp(a.y, b.y, t));
    return result.IsValid() ? result : GB_Point2d();
}

GB_Point2d GB_Point2d::LerpTo(const GB_Point2d& other, double t) const
{
    return Lerp(*this, other, t);
}

std::string GB_Point2d::SerializeToString() const
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "(" << GetClassType() << " " << std::setprecision(17) << x << "," << y << ")";
    return stream.str();
}

GB_ByteBuffer GB_Point2d::SerializeToBinary() const
{
    constexpr static uint16_t payloadVersion = 1;

    GB_ByteBuffer buffer;
    buffer.reserve(32);
    GB_ByteBufferIO::AppendUInt32LE(buffer, GB_ClassMagicNumber);
    GB_ByteBufferIO::AppendUInt64LE(buffer, GetClassTypeId());
    GB_ByteBufferIO::AppendUInt16LE(buffer, payloadVersion);
    GB_ByteBufferIO::AppendUInt16LE(buffer, 0);
    GB_ByteBufferIO::AppendDoubleLE(buffer, x);
    GB_ByteBufferIO::AppendDoubleLE(buffer, y);
    return buffer;
}

bool GB_Point2d::Deserialize(const std::string& data)
{
    std::istringstream stream(data);
    stream.imbue(std::locale::classic());

    char leftParenthesis = 0;
    std::string type;
    char comma = 0;
    char rightParenthesis = 0;
    double parsedX = GB_QuietNan;
    double parsedY = GB_QuietNan;

    if (!(stream >> leftParenthesis >> type >> parsedX >> comma >> parsedY >> rightParenthesis))
    {
        return false;
    }

    stream >> std::ws;
    if (!stream.eof() || leftParenthesis != '(' || rightParenthesis != ')' || comma != ',' || type != GetClassType())
    {
        return false;
    }

    x = parsedX;
    y = parsedY;
    return true;
}

bool GB_Point2d::Deserialize(const GB_ByteBuffer& data)
{
    constexpr static uint16_t expectedPayloadVersion = 1;
    constexpr static size_t expectedSize = 32;

    if (data.size() != expectedSize)
    {
        return false;
    }

    size_t offset = 0;
    uint32_t magic = 0;
    uint64_t typeId = 0;
    uint16_t payloadVersion = 0;
    uint16_t reserved = 0;
    double parsedX = GB_QuietNan;
    double parsedY = GB_QuietNan;

    if (!GB_ByteBufferIO::ReadUInt32LE(data, offset, magic)
        || !GB_ByteBufferIO::ReadUInt64LE(data, offset, typeId)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, payloadVersion)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, reserved)
        || !GB_ByteBufferIO::ReadDoubleLE(data, offset, parsedX)
        || !GB_ByteBufferIO::ReadDoubleLE(data, offset, parsedY))
    {
        return false;
    }

    if (magic != GB_ClassMagicNumber || typeId != GetClassTypeId() || payloadVersion != expectedPayloadVersion || reserved != 0 || offset != data.size())
    {
        return false;
    }

    x = parsedX;
    y = parsedY;
    return true;
}

GB_Point2d operator*(double scalar, const GB_Point2d& point)
{
    return point * scalar;
}
