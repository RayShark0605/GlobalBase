#include "GB_Polygon.h"

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4819)
#endif

#include <CGAL/Cartesian.h>
#include <CGAL/Polygon_2_algorithms.h>
#include <CGAL/boost_mp.h>

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using ExactNumber = boost::multiprecision::cpp_rational;
    using ExactInteger = boost::multiprecision::cpp_int;
    using ExactKernel = CGAL::Cartesian<ExactNumber>;
    using ExactPoint = ExactKernel::Point_2;

    static inline std::string TrimAscii(const std::string& text)
    {
        size_t beginIndex = 0;
        while (beginIndex < text.size() && std::isspace(static_cast<unsigned char>(text[beginIndex])) != 0)
        {
            beginIndex++;
        }

        size_t endIndex = text.size();
        while (endIndex > beginIndex && std::isspace(static_cast<unsigned char>(text[endIndex - 1])) != 0)
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    static inline std::string ToLowerAscii(const std::string& text)
    {
        std::string result = text;
        for (char& ch : result)
        {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return result;
    }

    static inline std::string DoubleToString(double value)
    {
        std::ostringstream oss;
        oss.imbue(std::locale::classic());
        oss << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
        return oss.str();
    }

    static bool TryParseDoubleText(const std::string& text, double& outValue)
    {
        const std::string trimmed = TrimAscii(text);
        if (trimmed.empty())
        {
            outValue = GB_QuietNan;
            return false;
        }

        std::istringstream iss(trimmed);
        iss.imbue(std::locale::classic());

        double value = GB_QuietNan;
        char tail = 0;
        if (!(iss >> value) || (iss >> tail))
        {
            outValue = GB_QuietNan;
            return false;
        }

        if (!std::isfinite(value))
        {
            outValue = GB_QuietNan;
            return false;
        }

        outValue = value;
        return true;
    }

    static bool TryParseSignedInteger64(const std::string& text, std::int64_t& outValue)
    {
        const std::string trimmed = TrimAscii(text);
        if (trimmed.empty())
        {
            outValue = 0;
            return false;
        }

        try
        {
            size_t parsedLength = 0;
            const long long value = std::stoll(trimmed, &parsedLength, 10);
            if (parsedLength != trimmed.size())
            {
                outValue = 0;
                return false;
            }
            outValue = static_cast<std::int64_t>(value);
            return true;
        }
        catch (...)
        {
            outValue = 0;
            return false;
        }
    }

    static bool TryParseSizeT(const std::string& text, size_t& outValue)
    {
        const std::string trimmed = TrimAscii(text);
        if (trimmed.empty())
        {
            outValue = 0;
            return false;
        }

        try
        {
            size_t parsedLength = 0;
            const unsigned long long value = std::stoull(trimmed, &parsedLength, 10);
            if (parsedLength != trimmed.size())
            {
                outValue = 0;
                return false;
            }
            outValue = static_cast<size_t>(value);
            return true;
        }
        catch (...)
        {
            outValue = 0;
            return false;
        }
    }

    static ExactInteger Pow10Exact(size_t exponent)
    {
        ExactInteger result = 1;
        ExactInteger base = 10;
        size_t currentExponent = exponent;

        while (currentExponent > 0)
        {
            if ((currentExponent & 1u) != 0u)
            {
                result *= base;
            }

            currentExponent >>= 1u;
            if (currentExponent > 0)
            {
                base *= base;
            }
        }

        return result;
    }

    static bool TryParseUnsignedDecimalDigits(const std::string& digits, ExactInteger& outInteger)
    {
        outInteger = 0;
        if (digits.empty())
        {
            return false;
        }

        for (char ch : digits)
        {
            if (ch < '0' || ch > '9')
            {
                outInteger = 0;
                return false;
            }

            outInteger *= 10;
            outInteger += static_cast<unsigned int>(ch - '0');
        }

        return true;
    }

    static bool TryParseFiniteDecimalOrScientific(const std::string& text, ExactNumber& outValue)
    {
        const std::string trimmed = TrimAscii(text);
        if (trimmed.empty())
        {
            outValue = ExactNumber(0);
            return false;
        }

        const std::string lowered = ToLowerAscii(trimmed);
        if (lowered == "nan" || lowered == "+nan" || lowered == "-nan"
            || lowered == "inf" || lowered == "+inf" || lowered == "-inf"
            || lowered == "infinity" || lowered == "+infinity" || lowered == "-infinity")
        {
            outValue = ExactNumber(0);
            return false;
        }

        size_t currentIndex = 0;
        bool isNegative = false;
        if (trimmed[currentIndex] == '+' || trimmed[currentIndex] == '-')
        {
            isNegative = (trimmed[currentIndex] == '-');
            currentIndex++;
        }

        if (currentIndex >= trimmed.size())
        {
            outValue = ExactNumber(0);
            return false;
        }

        size_t exponentPos = std::string::npos;
        for (size_t i = currentIndex; i < trimmed.size(); i++)
        {
            const char ch = trimmed[i];
            if (ch == 'e' || ch == 'E')
            {
                exponentPos = i;
                break;
            }
        }

        const std::string significand = (exponentPos == std::string::npos)
            ? trimmed.substr(currentIndex)
            : trimmed.substr(currentIndex, exponentPos - currentIndex);

        std::int64_t exponent10 = 0;
        if (exponentPos != std::string::npos)
        {
            if (!TryParseSignedInteger64(trimmed.substr(exponentPos + 1), exponent10))
            {
                outValue = ExactNumber(0);
                return false;
            }
        }

        std::string digits;
        digits.reserve(significand.size());

        bool hasDot = false;
        bool hasDigit = false;
        size_t fracDigits = 0;
        for (char ch : significand)
        {
            if (ch >= '0' && ch <= '9')
            {
                digits.push_back(ch);
                hasDigit = true;
                if (hasDot)
                {
                    fracDigits++;
                }
            }
            else if (ch == '.')
            {
                if (hasDot)
                {
                    outValue = ExactNumber(0);
                    return false;
                }
                hasDot = true;
            }
            else
            {
                outValue = ExactNumber(0);
                return false;
            }
        }

        if (!hasDigit)
        {
            outValue = ExactNumber(0);
            return false;
        }

        size_t firstNonZeroIndex = 0;
        while (firstNonZeroIndex < digits.size() && digits[firstNonZeroIndex] == '0')
        {
            firstNonZeroIndex++;
        }

        if (firstNonZeroIndex == digits.size())
        {
            outValue = ExactNumber(0);
            return true;
        }

        digits.erase(0, firstNonZeroIndex);

        ExactInteger integerValue = 0;
        if (!TryParseUnsignedDecimalDigits(digits, integerValue))
        {
            outValue = ExactNumber(0);
            return false;
        }

        const std::int64_t netExponent10 = exponent10 - static_cast<std::int64_t>(fracDigits);

        ExactInteger numerator = integerValue;
        ExactInteger denominator = 1;
        if (netExponent10 >= 0)
        {
            numerator *= Pow10Exact(static_cast<size_t>(netExponent10));
        }
        else
        {
            denominator = Pow10Exact(static_cast<size_t>(-netExponent10));
        }

        if (isNegative)
        {
            numerator = -numerator;
        }

        try
        {
            outValue = ExactNumber(numerator);
            if (denominator != 1)
            {
                outValue /= ExactNumber(denominator);
            }
            return true;
        }
        catch (...)
        {
            outValue = ExactNumber(0);
            return false;
        }
    }

    static bool TryParseExactNumber(const std::string& text, ExactNumber& outValue)
    {
        const std::string trimmed = TrimAscii(text);
        if (trimmed.empty())
        {
            outValue = ExactNumber(0);
            return false;
        }

        const size_t slashPos = trimmed.find('/');
        if (slashPos == std::string::npos)
        {
            return TryParseFiniteDecimalOrScientific(trimmed, outValue);
        }

        if (trimmed.find('/', slashPos + 1) != std::string::npos)
        {
            outValue = ExactNumber(0);
            return false;
        }

        ExactNumber numerator = ExactNumber(0);
        ExactNumber denominator = ExactNumber(0);
        if (!TryParseFiniteDecimalOrScientific(trimmed.substr(0, slashPos), numerator)
            || !TryParseFiniteDecimalOrScientific(trimmed.substr(slashPos + 1), denominator))
        {
            outValue = ExactNumber(0);
            return false;
        }

        if (denominator == ExactNumber(0))
        {
            outValue = ExactNumber(0);
            return false;
        }

        try
        {
            outValue = numerator / denominator;
            return true;
        }
        catch (...)
        {
            outValue = ExactNumber(0);
            return false;
        }
    }

    static bool TryConvertDoubleToExactNumber(double value, ExactNumber& outValue)
    {
        if (!std::isfinite(value))
        {
            outValue = ExactNumber(0);
            return false;
        }

        if (value == 0.0)
        {
            outValue = ExactNumber(0);
            return true;
        }

        if (std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::radix == 2 && sizeof(double) == sizeof(std::uint64_t))
        {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(double));

            const bool isNegative = ((bits >> 63u) != 0u);
            const std::uint64_t exponentBits = (bits >> 52u) & 0x7ffu;
            const std::uint64_t fractionBits = bits & 0x000fffffffffffffull;

            std::int64_t exponent2 = 0;
            std::uint64_t mantissa = 0;
            if (exponentBits == 0)
            {
                mantissa = fractionBits;
                exponent2 = -1074;
            }
            else
            {
                mantissa = fractionBits | (1ull << 52u);
                exponent2 = static_cast<std::int64_t>(exponentBits) - 1023 - 52;
            }

            ExactInteger numerator = mantissa;
            ExactInteger denominator = 1;
            if (exponent2 >= 0)
            {
                numerator <<= static_cast<unsigned int>(exponent2);
            }
            else
            {
                denominator <<= static_cast<unsigned int>(-exponent2);
            }

            if (isNegative)
            {
                numerator = -numerator;
            }

            outValue = ExactNumber(numerator);
            if (denominator != 1)
            {
                outValue /= ExactNumber(denominator);
            }
            return true;
        }

        return TryParseExactNumber(DoubleToString(value), outValue);
    }

    static bool TryConvertExactNumberToDouble(const ExactNumber& value, double& outValue)
    {
        try
        {
            const double converted = value.convert_to<double>();
            if (!std::isfinite(converted))
            {
                outValue = GB_QuietNan;
                return false;
            }
            outValue = converted;
            return true;
        }
        catch (...)
        {
            outValue = GB_QuietNan;
            return false;
        }
    }

    static std::string ExactNumberToString(const ExactNumber& value)
    {
        std::ostringstream oss;
        oss.imbue(std::locale::classic());
        oss << value;
        return oss.str();
    }

    static bool TryBuildExactPoints(const GB_Polygon& polygon, std::vector<ExactPoint>& outPoints)
    {
        outPoints.clear();
        const size_t numVertices = polygon.GetNumVertices();
        outPoints.reserve(numVertices);

        if (polygon.GetCoordinateStorageMode() == GB_Polygon::CoordinateStorageMode::Double)
        {
            const std::vector<GB_Point2d>& vertices = polygon.GetDoubleVertices();
            if (vertices.size() != numVertices)
            {
                return false;
            }

            for (const GB_Point2d& vertex : vertices)
            {
                ExactNumber x = ExactNumber(0);
                ExactNumber y = ExactNumber(0);
                if (!TryConvertDoubleToExactNumber(vertex.x, x) || !TryConvertDoubleToExactNumber(vertex.y, y))
                {
                    outPoints.clear();
                    return false;
                }
                outPoints.emplace_back(x, y);
            }
            return true;
        }

        const std::vector<GB_Polygon::ExactStringVertex>& exactVertices = polygon.GetExactStringVertices();
        if (exactVertices.size() != numVertices)
        {
            return false;
        }

        for (const GB_Polygon::ExactStringVertex& vertex : exactVertices)
        {
            ExactNumber x = ExactNumber(0);
            ExactNumber y = ExactNumber(0);
            if (!TryParseExactNumber(vertex.first, x) || !TryParseExactNumber(vertex.second, y))
            {
                outPoints.clear();
                return false;
            }
            outPoints.emplace_back(x, y);
        }

        return true;
    }

    static bool TryBuildExactPoint(const GB_Point2d& point, ExactPoint& outPoint)
    {
        ExactNumber x = ExactNumber(0);
        ExactNumber y = ExactNumber(0);
        if (!TryConvertDoubleToExactNumber(point.x, x) || !TryConvertDoubleToExactNumber(point.y, y))
        {
            return false;
        }
        outPoint = ExactPoint(x, y);
        return true;
    }

    static bool TryBuildExactPoint(const std::string& xText, const std::string& yText, ExactPoint& outPoint)
    {
        ExactNumber x = ExactNumber(0);
        ExactNumber y = ExactNumber(0);
        if (!TryParseExactNumber(xText, x) || !TryParseExactNumber(yText, y))
        {
            return false;
        }
        outPoint = ExactPoint(x, y);
        return true;
    }

    static inline ExactNumber Cross(const ExactPoint& origin, const ExactPoint& a, const ExactPoint& b)
    {
        const ExactNumber ax = a.x() - origin.x();
        const ExactNumber ay = a.y() - origin.y();
        const ExactNumber bx = b.x() - origin.x();
        const ExactNumber by = b.y() - origin.y();
        return ax * by - ay * bx;
    }

    static bool IsPointOnSegment(const ExactPoint& point, const ExactPoint& a, const ExactPoint& b)
    {
        if (Cross(a, b, point) != ExactNumber(0))
        {
            return false;
        }

        const ExactNumber minX = std::min(a.x(), b.x());
        const ExactNumber maxX = std::max(a.x(), b.x());
        const ExactNumber minY = std::min(a.y(), b.y());
        const ExactNumber maxY = std::max(a.y(), b.y());
        return point.x() >= minX && point.x() <= maxX && point.y() >= minY && point.y() <= maxY;
    }

    static ExactNumber ComputeSignedDoubleAreaTwice(const std::vector<ExactPoint>& points)
    {
        const size_t numVertices = points.size();
        if (numVertices < 2)
        {
            return ExactNumber(0);
        }

        ExactNumber areaTwice = ExactNumber(0);
        for (size_t i = 0; i < numVertices; i++)
        {
            const size_t nextIndex = (i + 1) % numVertices;
            areaTwice += points[i].x() * points[nextIndex].y() - points[nextIndex].x() * points[i].y();
        }
        return areaTwice;
    }

    static bool AreAllPointsCollinear(const std::vector<ExactPoint>& points)
    {
        const size_t numVertices = points.size();
        if (numVertices <= 2)
        {
            return true;
        }

        size_t firstIndex = 0;
        size_t secondIndex = 1;
        while (secondIndex < numVertices && points[secondIndex] == points[firstIndex])
        {
            secondIndex++;
        }

        if (secondIndex >= numVertices)
        {
            return true;
        }

        for (size_t i = secondIndex + 1; i < numVertices; i++)
        {
            if (Cross(points[firstIndex], points[secondIndex], points[i]) != ExactNumber(0))
            {
                return false;
            }
        }

        return true;
    }

    static std::vector<std::string> SplitByChar(const std::string& text, char delimiter)
    {
        std::vector<std::string> tokens;
        std::string currentToken;
        for (char ch : text)
        {
            if (ch == delimiter)
            {
                tokens.push_back(currentToken);
                currentToken.clear();
            }
            else
            {
                currentToken.push_back(ch);
            }
        }
        tokens.push_back(currentToken);
        return tokens;
    }

    static void AppendUInt8(GB_ByteBuffer& buffer, std::uint8_t value)
    {
        buffer.push_back(value);
    }

    static void AppendUInt16LE(GB_ByteBuffer& buffer, std::uint16_t value)
    {
        buffer.push_back(static_cast<unsigned char>(value & 0xffu));
        buffer.push_back(static_cast<unsigned char>((value >> 8u) & 0xffu));
    }

    static void AppendUInt32LE(GB_ByteBuffer& buffer, std::uint32_t value)
    {
        for (int i = 0; i < 4; i++)
        {
            buffer.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xffu));
        }
    }

    static void AppendUInt64LE(GB_ByteBuffer& buffer, std::uint64_t value)
    {
        for (int i = 0; i < 8; i++)
        {
            buffer.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xffu));
        }
    }

    static void AppendDoubleLE(GB_ByteBuffer& buffer, double value)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(double));
        AppendUInt64LE(buffer, bits);
    }

    static void AppendString(GB_ByteBuffer& buffer, const std::string& text)
    {
        AppendUInt64LE(buffer, static_cast<std::uint64_t>(text.size()));
        buffer.insert(buffer.end(), text.begin(), text.end());
    }

    static bool ReadUInt8(const GB_ByteBuffer& buffer, size_t& offset, std::uint8_t& outValue)
    {
        if (offset + 1 > buffer.size())
        {
            outValue = 0;
            return false;
        }
        outValue = static_cast<std::uint8_t>(buffer[offset]);
        offset += 1;
        return true;
    }

    static bool ReadUInt16LE(const GB_ByteBuffer& buffer, size_t& offset, std::uint16_t& outValue)
    {
        if (offset + 2 > buffer.size())
        {
            outValue = 0;
            return false;
        }

        outValue = static_cast<std::uint16_t>(buffer[offset])
            | (static_cast<std::uint16_t>(buffer[offset + 1]) << 8u);
        offset += 2;
        return true;
    }

    static bool ReadUInt32LE(const GB_ByteBuffer& buffer, size_t& offset, std::uint32_t& outValue)
    {
        if (offset + 4 > buffer.size())
        {
            outValue = 0;
            return false;
        }

        outValue = 0;
        for (int i = 0; i < 4; i++)
        {
            outValue |= (static_cast<std::uint32_t>(buffer[offset + i]) << (8u * i));
        }
        offset += 4;
        return true;
    }

    static bool ReadUInt64LE(const GB_ByteBuffer& buffer, size_t& offset, std::uint64_t& outValue)
    {
        if (offset + 8 > buffer.size())
        {
            outValue = 0;
            return false;
        }

        outValue = 0;
        for (int i = 0; i < 8; i++)
        {
            outValue |= (static_cast<std::uint64_t>(buffer[offset + i]) << (8u * i));
        }
        offset += 8;
        return true;
    }

    static bool ReadDoubleLE(const GB_ByteBuffer& buffer, size_t& offset, double& outValue)
    {
        std::uint64_t bits = 0;
        if (!ReadUInt64LE(buffer, offset, bits))
        {
            outValue = GB_QuietNan;
            return false;
        }

        std::memcpy(&outValue, &bits, sizeof(double));
        return true;
    }

    static bool ReadString(const GB_ByteBuffer& buffer, size_t& offset, std::string& outText)
    {
        std::uint64_t length = 0;
        if (!ReadUInt64LE(buffer, offset, length))
        {
            outText.clear();
            return false;
        }

        if (length > static_cast<std::uint64_t>(buffer.size() - offset))
        {
            outText.clear();
            return false;
        }

        outText.assign(reinterpret_cast<const char*>(buffer.data() + offset), static_cast<size_t>(length));
        offset += static_cast<size_t>(length);
        return true;
    }
}

