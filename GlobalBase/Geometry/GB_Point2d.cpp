#include "GB_Point2d.h"
#include "GB_Vector2d.h"
#include "GB_Matrix3x3.h"
#include "../GB_IO.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <assert.h>

const GB_Point2d GB_Point2d::Origin(0, 0);

GB_Point2d::GB_Point2d()
{
}

GB_Point2d::GB_Point2d(double x, double y) : x(x), y(y)
{
}

GB_Point2d::GB_Point2d(const GB_Vector2d& vec) : x(vec.x), y(vec.y)
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

    return (x * x + y * y <= tolerance * tolerance);
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
    if (!std::isfinite(scalar) || std::abs(scalar) <= GB_Epsilon)
    {
        return GB_Point2d();
    }

    const double inv = 1.0 / scalar;
    return GB_Point2d(x * inv, y * inv);
}

GB_Point2d& GB_Point2d::operator/=(double scalar)
{
    if (!std::isfinite(scalar) || std::abs(scalar) <= GB_Epsilon)
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
    return (index == 0) ? x : y;
}

const double& GB_Point2d::operator[](size_t index) const
{
    assert(index < 2);
    return (index == 0) ? x : y;
}

GB_Vector2d GB_Point2d::ToVector2d() const
{
    return GB_Vector2d(x, y);
}

double GB_Point2d::DistanceTo(const GB_Point2d& other) const
{
    return std::sqrt(DistanceToSquared(other));
}

double GB_Point2d::DistanceToSquared(const GB_Point2d& other) const
{
    if (!IsValid() || !other.IsValid())
    {
        return GB_QuietNan;
    }

    const double dx = x - other.x;
    const double dy = y - other.y;
    const double dist2 = dx * dx + dy * dy;
    return std::isfinite(dist2) ? dist2 : GB_QuietNan;
}

double GB_Point2d::DistanceToOrigin() const
{
    return std::sqrt(DistanceToOriginSquared());
}

double GB_Point2d::DistanceToOriginSquared() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const double dist2 = x * x + y * y;
    return std::isfinite(dist2) ? dist2 : GB_QuietNan;
}

bool GB_Point2d::IsNearEqual(const GB_Point2d& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double dx = x - other.x;
    const double dy = y - other.y;
    return (dx * dx + dy * dy <= tolerance * tolerance);
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

    const double c = std::cos(angle);
    const double s = std::sin(angle);

    const double localX = x - center.x;
    const double localY = y - center.y;

    const double rotatedX = localX * c - localY * s;
    const double rotatedY = localX * s + localY * c;

    return GB_Point2d(center.x + rotatedX, center.y + rotatedY);
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

    return GB_Point2d(x + deltaX, y + deltaY);
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

    return GB_Point2d(0.5 * (a.x + b.x), 0.5 * (a.y + b.y));
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

    return GB_Point2d(GB_Lerp(a.x, b.x, t), GB_Lerp(a.y, b.y, t));
}

GB_Point2d GB_Point2d::LerpTo(const GB_Point2d& other, double t) const
{
    return Lerp(*this, other, t);
}

std::string GB_Point2d::SerializeToString() const
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << "(" << GetClassType() << " " << std::setprecision(17) << x << "," << y << ")";
    return oss.str();
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

bool GB_Point2d::Deserialize(const GB_ByteBuffer& data)
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

GB_Point2d operator*(double scalar, const GB_Point2d& point)
{
    return point * scalar;
}






