#include "GB_Vector2d.h"
#include "GB_Matrix3x3.h"
#include "../GB_IO.h"
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <locale>
#include <cstdint>
#include <cstring>

namespace
{
    static inline GB_Vector2d MakeNanVector()
    {
        return GB_Vector2d(GB_QuietNan, GB_QuietNan);
    }
}

const GB_Vector2d GB_Vector2d::Zero(0, 0);
const GB_Vector2d GB_Vector2d::UnitX(1, 0);
const GB_Vector2d GB_Vector2d::UnitY(0, 1);

GB_Vector2d::GB_Vector2d() {}

GB_Vector2d::GB_Vector2d(double x, double y) : x(x), y(y) {}

GB_Vector2d::~GB_Vector2d() {}

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

    const double tolerance2 = tolerance * tolerance;
    return LengthSquared() <= tolerance2;
}

bool GB_Vector2d::IsUnit(double tolerance) const
{
    if (!IsValid())
    {
        return false;
    }

    const double length = Length();
    if (!std::isfinite(length) || length <= GB_Epsilon)
    {
        return false;
    }

    return std::abs(length - 1) <= std::abs(tolerance);
}

bool GB_Vector2d::IsNearEqual(const GB_Vector2d& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double dx = x - other.x;
    const double dy = y - other.y;

    const double tolerance2 = tolerance * tolerance;
    return (dx * dx + dy * dy) <= tolerance2;
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
    return GB_Vector2d(x * scalar, y * scalar);
}

GB_Vector2d GB_Vector2d::operator/(double scalar) const
{
    if (std::abs(scalar) <= GB_Epsilon)
    {
        return MakeNanVector();
    }

    const double inv = 1.0 / scalar;
    return GB_Vector2d(x * inv, y * inv);
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
    x *= scalar;
    y *= scalar;
    return *this;
}

GB_Vector2d& GB_Vector2d::operator/=(double scalar)
{
    if (std::abs(scalar) <= GB_Epsilon)
    {
        x = GB_QuietNan;
        y = GB_QuietNan;
        return *this;
    }

    const double inv = 1.0 / scalar;
    x *= inv;
    y *= inv;
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
    return std::sqrt(LengthSquared());
}

double GB_Vector2d::LengthSquared() const
{
    return x * x + y * y;
}

double GB_Vector2d::Angle() const
{
    if (!IsValid() || IsZero())
    {
        return GB_QuietNan;
    }

    double angle = std::atan2(y, x);
    if (angle < 0)
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
    if (!IsValid())
    {
        return MakeNanVector();
    }

    const double length = Length();
    if (!std::isfinite(length) || length <= GB_Epsilon)
    {
        return MakeNanVector();
    }

    return (*this) / length;
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
    if (!a.IsValid() || !b.IsValid())
    {
        return GB_QuietNan;
    }

    constexpr static double eps2 = GB_Epsilon * GB_Epsilon;
    if (a.LengthSquared() <= eps2 || b.LengthSquared() <= eps2)
    {
        return GB_QuietNan;
    }

    const double dot = DotProduct(a, b);
    const double crossAbs = std::abs(CrossProduct(a, b));
    return std::atan2(crossAbs, dot);
}

double GB_Vector2d::AngleBetween(const GB_Vector2d& other) const
{
    return AngleBetween(*this, other);
}

double GB_Vector2d::SignedAngleTo(const GB_Vector2d& other) const
{
    if (!IsValid() || !other.IsValid())
    {
        return GB_QuietNan;
    }

    constexpr static double eps2 = GB_Epsilon * GB_Epsilon;
    if (LengthSquared() <= eps2 || other.LengthSquared() <= eps2)
    {
        return GB_QuietNan;
    }

    const double dot = DotProduct(other);
    const double cross = CrossProduct(other);
    return std::atan2(cross, dot);
}

bool GB_Vector2d::IsParallelTo(const GB_Vector2d& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    constexpr static double eps2 = GB_Epsilon * GB_Epsilon;
    const double lenA2 = LengthSquared();
    const double lenB2 = other.LengthSquared();
    if (lenA2 <= eps2 || lenB2 <= eps2)
    {
        return false;
    }

    const double cross = CrossProduct(other);
    if (!std::isfinite(cross))
    {
        return false;
    }

    const double rhs = (tolerance * tolerance) * lenA2 * lenB2;
    return (cross * cross) <= rhs;
}

bool GB_Vector2d::IsPerpendicularTo(const GB_Vector2d& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    constexpr static double eps2 = GB_Epsilon * GB_Epsilon;
    const double lenA2 = LengthSquared();
    const double lenB2 = other.LengthSquared();
    if (lenA2 <= eps2 || lenB2 <= eps2)
    {
        return false;
    }

    const double dot = DotProduct(other);
    if (!std::isfinite(dot))
    {
        return false;
    }

    const double rhs = (tolerance * tolerance) * lenA2 * lenB2;
    return (dot * dot) <= rhs;
}

bool GB_Vector2d::IsCodirectionalTo(const GB_Vector2d& other, double tolerance) const
{
    if (!IsParallelTo(other, tolerance))
    {
        return false;
    }

    return DotProduct(other) > 0;
}

GB_Vector2d GB_Vector2d::Rotated(double angle) const
{
    if (!IsValid() || !std::isfinite(angle))
    {
        return MakeNanVector();
    }

    const double c = std::cos(angle);
    const double s = std::sin(angle);

    return GB_Vector2d(x * c - y * s, x * s + y * c);
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

    const double ontoLen2 = onto.LengthSquared();
    constexpr static double eps2 = GB_Epsilon * GB_Epsilon;
    if (!std::isfinite(ontoLen2) || ontoLen2 <= eps2)
    {
        return MakeNanVector();
    }

    const double scale = DotProduct(onto) / ontoLen2;
    return onto * scale;
}

std::string GB_Vector2d::SerializeToString() const
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
	oss << "(" << GetClassType() << " " << std::setprecision(17) << x << "," << y << ")";
    return oss.str();
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
    std::istringstream iss(data);
    iss.imbue(std::locale::classic());

    char leftParen = 0;
    std::string type;
    char comma = 0;
    char rightParen = 0;

    double parsedX = GB_QuietNan;
    double parsedY = GB_QuietNan;

    if (!(iss >> leftParen >> type >> parsedX >> comma >> parsedY >> rightParen))
    {
        x = GB_QuietNan;
        y = GB_QuietNan;
        return false;
    }

    if (leftParen != '(' || rightParen != ')' || comma != ',' || type != GetClassType())
    {
        x = GB_QuietNan;
        y = GB_QuietNan;
        return false;
    }

    x = parsedX;
    y = parsedY;
    return true;
}

bool GB_Vector2d::Deserialize(const GB_ByteBuffer& data)
{
    constexpr static uint16_t expectedPayloadVersion = 1;
    constexpr static size_t minSize = 32;

    if (data.size() < minSize)
    {
        x = GB_QuietNan;
        y = GB_QuietNan;
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
        x = GB_QuietNan;
        y = GB_QuietNan;
        return false;
    }

    if (magic != GB_ClassMagicNumber || typeId != GetClassTypeId() || payloadVersion != expectedPayloadVersion)
    {
        x = GB_QuietNan;
        y = GB_QuietNan;
        return false;
    }

    x = parsedX;
    y = parsedY;
    return true;
}
