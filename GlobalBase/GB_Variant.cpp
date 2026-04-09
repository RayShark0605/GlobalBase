#include "GB_Variant.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace
{
    static const unsigned char kMagic0 = 'G';
    static const unsigned char kMagic1 = 'B';
    static const unsigned char kMagic2 = 'V';
    static const unsigned char kMagic3 = 'R';
    static const unsigned short kCurrentVersion = 1;

    void SetSuccessFlag(bool* ok, const bool value) noexcept
    {
        if (ok != nullptr)
        {
            *ok = value;
        }
    }

    bool IsAsciiSpace(const unsigned char ch) noexcept
    {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
    }

    std::string TrimAscii(const std::string& text)
    {
        std::size_t beginIndex = 0;
        std::size_t endIndex = text.size();

        while (beginIndex < endIndex && IsAsciiSpace(static_cast<unsigned char>(text[beginIndex])))
        {
            beginIndex++;
        }

        while (endIndex > beginIndex && IsAsciiSpace(static_cast<unsigned char>(text[endIndex - 1])))
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    std::string ToLowerAscii(std::string text)
    {
        for (char& ch : text)
        {
            if (ch >= 'A' && ch <= 'Z')
            {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }

        return text;
    }

    template<typename TValue>
    std::string IntegerToString(const TValue value)
    {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    }

    template<typename TValue>
    std::string FloatingPointToString(const TValue value)
    {
        std::ostringstream stream;
        stream << std::setprecision(std::numeric_limits<TValue>::max_digits10) << value;
        return stream.str();
    }

    template<typename TValue>
    bool TryParseSignedInteger(const std::string& text, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value && std::is_signed<TValue>::value,
            "TValue must be a signed integer type.");

        const std::string trimmedText = TrimAscii(text);
        if (trimmedText.empty())
        {
            return false;
        }

        errno = 0;
        char* endPtr = nullptr;
        const long long parsedValue = std::strtoll(trimmedText.c_str(), &endPtr, 0);
        if (errno == ERANGE || endPtr == nullptr)
        {
            return false;
        }

        while (*endPtr != '\0')
        {
            if (!IsAsciiSpace(static_cast<unsigned char>(*endPtr)))
            {
                return false;
            }
            endPtr++;
        }

        if (parsedValue < static_cast<long long>(std::numeric_limits<TValue>::min())
            || parsedValue > static_cast<long long>(std::numeric_limits<TValue>::max()))
        {
            return false;
        }

        outValue = static_cast<TValue>(parsedValue);
        return true;
    }

    template<typename TValue>
    bool TryParseUnsignedInteger(const std::string& text, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value && !std::is_signed<TValue>::value,
            "TValue must be an unsigned integer type.");

        const std::string trimmedText = TrimAscii(text);
        if (trimmedText.empty())
        {
            return false;
        }

        if (trimmedText[0] == '-')
        {
            return false;
        }

        errno = 0;
        char* endPtr = nullptr;
        const unsigned long long parsedValue = std::strtoull(trimmedText.c_str(), &endPtr, 0);
        if (errno == ERANGE || endPtr == nullptr)
        {
            return false;
        }

        while (*endPtr != '\0')
        {
            if (!IsAsciiSpace(static_cast<unsigned char>(*endPtr)))
            {
                return false;
            }
            endPtr++;
        }

        if (parsedValue > static_cast<unsigned long long>(std::numeric_limits<TValue>::max()))
        {
            return false;
        }

        outValue = static_cast<TValue>(parsedValue);
        return true;
    }

    template<typename TValue>
    bool TryParseFloatingPoint(const std::string& text, TValue& outValue) noexcept
    {
        static_assert(std::is_floating_point<TValue>::value, "TValue must be a floating-point type.");

        const std::string trimmedText = TrimAscii(text);
        if (trimmedText.empty())
        {
            return false;
        }

        errno = 0;
        char* endPtr = nullptr;
        const long double parsedValue = std::strtold(trimmedText.c_str(), &endPtr);
        if (errno == ERANGE || endPtr == nullptr)
        {
            return false;
        }

        while (*endPtr != '\0')
        {
            if (!IsAsciiSpace(static_cast<unsigned char>(*endPtr)))
            {
                return false;
            }
            endPtr++;
        }

        if (parsedValue < -static_cast<long double>(std::numeric_limits<TValue>::max())
            || parsedValue > static_cast<long double>(std::numeric_limits<TValue>::max()))
        {
            return false;
        }

        outValue = static_cast<TValue>(parsedValue);
        return true;
    }

    template<typename TValue>
    bool ConvertFloatingPointToSignedInteger(const TValue value, long long& outValue) noexcept
    {
        if (!std::isfinite(static_cast<long double>(value)))
        {
            return false;
        }

        const long double truncatedValue = std::trunc(static_cast<long double>(value));
        if (truncatedValue < static_cast<long double>(std::numeric_limits<long long>::min())
            || truncatedValue > static_cast<long double>(std::numeric_limits<long long>::max()))
        {
            return false;
        }

        outValue = static_cast<long long>(truncatedValue);
        return true;
    }

    template<typename TValue>
    bool ConvertFloatingPointToUnsignedInteger(const TValue value, unsigned long long& outValue) noexcept
    {
        if (!std::isfinite(static_cast<long double>(value)))
        {
            return false;
        }

        const long double truncatedValue = std::trunc(static_cast<long double>(value));
        if (truncatedValue < 0.0L
            || truncatedValue > static_cast<long double>(std::numeric_limits<unsigned long long>::max()))
        {
            return false;
        }

        outValue = static_cast<unsigned long long>(truncatedValue);
        return true;
    }

    template<typename TValue>
    bool ConvertSignedIntegerToSignedInteger(const long long value, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value && std::is_signed<TValue>::value,
            "TValue must be a signed integer type.");

        if (value < static_cast<long long>(std::numeric_limits<TValue>::min())
            || value > static_cast<long long>(std::numeric_limits<TValue>::max()))
        {
            return false;
        }

        outValue = static_cast<TValue>(value);
        return true;
    }

    template<typename TValue>
    bool ConvertUnsignedIntegerToUnsignedInteger(const unsigned long long value, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value && !std::is_signed<TValue>::value,
            "TValue must be an unsigned integer type.");

        if (value > static_cast<unsigned long long>(std::numeric_limits<TValue>::max()))
        {
            return false;
        }

        outValue = static_cast<TValue>(value);
        return true;
    }

    std::string BinaryToHexString(const GB_ByteBuffer& data)
    {
        static const char* kDigits = "0123456789ABCDEF";

        std::string result;
        result.reserve(data.size() * 2);
        for (const unsigned char byteValue : data)
        {
            result.push_back(kDigits[(byteValue >> 4) & 0x0F]);
            result.push_back(kDigits[byteValue & 0x0F]);
        }

        return result;
    }

    void WriteUInt16(GB_ByteBuffer& data, const unsigned short value)
    {
        data.push_back(static_cast<unsigned char>(value & 0xFF));
        data.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    }

    void WriteUInt32(GB_ByteBuffer& data, const unsigned int value)
    {
        data.push_back(static_cast<unsigned char>(value & 0xFF));
        data.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
        data.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
        data.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
    }

    void WriteUInt64(GB_ByteBuffer& data, const unsigned long long value)
    {
        for (int index = 0; index < 8; index++)
        {
            data.push_back(static_cast<unsigned char>((value >> (index * 8)) & 0xFF));
        }
    }

    bool ReadUInt16(const GB_ByteBuffer& data, std::size_t& offset, unsigned short& outValue) noexcept
    {
        if (offset + 2 > data.size())
        {
            return false;
        }

        outValue = static_cast<unsigned short>(data[offset])
            | static_cast<unsigned short>(static_cast<unsigned short>(data[offset + 1]) << 8);
        offset += 2;
        return true;
    }

    bool ReadUInt32(const GB_ByteBuffer& data, std::size_t& offset, unsigned int& outValue) noexcept
    {
        if (offset + 4 > data.size())
        {
            return false;
        }

        outValue = static_cast<unsigned int>(data[offset])
            | static_cast<unsigned int>(static_cast<unsigned int>(data[offset + 1]) << 8)
            | static_cast<unsigned int>(static_cast<unsigned int>(data[offset + 2]) << 16)
            | static_cast<unsigned int>(static_cast<unsigned int>(data[offset + 3]) << 24);
        offset += 4;
        return true;
    }

    bool ReadUInt64(const GB_ByteBuffer& data, std::size_t& offset, unsigned long long& outValue) noexcept
    {
        if (offset + 8 > data.size())
        {
            return false;
        }

        outValue = 0;
        for (int index = 0; index < 8; index++)
        {
            outValue |= static_cast<unsigned long long>(data[offset + index]) << (index * 8);
        }

        offset += 8;
        return true;
    }

    bool ReadBytes(const GB_ByteBuffer& data,
        std::size_t& offset,
        const std::size_t length,
        GB_ByteBuffer& outBytes) noexcept
    {
        if (offset + length > data.size())
        {
            return false;
        }

        try
        {
            outBytes.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                data.begin() + static_cast<std::ptrdiff_t>(offset + length));
        }
        catch (...)
        {
            return false;
        }

        offset += length;
        return true;
    }

    bool ReadString(const GB_ByteBuffer& data,
        std::size_t& offset,
        const std::size_t length,
        std::string& outText) noexcept
    {
        if (offset + length > data.size())
        {
            return false;
        }

        try
        {
            outText.assign(reinterpret_cast<const char*>(&data[offset]), length);
        }
        catch (...)
        {
            return false;
        }

        offset += length;
        return true;
    }

    template<typename TValue>
    bool TryGetExactValue(const GB_Variant& variant, TValue& outValue) noexcept
    {
        const TValue* value = variant.AnyCast<TValue>();
        if (value == nullptr)
        {
            return false;
        }

        outValue = *value;
        return true;
    }

    bool TryGetSignedValue(const GB_Variant& variant, long long& outValue) noexcept
    {
        bool boolValue = false;
        if (TryGetExactValue(variant, boolValue))
        {
            outValue = boolValue ? 1LL : 0LL;
            return true;
        }

        char charValue = 0;
        if (TryGetExactValue(variant, charValue))
        {
            outValue = static_cast<long long>(charValue);
            return true;
        }

        signed char signedCharValue = 0;
        if (TryGetExactValue(variant, signedCharValue))
        {
            outValue = static_cast<long long>(signedCharValue);
            return true;
        }

        short shortValue = 0;
        if (TryGetExactValue(variant, shortValue))
        {
            outValue = static_cast<long long>(shortValue);
            return true;
        }

        int intValue = 0;
        if (TryGetExactValue(variant, intValue))
        {
            outValue = static_cast<long long>(intValue);
            return true;
        }

        long longValue = 0;
        if (TryGetExactValue(variant, longValue))
        {
            outValue = static_cast<long long>(longValue);
            return true;
        }

        long long longLongValue = 0;
        if (TryGetExactValue(variant, longLongValue))
        {
            outValue = longLongValue;
            return true;
        }

        unsigned char unsignedCharValue = 0;
        if (TryGetExactValue(variant, unsignedCharValue))
        {
            outValue = static_cast<long long>(unsignedCharValue);
            return true;
        }

        unsigned short unsignedShortValue = 0;
        if (TryGetExactValue(variant, unsignedShortValue))
        {
            outValue = static_cast<long long>(unsignedShortValue);
            return true;
        }

        unsigned int unsignedIntValue = 0;
        if (TryGetExactValue(variant, unsignedIntValue))
        {
            if (static_cast<unsigned long long>(unsignedIntValue)
                > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
            {
                return false;
            }

            outValue = static_cast<long long>(unsignedIntValue);
            return true;
        }

        unsigned long unsignedLongValue = 0;
        if (TryGetExactValue(variant, unsignedLongValue))
        {
            if (static_cast<unsigned long long>(unsignedLongValue)
                > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
            {
                return false;
            }

            outValue = static_cast<long long>(unsignedLongValue);
            return true;
        }

        unsigned long long unsignedLongLongValue = 0;
        if (TryGetExactValue(variant, unsignedLongLongValue))
        {
            if (unsignedLongLongValue > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
            {
                return false;
            }

            outValue = static_cast<long long>(unsignedLongLongValue);
            return true;
        }

        float floatValue = 0.0f;
        if (TryGetExactValue(variant, floatValue))
        {
            return ConvertFloatingPointToSignedInteger(floatValue, outValue);
        }

        double doubleValue = 0.0;
        if (TryGetExactValue(variant, doubleValue))
        {
            return ConvertFloatingPointToSignedInteger(doubleValue, outValue);
        }

        long double longDoubleValue = 0.0L;
        if (TryGetExactValue(variant, longDoubleValue))
        {
            return ConvertFloatingPointToSignedInteger(longDoubleValue, outValue);
        }

        std::string stringValue;
        if (TryGetExactValue(variant, stringValue))
        {
            return TryParseSignedInteger(stringValue, outValue);
        }

        return false;
    }

    bool TryGetUnsignedValue(const GB_Variant& variant, unsigned long long& outValue) noexcept
    {
        bool boolValue = false;
        if (TryGetExactValue(variant, boolValue))
        {
            outValue = boolValue ? 1ULL : 0ULL;
            return true;
        }

        unsigned char unsignedCharValue = 0;
        if (TryGetExactValue(variant, unsignedCharValue))
        {
            outValue = static_cast<unsigned long long>(unsignedCharValue);
            return true;
        }

        unsigned short unsignedShortValue = 0;
        if (TryGetExactValue(variant, unsignedShortValue))
        {
            outValue = static_cast<unsigned long long>(unsignedShortValue);
            return true;
        }

        unsigned int unsignedIntValue = 0;
        if (TryGetExactValue(variant, unsignedIntValue))
        {
            outValue = static_cast<unsigned long long>(unsignedIntValue);
            return true;
        }

        unsigned long unsignedLongValue = 0;
        if (TryGetExactValue(variant, unsignedLongValue))
        {
            outValue = static_cast<unsigned long long>(unsignedLongValue);
            return true;
        }

        unsigned long long unsignedLongLongValue = 0;
        if (TryGetExactValue(variant, unsignedLongLongValue))
        {
            outValue = unsignedLongLongValue;
            return true;
        }

        char charValue = 0;
        if (TryGetExactValue(variant, charValue))
        {
            if (charValue < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(charValue);
            return true;
        }

        signed char signedCharValue = 0;
        if (TryGetExactValue(variant, signedCharValue))
        {
            if (signedCharValue < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(signedCharValue);
            return true;
        }

        short shortValue = 0;
        if (TryGetExactValue(variant, shortValue))
        {
            if (shortValue < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(shortValue);
            return true;
        }

        int intValue = 0;
        if (TryGetExactValue(variant, intValue))
        {
            if (intValue < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(intValue);
            return true;
        }

        long longValue = 0;
        if (TryGetExactValue(variant, longValue))
        {
            if (longValue < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(longValue);
            return true;
        }

        long long longLongValue = 0;
        if (TryGetExactValue(variant, longLongValue))
        {
            if (longLongValue < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(longLongValue);
            return true;
        }

        float floatValue = 0.0f;
        if (TryGetExactValue(variant, floatValue))
        {
            return ConvertFloatingPointToUnsignedInteger(floatValue, outValue);
        }

        double doubleValue = 0.0;
        if (TryGetExactValue(variant, doubleValue))
        {
            return ConvertFloatingPointToUnsignedInteger(doubleValue, outValue);
        }

        long double longDoubleValue = 0.0L;
        if (TryGetExactValue(variant, longDoubleValue))
        {
            return ConvertFloatingPointToUnsignedInteger(longDoubleValue, outValue);
        }

        std::string stringValue;
        if (TryGetExactValue(variant, stringValue))
        {
            return TryParseUnsignedInteger(stringValue, outValue);
        }

        return false;
    }

    bool TryGetFloatingValue(const GB_Variant& variant, long double& outValue) noexcept
    {
        float floatValue = 0.0f;
        if (TryGetExactValue(variant, floatValue))
        {
            outValue = static_cast<long double>(floatValue);
            return true;
        }

        double doubleValue = 0.0;
        if (TryGetExactValue(variant, doubleValue))
        {
            outValue = static_cast<long double>(doubleValue);
            return true;
        }

        long double longDoubleValue = 0.0L;
        if (TryGetExactValue(variant, longDoubleValue))
        {
            outValue = longDoubleValue;
            return true;
        }

        bool boolValue = false;
        if (TryGetExactValue(variant, boolValue))
        {
            outValue = boolValue ? 1.0L : 0.0L;
            return true;
        }

        long long signedValue = 0;
        if (TryGetSignedValue(variant, signedValue))
        {
            outValue = static_cast<long double>(signedValue);
            return true;
        }

        unsigned long long unsignedValue = 0;
        if (TryGetUnsignedValue(variant, unsignedValue))
        {
            outValue = static_cast<long double>(unsignedValue);
            return true;
        }

        std::string stringValue;
        if (TryGetExactValue(variant, stringValue))
        {
            return TryParseFloatingPoint(stringValue, outValue);
        }

        return false;
    }

    bool ReadExactInteger(const GB_ByteBuffer& payload,
        const std::size_t expectedSize,
        unsigned long long& outValue) noexcept
    {
        if (payload.size() != expectedSize)
        {
            return false;
        }

        outValue = 0;
        for (std::size_t index = 0; index < expectedSize; index++)
        {
            outValue |= static_cast<unsigned long long>(payload[index]) << (index * 8);
        }

        return true;
    }

    template<typename TValue>
    bool DeserializeFloatingPointValue(const GB_ByteBuffer& payload, TValue& outValue) noexcept
    {
        if (payload.size() != sizeof(TValue))
        {
            return false;
        }

        std::memcpy(&outValue, payload.data(), sizeof(TValue));
        return true;
    }
}

template<typename TValue>
typename std::enable_if<std::is_integral<typename std::decay<TValue>::type>::value, bool>::type
GB_Variant::SerializeBuiltinValue(const TValue& value, GB_ByteBuffer& outData) noexcept
{
    typedef typename std::decay<TValue>::type ValueType;

    try
    {
        outData.clear();
        outData.reserve(sizeof(ValueType));
        for (std::size_t index = 0; index < sizeof(ValueType); index++)
        {
            const unsigned long long shiftedValue = static_cast<unsigned long long>(value);
            outData.push_back(static_cast<unsigned char>((shiftedValue >> (index * 8)) & 0xFF));
        }
    }
    catch (...)
    {
        outData.clear();
        return false;
    }

    return true;
}

template<typename TValue>
typename std::enable_if<std::is_floating_point<typename std::decay<TValue>::type>::value, bool>::type
GB_Variant::SerializeBuiltinValue(const TValue& value, GB_ByteBuffer& outData) noexcept
{
    typedef typename std::decay<TValue>::type ValueType;

    try
    {
        outData.resize(sizeof(ValueType));
        std::memcpy(outData.data(), &value, sizeof(ValueType));
    }
    catch (...)
    {
        outData.clear();
        return false;
    }

    return true;
}

bool GB_Variant::SerializeBuiltinValue(const std::string& value, GB_ByteBuffer& outData) noexcept
{
    try
    {
        outData.assign(reinterpret_cast<const unsigned char*>(value.data()),
            reinterpret_cast<const unsigned char*>(value.data()) + value.size());
    }
    catch (...)
    {
        outData.clear();
        return false;
    }

    return true;
}

bool GB_Variant::SerializeBuiltinValue(const GB_ByteBuffer& value, GB_ByteBuffer& outData) noexcept
{
    try
    {
        outData = value;
    }
    catch (...)
    {
        outData.clear();
        return false;
    }

    return true;
}

bool GB_Variant::DeserializeBuiltinValue(const std::string& stableTypeName,
    const GB_ByteBuffer& payload,
    HolderBase*& outHolder) noexcept
{
    outHolder = nullptr;

    try
    {
        if (stableTypeName == "bool")
        {
            if (payload.size() != 1)
            {
                return false;
            }

            outHolder = new Holder<bool>(payload[0] != 0);
            return true;
        }

        if (stableTypeName == "char")
        {
            if (payload.size() != sizeof(char))
            {
                return false;
            }

            outHolder = new Holder<char>(static_cast<char>(payload[0]));
            return true;
        }

        if (stableTypeName == "signed char")
        {
            if (payload.size() != sizeof(signed char))
            {
                return false;
            }

            outHolder = new Holder<signed char>(static_cast<signed char>(payload[0]));
            return true;
        }

        if (stableTypeName == "unsigned char")
        {
            if (payload.size() != sizeof(unsigned char))
            {
                return false;
            }

            outHolder = new Holder<unsigned char>(static_cast<unsigned char>(payload[0]));
            return true;
        }

        if (stableTypeName == "short")
        {
            unsigned long long rawValue = 0;
            if (!ReadExactInteger(payload, sizeof(short), rawValue))
            {
                return false;
            }

            outHolder = new Holder<short>(static_cast<short>(rawValue));
            return true;
        }

        if (stableTypeName == "unsigned short")
        {
            unsigned long long rawValue = 0;
            if (!ReadExactInteger(payload, sizeof(unsigned short), rawValue))
            {
                return false;
            }

            outHolder = new Holder<unsigned short>(static_cast<unsigned short>(rawValue));
            return true;
        }

        if (stableTypeName == "int")
        {
            unsigned long long rawValue = 0;
            if (!ReadExactInteger(payload, sizeof(int), rawValue))
            {
                return false;
            }

            outHolder = new Holder<int>(static_cast<int>(rawValue));
            return true;
        }

        if (stableTypeName == "unsigned int")
        {
            unsigned long long rawValue = 0;
            if (!ReadExactInteger(payload, sizeof(unsigned int), rawValue))
            {
                return false;
            }

            outHolder = new Holder<unsigned int>(static_cast<unsigned int>(rawValue));
            return true;
        }

        if (stableTypeName == "long")
        {
            unsigned long long rawValue = 0;
            if (!ReadExactInteger(payload, sizeof(long), rawValue))
            {
                return false;
            }

            outHolder = new Holder<long>(static_cast<long>(rawValue));
            return true;
        }

        if (stableTypeName == "unsigned long")
        {
            unsigned long long rawValue = 0;
            if (!ReadExactInteger(payload, sizeof(unsigned long), rawValue))
            {
                return false;
            }

            outHolder = new Holder<unsigned long>(static_cast<unsigned long>(rawValue));
            return true;
        }

        if (stableTypeName == "long long")
        {
            unsigned long long rawValue = 0;
            if (!ReadExactInteger(payload, sizeof(long long), rawValue))
            {
                return false;
            }

            outHolder = new Holder<long long>(static_cast<long long>(rawValue));
            return true;
        }

        if (stableTypeName == "unsigned long long")
        {
            unsigned long long rawValue = 0;
            if (!ReadExactInteger(payload, sizeof(unsigned long long), rawValue))
            {
                return false;
            }

            outHolder = new Holder<unsigned long long>(static_cast<unsigned long long>(rawValue));
            return true;
        }

        if (stableTypeName == "float")
        {
            float value = 0.0f;
            if (!DeserializeFloatingPointValue(payload, value))
            {
                return false;
            }

            outHolder = new Holder<float>(value);
            return true;
        }

        if (stableTypeName == "double")
        {
            double value = 0.0;
            if (!DeserializeFloatingPointValue(payload, value))
            {
                return false;
            }

            outHolder = new Holder<double>(value);
            return true;
        }

        if (stableTypeName == "long double")
        {
            long double value = 0.0L;
            if (!DeserializeFloatingPointValue(payload, value))
            {
                return false;
            }

            outHolder = new Holder<long double>(value);
            return true;
        }

        if (stableTypeName == "std::string")
        {
            outHolder = new Holder<std::string>(GB_ByteBufferToString(payload));
            return true;
        }

        if (stableTypeName == "GB_ByteBuffer")
        {
            outHolder = new Holder<GB_ByteBuffer>(payload);
            return true;
        }
    }
    catch (...)
    {
        delete outHolder;
        outHolder = nullptr;
        return false;
    }

    return false;
}

GB_Variant::GB_Variant()
    : holder_(nullptr)
{
}

GB_Variant::GB_Variant(std::nullptr_t)
    : holder_(nullptr)
{
}

GB_Variant::GB_Variant(const char* value)
    : holder_(nullptr)
{
    if (value != nullptr)
    {
        holder_ = new Holder<std::string>(std::string(value));
    }
}

GB_Variant::GB_Variant(char* value)
    : holder_(nullptr)
{
    if (value != nullptr)
    {
        holder_ = new Holder<std::string>(std::string(value));
    }
}

GB_Variant::GB_Variant(const std::string& value)
    : holder_(new Holder<std::string>(value))
{
}

GB_Variant::GB_Variant(std::string&& value)
    : holder_(new Holder<std::string>(std::move(value)))
{
}

GB_Variant::GB_Variant(const GB_ByteBuffer& value)
    : holder_(new Holder<GB_ByteBuffer>(value))
{
}

GB_Variant::GB_Variant(GB_ByteBuffer&& value)
    : holder_(new Holder<GB_ByteBuffer>(std::move(value)))
{
}

GB_Variant::GB_Variant(const GB_Variant& other)
    : holder_(nullptr)
{
    if (other.holder_ != nullptr)
    {
        holder_ = other.holder_->Clone();
    }
}

GB_Variant::GB_Variant(GB_Variant&& other) noexcept
    : holder_(other.holder_)
{
    other.holder_ = nullptr;
}

GB_Variant::~GB_Variant()
{
    delete holder_;
    holder_ = nullptr;
}

GB_Variant& GB_Variant::operator=(const GB_Variant& other)
{
    if (this == &other)
    {
        return *this;
    }

    HolderBase* newHolder = nullptr;
    if (other.holder_ != nullptr)
    {
        newHolder = other.holder_->Clone();
    }

    delete holder_;
    holder_ = newHolder;
    return *this;
}

GB_Variant& GB_Variant::operator=(GB_Variant&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    delete holder_;
    holder_ = other.holder_;
    other.holder_ = nullptr;
    return *this;
}

bool GB_Variant::IsEmpty() const noexcept
{
    return holder_ == nullptr;
}

GB_VariantType GB_Variant::Type() const noexcept
{
    if (holder_ == nullptr)
    {
        return GB_VariantType::Empty;
    }

    return holder_->GetVariantType();
}

const std::type_info& GB_Variant::TypeInfo() const noexcept
{
    return holder_ == nullptr ? typeid(void) : holder_->GetTypeInfo();
}

std::string GB_Variant::TypeName() const
{
    if (holder_ == nullptr)
    {
        return "Empty";
    }

    const std::string stableTypeName = holder_->GetStableTypeName();
    if (!stableTypeName.empty())
    {
        return stableTypeName;
    }

    const std::map<std::type_index, CustomTypeRegistration>& registryByType = GetCustomTypeRegistryByType();
    std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
    const std::map<std::type_index, CustomTypeRegistration>::const_iterator iter = registryByType.find(std::type_index(holder_->GetTypeInfo()));
    if (iter != registryByType.end())
    {
        return iter->second.typeName;
    }

    return holder_->GetTypeInfo().name();
}

void GB_Variant::Reset() noexcept
{
    delete holder_;
    holder_ = nullptr;
}

bool GB_Variant::ToBool(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    if (holder_ == nullptr)
    {
        return false;
    }

    bool boolValue = false;
    if (TryGetExactValue(*this, boolValue))
    {
        SetSuccessFlag(ok, true);
        return boolValue;
    }

    long long signedValue = 0;
    if (TryGetSignedValue(*this, signedValue))
    {
        SetSuccessFlag(ok, true);
        return signedValue != 0;
    }

    unsigned long long unsignedValue = 0;
    if (TryGetUnsignedValue(*this, unsignedValue))
    {
        SetSuccessFlag(ok, true);
        return unsignedValue != 0;
    }

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(*this, floatingValue))
    {
        SetSuccessFlag(ok, true);
        return floatingValue != 0.0L;
    }

    std::string stringValue;
    if (TryGetExactValue(*this, stringValue))
    {
        const std::string normalized = ToLowerAscii(TrimAscii(stringValue));
        if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on")
        {
            SetSuccessFlag(ok, true);
            return true;
        }

        if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off")
        {
            SetSuccessFlag(ok, true);
            return false;
        }
    }

    return false;
}

int GB_Variant::ToInt(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    long long value = 0;
    if (!TryGetSignedValue(*this, value))
    {
        return 0;
    }

    int result = 0;
    if (!ConvertSignedIntegerToSignedInteger(value, result))
    {
        return 0;
    }

    SetSuccessFlag(ok, true);
    return result;
}

unsigned int GB_Variant::ToUInt(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    unsigned long long value = 0;
    if (!TryGetUnsignedValue(*this, value))
    {
        return 0U;
    }

    unsigned int result = 0;
    if (!ConvertUnsignedIntegerToUnsignedInteger(value, result))
    {
        return 0U;
    }

    SetSuccessFlag(ok, true);
    return result;
}

long long GB_Variant::ToInt64(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    long long value = 0;
    if (!TryGetSignedValue(*this, value))
    {
        return 0LL;
    }

    SetSuccessFlag(ok, true);
    return value;
}

unsigned long long GB_Variant::ToUInt64(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    unsigned long long value = 0;
    if (!TryGetUnsignedValue(*this, value))
    {
        return 0ULL;
    }

    SetSuccessFlag(ok, true);
    return value;
}

std::size_t GB_Variant::ToSizeT(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    unsigned long long value = 0;
    if (!TryGetUnsignedValue(*this, value))
    {
        return 0U;
    }

    if (value > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
    {
        return 0U;
    }

    SetSuccessFlag(ok, true);
    return static_cast<std::size_t>(value);
}

float GB_Variant::ToFloat(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    long double value = 0.0L;
    if (!TryGetFloatingValue(*this, value))
    {
        return 0.0f;
    }

    if (value < -static_cast<long double>(std::numeric_limits<float>::max())
        || value > static_cast<long double>(std::numeric_limits<float>::max()))
    {
        return 0.0f;
    }

    SetSuccessFlag(ok, true);
    return static_cast<float>(value);
}

double GB_Variant::ToDouble(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    long double value = 0.0L;
    if (!TryGetFloatingValue(*this, value))
    {
        return 0.0;
    }

    if (value < -static_cast<long double>(std::numeric_limits<double>::max())
        || value > static_cast<long double>(std::numeric_limits<double>::max()))
    {
        return 0.0;
    }

    SetSuccessFlag(ok, true);
    return static_cast<double>(value);
}

std::string GB_Variant::ToString(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    if (holder_ == nullptr)
    {
        return std::string();
    }

    std::string stringValue;
    if (TryGetExactValue(*this, stringValue))
    {
        SetSuccessFlag(ok, true);
        return stringValue;
    }

    bool boolValue = false;
    if (TryGetExactValue(*this, boolValue))
    {
        SetSuccessFlag(ok, true);
        return boolValue ? "true" : "false";
    }

    char charValue = 0;
    if (TryGetExactValue(*this, charValue))
    {
        SetSuccessFlag(ok, true);
        return IntegerToString(static_cast<int>(charValue));
    }

    signed char signedCharValue = 0;
    if (TryGetExactValue(*this, signedCharValue))
    {
        SetSuccessFlag(ok, true);
        return IntegerToString(static_cast<int>(signedCharValue));
    }

    unsigned char unsignedCharValue = 0;
    if (TryGetExactValue(*this, unsignedCharValue))
    {
        SetSuccessFlag(ok, true);
        return IntegerToString(static_cast<unsigned int>(unsignedCharValue));
    }

    short shortValue = 0;
    if (TryGetExactValue(*this, shortValue))
    {
        SetSuccessFlag(ok, true);
        return IntegerToString(shortValue);
    }

    unsigned short unsignedShortValue = 0;
    if (TryGetExactValue(*this, unsignedShortValue))
    {
        SetSuccessFlag(ok, true);
        return IntegerToString(unsignedShortValue);
    }

    int intValue = 0;
    if (TryGetExactValue(*this, intValue))
    {
        SetSuccessFlag(ok, true);
        return IntegerToString(intValue);
    }

    unsigned int unsignedIntValue = 0;
    if (TryGetExactValue(*this, unsignedIntValue))
    {
        SetSuccessFlag(ok, true);
        return IntegerToString(unsignedIntValue);
    }

    long longValue = 0;
    if (TryGetExactValue(*this, longValue))
    {
        SetSuccessFlag(ok, true);
        return IntegerToString(longValue);
    }

    unsigned long unsignedLongValue = 0;
    if (TryGetExactValue(*this, unsignedLongValue))
    {
        SetSuccessFlag(ok, true);
        return IntegerToString(unsignedLongValue);
    }

    long long longLongValue = 0;
    if (TryGetExactValue(*this, longLongValue))
    {
        SetSuccessFlag(ok, true);
        return IntegerToString(longLongValue);
    }

    unsigned long long unsignedLongLongValue = 0;
    if (TryGetExactValue(*this, unsignedLongLongValue))
    {
        SetSuccessFlag(ok, true);
        return IntegerToString(unsignedLongLongValue);
    }

    float floatValue = 0.0f;
    if (TryGetExactValue(*this, floatValue))
    {
        SetSuccessFlag(ok, true);
        return FloatingPointToString(floatValue);
    }

    double doubleValue = 0.0;
    if (TryGetExactValue(*this, doubleValue))
    {
        SetSuccessFlag(ok, true);
        return FloatingPointToString(doubleValue);
    }

    long double longDoubleValue = 0.0L;
    if (TryGetExactValue(*this, longDoubleValue))
    {
        SetSuccessFlag(ok, true);
        return FloatingPointToString(longDoubleValue);
    }

    GB_ByteBuffer binaryValue;
    if (TryGetExactValue(*this, binaryValue))
    {
        SetSuccessFlag(ok, true);
        return BinaryToHexString(binaryValue);
    }

    return std::string();
}

GB_ByteBuffer GB_Variant::ToBinary(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    if (holder_ == nullptr)
    {
        return GB_ByteBuffer();
    }

    GB_ByteBuffer binaryValue;
    if (TryGetExactValue(*this, binaryValue))
    {
        SetSuccessFlag(ok, true);
        return binaryValue;
    }

    std::string stringValue;
    if (TryGetExactValue(*this, stringValue))
    {
        SetSuccessFlag(ok, true);
        return GB_StringToByteBuffer(stringValue);
    }

    if (Type() == GB_VariantType::Custom)
    {
        GB_ByteBuffer payload;
        if (holder_->SerializePayload(payload))
        {
            SetSuccessFlag(ok, true);
            return payload;
        }
    }

    return GB_ByteBuffer();
}

bool GB_Variant::Serialize(GB_ByteBuffer& outData) const noexcept
{
    outData.clear();

    try
    {
        outData.reserve(32);
        outData.push_back(kMagic0);
        outData.push_back(kMagic1);
        outData.push_back(kMagic2);
        outData.push_back(kMagic3);
        WriteUInt16(outData, kCurrentVersion);
        WriteUInt16(outData, static_cast<unsigned short>(Type()));

        std::string stableTypeName;
        GB_ByteBuffer payload;

        if (holder_ != nullptr)
        {
            stableTypeName = holder_->GetStableTypeName();
            if (stableTypeName.empty())
            {
                std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
                const std::map<std::type_index, CustomTypeRegistration>& registryByType = GetCustomTypeRegistryByType();
                const std::map<std::type_index, CustomTypeRegistration>::const_iterator iter =
                    registryByType.find(std::type_index(holder_->GetTypeInfo()));
                if (iter == registryByType.end())
                {
                    outData.clear();
                    return false;
                }

                stableTypeName = iter->second.typeName;
            }

            if (!holder_->SerializePayload(payload))
            {
                outData.clear();
                return false;
            }
        }

        WriteUInt32(outData, static_cast<unsigned int>(stableTypeName.size()));
        WriteUInt64(outData, static_cast<unsigned long long>(payload.size()));
        outData.insert(outData.end(), stableTypeName.begin(), stableTypeName.end());
        outData.insert(outData.end(), payload.begin(), payload.end());
    }
    catch (...)
    {
        outData.clear();
        return false;
    }

    return true;
}

GB_ByteBuffer GB_Variant::Serialize() const noexcept
{
    GB_ByteBuffer data;
    Serialize(data);
    return data;
}

bool GB_Variant::DeserializeFromBinary(const GB_ByteBuffer& data) noexcept
{
    GB_Variant value;
    if (!Deserialize(data, value))
    {
        return false;
    }

    *this = std::move(value);
    return true;
}

bool GB_Variant::Deserialize(const GB_ByteBuffer& data, GB_Variant& outValue) noexcept
{
    outValue.Reset();

    if (data.size() < 4 + 2 + 2 + 4 + 8)
    {
        return false;
    }

    if (data[0] != kMagic0 || data[1] != kMagic1 || data[2] != kMagic2 || data[3] != kMagic3)
    {
        return false;
    }

    std::size_t offset = 4;
    unsigned short version = 0;
    if (!ReadUInt16(data, offset, version))
    {
        return false;
    }

    if (version != kCurrentVersion)
    {
        return false;
    }

    unsigned short variantTypeValue = 0;
    if (!ReadUInt16(data, offset, variantTypeValue))
    {
        return false;
    }

    unsigned int typeNameLength = 0;
    if (!ReadUInt32(data, offset, typeNameLength))
    {
        return false;
    }

    unsigned long long payloadLength = 0;
    if (!ReadUInt64(data, offset, payloadLength))
    {
        return false;
    }

    if (static_cast<unsigned long long>(typeNameLength) > static_cast<unsigned long long>(data.size() - offset))
    {
        return false;
    }

    const std::size_t remainingSize = data.size() - offset - static_cast<std::size_t>(typeNameLength);
    if (payloadLength > static_cast<unsigned long long>(remainingSize))
    {
        return false;
    }

    std::string stableTypeName;
    if (!ReadString(data, offset, static_cast<std::size_t>(typeNameLength), stableTypeName))
    {
        return false;
    }

    GB_ByteBuffer payload;
    if (!ReadBytes(data, offset, static_cast<std::size_t>(payloadLength), payload))
    {
        return false;
    }

    if (offset != data.size())
    {
        return false;
    }

    const GB_VariantType variantType = static_cast<GB_VariantType>(variantTypeValue);
    if (variantType == GB_VariantType::Empty)
    {
        if (!stableTypeName.empty() || !payload.empty())
        {
            return false;
        }

        outValue.Reset();
        return true;
    }

    HolderBase* newHolder = nullptr;
    if (!DeserializeBuiltinValue(stableTypeName, payload, newHolder))
    {
        std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
        const std::map<std::string, const CustomTypeRegistration*>& registryByName = GetCustomTypeRegistryByName();
        const std::map<std::string, const CustomTypeRegistration*>::const_iterator iter = registryByName.find(stableTypeName);
        if (iter == registryByName.end())
        {
            return false;
        }

        try
        {
            newHolder = iter->second->deserializeFunc(payload);
        }
        catch (...)
        {
            newHolder = nullptr;
        }

        if (newHolder == nullptr)
        {
            return false;
        }
    }

    if (newHolder->GetVariantType() != variantType)
    {
        delete newHolder;
        return false;
    }

    outValue.Reset();
    outValue.holder_ = newHolder;
    return true;
}

bool GB_Variant::RegisterCustomType(const std::type_index& typeIndex,
    const std::string& typeName,
    std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFunc,
    std::function<HolderBase* (const GB_ByteBuffer& data)> deserializeFunc)
{
    if (typeName.empty() || !serializeFunc || !deserializeFunc)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
    std::map<std::type_index, CustomTypeRegistration>& registryByType = GetCustomTypeRegistryByType();
    std::map<std::string, const CustomTypeRegistration*>& registryByName = GetCustomTypeRegistryByName();

    if (registryByType.find(typeIndex) != registryByType.end())
    {
        return false;
    }

    if (registryByName.find(typeName) != registryByName.end())
    {
        return false;
    }

    CustomTypeRegistration registration;
    registration.typeName = typeName;
    registration.serializeFunc = std::move(serializeFunc);
    registration.deserializeFunc = std::move(deserializeFunc);

    const std::pair<std::map<std::type_index, CustomTypeRegistration>::iterator, bool> insertResult =
        registryByType.insert(std::make_pair(typeIndex, std::move(registration)));
    if (!insertResult.second)
    {
        return false;
    }

    registryByName.insert(std::make_pair(typeName, &insertResult.first->second));
    return true;
}

std::mutex& GB_Variant::GetCustomTypeRegistryMutex()
{
    static std::mutex customTypeRegistryMutex;
    return customTypeRegistryMutex;
}

std::map<std::type_index, GB_Variant::CustomTypeRegistration>& GB_Variant::GetCustomTypeRegistryByType()
{
    static std::map<std::type_index, CustomTypeRegistration> customTypeRegistryByType;
    return customTypeRegistryByType;
}

std::map<std::string, const GB_Variant::CustomTypeRegistration*>& GB_Variant::GetCustomTypeRegistryByName()
{
    static std::map<std::string, const CustomTypeRegistration*> customTypeRegistryByName;
    return customTypeRegistryByName;
}

const GB_Variant::HolderBase* GB_Variant::GetHolder() const noexcept
{
    return holder_;
}

GB_Variant::HolderBase* GB_Variant::GetHolder() noexcept
{
    return holder_;
}

template bool GB_Variant::SerializeBuiltinValue<bool>(const bool& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<char>(const char& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<signed char>(const signed char& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<unsigned char>(const unsigned char& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<short>(const short& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<unsigned short>(const unsigned short& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<int>(const int& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<unsigned int>(const unsigned int& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<long>(const long& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<unsigned long>(const unsigned long& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<long long>(const long long& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<unsigned long long>(const unsigned long long& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<float>(const float& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<double>(const double& value, GB_ByteBuffer& outData) noexcept;
template bool GB_Variant::SerializeBuiltinValue<long double>(const long double& value, GB_ByteBuffer& outData) noexcept;
