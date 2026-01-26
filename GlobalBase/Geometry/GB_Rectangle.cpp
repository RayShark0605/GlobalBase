#include "GB_Rectangle.h"
#include "GB_Vector2d.h"
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
    static inline bool IsFinite4(double a, double b, double c, double d)
    {
        return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d);
    }

    static inline double AbsTol(double tolerance)
    {
        if (!std::isfinite(tolerance))
        {
            return 0;
        }
        return std::abs(tolerance);
    }
}

const GB_Rectangle GB_Rectangle::Invalid = GB_Rectangle();

GB_Rectangle::GB_Rectangle()
{
}

GB_Rectangle::GB_Rectangle(const GB_Point2d& point)
{
    SetFromPoint(point);
}

GB_Rectangle::GB_Rectangle(const GB_Point2d& corner1, const GB_Point2d& corner2)
{
    SetFromCorners(corner1, corner2);
}

GB_Rectangle::GB_Rectangle(const GB_Point2d& center, double width, double height)
{
    SetFromCenter(center, width, height);
}

GB_Rectangle::GB_Rectangle(double minX, double minY, double maxX, double maxY)
{
    Set(minX, minY, maxX, maxY);
}

GB_Rectangle::~GB_Rectangle()
{
}

const std::string& GB_Rectangle::GetClassType() const
{
    static const std::string classType = "GB_Rectangle";
    return classType;
}

uint64_t GB_Rectangle::GetClassTypeId() const
{
    static const uint64_t classTypeId = GB_GenerateClassTypeId(GetClassType()); // 7893821985628433498
    return classTypeId;
}

void GB_Rectangle::Reset()
{
    minX = GB_QuietNan;
    minY = GB_QuietNan;
    maxX = GB_QuietNan;
    maxY = GB_QuietNan;
}

void GB_Rectangle::Set(double minX, double minY, double maxX, double maxY)
{
    if (!IsFinite4(minX, minY, maxX, maxY))
    {
        Reset();
        return;
    }

    this->minX = minX;
    this->minY = minY;
    this->maxX = maxX;
    this->maxY = maxY;

    Normalize();
}

void GB_Rectangle::SetFromPoint(const GB_Point2d& point)
{
    if (!point.IsValid())
    {
        Reset();
        return;
    }

    minX = point.x;
    minY = point.y;
    maxX = point.x;
    maxY = point.y;
}

void GB_Rectangle::SetFromCorners(const GB_Point2d& corner1, const GB_Point2d& corner2)
{
    if (!corner1.IsValid() || !corner2.IsValid())
    {
        Reset();
        return;
    }

    minX = std::min(corner1.x, corner2.x);
    minY = std::min(corner1.y, corner2.y);
    maxX = std::max(corner1.x, corner2.x);
    maxY = std::max(corner1.y, corner2.y);
}

bool GB_Rectangle::SetFromCenter(const GB_Point2d& center, double width, double height)
{
    if (!center.IsValid() || !std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0)
    {
        Reset();
        return false;
    }

    const double halfWidth = 0.5 * width;
    const double halfHeight = 0.5 * height;
    if (!std::isfinite(halfWidth) || !std::isfinite(halfHeight))
    {
        Reset();
        return false;
    }

    minX = center.x - halfWidth;
    maxX = center.x + halfWidth;
    minY = center.y - halfHeight;
    maxY = center.y + halfHeight;

    if (!IsFinite4(minX, minY, maxX, maxY))
    {
        Reset();
        return false;
    }

    Normalize();
    return IsValid();
}

bool GB_Rectangle::IsValid() const
{
    return IsFinite4(minX, minY, maxX, maxY) && (minX <= maxX) && (minY <= maxY);
}

void GB_Rectangle::Normalize()
{
    if (!IsFinite4(minX, minY, maxX, maxY))
    {
        return;
    }

    if (minX > maxX)
    {
        std::swap(minX, maxX);
    }

    if (minY > maxY)
    {
        std::swap(minY, maxY);
    }
}

double GB_Rectangle::Width() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const double value = maxX - minX;
    return std::isfinite(value) ? value : GB_QuietNan;
}

double GB_Rectangle::Height() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const double value = maxY - minY;
    return std::isfinite(value) ? value : GB_QuietNan;
}

double GB_Rectangle::Perimeter() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const double width = Width();
    const double height = Height();
    if (!std::isfinite(width) || !std::isfinite(height))
    {
        return GB_QuietNan;
    }

    const double value = 2 * (width + height);
    return std::isfinite(value) ? value : GB_QuietNan;
}