const GB_Polygon GB_Polygon::Invalid = GB_Polygon();

GB_Polygon::GB_Polygon()
{
}

GB_Polygon::GB_Polygon(const std::vector<GB_Point2d>& vertices)
{
    SetVertices(vertices);
}

GB_Polygon::GB_Polygon(std::initializer_list<GB_Point2d> vertices)
{
    SetVertices(std::vector<GB_Point2d>(vertices));
}

GB_Polygon::GB_Polygon(const std::vector<ExactStringVertex>& exactStringVertices)
{
    SetVertices(exactStringVertices);
}

GB_Polygon::GB_Polygon(std::initializer_list<ExactStringVertex> exactStringVertices)
{
    SetVertices(std::vector<ExactStringVertex>(exactStringVertices));
}

GB_Polygon::~GB_Polygon()
{
}

GB_Polygon::GB_Polygon(const GB_Polygon& other) = default;
GB_Polygon::GB_Polygon(GB_Polygon&& other) noexcept = default;
GB_Polygon& GB_Polygon::operator=(const GB_Polygon& other) = default;
GB_Polygon& GB_Polygon::operator=(GB_Polygon&& other) noexcept = default;

const std::string& GB_Polygon::GetClassType() const
{
    static const std::string classType = "GB_Polygon";
    return classType;
}

