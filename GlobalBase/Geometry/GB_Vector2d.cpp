#include "GB_Vector2d.h"
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
    struct NormalizedVector2d
    {
        double x = 0.0;
        double y = 0.0;
        double scaledLength = 0.0;
        double componentScale = 0.0;
    };

    static inline GB_Vector2d MakeNanVector()
    {
        return GB_Vector2d(GB_QuietNan, GB_QuietNan);
    }

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

    static bool TryNormalizeComponents(double x, double y, NormalizedVector2d& normalizedVector)
    {
        if (!std::isfinite(x) || !std::isfinite(y))
        {
            return false;
        }

        const double componentScale = std::max(std::abs(x), std::abs(y));
        if (componentScale == 0.0)
        {
            return false;
        }

        const double scaledX = x / componentScale;
        const double scaledY = y / componentScale;
        const double scaledLength = std::hypot(scaledX, scaledY);
        if (!std::isfinite(scaledLength) || scaledLength <= 0.0)
        {
            return false;
        }

        normalizedVector.x = scaledX / scaledLength;
        normalizedVector.y = scaledY / scaledLength;
        normalizedVector.scaledLength = scaledLength;
        normalizedVector.componentScale = componentScale;
        return true;
    }

    static double SquareLengthOrInfinity(double x, double y)
    {
        NormalizedVector2d normalizedVector;
        if (!TryNormalizeComponents(x, y, normalizedVector))
        {
            return x == 0.0 && y == 0.0 ? 0.0 : GB_QuietNan;
        }

        const double normalizedSquareLength = normalizedVector.scaledLength * normalizedVector.scaledLength;
        const double maxSafeScale = std::sqrt(std::numeric_limits<double>::max() / normalizedSquareLength);
        if (normalizedVector.componentScale > maxSafeScale)
        {
            return std::numeric_limits<double>::infinity();
        }

        return normalizedVector.componentScale * normalizedVector.componentScale * normalizedSquareLength;
    }

    static bool IsNonDegenerate(const GB_Vector2d& vector)
    {
        return vector.IsValid() && (vector.x != 0.0 || vector.y != 0.0);
    }

    static bool TryGetNormalizedPair(const GB_Vector2d& firstVector, const GB_Vector2d& secondVector, NormalizedVector2d& firstNormalized, NormalizedVector2d& secondNormalized)
    {
        return TryNormalizeComponents(firstVector.x, firstVector.y, firstNormalized) && TryNormalizeComponents(secondVector.x, secondVector.y, secondNormalized);
    }
}

const GB_Vector2d GB_Vector2d::Zero(0, 0);
const GB_Vector2d GB_Vector2d::UnitX(1, 0);
const GB_Vector2d GB_Vector2d::UnitY(0, 1);

GB_Vector2d::GB_Vector2d()
{
}

GB_Vector2d::GB_Vector2d(double x, double y)
    : x(x)
    , y(y)
{
}

GB_Vector2d::~GB_Vector2d()
{
}

const std::string& GB_Vector2d::GetClassType() const
{
    static const std::string classType = "GB_Vector2d";
    return classType;
}

uint64_t GB_Vector2d::GetClassTypeId() const
{
    static const uint64_t classTypeId = GB_GenerateClassTypeId(GetClassType()); // 15623057110163869400
    return classTypeId;
}

void GB_Vector2d::Set(double x, double y)
{
    this->x = x;
    this->y = y;
}

bool GB_Vector2d::IsValid() const
{
    return std::isfinite(x) && std::isfinite(y);
}

bool GB_Vector2d::IsZero(double tolerance) const
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

bool GB_Vector2d::IsUnit(double tolerance) const
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

    const double length = Length();
    return std::isfinite(length) && std::abs(length - 1.0) <= absoluteTolerance;
}

bool GB_Vector2d::IsNearEqual(const GB_Vector2d& other, double tolerance) const
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

    return std::hypot(x - other.x, y - other.y) <= absoluteTolerance;
}

GB_Vector2d GB_Vector2d::operator+(const GB_Vector2d& other) const
{
    return GB_Vector2d(x + other.x, y + other.y);
}

GB_Vector2d GB_Vector2d::operator-(const GB_Vector2d& other) const
{
    return GB_Vector2d(x - other.x, y - other.y);
}

