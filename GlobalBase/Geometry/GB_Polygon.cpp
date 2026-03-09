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

    static bool TryGetExactPointAt(const GB_Polygon& polygon, size_t index, ExactPoint& outPoint)
    {
        if (index >= polygon.GetNumVertices())
        {
            return false;
        }

        if (polygon.GetCoordinateStorageMode() == GB_Polygon::CoordinateStorageMode::Double)
        {
            const std::vector<GB_Point2d>& doubleVertices = polygon.GetDoubleVertices();
            if (doubleVertices.size() != polygon.GetNumVertices())
            {
                return false;
            }

            return TryBuildExactPoint(doubleVertices[index], outPoint);
        }

        const std::vector<GB_Polygon::ExactStringVertex>& exactVertices = polygon.GetExactStringVertices();
        if (exactVertices.size() != polygon.GetNumVertices())
        {
            return false;
        }

        return TryBuildExactPoint(exactVertices[index].first, exactVertices[index].second, outPoint);
    }

    static bool TryGetDoublePointAt(const GB_Polygon& polygon, size_t index, GB_Point2d& outPoint)
    {
        outPoint = GB_Point2d();
        if (index >= polygon.GetNumVertices())
        {
            return false;
        }

        if (polygon.GetCoordinateStorageMode() == GB_Polygon::CoordinateStorageMode::Double)
        {
            const std::vector<GB_Point2d>& doubleVertices = polygon.GetDoubleVertices();
            if (doubleVertices.size() != polygon.GetNumVertices())
            {
                return false;
            }

            outPoint = doubleVertices[index];
            return outPoint.IsValid();
        }

        const std::vector<GB_Polygon::ExactStringVertex>& exactVertices = polygon.GetExactStringVertices();
        if (exactVertices.size() != polygon.GetNumVertices())
        {
            return false;
        }

        ExactNumber x = ExactNumber(0);
        ExactNumber y = ExactNumber(0);
        double xDouble = GB_QuietNan;
        double yDouble = GB_QuietNan;
        if (!TryParseExactNumber(exactVertices[index].first, x)
            || !TryParseExactNumber(exactVertices[index].second, y)
            || !TryConvertExactNumberToDouble(x, xDouble)
            || !TryConvertExactNumberToDouble(y, yDouble))
        {
            return false;
        }

        outPoint = GB_Point2d(xDouble, yDouble);
        return true;
    }

    static bool TryComputeSignedDoubleAreaTwice(const GB_Polygon& polygon, ExactNumber& outAreaTwice)
    {
        outAreaTwice = ExactNumber(0);

        const size_t numVertices = polygon.GetNumVertices();
        if (numVertices < 2)
        {
            return true;
        }

        ExactPoint firstPoint;
        ExactPoint prevPoint;
        if (!TryGetExactPointAt(polygon, 0, firstPoint))
        {
            return false;
        }

        prevPoint = firstPoint;
        for (size_t i = 1; i < numVertices; i++)
        {
            ExactPoint curPoint;
            if (!TryGetExactPointAt(polygon, i, curPoint))
            {
                return false;
            }

            outAreaTwice += prevPoint.x() * curPoint.y() - curPoint.x() * prevPoint.y();
            prevPoint = curPoint;
        }

        outAreaTwice += prevPoint.x() * firstPoint.y() - firstPoint.x() * prevPoint.y();
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

        if (length > static_cast<std::uint64_t>(buffer.size() - offset) || length > static_cast<std::uint64_t>(std::numeric_limits<size_t>::max()))
        {
            outText.clear();
            return false;
        }

        outText.assign(reinterpret_cast<const char*>(buffer.data() + offset), static_cast<size_t>(length));
        offset += static_cast<size_t>(length);
        return true;
    }

    static bool TryComputeSerializedStringTokenCount(size_t numVertices, size_t& outTokenCount)
    {
        if (numVertices > (std::numeric_limits<size_t>::max() - 3) / 2)
        {
            outTokenCount = 0;
            return false;
        }

        outTokenCount = 3 + numVertices * 2;
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

        if (storageMode == GB_Polygon::CoordinateStorageMode::ExactString)
        {
            outPolygon.SetVertices(std::vector<GB_Polygon::ExactStringVertex>());
            return true;
        }

        return false;
    }

    static long double HypotLongDouble(long double dx, long double dy)
    {
        return std::hypot(dx, dy);
    }

    static GB_Polygon::PointContainment ClassifyPointOddEvenImpl(const std::vector<ExactPoint>& polygonPoints, const ExactPoint& queryPoint)
    {
        bool isInside = false;
        for (size_t i = 0; i < polygonPoints.size(); i++)
        {
            const ExactPoint& a = polygonPoints[i];
            const ExactPoint& b = polygonPoints[(i + 1) % polygonPoints.size()];

            if (IsPointOnSegment(queryPoint, a, b))
            {
                return GB_Polygon::PointContainment::OnBoundary;
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

        return isInside ? GB_Polygon::PointContainment::Inside : GB_Polygon::PointContainment::Outside;
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

    for (const GB_Point2d& vertex : vertices)
    {
        if (!vertex.IsValid())
        {
            Clear();
            return false;
        }
    }

    this->vertices = vertices;
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
    return GetNumVertices() >= 2;
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
    if (storageMode == CoordinateStorageMode::Double)
    {
        return vertices;
    }

    std::vector<GB_Point2d> result;
    const size_t numVertices = GetNumVertices();
    result.reserve(numVertices);

    for (size_t i = 0; i < numVertices; i++)
    {
        GB_Point2d vertexAsDouble;
        if (TryGetDoublePointAt(*this, i, vertexAsDouble))
        {
            result.emplace_back(vertexAsDouble);
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

    return TryGetDoublePointAt(*this, index, outVertex);
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

    const size_t numVertices = GetNumVertices();
    if (numVertices == 0)
    {
        return false;
    }

    ExactPoint point;
    if (!TryGetExactPointAt(*this, 0, point))
    {
        return false;
    }

    ExactNumber minX = point.x();
    ExactNumber minY = point.y();
    ExactNumber maxX = point.x();
    ExactNumber maxY = point.y();

    if (storageMode == CoordinateStorageMode::ExactString)
    {
        outMinXText = exactStringVertices[0].first;
        outMinYText = exactStringVertices[0].second;
        outMaxXText = exactStringVertices[0].first;
        outMaxYText = exactStringVertices[0].second;
    }
    else
    {
        outMinXText = DoubleToString(vertices[0].x);
        outMinYText = DoubleToString(vertices[0].y);
        outMaxXText = DoubleToString(vertices[0].x);
        outMaxYText = DoubleToString(vertices[0].y);
    }

    for (size_t i = 1; i < numVertices; i++)
    {
        if (!TryGetExactPointAt(*this, i, point))
        {
            outMinXText.clear();
            outMinYText.clear();
            outMaxXText.clear();
            outMaxYText.clear();
            return false;
        }

        const std::string xText = (storageMode == CoordinateStorageMode::ExactString)
            ? exactStringVertices[i].first
            : DoubleToString(vertices[i].x);
        const std::string yText = (storageMode == CoordinateStorageMode::ExactString)
            ? exactStringVertices[i].second
            : DoubleToString(vertices[i].y);

        if (point.x() < minX)
        {
            minX = point.x();
            outMinXText = xText;
        }
        if (point.y() < minY)
        {
            minY = point.y();
            outMinYText = yText;
        }
        if (point.x() > maxX)
        {
            maxX = point.x();
            outMaxXText = xText;
        }
        if (point.y() > maxY)
        {
            maxY = point.y();
            outMaxYText = yText;
        }
    }

    return true;
}

bool GB_Polygon::HasDuplicateAdjacentVertices() const
{
    const size_t numVertices = GetNumVertices();
    if (numVertices < 2)
    {
        return false;
    }

    ExactPoint firstPoint;
    ExactPoint prevPoint;
    if (!TryGetExactPointAt(*this, 0, firstPoint))
    {
        return false;
    }

    prevPoint = firstPoint;
    for (size_t i = 1; i < numVertices; i++)
    {
        ExactPoint curPoint;
        if (!TryGetExactPointAt(*this, i, curPoint))
        {
            return false;
        }

        if (curPoint == prevPoint)
        {
            return true;
        }

        prevPoint = curPoint;
    }

    return prevPoint == firstPoint;
}

bool GB_Polygon::HasZeroLengthEdges() const
{
    return HasDuplicateAdjacentVertices();
}

bool GB_Polygon::HasCollinearAdjacentTriples() const
{
    const size_t numVertices = GetNumVertices();
    if (numVertices < 3)
    {
        return false;
    }

    ExactPoint firstPoint;
    ExactPoint secondPoint;
    if (!TryGetExactPointAt(*this, 0, firstPoint) || !TryGetExactPointAt(*this, 1, secondPoint))
    {
        return false;
    }

    ExactPoint prevPrevPoint = firstPoint;
    ExactPoint prevPoint = secondPoint;
    for (size_t i = 2; i < numVertices; i++)
    {
        ExactPoint curPoint;
        if (!TryGetExactPointAt(*this, i, curPoint))
        {
            return false;
        }

        if (Cross(prevPrevPoint, prevPoint, curPoint) == ExactNumber(0))
        {
            return true;
        }

        prevPrevPoint = prevPoint;
        prevPoint = curPoint;
    }

    return Cross(prevPrevPoint, prevPoint, firstPoint) == ExactNumber(0)
        || Cross(prevPoint, firstPoint, secondPoint) == ExactNumber(0);
}

bool GB_Polygon::AreAllVerticesCollinear() const
{
    const size_t numVertices = GetNumVertices();
    if (numVertices <= 2)
    {
        return true;
    }

    ExactPoint firstPoint;
    if (!TryGetExactPointAt(*this, 0, firstPoint))
    {
        return false;
    }

    size_t secondIndex = 1;
    ExactPoint secondPoint;
    while (secondIndex < numVertices)
    {
        if (!TryGetExactPointAt(*this, secondIndex, secondPoint))
        {
            return false;
        }

        if (secondPoint != firstPoint)
        {
            break;
        }

        secondIndex++;
    }

    if (secondIndex >= numVertices)
    {
        return true;
    }

    for (size_t i = secondIndex + 1; i < numVertices; i++)
    {
        ExactPoint curPoint;
        if (!TryGetExactPointAt(*this, i, curPoint))
        {
            return false;
        }

        if (Cross(firstPoint, secondPoint, curPoint) != ExactNumber(0))
        {
            return false;
        }
    }

    return true;
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
    return IsValid() && !IsSimple();
}

GB_Polygon::Orientation GB_Polygon::GetOrientation() const
{
    ExactNumber areaTwice = ExactNumber(0);
    if (!TryComputeSignedDoubleAreaTwice(*this, areaTwice) || GetNumVertices() < 2)
    {
        return Orientation::Degenerate;
    }

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
    ExactNumber areaTwice = ExactNumber(0);
    if (!TryComputeSignedDoubleAreaTwice(*this, areaTwice))
    {
        return GB_QuietNan;
    }

    const ExactNumber area = areaTwice / ExactNumber(2);
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
    ExactNumber areaTwice = ExactNumber(0);
    if (!TryComputeSignedDoubleAreaTwice(*this, areaTwice))
    {
        return "";
    }

    return ExactNumberToString(areaTwice / ExactNumber(2));
}

double GB_Polygon::GetPerimeter() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const size_t numVertices = GetNumVertices();
    if (numVertices < 2)
    {
        return GB_QuietNan;
    }

    GB_Point2d firstPoint;
    GB_Point2d prevPoint;
    if (!TryGetDoublePointAt(*this, 0, firstPoint))
    {
        return GB_QuietNan;
    }

    prevPoint = firstPoint;
    long double perimeter = 0.0L;
    for (size_t i = 1; i < numVertices; i++)
    {
        GB_Point2d curPoint;
        if (!TryGetDoublePointAt(*this, i, curPoint))
        {
            return GB_QuietNan;
        }

        const long double dx = static_cast<long double>(curPoint.x) - static_cast<long double>(prevPoint.x);
        const long double dy = static_cast<long double>(curPoint.y) - static_cast<long double>(prevPoint.y);
        perimeter += HypotLongDouble(dx, dy);
        prevPoint = curPoint;
    }

    {
        const long double dx = static_cast<long double>(firstPoint.x) - static_cast<long double>(prevPoint.x);
        const long double dy = static_cast<long double>(firstPoint.y) - static_cast<long double>(prevPoint.y);
        perimeter += HypotLongDouble(dx, dy);
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

    return ClassifyPointOddEvenImpl(polygonPoints, queryPoint);
}

GB_Polygon::PointContainment GB_Polygon::ClassifyPointOddEven(const std::string& xText, const std::string& yText) const
{
    std::vector<ExactPoint> polygonPoints;
    ExactPoint queryPoint;
    if (!TryBuildExactPoints(*this, polygonPoints) || polygonPoints.size() < 2 || !TryBuildExactPoint(xText, yText, queryPoint))
    {
        return PointContainment::Outside;
    }

    return ClassifyPointOddEvenImpl(polygonPoints, queryPoint);
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

    size_t expectedTokenCount = 0;
    if (!TryComputeSerializedStringTokenCount(numVertices, expectedTokenCount) || tokens.size() != expectedTokenCount)
    {
        Clear();
        return false;
    }

    if (storageModeValue == static_cast<size_t>(CoordinateStorageMode::Double))
    {
        if (numVertices == 0)
        {
            return TrySetEmptyPolygon(CoordinateStorageMode::Double, *this);
        }

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
        if (numVertices == 0)
        {
            return TrySetEmptyPolygon(CoordinateStorageMode::ExactString, *this);
        }

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
        if (numVertices == 0)
        {
            return offset == data.size() && TrySetEmptyPolygon(CoordinateStorageMode::Double, *this);
        }

        size_t numVerticesAsSizeT = 0;
        if (!TryConvertVertexCountToSizeT(numVertices, numVerticesAsSizeT))
        {
            Clear();
            return false;
        }

        const size_t remainingBytes = data.size() - offset;
        if (numVerticesAsSizeT > remainingBytes / (sizeof(double) * 2))
        {
            Clear();
            return false;
        }

        std::vector<GB_Point2d> parsedVertices;
        parsedVertices.reserve(numVerticesAsSizeT);
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
        if (numVertices == 0)
        {
            return offset == data.size() && TrySetEmptyPolygon(CoordinateStorageMode::ExactString, *this);
        }

        size_t numVerticesAsSizeT = 0;
        if (!TryConvertVertexCountToSizeT(numVertices, numVerticesAsSizeT))
        {
            Clear();
            return false;
        }

        std::vector<ExactStringVertex> parsedVertices;
        parsedVertices.reserve(numVerticesAsSizeT);
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