uint64_t GB_Polygon::GetClassTypeId() const
{
    static const uint64_t classTypeId = GB_GenerateClassTypeId(GetClassType());
    return classTypeId;
}

void GB_Polygon::Clear()
{
    storageMode = CoordinateStorageMode::Double;
    vertices.clear();
    exactStringVertices.clear();
}

bool GB_Polygon::SetVertices(const std::vector<GB_Point2d>& vertices)
{
    Clear();
    storageMode = CoordinateStorageMode::Double;
    this->vertices = vertices;

    for (const GB_Point2d& vertex : this->vertices)
    {
        if (!vertex.IsValid())
        {
            Clear();
            return false;
        }
    }

    return this->vertices.size() >= 2;
}

bool GB_Polygon::SetVertices(const std::vector<ExactStringVertex>& exactStringVertices)
{
    Clear();
    storageMode = CoordinateStorageMode::ExactString;
    this->exactStringVertices.reserve(exactStringVertices.size());

    for (const ExactStringVertex& vertex : exactStringVertices)
    {
        const std::string xText = TrimAscii(vertex.first);
        const std::string yText = TrimAscii(vertex.second);

        ExactNumber x = ExactNumber(0);
        ExactNumber y = ExactNumber(0);
        if (!TryParseExactNumber(xText, x) || !TryParseExactNumber(yText, y))
        {
            Clear();
            return false;
        }

        this->exactStringVertices.emplace_back(xText, yText);
    }

    return this->exactStringVertices.size() >= 2;
}