double GB_Rectangle::Area() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const double width = Width();
    const double height = Height();
    if (!std::isfinite(width) || !std::isfinite(height))
    {
        return GB_QuietNan;
    }

    const double value = width * height;
    return std::isfinite(value) ? value : GB_QuietNan;
}

double GB_Rectangle::DiagLength() const
{
    return std::sqrt(DiagLengthSquared());
}

double GB_Rectangle::DiagLengthSquared() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const double dx = maxX - minX;
    const double dy = maxY - minY;
    const double value = dx * dx + dy * dy;
    return std::isfinite(value) ? value : GB_QuietNan;
}

GB_Point2d GB_Rectangle::MinPoint() const
{
    if (!IsValid())
    {
        return GB_Point2d();
    }

    return GB_Point2d(minX, minY);
}

GB_Point2d GB_Rectangle::MaxPoint() const
{
    if (!IsValid())
    {
        return GB_Point2d();
    }

    return GB_Point2d(maxX, maxY);
}

GB_Point2d GB_Rectangle::Center() const
{
    if (!IsValid())
    {
        return GB_Point2d();
    }

    const double cx = 0.5 * (minX + maxX);
    const double cy = 0.5 * (minY + maxY);
    return GB_Point2d(cx, cy);
}

std::vector<GB_Point2d> GB_Rectangle::GetCorners() const
{
    std::vector<GB_Point2d> corners;
    if (!IsValid())
    {
        return corners;
    }

    corners.reserve(4);
    corners.emplace_back(minX, minY);
    corners.emplace_back(maxX, minY);
    corners.emplace_back(maxX, maxY);
    corners.emplace_back(minX, maxY);
    return corners;
}

GB_Rectangle GB_Rectangle::Offsetted(double deltaX, double deltaY) const
{
    GB_Rectangle result = *this;
    result.Offset(deltaX, deltaY);
    return result;
}

void GB_Rectangle::Offset(double deltaX, double deltaY)
{
    if (!IsValid() || !std::isfinite(deltaX) || !std::isfinite(deltaY))
    {
        Reset();
        return;
    }

    minX += deltaX;
    maxX += deltaX;
    minY += deltaY;
    maxY += deltaY;

    if (!IsFinite4(minX, minY, maxX, maxY))
    {
        Reset();
    }
}

GB_Rectangle GB_Rectangle::Offsetted(const GB_Vector2d& translation) const
{
    GB_Rectangle result = *this;
    result.Offset(translation);
    return result;
}

void GB_Rectangle::Offset(const GB_Vector2d& translation)
{
    if (!translation.IsValid())
    {
        Reset();
        return;
    }

    Offset(translation.x, translation.y);
}

GB_Rectangle GB_Rectangle::Scaled(double scaleFactor) const
{
    return Scaled(scaleFactor, Center());
}

GB_Rectangle GB_Rectangle::Scaled(double scaleFactor, const GB_Point2d& center) const
{
    return Scaled(scaleFactor, scaleFactor, center);
}

GB_Rectangle GB_Rectangle::Scaled(double scaleX, double scaleY, const GB_Point2d& center) const
{
    if (!IsValid() || !center.IsValid() || !std::isfinite(scaleX) || !std::isfinite(scaleY))
    {
        return GB_Rectangle();
    }

    const double newMinX = center.x + (minX - center.x) * scaleX;
    const double newMaxX = center.x + (maxX - center.x) * scaleX;
    const double newMinY = center.y + (minY - center.y) * scaleY;
    const double newMaxY = center.y + (maxY - center.y) * scaleY;

    GB_Rectangle result;
    result.Set(newMinX, newMinY, newMaxX, newMaxY);
    return result;
}

void GB_Rectangle::Scale(double scaleFactor)
{
    *this = Scaled(scaleFactor);
}

void GB_Rectangle::Scale(double scaleFactor, const GB_Point2d& center)
{
    *this = Scaled(scaleFactor, center);
}

void GB_Rectangle::Scale(double scaleX, double scaleY, const GB_Point2d& center)
{
    *this = Scaled(scaleX, scaleY, center);
}

GB_Rectangle GB_Rectangle::Buffered(double delta) const
{
    return Buffered(delta, delta);
}

GB_Rectangle GB_Rectangle::Buffered(double deltaX, double deltaY) const
{
    if (!IsValid() || !std::isfinite(deltaX) || !std::isfinite(deltaY))
    {
        return GB_Rectangle();
    }

    const double newMinX = minX - deltaX;
    const double newMinY = minY - deltaY;
    const double newMaxX = maxX + deltaX;
    const double newMaxY = maxY + deltaY;

    if (!IsFinite4(newMinX, newMinY, newMaxX, newMaxY) || newMinX > newMaxX || newMinY > newMaxY)
    {
        return GB_Rectangle();
    }

    GB_Rectangle result;
    result.minX = newMinX;
    result.minY = newMinY;
    result.maxX = newMaxX;
    result.maxY = newMaxY;
    return result;
}