GB_Vector2d GB_Vector2d::operator*(double scalar) const
{
    if (!std::isfinite(scalar))
    {
        return MakeNanVector();
    }

    return GB_Vector2d(x * scalar, y * scalar);
}

GB_Vector2d GB_Vector2d::operator/(double scalar) const
{
    if (!std::isfinite(scalar) || scalar == 0.0)
    {
        return MakeNanVector();
    }

    return GB_Vector2d(x / scalar, y / scalar);
}

GB_Vector2d& GB_Vector2d::operator+=(const GB_Vector2d& other)
{
    x += other.x;
    y += other.y;
    return *this;
}

GB_Vector2d& GB_Vector2d::operator-=(const GB_Vector2d& other)
{
    x -= other.x;
    y -= other.y;
    return *this;
}

GB_Vector2d& GB_Vector2d::operator*=(double scalar)
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

GB_Vector2d& GB_Vector2d::operator/=(double scalar)
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

GB_Vector2d GB_Vector2d::operator-() const
{
    return GB_Vector2d(-x, -y);
}

bool GB_Vector2d::operator==(const GB_Vector2d& other) const
{
    return x == other.x && y == other.y;
}

bool GB_Vector2d::operator!=(const GB_Vector2d& other) const
{
    return !(*this == other);
}

double GB_Vector2d::Length() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    return std::hypot(x, y);
}

double GB_Vector2d::LengthSquared() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    return SquareLengthOrInfinity(x, y);
}

double GB_Vector2d::Angle() const
{
    if (!IsNonDegenerate(*this))
    {
        return GB_QuietNan;
    }

    double angle = std::atan2(y, x);
    if (angle < 0.0)
    {
        angle += GB_2Pi;
    }
    return angle;
}

GB_Vector2d GB_Vector2d::FromAngle(double angle)
{
    if (!std::isfinite(angle))
    {
        return MakeNanVector();
    }

    return GB_Vector2d(std::cos(angle), std::sin(angle));
}

GB_Vector2d GB_Vector2d::Normalized() const
{
    NormalizedVector2d normalizedVector;
    if (!TryNormalizeComponents(x, y, normalizedVector))
    {
        return MakeNanVector();
    }

    return GB_Vector2d(normalizedVector.x, normalizedVector.y);
}

void GB_Vector2d::Normalize()
{
    *this = Normalized();
}

double GB_Vector2d::DotProduct(const GB_Vector2d& a, const GB_Vector2d& b)
{
    return a.x * b.x + a.y * b.y;
}

double GB_Vector2d::DotProduct(const GB_Vector2d& other) const
{
    return DotProduct(*this, other);
}

double GB_Vector2d::CrossProduct(const GB_Vector2d& a, const GB_Vector2d& b)
{
    return a.x * b.y - a.y * b.x;
}

double GB_Vector2d::CrossProduct(const GB_Vector2d& other) const
{
    return CrossProduct(*this, other);
}

GB_Vector2d GB_Vector2d::Transform(const GB_Vector2d& vec, const GB_Matrix3x3& mat)
{
    return mat.TransformVector(vec);
}

void GB_Vector2d::Transform(const GB_Matrix3x3& mat)
{
    *this = Transform(*this, mat);
}

GB_Vector2d GB_Vector2d::Transformed(const GB_Matrix3x3& mat) const
{
    return Transform(*this, mat);
}

double GB_Vector2d::AngleBetween(const GB_Vector2d& a, const GB_Vector2d& b)
{
    NormalizedVector2d firstNormalized;
    NormalizedVector2d secondNormalized;
    if (!TryGetNormalizedPair(a, b, firstNormalized, secondNormalized))
    {
        return GB_QuietNan;
    }

    double dot = firstNormalized.x * secondNormalized.x + firstNormalized.y * secondNormalized.y;
    double cross = firstNormalized.x * secondNormalized.y - firstNormalized.y * secondNormalized.x;
    dot = std::max(-1.0, std::min(1.0, dot));
    cross = std::max(-1.0, std::min(1.0, cross));
    return std::atan2(std::abs(cross), dot);
}

double GB_Vector2d::AngleBetween(const GB_Vector2d& other) const
{
    return AngleBetween(*this, other);
}