GB_Polygon::CoordinateStorageMode GB_Polygon::GetCoordinateStorageMode() const
{
    return storageMode;
}

bool GB_Polygon::UsesExactStringCoordinates() const
{
    return storageMode == CoordinateStorageMode::ExactString;
}

bool GB_Polygon::IsEmpty() const
{
    return GetNumVertices() == 0;
}

bool GB_Polygon::IsValid() const
{
    const size_t numVertices = GetNumVertices();
    if (numVertices < 2)
    {
        return false;
    }

    if (storageMode == CoordinateStorageMode::Double)
    {
        if (vertices.size() != numVertices)
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

    if (exactStringVertices.size() != numVertices)
    {
        return false;
    }

    for (const ExactStringVertex& vertex : exactStringVertices)
    {
        ExactNumber x = ExactNumber(0);
        ExactNumber y = ExactNumber(0);
        if (!TryParseExactNumber(vertex.first, x) || !TryParseExactNumber(vertex.second, y))
        {
            return false;
        }
    }

    return true;
}

bool GB_Polygon::IsClosed() const
{
    return IsValid();
}

size_t GB_Polygon::GetNumVertices() const
{
    if (storageMode == CoordinateStorageMode::Double)
    {
        return vertices.size();
    }
    return exactStringVertices.size();
}

size_t GB_Polygon::GetNumEdges() const
{
    return GetNumVertices();
}

const std::vector<GB_Point2d>& GB_Polygon::GetDoubleVertices() const
{
    return vertices;
}

const std::vector<GB_Polygon::ExactStringVertex>& GB_Polygon::GetExactStringVertices() const
{
    return exactStringVertices;
}

std::vector<GB_Point2d> GB_Polygon::GetVerticesAsDouble() const
{
    std::vector<GB_Point2d> result;
    const size_t numVertices = GetNumVertices();
    result.reserve(numVertices);

    if (storageMode == CoordinateStorageMode::Double)
    {
        result = vertices;
        return result;
    }

    for (const ExactStringVertex& vertex : exactStringVertices)
    {
        ExactNumber x = ExactNumber(0);
        ExactNumber y = ExactNumber(0);
        double xDouble = GB_QuietNan;
        double yDouble = GB_QuietNan;

        if (TryParseExactNumber(vertex.first, x) && TryParseExactNumber(vertex.second, y)
            && TryConvertExactNumberToDouble(x, xDouble) && TryConvertExactNumberToDouble(y, yDouble))
        {
            result.emplace_back(xDouble, yDouble);
        }
        else
        {
            result.emplace_back(GB_QuietNan, GB_QuietNan);
        }
    }

    return result;
}

bool GB_Polygon::TryGetVertexAsDouble(size_t index, GB_Point2d& outVertex) const
{
    outVertex = GB_Point2d();
    if (index >= GetNumVertices())
    {
        return false;
    }

    if (storageMode == CoordinateStorageMode::Double)
    {
        outVertex = vertices[index];
        return outVertex.IsValid();
    }

    ExactNumber x = ExactNumber(0);
    ExactNumber y = ExactNumber(0);
    double xDouble = GB_QuietNan;
    double yDouble = GB_QuietNan;
    if (!TryParseExactNumber(exactStringVertices[index].first, x)
        || !TryParseExactNumber(exactStringVertices[index].second, y)
        || !TryConvertExactNumberToDouble(x, xDouble)
        || !TryConvertExactNumberToDouble(y, yDouble))
    {
        return false;
    }

    outVertex = GB_Point2d(xDouble, yDouble);
    return true;
}

bool GB_Polygon::TryGetVertexExactStrings(size_t index, std::string& outXText, std::string& outYText) const
{
    outXText.clear();
    outYText.clear();
    if (index >= GetNumVertices())
    {
        return false;
    }

    if (storageMode == CoordinateStorageMode::ExactString)
    {
        outXText = exactStringVertices[index].first;
        outYText = exactStringVertices[index].second;
        return true;
    }

    outXText = DoubleToString(vertices[index].x);
    outYText = DoubleToString(vertices[index].y);
    return true;
}

GB_Rectangle GB_Polygon::GetBoundingBox() const
{
    GB_Rectangle bounds;
    if (!IsValid())
    {
        return bounds;
    }

    if (storageMode == CoordinateStorageMode::Double)
    {
        bounds.Expand(vertices);
        return bounds;
    }

    for (const ExactStringVertex& vertex : exactStringVertices)
    {
        ExactNumber x = ExactNumber(0);
        ExactNumber y = ExactNumber(0);
        double xDouble = GB_QuietNan;
        double yDouble = GB_QuietNan;
        if (!TryParseExactNumber(vertex.first, x)
            || !TryParseExactNumber(vertex.second, y)
            || !TryConvertExactNumberToDouble(x, xDouble)
            || !TryConvertExactNumberToDouble(y, yDouble))
        {
            return GB_Rectangle();
        }

        bounds.Expand(GB_Point2d(xDouble, yDouble));
    }

    return bounds;
}

bool GB_Polygon::TryGetBoundingBoxExactStrings(std::string& outMinXText, std::string& outMinYText, std::string& outMaxXText, std::string& outMaxYText) const
{
    outMinXText.clear();
    outMinYText.clear();
    outMaxXText.clear();
    outMaxYText.clear();

    std::vector<ExactPoint> points;
    if (!TryBuildExactPoints(*this, points) || points.empty())
    {
        return false;
    }

    size_t minXIndex = 0;
    size_t minYIndex = 0;
    size_t maxXIndex = 0;
    size_t maxYIndex = 0;
    for (size_t i = 1; i < points.size(); i++)
    {
        if (points[i].x() < points[minXIndex].x())
        {
            minXIndex = i;
        }
        if (points[i].y() < points[minYIndex].y())
        {
            minYIndex = i;
        }
        if (points[i].x() > points[maxXIndex].x())
        {
            maxXIndex = i;
        }
        if (points[i].y() > points[maxYIndex].y())
        {
            maxYIndex = i;
        }
    }

    if (storageMode == CoordinateStorageMode::ExactString)
    {
        outMinXText = exactStringVertices[minXIndex].first;
        outMinYText = exactStringVertices[minYIndex].second;
        outMaxXText = exactStringVertices[maxXIndex].first;
        outMaxYText = exactStringVertices[maxYIndex].second;
        return true;
    }

    outMinXText = DoubleToString(vertices[minXIndex].x);
    outMinYText = DoubleToString(vertices[minYIndex].y);
    outMaxXText = DoubleToString(vertices[maxXIndex].x);
    outMaxYText = DoubleToString(vertices[maxYIndex].y);
    return true;
}

bool GB_Polygon::HasDuplicateAdjacentVertices() const
{
    std::vector<ExactPoint> points;
    if (!TryBuildExactPoints(*this, points) || points.size() < 2)
    {
        return false;
    }

    for (size_t i = 0; i < points.size(); i++)
    {
        const size_t nextIndex = (i + 1) % points.size();
        if (points[i] == points[nextIndex])
        {
            return true;
        }
    }

    return false;
}

bool GB_Polygon::HasZeroLengthEdges() const
{
    return HasDuplicateAdjacentVertices();
}

bool GB_Polygon::HasCollinearAdjacentTriples() const
{
    std::vector<ExactPoint> points;
    if (!TryBuildExactPoints(*this, points) || points.size() < 3)
    {
        return false;
    }

    for (size_t i = 0; i < points.size(); i++)
    {
        const size_t prevIndex = (i + points.size() - 1) % points.size();
        const size_t nextIndex = (i + 1) % points.size();
        if (Cross(points[prevIndex], points[i], points[nextIndex]) == ExactNumber(0))
        {
            return true;
        }
    }

    return false;
}

bool GB_Polygon::AreAllVerticesCollinear() const
{
    std::vector<ExactPoint> points;
    if (!TryBuildExactPoints(*this, points))
    {
        return false;
    }
    return AreAllPointsCollinear(points);
}

bool GB_Polygon::IsSimple() const
{
    std::vector<ExactPoint> points;
    if (!TryBuildExactPoints(*this, points) || points.size() < 3)
    {
        return false;
    }

    return CGAL::is_simple_2(points.begin(), points.end(), ExactKernel());
}

bool GB_Polygon::HasSelfIntersections() const
{
    if (!IsValid() || GetNumVertices() < 3)
    {
        return false;
    }
    return !IsSimple();
}

GB_Polygon::Orientation GB_Polygon::GetOrientation() const
{
    std::vector<ExactPoint> points;
    if (!TryBuildExactPoints(*this, points) || points.size() < 2)
    {
        return Orientation::Degenerate;
    }

    const ExactNumber areaTwice = ComputeSignedDoubleAreaTwice(points);
    if (areaTwice > ExactNumber(0))
    {
        return Orientation::CounterClockwise;
    }
    if (areaTwice < ExactNumber(0))
    {
        return Orientation::Clockwise;
    }
    return Orientation::Degenerate;
}

bool GB_Polygon::IsClockwise() const
{
    return GetOrientation() == Orientation::Clockwise;
}

bool GB_Polygon::IsCounterClockwise() const
{
    return GetOrientation() == Orientation::CounterClockwise;
}

double GB_Polygon::GetSignedArea() const
{
    std::vector<ExactPoint> points;
    if (!TryBuildExactPoints(*this, points))
    {
        return GB_QuietNan;
    }

    const ExactNumber area = ComputeSignedDoubleAreaTwice(points) / ExactNumber(2);
    double areaDouble = GB_QuietNan;
    if (!TryConvertExactNumberToDouble(area, areaDouble))
    {
        return GB_QuietNan;
    }
    return areaDouble;
}

double GB_Polygon::GetUnsignedArea() const
{
    const double signedArea = GetSignedArea();
    return std::isfinite(signedArea) ? std::abs(signedArea) : GB_QuietNan;
}

std::string GB_Polygon::GetSignedAreaExactString() const
{
    std::vector<ExactPoint> points;
    if (!TryBuildExactPoints(*this, points))
    {
        return "";
    }

    const ExactNumber area = ComputeSignedDoubleAreaTwice(points) / ExactNumber(2);
    return ExactNumberToString(area);
}

double GB_Polygon::GetPerimeter() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const std::vector<GB_Point2d> approxVertices = GetVerticesAsDouble();
    if (approxVertices.size() < 2)
    {
        return GB_QuietNan;
    }

    long double perimeter = 0.0L;
    for (size_t i = 0; i < approxVertices.size(); i++)
    {
        const size_t nextIndex = (i + 1) % approxVertices.size();
        const GB_Point2d& a = approxVertices[i];
        const GB_Point2d& b = approxVertices[nextIndex];
        if (!a.IsValid() || !b.IsValid())
        {
            return GB_QuietNan;
        }

        perimeter += std::hypotl(static_cast<long double>(b.x) - static_cast<long double>(a.x),
            static_cast<long double>(b.y) - static_cast<long double>(a.y));
    }

    const double perimeterDouble = static_cast<double>(perimeter);
    return std::isfinite(perimeterDouble) ? perimeterDouble : GB_QuietNan;
}