void GB_Rectangle::Buffer(double delta)
{
    Buffer(delta, delta);
}

void GB_Rectangle::Buffer(double deltaX, double deltaY)
{
    *this = Buffered(deltaX, deltaY);
}

void GB_Rectangle::Expand(const GB_Point2d& point)
{
    if (!point.IsValid())
    {
        return;
    }

    if (!IsValid())
    {
        SetFromPoint(point);
        return;
    }

    minX = std::min(minX, point.x);
    minY = std::min(minY, point.y);
    maxX = std::max(maxX, point.x);
    maxY = std::max(maxY, point.y);
}

void GB_Rectangle::Expand(const std::vector<GB_Point2d>& points)
{
    for (const GB_Point2d& point : points)
    {
        Expand(point);
    }
}

void GB_Rectangle::Expand(const GB_Rectangle& rect)
{
    if (!rect.IsValid())
    {
        return;
    }

    if (!IsValid())
    {
        *this = rect;
        return;
    }

    minX = std::min(minX, rect.minX);
    minY = std::min(minY, rect.minY);
    maxX = std::max(maxX, rect.maxX);
    maxY = std::max(maxY, rect.maxY);
}

void GB_Rectangle::Expand(const std::vector<GB_Rectangle>& rectangles)
{
    for (const GB_Rectangle& rect : rectangles)
    {
        Expand(rect);
    }
}

bool GB_Rectangle::IsIntersects(const GB_Rectangle& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double tol = AbsTol(tolerance);
    if (maxX < other.minX - tol)
    {
        return false;
    }
    if (other.maxX < minX - tol)
    {
        return false;
    }
    if (maxY < other.minY - tol)
    {
        return false;
    }
    if (other.maxY < minY - tol)
    {
        return false;
    }

    return true;
}

GB_Rectangle GB_Rectangle::Intersected(const GB_Rectangle& other, double tolerance) const
{
    if (!IsIntersects(other, tolerance))
    {
        return GB_Rectangle();
    }

    const double newMinX = std::max(minX, other.minX);
    const double newMinY = std::max(minY, other.minY);
    const double newMaxX = std::min(maxX, other.maxX);
    const double newMaxY = std::min(maxY, other.maxY);

    if (!IsFinite4(newMinX, newMinY, newMaxX, newMaxY) || newMinX > newMaxX || newMinY > newMaxY)
    {
        return GB_Rectangle();
    }

    GB_Rectangle result;
    result.minX = newMinX;
    result.minY = newMinY;
    result.maxX = newMaxX;
    result.maxY = newMaxY;
    return result;
}

bool GB_Rectangle::IsContains(const GB_Point2d& point, double tolerance) const
{
    if (!IsValid() || !point.IsValid())
    {
        return false;
    }

    const double tol = AbsTol(tolerance);
    return point.x >= minX - tol && point.x <= maxX + tol
        && point.y >= minY - tol && point.y <= maxY + tol;
}

bool GB_Rectangle::IsContains(const GB_Rectangle& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double tol = AbsTol(tolerance);
    return other.minX >= minX - tol && other.maxX <= maxX + tol
        && other.minY >= minY - tol && other.maxY <= maxY + tol;
}

double GB_Rectangle::DistanceTo(const GB_Point2d& point) const
{
    return std::sqrt(DistanceToSquared(point));
}

double GB_Rectangle::DistanceToSquared(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return GB_QuietNan;
    }

    double dx = 0;
    if (point.x < minX)
    {
        dx = minX - point.x;
    }
    else if (point.x > maxX)
    {
        dx = point.x - maxX;
    }

    double dy = 0;
    if (point.y < minY)
    {
        dy = minY - point.y;
    }
    else if (point.y > maxY)
    {
        dy = point.y - maxY;
    }

    const double value = dx * dx + dy * dy;
    return std::isfinite(value) ? value : GB_QuietNan;
}

GB_Point2d GB_Rectangle::ClampPoint(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return GB_Point2d();
    }

    const double clampedX = GB_Clamp(point.x, minX, maxX);
    const double clampedY = GB_Clamp(point.y, minY, maxY);
    return GB_Point2d(clampedX, clampedY);
}

