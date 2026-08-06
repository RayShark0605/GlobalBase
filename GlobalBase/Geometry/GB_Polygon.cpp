#include "GB_Polygon.h"
#include "GB_Rectangle.h"

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4819)
#endif

#if defined(__has_include)
#  if __has_include(<CGAL/Boolean_set_operations_2.h>) && __has_include(<CGAL/Cartesian.h>) && __has_include(<CGAL/Polygon_2.h>) && __has_include(<CGAL/Polygon_2_algorithms.h>) && __has_include(<CGAL/Polygon_set_2.h>) && __has_include(<CGAL/Polygon_with_holes_2.h>) && __has_include(<CGAL/boost_mp.h>)
#    define GB_POLYGON_HAS_CGAL 1
#    include <CGAL/Boolean_set_operations_2.h>
#    include <CGAL/Cartesian.h>
#    include <CGAL/Polygon_2.h>
#    include <CGAL/Polygon_2_algorithms.h>
#    include <CGAL/Polygon_set_2.h>
#    include <CGAL/Polygon_with_holes_2.h>
#    include <CGAL/boost_mp.h>
#  else
#    define GB_POLYGON_HAS_CGAL 0
#  endif
#else
#  define GB_POLYGON_HAS_CGAL 0
#endif

#if GB_POLYGON_HAS_CGAL && defined(__has_include)
#  if __has_include(<CGAL/Multipolygon_with_holes_2.h>) && __has_include(<CGAL/Polygon_repair/repair.h>) && __has_include(<CGAL/Polygon_repair/Even_odd_rule.h>)
#    define GB_POLYGON_HAS_CGAL_POLYGON_REPAIR 1
#    include <CGAL/Multipolygon_with_holes_2.h>
#    include <CGAL/Polygon_repair/repair.h>
#    include <CGAL/Polygon_repair/Even_odd_rule.h>
#  else
#    define GB_POLYGON_HAS_CGAL_POLYGON_REPAIR 0
#  endif
#else
#  define GB_POLYGON_HAS_CGAL_POLYGON_REPAIR 0
#endif

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
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

    struct ExactPoint
    {
        ExactNumber x = ExactNumber(0);
        ExactNumber y = ExactNumber(0);

        ExactPoint()
        {
        }

        ExactPoint(const ExactNumber& xValue, const ExactNumber& yValue)
            : x(xValue)
            , y(yValue)
        {
        }

        bool operator==(const ExactPoint& other) const
        {
            return x == other.x && y == other.y;
        }

        bool operator!=(const ExactPoint& other) const
        {
            return !(*this == other);
        }
    };

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
            if (parsedLength != trimmed.size() || value > static_cast<unsigned long long>(std::numeric_limits<size_t>::max()))
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

    static bool TryGetAbsInt64AsSizeT(std::int64_t value, size_t& outAbsValue)
    {
        if (value >= 0)
        {
            const std::uint64_t unsignedValue = static_cast<std::uint64_t>(value);
            if (unsignedValue > static_cast<std::uint64_t>(std::numeric_limits<size_t>::max()))
            {
                outAbsValue = 0;
                return false;
            }

            outAbsValue = static_cast<size_t>(unsignedValue);
            return true;
        }

        const std::uint64_t magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1u;
        if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<size_t>::max()))
        {
            outAbsValue = 0;
            return false;
        }

        outAbsValue = static_cast<size_t>(magnitude);
        return true;
    }

    static bool TryComputePow10Exponents(std::int64_t exponent10, size_t fracDigits, size_t& outNumeratorExponent, size_t& outDenominatorExponent)
    {
        outNumeratorExponent = 0;
        outDenominatorExponent = 0;

        if (exponent10 >= 0)
        {
            size_t exponentMagnitude = 0;
            if (!TryGetAbsInt64AsSizeT(exponent10, exponentMagnitude))
            {
                return false;
            }

            if (exponentMagnitude >= fracDigits)
            {
                outNumeratorExponent = exponentMagnitude - fracDigits;
            }
            else
            {
                outDenominatorExponent = fracDigits - exponentMagnitude;
            }
            return true;
        }

        size_t exponentMagnitude = 0;
        if (!TryGetAbsInt64AsSizeT(exponent10, exponentMagnitude))
        {
            return false;
        }

        if (exponentMagnitude > std::numeric_limits<size_t>::max() - fracDigits)
        {
            return false;
        }

        outDenominatorExponent = exponentMagnitude + fracDigits;
        return true;
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

        size_t numeratorExponent10 = 0;
        size_t denominatorExponent10 = 0;
        if (!TryComputePow10Exponents(exponent10, fracDigits, numeratorExponent10, denominatorExponent10))
        {
            outValue = ExactNumber(0);
            return false;
        }

        ExactInteger numerator = integerValue;
        ExactInteger denominator = 1;
        if (numeratorExponent10 > 0)
        {
            numerator *= Pow10Exact(numeratorExponent10);
        }
        else if (denominatorExponent10 > 0)
        {
            denominator = Pow10Exact(denominatorExponent10);
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


    static bool TrySetDoubleVerticesFromDeserialization(GB_Polygon& polygon, std::vector<GB_Point2d>&& parsedVertices)
    {
        const size_t numVertices = parsedVertices.size();
        if (polygon.SetVertices(std::move(parsedVertices)))
        {
            return true;
        }

        if (numVertices < 2 && polygon.GetCoordinateStorageMode() == GB_Polygon::CoordinateStorageMode::Double && polygon.GetNumVertices() == numVertices)
        {
            return true;
        }

        polygon.Clear();
        return false;
    }

    static bool TrySetExactStringVerticesFromDeserialization(GB_Polygon& polygon, std::vector<GB_Polygon::ExactStringVertex>&& parsedVertices)
    {
        const size_t numVertices = parsedVertices.size();
        if (polygon.SetVertices(std::move(parsedVertices)))
        {
            return true;
        }

        if (numVertices < 2 && polygon.GetCoordinateStorageMode() == GB_Polygon::CoordinateStorageMode::ExactString && polygon.GetNumVertices() == numVertices)
        {
            return true;
        }

        polygon.Clear();
        return false;
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
        const ExactNumber ax = a.x - origin.x;
        const ExactNumber ay = a.y - origin.y;
        const ExactNumber bx = b.x - origin.x;
        const ExactNumber by = b.y - origin.y;
        return ax * by - ay * bx;
    }

    static bool IsPointOnSegment(const ExactPoint& point, const ExactPoint& a, const ExactPoint& b)
    {
        if (Cross(a, b, point) != ExactNumber(0))
        {
            return false;
        }

        const ExactNumber minX = std::min(a.x, b.x);
        const ExactNumber maxX = std::max(a.x, b.x);
        const ExactNumber minY = std::min(a.y, b.y);
        const ExactNumber maxY = std::max(a.y, b.y);
        return point.x >= minX && point.x <= maxX && point.y >= minY && point.y <= maxY;
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
            areaTwice += points[i].x * points[nextIndex].y - points[nextIndex].x * points[i].y;
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

    static int GetSign(const ExactNumber& value)
    {
        if (value > ExactNumber(0))
        {
            return 1;
        }
        if (value < ExactNumber(0))
        {
            return -1;
        }
        return 0;
    }

    static bool DoBoxesOverlap(const ExactPoint& a, const ExactPoint& b, const ExactPoint& c, const ExactPoint& d)
    {
        const ExactNumber abMinX = std::min(a.x, b.x);
        const ExactNumber abMaxX = std::max(a.x, b.x);
        const ExactNumber abMinY = std::min(a.y, b.y);
        const ExactNumber abMaxY = std::max(a.y, b.y);
        const ExactNumber cdMinX = std::min(c.x, d.x);
        const ExactNumber cdMaxX = std::max(c.x, d.x);
        const ExactNumber cdMinY = std::min(c.y, d.y);
        const ExactNumber cdMaxY = std::max(c.y, d.y);
        return !(abMaxX < cdMinX || cdMaxX < abMinX || abMaxY < cdMinY || cdMaxY < abMinY);
    }

    static bool DoSegmentsIntersect(const ExactPoint& a, const ExactPoint& b, const ExactPoint& c, const ExactPoint& d)
    {
        if (!DoBoxesOverlap(a, b, c, d))
        {
            return false;
        }

        const ExactNumber cross1 = Cross(a, b, c);
        const ExactNumber cross2 = Cross(a, b, d);
        const ExactNumber cross3 = Cross(c, d, a);
        const ExactNumber cross4 = Cross(c, d, b);

        const int sign1 = GetSign(cross1);
        const int sign2 = GetSign(cross2);
        const int sign3 = GetSign(cross3);
        const int sign4 = GetSign(cross4);

        if (sign1 == 0 && IsPointOnSegment(c, a, b))
        {
            return true;
        }
        if (sign2 == 0 && IsPointOnSegment(d, a, b))
        {
            return true;
        }
        if (sign3 == 0 && IsPointOnSegment(a, c, d))
        {
            return true;
        }
        if (sign4 == 0 && IsPointOnSegment(b, c, d))
        {
            return true;
        }

        return sign1 * sign2 < 0 && sign3 * sign4 < 0;
    }

    static bool AreEdgeIndicesAdjacent(size_t edgeIndexA, size_t edgeIndexB, size_t numPoints)
    {
        if (edgeIndexA == edgeIndexB)
        {
            return true;
        }

        if ((edgeIndexA + 1) % numPoints == edgeIndexB)
        {
            return true;
        }

        if ((edgeIndexB + 1) % numPoints == edgeIndexA)
        {
            return true;
        }

        return false;
    }

    static bool IsSimpleRingQuadratic(const std::vector<ExactPoint>& points)
    {
        const size_t numPoints = points.size();
        if (numPoints < 3)
        {
            return false;
        }

        for (size_t edgeIndexA = 0; edgeIndexA < numPoints; edgeIndexA++)
        {
            const ExactPoint& a0 = points[edgeIndexA];
            const ExactPoint& a1 = points[(edgeIndexA + 1) % numPoints];

            for (size_t edgeIndexB = edgeIndexA + 1; edgeIndexB < numPoints; edgeIndexB++)
            {
                if (AreEdgeIndicesAdjacent(edgeIndexA, edgeIndexB, numPoints))
                {
                    continue;
                }

                const ExactPoint& b0 = points[edgeIndexB];
                const ExactPoint& b1 = points[(edgeIndexB + 1) % numPoints];
                if (DoSegmentsIntersect(a0, a1, b0, b1))
                {
                    return false;
                }
            }
        }

        return true;
    }

#if GB_POLYGON_HAS_CGAL
    static bool TryIsSimpleRingWithCgal(const std::vector<ExactPoint>& points, bool& outIsSimple)
    {
        using CgalExactKernel = CGAL::Cartesian<ExactNumber>;
        using CgalExactPoint = CgalExactKernel::Point_2;

        std::vector<CgalExactPoint> cgalPoints;
        cgalPoints.reserve(points.size());
        for (size_t i = 0; i < points.size(); i++)
        {
            cgalPoints.emplace_back(points[i].x, points[i].y);
        }

        outIsSimple = CGAL::is_simple_2(cgalPoints.begin(), cgalPoints.end(), CgalExactKernel());
        return true;
    }
#endif

    static bool IsSimpleRing(const std::vector<ExactPoint>& points)
    {
#if GB_POLYGON_HAS_CGAL
        bool isSimple = false;
        if (TryIsSimpleRingWithCgal(points, isSimple))
        {
            return isSimple;
        }
#endif
        return IsSimpleRingQuadratic(points);
    }


    static bool ReadNextSerializedField(const std::string& text, size_t& ioOffset, std::string& outField)
    {
        if (ioOffset > text.size())
        {
            outField.clear();
            return false;
        }

        const size_t delimiterPos = text.find('|', ioOffset);
        if (delimiterPos == std::string::npos)
        {
            outField.assign(text.data() + ioOffset, text.size() - ioOffset);
            ioOffset = text.size();
            return true;
        }

        outField.assign(text.data() + ioOffset, delimiterPos - ioOffset);
        ioOffset = delimiterPos + 1;
        return true;
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

        if (length > static_cast<std::uint64_t>(buffer.size() - offset) || length > static_cast<std::uint64_t>(std::numeric_limits<size_t>::max()))
        {
            outText.clear();
            return false;
        }

        outText.assign(reinterpret_cast<const char*>(buffer.data() + offset), static_cast<size_t>(length));
        offset += static_cast<size_t>(length);
        return true;
    }

    static size_t GetBinaryReserveHint(size_t numVertices)
    {
        constexpr static size_t baseReserve = 64;
        constexpr static size_t bytesPerVertexHint = 32;

        if (numVertices > (std::numeric_limits<size_t>::max() - baseReserve) / bytesPerVertexHint)
        {
            return baseReserve;
        }

        return baseReserve + numVertices * bytesPerVertexHint;
    }

    static bool TryConvertVertexCountToSizeT(std::uint64_t numVertices, size_t& outValue)
    {
        if (numVertices > static_cast<std::uint64_t>(std::numeric_limits<size_t>::max()))
        {
            outValue = 0;
            return false;
        }

        outValue = static_cast<size_t>(numVertices);
        return true;
    }

    static bool TrySetEmptyPolygon(GB_Polygon::CoordinateStorageMode storageMode, GB_Polygon& outPolygon)
    {
        outPolygon.Clear();
        if (storageMode == GB_Polygon::CoordinateStorageMode::Double)
        {
            outPolygon.SetVertices(std::vector<GB_Point2d>());
            return true;
        }

        outPolygon.SetVertices(std::vector<GB_Polygon::ExactStringVertex>());
        return true;
    }

    enum class PolygonBooleanOperation
    {
        Intersection = 0,
        Union = 1,
        Difference = 2
    };

#if GB_POLYGON_HAS_CGAL
    using CgalExactKernel = CGAL::Cartesian<ExactNumber>;
    using CgalExactPoint = CgalExactKernel::Point_2;
    using CgalPolygon2 = CGAL::Polygon_2<CgalExactKernel>;
    using CgalPolygonWithHoles2 = CGAL::Polygon_with_holes_2<CgalExactKernel>;
    using CgalPolygonSet2 = CGAL::Polygon_set_2<CgalExactKernel>;
#endif

#if GB_POLYGON_HAS_CGAL_POLYGON_REPAIR
    using CgalMultipolygonWithHoles2 = CGAL::Multipolygon_with_holes_2<CgalExactKernel>;
#endif

    static std::vector<ExactPoint> NormalizeExactRing(const std::vector<ExactPoint>& points)
    {
        std::vector<ExactPoint> normalizedPoints;
        normalizedPoints.reserve(points.size());
        for (size_t i = 0; i < points.size(); i++)
        {
            if (normalizedPoints.empty() || points[i] != normalizedPoints.back())
            {
                normalizedPoints.push_back(points[i]);
            }
        }

        if (normalizedPoints.size() > 1 && normalizedPoints.front() == normalizedPoints.back())
        {
            normalizedPoints.pop_back();
        }

        return normalizedPoints;
    }

    static bool HasDuplicateAdjacentVerticesInClosedRing(const std::vector<ExactPoint>& points)
    {
        if (points.size() < 2)
        {
            return false;
        }

        for (size_t i = 0; i < points.size(); i++)
        {
            if (points[i] == points[(i + 1) % points.size()])
            {
                return true;
            }
        }

        return false;
    }

    static bool IsAxisAlignedNonZeroSegment(const ExactPoint& startPoint, const ExactPoint& endPoint)
    {
        if (startPoint == endPoint)
        {
            return false;
        }

        return startPoint.x == endPoint.x || startPoint.y == endPoint.y;
    }

    static void RemoveAxisAlignedCollinearVertices(std::vector<ExactPoint>& ioPoints)
    {
        if (ioPoints.size() < 3)
        {
            return;
        }

        bool isChanged = true;
        while (isChanged && ioPoints.size() >= 3)
        {
            isChanged = false;
            for (size_t i = 0; i < ioPoints.size() && ioPoints.size() >= 3; )
            {
                const size_t prevIndex = (i + ioPoints.size() - 1) % ioPoints.size();
                const size_t nextIndex = (i + 1) % ioPoints.size();
                const ExactPoint& prevPoint = ioPoints[prevIndex];
                const ExactPoint& curPoint = ioPoints[i];
                const ExactPoint& nextPoint = ioPoints[nextIndex];

                const bool isSameVerticalLine = (prevPoint.x == curPoint.x) && (curPoint.x == nextPoint.x);
                const bool isSameHorizontalLine = (prevPoint.y == curPoint.y) && (curPoint.y == nextPoint.y);
                if ((isSameVerticalLine || isSameHorizontalLine) && Cross(prevPoint, curPoint, nextPoint) == ExactNumber(0))
                {
                    ioPoints.erase(ioPoints.begin() + static_cast<std::ptrdiff_t>(i));
                    isChanged = true;
                    if (i >= ioPoints.size())
                    {
                        i = 0;
                    }
                    continue;
                }

                i++;
            }
        }
    }

    static bool TryBuildAxisAlignedRectangleFromRing(const std::vector<ExactPoint>& inputPoints, GB_Rectangle& outRectangle)
    {
        outRectangle.Reset();

        std::vector<ExactPoint> points = NormalizeExactRing(inputPoints);
        if (points.size() < 4)
        {
            return false;
        }

        ExactNumber minX = points[0].x;
        ExactNumber minY = points[0].y;
        ExactNumber maxX = points[0].x;
        ExactNumber maxY = points[0].y;
        for (size_t i = 0; i < points.size(); i++)
        {
            const ExactPoint& curPoint = points[i];
            const ExactPoint& nextPoint = points[(i + 1) % points.size()];
            if (!IsAxisAlignedNonZeroSegment(curPoint, nextPoint))
            {
                return false;
            }

            if (curPoint.x < minX)
            {
                minX = curPoint.x;
            }
            if (curPoint.y < minY)
            {
                minY = curPoint.y;
            }
            if (curPoint.x > maxX)
            {
                maxX = curPoint.x;
            }
            if (curPoint.y > maxY)
            {
                maxY = curPoint.y;
            }
        }

        if (minX >= maxX || minY >= maxY)
        {
            return false;
        }

        RemoveAxisAlignedCollinearVertices(points);
        if (points.size() != 4)
        {
            return false;
        }

        const std::vector<ExactPoint> corners =
        {
            ExactPoint(minX, minY),
            ExactPoint(maxX, minY),
            ExactPoint(maxX, maxY),
            ExactPoint(minX, maxY)
        };

        bool hasCorner[4] = { false, false, false, false };
        for (size_t i = 0; i < points.size(); i++)
        {
            size_t matchedCornerIndex = static_cast<size_t>(-1);
            for (size_t j = 0; j < corners.size(); j++)
            {
                if (points[i] == corners[j])
                {
                    matchedCornerIndex = j;
                    break;
                }
            }

            if (matchedCornerIndex == static_cast<size_t>(-1) || hasCorner[matchedCornerIndex])
            {
                return false;
            }

            hasCorner[matchedCornerIndex] = true;
        }

        ExactNumber areaTwice = ComputeSignedDoubleAreaTwice(points);
        if (areaTwice < ExactNumber(0))
        {
            areaTwice = -areaTwice;
        }

        const ExactNumber expectedAreaTwice = ExactNumber(2) * (maxX - minX) * (maxY - minY);
        if (areaTwice != expectedAreaTwice)
        {
            return false;
        }

        double minXDouble = GB_QuietNan;
        double minYDouble = GB_QuietNan;
        double maxXDouble = GB_QuietNan;
        double maxYDouble = GB_QuietNan;
        if (!TryConvertExactNumberToDouble(minX, minXDouble)
            || !TryConvertExactNumberToDouble(minY, minYDouble)
            || !TryConvertExactNumberToDouble(maxX, maxXDouble)
            || !TryConvertExactNumberToDouble(maxY, maxYDouble))
        {
            return false;
        }

        outRectangle.Set(minXDouble, minYDouble, maxXDouble, maxYDouble);
        return outRectangle.IsValid();
    }

#if GB_POLYGON_HAS_CGAL
    static bool TryBuildCgalPolygon(const std::vector<ExactPoint>& inputPoints, CgalPolygon2& outPolygon)
    {
        outPolygon.clear();

        if (inputPoints.size() < 3)
        {
            return false;
        }

        if (HasDuplicateAdjacentVerticesInClosedRing(inputPoints))
        {
            return false;
        }

        if (AreAllPointsCollinear(inputPoints))
        {
            return false;
        }

        if (!IsSimpleRing(inputPoints))
        {
            return false;
        }

        std::vector<ExactPoint> points = inputPoints;
        const ExactNumber signedAreaTwice = ComputeSignedDoubleAreaTwice(points);
        if (signedAreaTwice == ExactNumber(0))
        {
            return false;
        }

        if (signedAreaTwice < ExactNumber(0))
        {
            std::reverse(points.begin(), points.end());
        }

        for (size_t i = 0; i < points.size(); i++)
        {
            outPolygon.push_back(CgalExactPoint(points[i].x, points[i].y));
        }

        return true;
    }

    static GB_Polygon BuildPolygonFromCgalRing(const CgalPolygon2& polygon)
    {
        std::vector<GB_Polygon::ExactStringVertex> exactStringVertices;
        exactStringVertices.reserve(static_cast<size_t>(polygon.size()));
        for (CgalPolygon2::Vertex_const_iterator it = polygon.vertices_begin(); it != polygon.vertices_end(); ++it)
        {
            exactStringVertices.emplace_back(ExactNumberToString(it->x()), ExactNumberToString(it->y()));
        }

        return GB_Polygon(exactStringVertices);
    }

    static void EnsurePolygonOrientation(GB_Polygon& ioPolygon, GB_Polygon::Orientation expectedOrientation)
    {
        if (ioPolygon.GetOrientation() != expectedOrientation)
        {
            ioPolygon.Reverse();
        }
    }

    static bool TryExecutePolygonBooleanOperation(const std::vector<ExactPoint>& firstInputPoints,
        const std::vector<ExactPoint>& secondInputPoints,
        PolygonBooleanOperation operation,
        std::vector<GB_Polygon>& outOuterBoundaries,
        std::vector<std::vector<GB_Polygon>>& outHoleBoundaries)
    {
        outOuterBoundaries.clear();
        outHoleBoundaries.clear();

        CgalPolygon2 firstPolygon;
        CgalPolygon2 secondPolygon;
        if (!TryBuildCgalPolygon(firstInputPoints, firstPolygon) || !TryBuildCgalPolygon(secondInputPoints, secondPolygon))
        {
            return false;
        }

        CgalPolygonSet2 polygonSet;
        polygonSet.insert(firstPolygon);

        switch (operation)
        {
        case PolygonBooleanOperation::Intersection:
            polygonSet.intersection(secondPolygon);
            break;
        case PolygonBooleanOperation::Union:
            polygonSet.join(secondPolygon);
            break;
        case PolygonBooleanOperation::Difference:
            polygonSet.difference(secondPolygon);
            break;
        default:
            return false;
        }

        std::vector<CgalPolygonWithHoles2> polygonWithHolesList;
        polygonSet.polygons_with_holes(std::back_inserter(polygonWithHolesList));

        outOuterBoundaries.reserve(polygonWithHolesList.size());
        outHoleBoundaries.reserve(polygonWithHolesList.size());
        for (std::vector<CgalPolygonWithHoles2>::const_iterator pwhIt = polygonWithHolesList.begin(); pwhIt != polygonWithHolesList.end(); ++pwhIt)
        {
            GB_Polygon outerBoundary = BuildPolygonFromCgalRing(pwhIt->outer_boundary());
            EnsurePolygonOrientation(outerBoundary, GB_Polygon::Orientation::CounterClockwise);
            outOuterBoundaries.push_back(std::move(outerBoundary));

            std::vector<GB_Polygon> holeBoundaries;
            for (CgalPolygonWithHoles2::Hole_const_iterator holeIt = pwhIt->holes_begin(); holeIt != pwhIt->holes_end(); ++holeIt)
            {
                GB_Polygon holeBoundary = BuildPolygonFromCgalRing(*holeIt);
                EnsurePolygonOrientation(holeBoundary, GB_Polygon::Orientation::Clockwise);
                holeBoundaries.push_back(std::move(holeBoundary));
            }
            outHoleBoundaries.push_back(std::move(holeBoundaries));
        }

        return true;
    }
#endif

#if GB_POLYGON_HAS_CGAL_POLYGON_REPAIR
    static bool TryConvertCgalMultipolygonToGbPolygons(const CgalMultipolygonWithHoles2& multipolygon,
        std::vector<GB_Polygon>& outOuterBoundaries,
        std::vector<std::vector<GB_Polygon>>& outHoleBoundaries)
    {
        outOuterBoundaries.clear();
        outHoleBoundaries.clear();

        const size_t numPolygons = static_cast<size_t>(multipolygon.number_of_polygons_with_holes());
        outOuterBoundaries.reserve(numPolygons);
        outHoleBoundaries.reserve(numPolygons);

        for (CgalMultipolygonWithHoles2::Polygon_with_holes_const_iterator polygonIt = multipolygon.polygons_with_holes_begin(); polygonIt != multipolygon.polygons_with_holes_end(); ++polygonIt)
        {
            if (polygonIt->is_unbounded())
            {
                outOuterBoundaries.clear();
                outHoleBoundaries.clear();
                return false;
            }

            GB_Polygon outerBoundary = BuildPolygonFromCgalRing(polygonIt->outer_boundary());
            EnsurePolygonOrientation(outerBoundary, GB_Polygon::Orientation::CounterClockwise);
            outOuterBoundaries.push_back(std::move(outerBoundary));

            std::vector<GB_Polygon> holeBoundaries;
            holeBoundaries.reserve(static_cast<size_t>(polygonIt->number_of_holes()));
            for (CgalPolygonWithHoles2::Hole_const_iterator holeIt = polygonIt->holes_begin(); holeIt != polygonIt->holes_end(); ++holeIt)
            {
                GB_Polygon holeBoundary = BuildPolygonFromCgalRing(*holeIt);
                EnsurePolygonOrientation(holeBoundary, GB_Polygon::Orientation::Clockwise);
                holeBoundaries.push_back(std::move(holeBoundary));
            }

            outHoleBoundaries.push_back(std::move(holeBoundaries));
        }

        return true;
    }
#endif

    static GB_Polygon::PointContainment ClassifyPointOddEvenImpl(const std::vector<ExactPoint>& polygonPoints, const ExactPoint& queryPoint)
    {
        const size_t numPoints = polygonPoints.size();
        if (numPoints < 2)
        {
            return GB_Polygon::PointContainment::Outside;
        }

        bool isInside = false;

        for (size_t i = 0; i < numPoints; i++)
        {
            const ExactPoint& a = polygonPoints[i];
            const ExactPoint& b = polygonPoints[(i + 1) % numPoints];

            if (IsPointOnSegment(queryPoint, a, b))
            {
                return GB_Polygon::PointContainment::OnBoundary;
            }

            const bool aAbove = (a.y > queryPoint.y);
            const bool bAbove = (b.y > queryPoint.y);

            if (aAbove == bAbove)
            {
                continue;
            }

            const ExactNumber crossValue = Cross(a, b, queryPoint);

            if (b.y > a.y)
            {
                if (crossValue > ExactNumber(0))
                {
                    isInside = !isInside;
                }
            }
            else
            {
                if (crossValue < ExactNumber(0))
                {
                    isInside = !isInside;
                }
            }
        }

        return isInside
            ? GB_Polygon::PointContainment::Inside
            : GB_Polygon::PointContainment::Outside;
    }
}

struct GB_Polygon::ExactCacheData
{
    std::uint64_t version = 0;
    std::vector<ExactPoint> exactPoints;
    std::vector<GB_Point2d> doubleVertices;
    bool hasAllFiniteDoubleVertices = true;

    bool hasBoundingBoxDouble = false;
    GB_Rectangle boundingBoxDouble;

    bool hasBoundingBoxExactStrings = false;
    std::string minXText;
    std::string minYText;
    std::string maxXText;
    std::string maxYText;

    ExactNumber signedAreaTwice = ExactNumber(0);
    double signedAreaDouble = GB_QuietNan;
    std::string signedAreaExactString;

    bool hasDuplicateAdjacentVertices = false;
    bool hasCollinearAdjacentTriples = false;
    bool areAllVerticesCollinear = true;
    bool isSimple = false;

    bool hasPerimeter = false;
    double perimeter = GB_QuietNan;
};

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

GB_Polygon::GB_Polygon(const GB_Rectangle& rectangle)
{
    if (!rectangle.IsValid())
    {
        return;
    }

    SetVertices(rectangle.GetCorners());
}

GB_Polygon::~GB_Polygon()
{
}

GB_Polygon::GB_Polygon(const GB_Polygon& other)
    : storageMode(other.storageMode)
    , vertices(other.vertices)
    , exactStringVertices(other.exactStringVertices)
    , cacheVersion(other.GetCurrentCacheVersion())
    , exactCache()
{
    const std::uint64_t otherVersion = other.GetCurrentCacheVersion();
    const std::shared_ptr<const ExactCacheData> otherCache = std::atomic_load_explicit(&other.exactCache, std::memory_order_acquire);
    if (otherCache && otherCache->version == otherVersion)
    {
        std::atomic_store_explicit(&exactCache, otherCache, std::memory_order_release);
    }
}

GB_Polygon::GB_Polygon(GB_Polygon&& other) noexcept
    : storageMode(other.storageMode)
    , vertices(std::move(other.vertices))
    , exactStringVertices(std::move(other.exactStringVertices))
    , cacheVersion(other.GetCurrentCacheVersion())
    , exactCache()
{
    const std::uint64_t otherVersion = other.GetCurrentCacheVersion();
    const std::shared_ptr<const ExactCacheData> otherCache = std::atomic_load_explicit(&other.exactCache, std::memory_order_acquire);
    if (otherCache && otherCache->version == otherVersion)
    {
        std::atomic_store_explicit(&exactCache, otherCache, std::memory_order_release);
    }

    other.storageMode = CoordinateStorageMode::Double;
    other.vertices.clear();
    other.exactStringVertices.clear();
    other.InvalidateCaches();
}

GB_Polygon& GB_Polygon::operator=(const GB_Polygon& other)
{
    if (this == &other)
    {
        return *this;
    }

    storageMode = other.storageMode;
    vertices = other.vertices;
    exactStringVertices = other.exactStringVertices;

    const std::uint64_t otherVersion = other.GetCurrentCacheVersion();
    const std::shared_ptr<const ExactCacheData> otherCache = std::atomic_load_explicit(&other.exactCache, std::memory_order_acquire);
    cacheVersion.store(otherVersion, std::memory_order_release);
    if (otherCache && otherCache->version == otherVersion)
    {
        std::atomic_store_explicit(&exactCache, otherCache, std::memory_order_release);
    }
    else
    {
        std::atomic_store_explicit(&exactCache, std::shared_ptr<const ExactCacheData>(), std::memory_order_release);
    }

    return *this;
}

GB_Polygon& GB_Polygon::operator=(GB_Polygon&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    storageMode = other.storageMode;
    vertices = std::move(other.vertices);
    exactStringVertices = std::move(other.exactStringVertices);

    const std::uint64_t otherVersion = other.GetCurrentCacheVersion();
    const std::shared_ptr<const ExactCacheData> otherCache = std::atomic_load_explicit(&other.exactCache, std::memory_order_acquire);
    cacheVersion.store(otherVersion, std::memory_order_release);
    if (otherCache && otherCache->version == otherVersion)
    {
        std::atomic_store_explicit(&exactCache, otherCache, std::memory_order_release);
    }
    else
    {
        std::atomic_store_explicit(&exactCache, std::shared_ptr<const ExactCacheData>(), std::memory_order_release);
    }

    other.storageMode = CoordinateStorageMode::Double;
    other.vertices.clear();
    other.exactStringVertices.clear();
    other.InvalidateCaches();
    return *this;
}

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

void GB_Polygon::InvalidateCaches()
{
    cacheVersion.fetch_add(1, std::memory_order_release);
    std::atomic_store_explicit(&exactCache, std::shared_ptr<const ExactCacheData>(), std::memory_order_release);
}

std::uint64_t GB_Polygon::GetCurrentCacheVersion() const
{
    return cacheVersion.load(std::memory_order_acquire);
}

std::shared_ptr<const GB_Polygon::ExactCacheData> GB_Polygon::GetOrBuildExactCache() const
{
    const std::uint64_t currentVersion = GetCurrentCacheVersion();
    std::shared_ptr<const ExactCacheData> cache = std::atomic_load_explicit(&exactCache, std::memory_order_acquire);
    if (cache && cache->version == currentVersion)
    {
        return cache;
    }

    std::shared_ptr<ExactCacheData> newCache = std::make_shared<ExactCacheData>();
    newCache->version = currentVersion;

    const size_t numVertices = GetNumVertices();
    newCache->exactPoints.reserve(numVertices);
    newCache->doubleVertices.reserve(numVertices);

    bool bboxExactInitialized = false;
    ExactNumber minX = ExactNumber(0);
    ExactNumber minY = ExactNumber(0);
    ExactNumber maxX = ExactNumber(0);
    ExactNumber maxY = ExactNumber(0);

    bool bboxDoubleInitialized = false;
    double minXDouble = GB_QuietNan;
    double minYDouble = GB_QuietNan;
    double maxXDouble = GB_QuietNan;
    double maxYDouble = GB_QuietNan;

    if (storageMode == CoordinateStorageMode::Double)
    {
        if (vertices.size() != numVertices)
        {
            return std::shared_ptr<const ExactCacheData>();
        }

        for (size_t i = 0; i < numVertices; i++)
        {
            const GB_Point2d& vertex = vertices[i];
            if (!vertex.IsValid())
            {
                return std::shared_ptr<const ExactCacheData>();
            }

            ExactNumber x = ExactNumber(0);
            ExactNumber y = ExactNumber(0);
            if (!TryConvertDoubleToExactNumber(vertex.x, x) || !TryConvertDoubleToExactNumber(vertex.y, y))
            {
                return std::shared_ptr<const ExactCacheData>();
            }

            newCache->exactPoints.emplace_back(x, y);
            newCache->doubleVertices.emplace_back(vertex);

            if (!bboxExactInitialized)
            {
                minX = x;
                minY = y;
                maxX = x;
                maxY = y;
                newCache->minXText = DoubleToString(vertex.x);
                newCache->minYText = DoubleToString(vertex.y);
                newCache->maxXText = newCache->minXText;
                newCache->maxYText = newCache->minYText;
                bboxExactInitialized = true;

                minXDouble = vertex.x;
                minYDouble = vertex.y;
                maxXDouble = vertex.x;
                maxYDouble = vertex.y;
                bboxDoubleInitialized = true;
            }
            else
            {
                if (x < minX)
                {
                    minX = x;
                    newCache->minXText = DoubleToString(vertex.x);
                }
                if (y < minY)
                {
                    minY = y;
                    newCache->minYText = DoubleToString(vertex.y);
                }
                if (x > maxX)
                {
                    maxX = x;
                    newCache->maxXText = DoubleToString(vertex.x);
                }
                if (y > maxY)
                {
                    maxY = y;
                    newCache->maxYText = DoubleToString(vertex.y);
                }

                if (vertex.x < minXDouble)
                {
                    minXDouble = vertex.x;
                }
                if (vertex.y < minYDouble)
                {
                    minYDouble = vertex.y;
                }
                if (vertex.x > maxXDouble)
                {
                    maxXDouble = vertex.x;
                }
                if (vertex.y > maxYDouble)
                {
                    maxYDouble = vertex.y;
                }
            }
        }
    }
    else
    {
        if (exactStringVertices.size() != numVertices)
        {
            return std::shared_ptr<const ExactCacheData>();
        }

        for (size_t i = 0; i < numVertices; i++)
        {
            const ExactStringVertex& vertex = exactStringVertices[i];
            ExactNumber x = ExactNumber(0);
            ExactNumber y = ExactNumber(0);
            if (!TryParseExactNumber(vertex.first, x) || !TryParseExactNumber(vertex.second, y))
            {
                return std::shared_ptr<const ExactCacheData>();
            }

            newCache->exactPoints.emplace_back(x, y);

            double xDouble = GB_QuietNan;
            double yDouble = GB_QuietNan;
            if (TryConvertExactNumberToDouble(x, xDouble) && TryConvertExactNumberToDouble(y, yDouble))
            {
                newCache->doubleVertices.emplace_back(xDouble, yDouble);
            }
            else
            {
                newCache->doubleVertices.emplace_back(GB_QuietNan, GB_QuietNan);
                newCache->hasAllFiniteDoubleVertices = false;
            }

            if (!bboxExactInitialized)
            {
                minX = x;
                minY = y;
                maxX = x;
                maxY = y;
                newCache->minXText = vertex.first;
                newCache->minYText = vertex.second;
                newCache->maxXText = vertex.first;
                newCache->maxYText = vertex.second;
                bboxExactInitialized = true;

                if (newCache->hasAllFiniteDoubleVertices)
                {
                    minXDouble = xDouble;
                    minYDouble = yDouble;
                    maxXDouble = xDouble;
                    maxYDouble = yDouble;
                    bboxDoubleInitialized = true;
                }
            }
            else
            {
                if (x < minX)
                {
                    minX = x;
                    newCache->minXText = vertex.first;
                }
                if (y < minY)
                {
                    minY = y;
                    newCache->minYText = vertex.second;
                }
                if (x > maxX)
                {
                    maxX = x;
                    newCache->maxXText = vertex.first;
                }
                if (y > maxY)
                {
                    maxY = y;
                    newCache->maxYText = vertex.second;
                }

                if (newCache->hasAllFiniteDoubleVertices)
                {
                    if (!bboxDoubleInitialized)
                    {
                        minXDouble = xDouble;
                        minYDouble = yDouble;
                        maxXDouble = xDouble;
                        maxYDouble = yDouble;
                        bboxDoubleInitialized = true;
                    }
                    else
                    {
                        if (xDouble < minXDouble)
                        {
                            minXDouble = xDouble;
                        }
                        if (yDouble < minYDouble)
                        {
                            minYDouble = yDouble;
                        }
                        if (xDouble > maxXDouble)
                        {
                            maxXDouble = xDouble;
                        }
                        if (yDouble > maxYDouble)
                        {
                            maxYDouble = yDouble;
                        }
                    }
                }
            }
        }
    }

    newCache->hasBoundingBoxExactStrings = bboxExactInitialized;
    newCache->signedAreaTwice = ComputeSignedDoubleAreaTwice(newCache->exactPoints);

    const ExactNumber signedArea = newCache->signedAreaTwice / ExactNumber(2);
    TryConvertExactNumberToDouble(signedArea, newCache->signedAreaDouble);
    newCache->signedAreaExactString = ExactNumberToString(signedArea);

    const size_t numPoints = newCache->exactPoints.size();
    if (numPoints >= 2)
    {
        const ExactPoint& firstPoint = newCache->exactPoints.front();
        ExactPoint prevPoint = firstPoint;

        long double perimeter = 0.0L;
        const GB_Point2d* firstDoublePoint = nullptr;
        const GB_Point2d* prevDoublePoint = nullptr;
        if (IsValid() && newCache->hasAllFiniteDoubleVertices)
        {
            firstDoublePoint = &newCache->doubleVertices.front();
            prevDoublePoint = firstDoublePoint;
        }

        for (size_t i = 1; i < numPoints; i++)
        {
            const ExactPoint& curPoint = newCache->exactPoints[i];
            if (curPoint == prevPoint)
            {
                newCache->hasDuplicateAdjacentVertices = true;
            }

            if (prevDoublePoint)
            {
                const GB_Point2d& curDoublePoint = newCache->doubleVertices[i];
                const long double dx = static_cast<long double>(curDoublePoint.x) - static_cast<long double>(prevDoublePoint->x);
                const long double dy = static_cast<long double>(curDoublePoint.y) - static_cast<long double>(prevDoublePoint->y);
                perimeter += std::hypot(dx, dy);
                prevDoublePoint = &curDoublePoint;
            }

            prevPoint = curPoint;
        }
        if (prevPoint == firstPoint)
        {
            newCache->hasDuplicateAdjacentVertices = true;
        }

        if (prevDoublePoint && firstDoublePoint)
        {
            const long double dx = static_cast<long double>(firstDoublePoint->x) - static_cast<long double>(prevDoublePoint->x);
            const long double dy = static_cast<long double>(firstDoublePoint->y) - static_cast<long double>(prevDoublePoint->y);
            perimeter += std::hypot(dx, dy);

            const double perimeterDouble = static_cast<double>(perimeter);
            if (std::isfinite(perimeterDouble))
            {
                newCache->hasPerimeter = true;
                newCache->perimeter = perimeterDouble;
            }
        }
    }

    if (numPoints >= 3)
    {
        for (size_t i = 0; i < numPoints; i++)
        {
            const ExactPoint& prevPoint = newCache->exactPoints[(i + numPoints - 1) % numPoints];
            const ExactPoint& curPoint = newCache->exactPoints[i];
            const ExactPoint& nextPoint = newCache->exactPoints[(i + 1) % numPoints];
            if (Cross(prevPoint, curPoint, nextPoint) == ExactNumber(0))
            {
                newCache->hasCollinearAdjacentTriples = true;
                break;
            }
        }
    }

    newCache->areAllVerticesCollinear = AreAllPointsCollinear(newCache->exactPoints);
    if (numPoints < 3 || newCache->hasDuplicateAdjacentVertices || newCache->areAllVerticesCollinear)
    {
        newCache->isSimple = false;
    }
    else
    {
        newCache->isSimple = IsSimpleRing(newCache->exactPoints);
    }

    if (IsValid() && newCache->hasAllFiniteDoubleVertices && bboxDoubleInitialized)
    {
        newCache->boundingBoxDouble.Set(minXDouble, minYDouble, maxXDouble, maxYDouble);
        newCache->hasBoundingBoxDouble = newCache->boundingBoxDouble.IsValid();
    }

    cache = std::shared_ptr<const ExactCacheData>(newCache);
    std::atomic_store_explicit(&exactCache, cache, std::memory_order_release);
    return cache;
}

void GB_Polygon::Clear()
{
    storageMode = CoordinateStorageMode::Double;
    vertices.clear();
    exactStringVertices.clear();
    InvalidateCaches();
}

bool GB_Polygon::SetVertices(const std::vector<GB_Point2d>& vertices)
{
    for (const GB_Point2d& vertex : vertices)
    {
        if (!vertex.IsValid())
        {
            return false;
        }
    }

    storageMode = CoordinateStorageMode::Double;
    this->vertices = vertices;
    exactStringVertices.clear();
    InvalidateCaches();
    return this->vertices.size() >= 2;
}

bool GB_Polygon::SetVertices(std::vector<GB_Point2d>&& vertices)
{
    for (const GB_Point2d& vertex : vertices)
    {
        if (!vertex.IsValid())
        {
            return false;
        }
    }

    storageMode = CoordinateStorageMode::Double;
    this->vertices = std::move(vertices);
    exactStringVertices.clear();
    InvalidateCaches();
    return this->vertices.size() >= 2;
}

bool GB_Polygon::SetVertices(const std::vector<ExactStringVertex>& exactStringVertices)
{
    std::vector<ExactStringVertex> normalizedVertices;
    normalizedVertices.reserve(exactStringVertices.size());

    for (size_t i = 0; i < exactStringVertices.size(); i++)
    {
        std::string xText = TrimAscii(exactStringVertices[i].first);
        std::string yText = TrimAscii(exactStringVertices[i].second);

        ExactNumber x = ExactNumber(0);
        ExactNumber y = ExactNumber(0);
        if (!TryParseExactNumber(xText, x) || !TryParseExactNumber(yText, y))
        {
            return false;
        }

        normalizedVertices.emplace_back(std::move(xText), std::move(yText));
    }

    storageMode = CoordinateStorageMode::ExactString;
    vertices.clear();
    this->exactStringVertices = std::move(normalizedVertices);
    InvalidateCaches();
    return this->exactStringVertices.size() >= 2;
}

bool GB_Polygon::SetVertices(std::vector<ExactStringVertex>&& exactStringVertices)
{
    for (size_t i = 0; i < exactStringVertices.size(); i++)
    {
        std::string& xText = exactStringVertices[i].first;
        std::string& yText = exactStringVertices[i].second;

        xText = TrimAscii(xText);
        yText = TrimAscii(yText);

        ExactNumber x = ExactNumber(0);
        ExactNumber y = ExactNumber(0);
        if (!TryParseExactNumber(xText, x) || !TryParseExactNumber(yText, y))
        {
            return false;
        }
    }

    storageMode = CoordinateStorageMode::ExactString;
    vertices.clear();
    this->exactStringVertices = std::move(exactStringVertices);
    InvalidateCaches();
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
    return GetNumVertices() >= 2;
}

bool GB_Polygon::IsClosed() const
{
    return IsValid();
}

size_t GB_Polygon::GetNumVertices() const
{
    return storageMode == CoordinateStorageMode::Double ? vertices.size() : exactStringVertices.size();
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
    if (storageMode == CoordinateStorageMode::Double)
    {
        return vertices;
    }

    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    if (!cache)
    {
        return std::vector<GB_Point2d>();
    }

    return cache->doubleVertices;
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

    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    if (!cache || index >= cache->doubleVertices.size())
    {
        return false;
    }

    outVertex = cache->doubleVertices[index];
    return outVertex.IsValid();
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
    if (!IsValid())
    {
        return GB_Rectangle();
    }

    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    if (!cache || !cache->hasBoundingBoxDouble)
    {
        return GB_Rectangle();
    }

    return cache->boundingBoxDouble;
}

bool GB_Polygon::TryGetBoundingBoxExactStrings(std::string& outMinXText, std::string& outMinYText, std::string& outMaxXText, std::string& outMaxYText) const
{
    outMinXText.clear();
    outMinYText.clear();
    outMaxXText.clear();
    outMaxYText.clear();

    if (!IsValid())
    {
        return false;
    }

    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    if (!cache || !cache->hasBoundingBoxExactStrings)
    {
        return false;
    }

    outMinXText = cache->minXText;
    outMinYText = cache->minYText;
    outMaxXText = cache->maxXText;
    outMaxYText = cache->maxYText;
    return true;
}

bool GB_Polygon::TryGetAxisAlignedRectangle(GB_Rectangle& outRectangle) const
{
    outRectangle.Reset();
    if (!IsValid())
    {
        return false;
    }

    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    if (!cache)
    {
        return false;
    }

    return TryBuildAxisAlignedRectangleFromRing(cache->exactPoints, outRectangle);
}

bool GB_Polygon::HasDuplicateAdjacentVertices() const
{
    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    return cache ? cache->hasDuplicateAdjacentVertices : false;
}

bool GB_Polygon::HasZeroLengthEdges() const
{
    return HasDuplicateAdjacentVertices();
}

bool GB_Polygon::HasCollinearAdjacentTriples() const
{
    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    return cache ? cache->hasCollinearAdjacentTriples : false;
}

bool GB_Polygon::AreAllVerticesCollinear() const
{
    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    return cache ? cache->areAllVerticesCollinear : false;
}

bool GB_Polygon::IsSimple() const
{
    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    return cache ? cache->isSimple : false;
}

bool GB_Polygon::HasSelfIntersections() const
{
    return IsValid() && !IsSimple();
}

GB_Polygon::Orientation GB_Polygon::GetOrientation() const
{
    if (GetNumVertices() < 2)
    {
        return Orientation::Degenerate;
    }

    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    if (!cache)
    {
        return Orientation::Degenerate;
    }

    if (cache->signedAreaTwice > ExactNumber(0))
    {
        return Orientation::CounterClockwise;
    }
    if (cache->signedAreaTwice < ExactNumber(0))
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
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    return cache ? cache->signedAreaDouble : GB_QuietNan;
}

double GB_Polygon::GetUnsignedArea() const
{
    const double signedArea = GetSignedArea();
    return std::isfinite(signedArea) ? std::abs(signedArea) : GB_QuietNan;
}

std::string GB_Polygon::GetSignedAreaExactString() const
{
    if (!IsValid())
    {
        return std::string();
    }

    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    return cache ? cache->signedAreaExactString : std::string();
}

double GB_Polygon::GetPerimeter() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    if (!cache || !cache->hasPerimeter)
    {
        return GB_QuietNan;
    }

    return cache->perimeter;
}

GB_Polygon::PointContainment GB_Polygon::ClassifyPointOddEven(const GB_Point2d& point) const
{
    if (!point.IsValid())
    {
        return PointContainment::Outside;
    }

    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    ExactPoint queryPoint;
    if (!cache || cache->exactPoints.size() < 2 || !TryBuildExactPoint(point, queryPoint))
    {
        return PointContainment::Outside;
    }

    return ClassifyPointOddEvenImpl(cache->exactPoints, queryPoint);
}

GB_Polygon::PointContainment GB_Polygon::ClassifyPointOddEven(const std::string& xText, const std::string& yText) const
{
    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    ExactPoint queryPoint;
    if (!cache || cache->exactPoints.size() < 2 || !TryBuildExactPoint(xText, yText, queryPoint))
    {
        return PointContainment::Outside;
    }

    return ClassifyPointOddEvenImpl(cache->exactPoints, queryPoint);
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

    InvalidateCaches();
}

bool GB_Polygon::ComputeIntersection(const GB_Polygon& other, std::vector<GB_Polygon>& outOuterBoundaries, std::vector<std::vector<GB_Polygon>>& outHoleBoundaries) const
{
    outOuterBoundaries.clear();
    outHoleBoundaries.clear();

#if GB_POLYGON_HAS_CGAL
    const std::shared_ptr<const ExactCacheData> firstCache = GetOrBuildExactCache();
    const std::shared_ptr<const ExactCacheData> secondCache = other.GetOrBuildExactCache();
    if (!firstCache || !secondCache)
    {
        return false;
    }

    return TryExecutePolygonBooleanOperation(firstCache->exactPoints, secondCache->exactPoints, PolygonBooleanOperation::Intersection, outOuterBoundaries, outHoleBoundaries);
#else
    (void)other;
    return false;
#endif
}

bool GB_Polygon::ComputeUnion(const GB_Polygon& other, std::vector<GB_Polygon>& outOuterBoundaries, std::vector<std::vector<GB_Polygon>>& outHoleBoundaries) const
{
    outOuterBoundaries.clear();
    outHoleBoundaries.clear();

#if GB_POLYGON_HAS_CGAL
    const std::shared_ptr<const ExactCacheData> firstCache = GetOrBuildExactCache();
    const std::shared_ptr<const ExactCacheData> secondCache = other.GetOrBuildExactCache();
    if (!firstCache || !secondCache)
    {
        return false;
    }

    return TryExecutePolygonBooleanOperation(firstCache->exactPoints, secondCache->exactPoints, PolygonBooleanOperation::Union, outOuterBoundaries, outHoleBoundaries);
#else
    (void)other;
    return false;
#endif
}

bool GB_Polygon::ComputeDifference(const GB_Polygon& other, std::vector<GB_Polygon>& outOuterBoundaries, std::vector<std::vector<GB_Polygon>>& outHoleBoundaries) const
{
    outOuterBoundaries.clear();
    outHoleBoundaries.clear();

#if GB_POLYGON_HAS_CGAL
    const std::shared_ptr<const ExactCacheData> firstCache = GetOrBuildExactCache();
    const std::shared_ptr<const ExactCacheData> secondCache = other.GetOrBuildExactCache();
    if (!firstCache || !secondCache)
    {
        return false;
    }

    return TryExecutePolygonBooleanOperation(firstCache->exactPoints, secondCache->exactPoints, PolygonBooleanOperation::Difference, outOuterBoundaries, outHoleBoundaries);
#else
    (void)other;
    return false;
#endif
}

bool GB_Polygon::ComputeNormalizedPolygons(std::vector<GB_Polygon>& outOuterBoundaries, std::vector<std::vector<GB_Polygon>>& outHoleBoundaries) const
{
    outOuterBoundaries.clear();
    outHoleBoundaries.clear();

    const std::shared_ptr<const ExactCacheData> cache = GetOrBuildExactCache();
    if (!cache)
    {
        return false;
    }

    const std::vector<ExactPoint> normalizedPoints = NormalizeExactRing(cache->exactPoints);
    if (normalizedPoints.size() < 3 || AreAllPointsCollinear(normalizedPoints))
    {
        return false;
    }

#if GB_POLYGON_HAS_CGAL
    CgalPolygon2 simplePolygon;
    if (TryBuildCgalPolygon(normalizedPoints, simplePolygon))
    {
        GB_Polygon outerBoundary = BuildPolygonFromCgalRing(simplePolygon);
        EnsurePolygonOrientation(outerBoundary, Orientation::CounterClockwise);
        outOuterBoundaries.push_back(std::move(outerBoundary));
        outHoleBoundaries.resize(1);
        return true;
    }
#endif

#if GB_POLYGON_HAS_CGAL_POLYGON_REPAIR
    CgalPolygon2 inputPolygon;
    for (size_t i = 0; i < normalizedPoints.size(); i++)
    {
        inputPolygon.push_back(CgalExactPoint(normalizedPoints[i].x, normalizedPoints[i].y));
    }

    const CgalMultipolygonWithHoles2 repairedMultipolygon = CGAL::Polygon_repair::repair(inputPolygon, CGAL::Polygon_repair::Even_odd_rule());
    return TryConvertCgalMultipolygonToGbPolygons(repairedMultipolygon, outOuterBoundaries, outHoleBoundaries);
#else
    return false;
#endif
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
    buffer.reserve(GetBinaryReserveHint(GetNumVertices()));

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
    const std::string trimmed = TrimAscii(data);
    if (trimmed.size() < 2 || trimmed.front() != '(' || trimmed.back() != ')')
    {
        return false;
    }

    const std::string body = trimmed.substr(1, trimmed.size() - 2);
    size_t offset = 0;
    std::string field;

    if (!ReadNextSerializedField(body, offset, field) || field != GetClassType())
    {
        return false;
    }

    if (!ReadNextSerializedField(body, offset, field))
    {
        return false;
    }

    size_t storageModeValue = 0;
    if (!TryParseSizeT(field, storageModeValue))
    {
        return false;
    }

    if (storageModeValue > static_cast<size_t>(CoordinateStorageMode::ExactString))
    {
        return false;
    }

    const CoordinateStorageMode parsedStorageMode = static_cast<CoordinateStorageMode>(storageModeValue);

    if (!ReadNextSerializedField(body, offset, field))
    {
        return false;
    }

    size_t numVertices = 0;
    if (!TryParseSizeT(field, numVertices))
    {
        return false;
    }

    if (numVertices == 0)
    {
        if (offset != body.size())
        {
            return false;
        }

        GB_Polygon parsedPolygon;
        if (!TrySetEmptyPolygon(parsedStorageMode, parsedPolygon))
        {
            return false;
        }

        *this = std::move(parsedPolygon);
        return true;
    }

    if (parsedStorageMode == CoordinateStorageMode::Double)
    {
        std::vector<GB_Point2d> parsedVertices;
        parsedVertices.reserve(numVertices);

        for (size_t i = 0; i < numVertices; i++)
        {
            std::string xText;
            std::string yText;
            if (!ReadNextSerializedField(body, offset, xText)
                || !ReadNextSerializedField(body, offset, yText))
            {
                return false;
            }

            double x = GB_QuietNan;
            double y = GB_QuietNan;
            if (!TryParseDoubleText(xText, x) || !TryParseDoubleText(yText, y) || !std::isfinite(x) || !std::isfinite(y))
            {
                return false;
            }

            parsedVertices.emplace_back(x, y);
        }

        if (offset != body.size())
        {
            return false;
        }

        GB_Polygon parsedPolygon;
        if (!TrySetDoubleVerticesFromDeserialization(parsedPolygon, std::move(parsedVertices)))
        {
            return false;
        }

        *this = std::move(parsedPolygon);
        return true;
    }

    std::vector<ExactStringVertex> parsedVertices;
    parsedVertices.reserve(numVertices);
    for (size_t i = 0; i < numVertices; i++)
    {
        std::string xText;
        std::string yText;
        if (!ReadNextSerializedField(body, offset, xText)
            || !ReadNextSerializedField(body, offset, yText))
        {
            return false;
        }

        parsedVertices.emplace_back(std::move(xText), std::move(yText));
    }

    if (offset != body.size())
    {
        return false;
    }

    GB_Polygon parsedPolygon;
    if (!TrySetExactStringVerticesFromDeserialization(parsedPolygon, std::move(parsedVertices)))
    {
        return false;
    }

    *this = std::move(parsedPolygon);
    return true;
}

bool GB_Polygon::Deserialize(const GB_ByteBuffer& data)
{
    constexpr static std::uint16_t supportedPayloadVersion = 1;

    size_t offset = 0;

    std::uint32_t magicNumber = 0;
    if (!ReadUInt32LE(data, offset, magicNumber) || magicNumber != GB_ClassMagicNumber)
    {
        return false;
    }

    std::uint64_t classTypeId = 0;
    if (!ReadUInt64LE(data, offset, classTypeId) || classTypeId != GetClassTypeId())
    {
        return false;
    }

    std::uint16_t payloadVersion = 0;
    if (!ReadUInt16LE(data, offset, payloadVersion) || payloadVersion != supportedPayloadVersion)
    {
        return false;
    }

    std::uint16_t reservedHeader = 0;
    if (!ReadUInt16LE(data, offset, reservedHeader))
    {
        return false;
    }

    std::uint8_t storageModeValue = 0;
    if (!ReadUInt8(data, offset, storageModeValue))
    {
        return false;
    }

    if (storageModeValue > static_cast<std::uint8_t>(CoordinateStorageMode::ExactString))
    {
        return false;
    }

    const CoordinateStorageMode parsedStorageMode = static_cast<CoordinateStorageMode>(storageModeValue);

    std::uint8_t reservedStorage = 0;
    if (!ReadUInt8(data, offset, reservedStorage))
    {
        return false;
    }

    std::uint16_t reservedTrailing = 0;
    if (!ReadUInt16LE(data, offset, reservedTrailing))
    {
        return false;
    }

    if (reservedHeader != 0 || reservedStorage != 0 || reservedTrailing != 0)
    {
        return false;
    }

    std::uint64_t numVertices64 = 0;
    if (!ReadUInt64LE(data, offset, numVertices64))
    {
        return false;
    }

    size_t numVertices = 0;
    if (!TryConvertVertexCountToSizeT(numVertices64, numVertices))
    {
        return false;
    }

    if (numVertices == 0)
    {
        if (offset != data.size())
        {
            return false;
        }
        GB_Polygon parsedPolygon;
        if (!TrySetEmptyPolygon(parsedStorageMode, parsedPolygon))
        {
            return false;
        }

        *this = std::move(parsedPolygon);
        return true;
    }

    if (parsedStorageMode == CoordinateStorageMode::Double)
    {
        std::vector<GB_Point2d> parsedVertices;
        parsedVertices.reserve(numVertices);

        for (size_t i = 0; i < numVertices; i++)
        {
            double x = GB_QuietNan;
            double y = GB_QuietNan;
            if (!ReadDoubleLE(data, offset, x) || !ReadDoubleLE(data, offset, y))
            {
                return false;
            }

            if (!std::isfinite(x) || !std::isfinite(y))
            {
                return false;
            }

            parsedVertices.emplace_back(x, y);
        }

        if (offset != data.size())
        {
            return false;
        }

        GB_Polygon parsedPolygon;
        if (!TrySetDoubleVerticesFromDeserialization(parsedPolygon, std::move(parsedVertices)))
        {
            return false;
        }

        *this = std::move(parsedPolygon);
        return true;
    }

    std::vector<ExactStringVertex> parsedVertices;
    parsedVertices.reserve(numVertices);

    for (size_t i = 0; i < numVertices; i++)
    {
        std::string xText;
        std::string yText;
        if (!ReadString(data, offset, xText) || !ReadString(data, offset, yText))
        {
            return false;
        }

        parsedVertices.emplace_back(std::move(xText), std::move(yText));
    }

    if (offset != data.size())
    {
        return false;
    }

    GB_Polygon parsedPolygon;
    if (!TrySetExactStringVerticesFromDeserialization(parsedPolygon, std::move(parsedVertices)))
    {
        return false;
    }

    *this = std::move(parsedPolygon);
    return true;
}

#ifdef _MSC_VER
#  pragma warning(pop)
#endif