GB_Polygon::PointContainment GB_Polygon::ClassifyPointOddEven(const GB_Point2d& point) const
{
    if (!point.IsValid())
    {
        return PointContainment::Outside;
    }

    std::vector<ExactPoint> polygonPoints;
    ExactPoint queryPoint;
    if (!TryBuildExactPoints(*this, polygonPoints) || polygonPoints.size() < 2 || !TryBuildExactPoint(point, queryPoint))
    {
        return PointContainment::Outside;
    }

    bool isInside = false;
    for (size_t i = 0; i < polygonPoints.size(); i++)
    {
        const ExactPoint& a = polygonPoints[i];
        const ExactPoint& b = polygonPoints[(i + 1) % polygonPoints.size()];

        if (IsPointOnSegment(queryPoint, a, b))
        {
            return PointContainment::OnBoundary;
        }

        const bool intersectsHalfOpenYRange = ((a.y() > queryPoint.y()) != (b.y() > queryPoint.y()));
        if (!intersectsHalfOpenYRange)
        {
            continue;
        }

        const ExactNumber lhs = (b.x() - a.x()) * (queryPoint.y() - a.y());
        const ExactNumber rhs = (queryPoint.x() - a.x()) * (b.y() - a.y());
        const bool isAscending = (b.y() > a.y());
        const bool isRayCrossingRightSide = isAscending ? (lhs > rhs) : (lhs < rhs);
        if (isRayCrossingRightSide)
        {
            isInside = !isInside;
        }
    }

    return isInside ? PointContainment::Inside : PointContainment::Outside;
}