GB_Rectangle GB_Rectangle::Transformed(const GB_Matrix3x3& mat) const
{
    if (!IsValid() || !mat.IsValid())
    {
        return GB_Rectangle();
    }

    const std::vector<GB_Point2d> corners = GetCorners();
    GB_Rectangle bounds;
    for (const GB_Point2d& corner : corners)
    {
        const GB_Point2d transformed = mat.TransformPoint(corner);
        bounds.Expand(transformed);
    }

    return bounds;
}

void GB_Rectangle::Transform(const GB_Matrix3x3& mat)
{
    *this = Transformed(mat);
}

bool GB_Rectangle::operator==(const GB_Rectangle& other) const
{
    return minX == other.minX && minY == other.minY && maxX == other.maxX && maxY == other.maxY;
}

bool GB_Rectangle::operator!=(const GB_Rectangle& other) const
{
    return !(*this == other);
}

bool GB_Rectangle::IsNearEqual(const GB_Rectangle& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double tol = AbsTol(tolerance);
    return std::abs(minX - other.minX) <= tol
        && std::abs(minY - other.minY) <= tol
        && std::abs(maxX - other.maxX) <= tol
        && std::abs(maxY - other.maxY) <= tol;
}

std::string GB_Rectangle::SerializeToString() const
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << "(" << GetClassType() << " " << std::setprecision(17)
        << minX << "," << minY << "," << maxX << "," << maxY << ")";
    return oss.str();
}

GB_ByteBuffer GB_Rectangle::SerializeToBinary() const
{
    constexpr static uint16_t payloadVersion = 1;

    GB_ByteBuffer buffer;
    buffer.reserve(48);

    GB_ByteBufferIO::AppendUInt32LE(buffer, GB_ClassMagicNumber);
    GB_ByteBufferIO::AppendUInt64LE(buffer, GetClassTypeId());
    GB_ByteBufferIO::AppendUInt16LE(buffer, payloadVersion);
    GB_ByteBufferIO::AppendUInt16LE(buffer, 0);

    GB_ByteBufferIO::AppendDoubleLE(buffer, minX);
    GB_ByteBufferIO::AppendDoubleLE(buffer, minY);
    GB_ByteBufferIO::AppendDoubleLE(buffer, maxX);
    GB_ByteBufferIO::AppendDoubleLE(buffer, maxY);

    return buffer;
}

bool GB_Rectangle::Deserialize(const std::string& data)
{
    std::istringstream iss(data);
    iss.imbue(std::locale::classic());

    char leftParen = 0;
    std::string type = "";
    char comma1 = 0;
    char comma2 = 0;
    char comma3 = 0;
    char rightParen = 0;

    double parsedMinX = GB_QuietNan;
    double parsedMinY = GB_QuietNan;
    double parsedMaxX = GB_QuietNan;
    double parsedMaxY = GB_QuietNan;

    if (!(iss >> leftParen >> type >> parsedMinX >> comma1 >> parsedMinY >> comma2 >> parsedMaxX >> comma3 >> parsedMaxY >> rightParen))
    {
        Reset();
        return false;
    }

    if (leftParen != '(' || rightParen != ')' || comma1 != ',' || comma2 != ',' || comma3 != ',' || type != GetClassType())
    {
        Reset();
        return false;
    }

    minX = parsedMinX;
    minY = parsedMinY;
    maxX = parsedMaxX;
    maxY = parsedMaxY;

    Normalize();
    return true;
}

bool GB_Rectangle::Deserialize(const GB_ByteBuffer& data)
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

    double parsedMinX = GB_QuietNan;
    double parsedMinY = GB_QuietNan;
    double parsedMaxX = GB_QuietNan;
    double parsedMaxY = GB_QuietNan;

    if (!GB_ByteBufferIO::ReadUInt32LE(data, offset, magic)
        || !GB_ByteBufferIO::ReadUInt64LE(data, offset, typeId)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, payloadVersion)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, reserved)
        || !GB_ByteBufferIO::ReadDoubleLE(data, offset, parsedMinX)
        || !GB_ByteBufferIO::ReadDoubleLE(data, offset, parsedMinY)
        || !GB_ByteBufferIO::ReadDoubleLE(data, offset, parsedMaxX)
        || !GB_ByteBufferIO::ReadDoubleLE(data, offset, parsedMaxY))
    {
        Reset();
        return false;
    }

    if (magic != GB_ClassMagicNumber || typeId != GetClassTypeId() || payloadVersion != expectedPayloadVersion)
    {
        Reset();
        return false;
    }

    minX = parsedMinX;
    minY = parsedMinY;
    maxX = parsedMaxX;
    maxY = parsedMaxY;

    Normalize();
    return true;
}