double GB_Vector2d::SignedAngleTo(const GB_Vector2d& other) const
{
    NormalizedVector2d firstNormalized;
    NormalizedVector2d secondNormalized;
    if (!TryGetNormalizedPair(*this, other, firstNormalized, secondNormalized))
    {
        return GB_QuietNan;
    }

    double dot = firstNormalized.x * secondNormalized.x + firstNormalized.y * secondNormalized.y;
    double cross = firstNormalized.x * secondNormalized.y - firstNormalized.y * secondNormalized.x;
    dot = std::max(-1.0, std::min(1.0, dot));
    cross = std::max(-1.0, std::min(1.0, cross));
    return std::atan2(cross, dot);
}

bool GB_Vector2d::IsParallelTo(const GB_Vector2d& other, double tolerance) const
{
    double absoluteTolerance = 0.0;
    if (!TryGetAbsoluteTolerance(tolerance, absoluteTolerance))
    {
        return false;
    }

    NormalizedVector2d firstNormalized;
    NormalizedVector2d secondNormalized;
    if (!TryGetNormalizedPair(*this, other, firstNormalized, secondNormalized))
    {
        return false;
    }

    const double normalizedCross = firstNormalized.x * secondNormalized.y - firstNormalized.y * secondNormalized.x;
    return std::abs(normalizedCross) <= absoluteTolerance;
}

bool GB_Vector2d::IsPerpendicularTo(const GB_Vector2d& other, double tolerance) const
{
    double absoluteTolerance = 0.0;
    if (!TryGetAbsoluteTolerance(tolerance, absoluteTolerance))
    {
        return false;
    }

    NormalizedVector2d firstNormalized;
    NormalizedVector2d secondNormalized;
    if (!TryGetNormalizedPair(*this, other, firstNormalized, secondNormalized))
    {
        return false;
    }

    const double normalizedDot = firstNormalized.x * secondNormalized.x + firstNormalized.y * secondNormalized.y;
    return std::abs(normalizedDot) <= absoluteTolerance;
}

bool GB_Vector2d::IsCodirectionalTo(const GB_Vector2d& other, double tolerance) const
{
    if (!IsParallelTo(other, tolerance))
    {
        return false;
    }

    NormalizedVector2d firstNormalized;
    NormalizedVector2d secondNormalized;
    if (!TryGetNormalizedPair(*this, other, firstNormalized, secondNormalized))
    {
        return false;
    }

    return firstNormalized.x * secondNormalized.x + firstNormalized.y * secondNormalized.y > 0.0;
}

GB_Vector2d GB_Vector2d::Rotated(double angle) const
{
    if (!IsValid() || !std::isfinite(angle))
    {
        return MakeNanVector();
    }

    const double cosAngle = std::cos(angle);
    const double sinAngle = std::sin(angle);
    const GB_Vector2d result(x * cosAngle - y * sinAngle, x * sinAngle + y * cosAngle);
    return result.IsValid() ? result : MakeNanVector();
}

void GB_Vector2d::Rotate(double angle)
{
    *this = Rotated(angle);
}

GB_Vector2d GB_Vector2d::ProjectOn(const GB_Vector2d& onto) const
{
    if (!IsValid() || !onto.IsValid())
    {
        return MakeNanVector();
    }

    NormalizedVector2d normalizedOnto;
    if (!TryNormalizeComponents(onto.x, onto.y, normalizedOnto))
    {
        return MakeNanVector();
    }

    const double componentScale = std::max(std::abs(x), std::abs(y));
    if (componentScale == 0.0)
    {
        return GB_Vector2d::Zero;
    }

    const double scaledX = x / componentScale;
    const double scaledY = y / componentScale;
    const double scaledProjectionLength = scaledX * normalizedOnto.x + scaledY * normalizedOnto.y;
    const double resultX = componentScale * (normalizedOnto.x * scaledProjectionLength);
    const double resultY = componentScale * (normalizedOnto.y * scaledProjectionLength);
    const GB_Vector2d result(resultX, resultY);
    return result.IsValid() ? result : MakeNanVector();
}

std::string GB_Vector2d::SerializeToString() const
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "(" << GetClassType() << " " << std::setprecision(17) << x << "," << y << ")";
    return stream.str();
}

GB_ByteBuffer GB_Vector2d::SerializeToBinary() const
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

bool GB_Vector2d::Deserialize(const std::string& data)
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

bool GB_Vector2d::Deserialize(const GB_ByteBuffer& data)
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