GB_Polygon::PointContainment GB_Polygon::ClassifyPointOddEven(const std::string& xText, const std::string& yText) const
{
    std::vector<ExactPoint> polygonPoints;
    ExactPoint queryPoint;
    if (!TryBuildExactPoints(*this, polygonPoints) || polygonPoints.size() < 2 || !TryBuildExactPoint(xText, yText, queryPoint))
    {
        return PointContainment::Outside;
    }

    bool isInside = false;
    for (size_t i = 0; i < polygonPoints.size(); i++)
    {
        const ExactPoint& a = polygonPoints[i];
        const ExactPoint& b = polygonPoints[(i + 1) % polygonPoints.size()];

        if (IsPointOnSegment(queryPoint, a, b))
        {
            return PointContainment::OnBoundary;
        }

        const bool intersectsHalfOpenYRange = ((a.y() > queryPoint.y()) != (b.y() > queryPoint.y()));
        if (!intersectsHalfOpenYRange)
        {
            continue;
        }

        const ExactNumber lhs = (b.x() - a.x()) * (queryPoint.y() - a.y());
        const ExactNumber rhs = (queryPoint.x() - a.x()) * (b.y() - a.y());
        const bool isAscending = (b.y() > a.y());
        const bool isRayCrossingRightSide = isAscending ? (lhs > rhs) : (lhs < rhs);
        if (isRayCrossingRightSide)
        {
            isInside = !isInside;
        }
    }

    return isInside ? PointContainment::Inside : PointContainment::Outside;
}

GB_Polygon GB_Polygon::Reversed() const
{
    GB_Polygon result = *this;
    result.Reverse();
    return result;
}

void GB_Polygon::Reverse()
{
    if (storageMode == CoordinateStorageMode::Double)
    {
        std::reverse(vertices.begin(), vertices.end());
    }
    else
    {
        std::reverse(exactStringVertices.begin(), exactStringVertices.end());
    }
}

bool GB_Polygon::operator==(const GB_Polygon& other) const
{
    return storageMode == other.storageMode
        && vertices == other.vertices
        && exactStringVertices == other.exactStringVertices;
}

bool GB_Polygon::operator!=(const GB_Polygon& other) const
{
    return !(*this == other);
}

bool GB_Polygon::IsNearEqual(const GB_Polygon& other, double tolerance) const
{
    const double absTolerance = std::abs(tolerance);
    if (GetNumVertices() != other.GetNumVertices())
    {
        return false;
    }

    for (size_t i = 0; i < GetNumVertices(); i++)
    {
        GB_Point2d thisPoint;
        GB_Point2d otherPoint;
        if (!TryGetVertexAsDouble(i, thisPoint) || !other.TryGetVertexAsDouble(i, otherPoint))
        {
            return false;
        }

        if (!thisPoint.IsNearEqual(otherPoint, absTolerance))
        {
            return false;
        }
    }

    return true;
}

std::string GB_Polygon::SerializeToString() const
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << "(" << GetClassType() << "|" << static_cast<unsigned int>(storageMode) << "|" << GetNumVertices();

    if (storageMode == CoordinateStorageMode::Double)
    {
        for (const GB_Point2d& vertex : vertices)
        {
            oss << "|" << DoubleToString(vertex.x) << "|" << DoubleToString(vertex.y);
        }
    }
    else
    {
        for (const ExactStringVertex& vertex : exactStringVertices)
        {
            oss << "|" << vertex.first << "|" << vertex.second;
        }
    }

    oss << ")";
    return oss.str();
}

GB_ByteBuffer GB_Polygon::SerializeToBinary() const
{
    constexpr static std::uint16_t payloadVersion = 1;

    GB_ByteBuffer buffer;
    buffer.reserve(64 + GetNumVertices() * 32);

    AppendUInt32LE(buffer, GB_ClassMagicNumber);
    AppendUInt64LE(buffer, GetClassTypeId());
    AppendUInt16LE(buffer, payloadVersion);
    AppendUInt16LE(buffer, 0);
    AppendUInt8(buffer, static_cast<std::uint8_t>(storageMode));
    AppendUInt8(buffer, 0);
    AppendUInt16LE(buffer, 0);
    AppendUInt64LE(buffer, static_cast<std::uint64_t>(GetNumVertices()));

    if (storageMode == CoordinateStorageMode::Double)
    {
        for (const GB_Point2d& vertex : vertices)
        {
            AppendDoubleLE(buffer, vertex.x);
            AppendDoubleLE(buffer, vertex.y);
        }
    }
    else
    {
        for (const ExactStringVertex& vertex : exactStringVertices)
        {
            AppendString(buffer, vertex.first);
            AppendString(buffer, vertex.second);
        }
    }

    return buffer;
}

bool GB_Polygon::Deserialize(const std::string& data)
{
    Clear();

    if (data.size() < 2 || data.front() != '(' || data.back() != ')')
    {
        return false;
    }

    const std::string body = data.substr(1, data.size() - 2);
    const std::vector<std::string> tokens = SplitByChar(body, '|');
    if (tokens.size() < 3 || tokens[0] != GetClassType())
    {
        return false;
    }

    size_t storageModeValue = 0;
    size_t numVertices = 0;
    if (!TryParseSizeT(tokens[1], storageModeValue) || !TryParseSizeT(tokens[2], numVertices))
    {
        Clear();
        return false;
    }

    if (tokens.size() != 3 + numVertices * 2)
    {
        Clear();
        return false;
    }

    if (storageModeValue == static_cast<size_t>(CoordinateStorageMode::Double))
    {
        std::vector<GB_Point2d> parsedVertices;
        parsedVertices.reserve(numVertices);
        for (size_t i = 0; i < numVertices; i++)
        {
            double x = GB_QuietNan;
            double y = GB_QuietNan;
            if (!TryParseDoubleText(tokens[3 + i * 2], x) || !TryParseDoubleText(tokens[4 + i * 2], y))
            {
                Clear();
                return false;
            }
            parsedVertices.emplace_back(x, y);
        }
        return SetVertices(parsedVertices);
    }

    if (storageModeValue == static_cast<size_t>(CoordinateStorageMode::ExactString))
    {
        std::vector<ExactStringVertex> parsedVertices;
        parsedVertices.reserve(numVertices);
        for (size_t i = 0; i < numVertices; i++)
        {
            parsedVertices.emplace_back(tokens[3 + i * 2], tokens[4 + i * 2]);
        }
        return SetVertices(parsedVertices);
    }

    Clear();
    return false;
}

bool GB_Polygon::Deserialize(const GB_ByteBuffer& data)
{
    constexpr static std::uint16_t expectedPayloadVersion = 1;
    constexpr static size_t minSize = 4 + 8 + 2 + 2 + 1 + 1 + 2 + 8;

    Clear();
    if (data.size() < minSize)
    {
        return false;
    }

    size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint64_t typeId = 0;
    std::uint16_t payloadVersion = 0;
    std::uint16_t reservedHeader = 0;
    std::uint8_t storageModeValue = 0;
    std::uint8_t reservedByte = 0;
    std::uint16_t reservedTail = 0;
    std::uint64_t numVertices = 0;

    if (!ReadUInt32LE(data, offset, magic)
        || !ReadUInt64LE(data, offset, typeId)
        || !ReadUInt16LE(data, offset, payloadVersion)
        || !ReadUInt16LE(data, offset, reservedHeader)
        || !ReadUInt8(data, offset, storageModeValue)
        || !ReadUInt8(data, offset, reservedByte)
        || !ReadUInt16LE(data, offset, reservedTail)
        || !ReadUInt64LE(data, offset, numVertices))
    {
        Clear();
        return false;
    }

    if (magic != GB_ClassMagicNumber || typeId != GetClassTypeId() || payloadVersion != expectedPayloadVersion)
    {
        Clear();
        return false;
    }

    if (storageModeValue == static_cast<std::uint8_t>(CoordinateStorageMode::Double))
    {
        std::vector<GB_Point2d> parsedVertices;
        parsedVertices.reserve(static_cast<size_t>(numVertices));
        for (std::uint64_t i = 0; i < numVertices; i++)
        {
            double x = GB_QuietNan;
            double y = GB_QuietNan;
            if (!ReadDoubleLE(data, offset, x) || !ReadDoubleLE(data, offset, y) || !std::isfinite(x) || !std::isfinite(y))
            {
                Clear();
                return false;
            }
            parsedVertices.emplace_back(x, y);
        }

        if (offset != data.size())
        {
            Clear();
            return false;
        }

        return SetVertices(parsedVertices);
    }

    if (storageModeValue == static_cast<std::uint8_t>(CoordinateStorageMode::ExactString))
    {
        std::vector<ExactStringVertex> parsedVertices;
        parsedVertices.reserve(static_cast<size_t>(numVertices));
        for (std::uint64_t i = 0; i < numVertices; i++)
        {
            std::string xText;
            std::string yText;
            if (!ReadString(data, offset, xText) || !ReadString(data, offset, yText))
            {
                Clear();
                return false;
            }
            parsedVertices.emplace_back(xText, yText);
        }

        if (offset != data.size())
        {
            Clear();
            return false;
        }

        return SetVertices(parsedVertices);
    }

    Clear();
    return false;
}

#ifdef _MSC_VER
#  pragma warning(pop)
#endif