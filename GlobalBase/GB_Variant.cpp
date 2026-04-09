#include "GB_Variant.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>

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
        static_assert(std::is_integral<TValue>::value && std::is_signed<TValue>::value, "TValue must be a signed integer type.");

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
        static_assert(std::is_integral<TValue>::value && !std::is_signed<TValue>::value, "TValue must be an unsigned integer type.");

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

        if (!std::isfinite(static_cast<double>(parsedValue)))
        {
            return false;
        }

        if (parsedValue < -static_cast<long double>(std::numeric_limits<TValue>::max())
            || parsedValue > static_cast<long double>(std::numeric_limits<TValue>::max()))
        {
            return false;
        }

        outValue = static_cast<TValue>(parsedValue);
        return true;
    }

    bool TryParseBool(const std::string& text, bool& outValue) noexcept
    {
        const std::string loweredText = ToLowerAscii(TrimAscii(text));
        if (loweredText.empty())
        {
            return false;
        }

        if (loweredText == "true" || loweredText == "1" || loweredText == "yes" || loweredText == "on")
        {
            outValue = true;
            return true;
        }

        if (loweredText == "false" || loweredText == "0" || loweredText == "no" || loweredText == "off")
        {
            outValue = false;
            return true;
        }

        return false;
    }

    template<typename TValue>
    bool CheckedFloatToInteger(const long double value, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value, "TValue must be an integer type.");

        if (!std::isfinite(static_cast<double>(value)))
        {
            return false;
        }

        long double integralPart = 0.0L;
        const long double fractionalPart = std::modf(value, &integralPart);
        if (fractionalPart != 0.0L)
        {
            return false;
        }

        if constexpr (std::is_signed<TValue>::value)
        {
            if (integralPart < static_cast<long double>(std::numeric_limits<TValue>::min())
                || integralPart > static_cast<long double>(std::numeric_limits<TValue>::max()))
            {
                return false;
            }
        }
        else
        {
            if (integralPart < 0.0L
                || integralPart > static_cast<long double>(std::numeric_limits<TValue>::max()))
            {
                return false;
            }
        }

        outValue = static_cast<TValue>(integralPart);
        return true;
    }

    template<typename TValue>
    bool ConvertSignedIntegerToSignedInteger(const long long value, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value && std::is_signed<TValue>::value, "TValue must be a signed integer type.");

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
        static_assert(std::is_integral<TValue>::value && !std::is_signed<TValue>::value, "TValue must be an unsigned integer type.");

        if (value > static_cast<unsigned long long>(std::numeric_limits<TValue>::max()))
        {
            return false;
        }

        outValue = static_cast<TValue>(value);
        return true;
    }

    template<typename TValue>
    bool ConvertUnsignedIntegerToSignedInteger(const unsigned long long value, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value && std::is_signed<TValue>::value, "TValue must be a signed integer type.");

        if (value > static_cast<unsigned long long>(std::numeric_limits<TValue>::max()))
        {
            return false;
        }

        outValue = static_cast<TValue>(value);
        return true;
    }

    template<typename TValue>
    bool ConvertSignedIntegerToUnsignedInteger(const long long value, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value && !std::is_signed<TValue>::value, "TValue must be an unsigned integer type.");

        if (value < 0)
        {
            return false;
        }

        if (static_cast<unsigned long long>(value) > static_cast<unsigned long long>(std::numeric_limits<TValue>::max()))
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

        for (unsigned char byteValue : data)
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
    const TValue* GetValueIfExact(const GB_Variant& variant) noexcept
    {
        return variant.AnyCast<TValue>();
    }

    template<typename TValue>
    bool TryGetExactValue(const GB_Variant& variant, TValue& outValue) noexcept
    {
        const TValue* value = GetValueIfExact<TValue>(variant);
        if (value == nullptr)
        {
            return false;
        }

        outValue = *value;
        return true;
    }

    bool TryGetSignedValue(const GB_Variant& variant, long long& outValue) noexcept
    {
        if (const bool* value = variant.AnyCast<bool>())
        {
            outValue = *value ? 1LL : 0LL;
            return true;
        }

        if (const char* value = variant.AnyCast<char>())
        {
            outValue = static_cast<long long>(*value);
            return true;
        }

        if (const signed char* value = variant.AnyCast<signed char>())
        {
            outValue = static_cast<long long>(*value);
            return true;
        }

        if (const short* value = variant.AnyCast<short>())
        {
            outValue = static_cast<long long>(*value);
            return true;
        }

        if (const int* value = variant.AnyCast<int>())
        {
            outValue = static_cast<long long>(*value);
            return true;
        }

        if (const long* value = variant.AnyCast<long>())
        {
            outValue = static_cast<long long>(*value);
            return true;
        }

        if (const long long* value = variant.AnyCast<long long>())
        {
            outValue = *value;
            return true;
        }

        return false;
    }

    bool TryGetUnsignedValue(const GB_Variant& variant, unsigned long long& outValue) noexcept
    {
        if (const bool* value = variant.AnyCast<bool>())
        {
            outValue = *value ? 1ULL : 0ULL;
            return true;
        }

        if (const unsigned char* value = variant.AnyCast<unsigned char>())
        {
            outValue = static_cast<unsigned long long>(*value);
            return true;
        }

        if (const unsigned short* value = variant.AnyCast<unsigned short>())
        {
            outValue = static_cast<unsigned long long>(*value);
            return true;
        }

        if (const unsigned int* value = variant.AnyCast<unsigned int>())
        {
            outValue = static_cast<unsigned long long>(*value);
            return true;
        }

        if (const unsigned long* value = variant.AnyCast<unsigned long>())
        {
            outValue = static_cast<unsigned long long>(*value);
            return true;
        }

        if (const unsigned long long* value = variant.AnyCast<unsigned long long>())
        {
            outValue = *value;
            return true;
        }

        if (const char* value = variant.AnyCast<char>())
        {
            if (*value < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(*value);
            return true;
        }

        if (const signed char* value = variant.AnyCast<signed char>())
        {
            if (*value < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(*value);
            return true;
        }

        if (const short* value = variant.AnyCast<short>())
        {
            if (*value < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(*value);
            return true;
        }

        if (const int* value = variant.AnyCast<int>())
        {
            if (*value < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(*value);
            return true;
        }

        if (const long* value = variant.AnyCast<long>())
        {
            if (*value < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(*value);
            return true;
        }

        if (const long long* value = variant.AnyCast<long long>())
        {
            if (*value < 0)
            {
                return false;
            }

            outValue = static_cast<unsigned long long>(*value);
            return true;
        }

        return false;
    }

    bool TryGetFloatingValue(const GB_Variant& variant, long double& outValue) noexcept
    {
        if (const float* value = variant.AnyCast<float>())
        {
            outValue = static_cast<long double>(*value);
            return true;
        }

        if (const double* value = variant.AnyCast<double>())
        {
            outValue = static_cast<long double>(*value);
            return true;
        }

        if (const long double* value = variant.AnyCast<long double>())
        {
            outValue = *value;
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

        return false;
    }

    std::string GetBuiltInTypeToken(const std::type_info& typeInfo)
    {
        if (typeInfo == typeid(bool))
        {
            return "bool";
        }

        if (typeInfo == typeid(char))
        {
            return "char";
        }

        if (typeInfo == typeid(signed char))
        {
            return "signed char";
        }

        if (typeInfo == typeid(unsigned char))
        {
            return "unsigned char";
        }

        if (typeInfo == typeid(short))
        {
            return "short";
        }

        if (typeInfo == typeid(unsigned short))
        {
            return "unsigned short";
        }

        if (typeInfo == typeid(int))
        {
            return "int";
        }

        if (typeInfo == typeid(unsigned int))
        {
            return "unsigned int";
        }

        if (typeInfo == typeid(long))
        {
            return "long";
        }

        if (typeInfo == typeid(unsigned long))
        {
            return "unsigned long";
        }

        if (typeInfo == typeid(long long))
        {
            return "long long";
        }

        if (typeInfo == typeid(unsigned long long))
        {
            return "unsigned long long";
        }

        if (typeInfo == typeid(float))
        {
            return "float";
        }

        if (typeInfo == typeid(double))
        {
            return "double";
        }

        if (typeInfo == typeid(long double))
        {
            return "long double";
        }

        if (typeInfo == typeid(std::string))
        {
            return "std::string";
        }

        if (typeInfo == typeid(GB_ByteBuffer))
        {
            return "binary";
        }

        return std::string();
    }

    bool SerializeBuiltInValue(const GB_Variant& variant,
        const std::string& typeToken,
        GB_ByteBuffer& outPayload) noexcept
    {
        try
        {
            outPayload.clear();

            if (typeToken == "bool")
            {
                const bool* value = variant.AnyCast<bool>();
                if (value == nullptr)
                {
                    return false;
                }

                outPayload.push_back(*value ? 1U : 0U);
                return true;
            }

            if (typeToken == "char")
            {
                const char* value = variant.AnyCast<char>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = IntegerToString(static_cast<int>(*value));
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "signed char")
            {
                const signed char* value = variant.AnyCast<signed char>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = IntegerToString(static_cast<int>(*value));
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "unsigned char")
            {
                const unsigned char* value = variant.AnyCast<unsigned char>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = IntegerToString(static_cast<unsigned int>(*value));
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "short")
            {
                const short* value = variant.AnyCast<short>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = IntegerToString(*value);
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "unsigned short")
            {
                const unsigned short* value = variant.AnyCast<unsigned short>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = IntegerToString(*value);
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "int")
            {
                const int* value = variant.AnyCast<int>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = IntegerToString(*value);
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "unsigned int")
            {
                const unsigned int* value = variant.AnyCast<unsigned int>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = IntegerToString(*value);
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "long")
            {
                const long* value = variant.AnyCast<long>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = IntegerToString(*value);
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "unsigned long")
            {
                const unsigned long* value = variant.AnyCast<unsigned long>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = IntegerToString(*value);
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "long long")
            {
                const long long* value = variant.AnyCast<long long>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = IntegerToString(*value);
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "unsigned long long")
            {
                const unsigned long long* value = variant.AnyCast<unsigned long long>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = IntegerToString(*value);
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "float")
            {
                const float* value = variant.AnyCast<float>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = FloatingPointToString(*value);
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "double")
            {
                const double* value = variant.AnyCast<double>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = FloatingPointToString(*value);
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "long double")
            {
                const long double* value = variant.AnyCast<long double>();
                if (value == nullptr)
                {
                    return false;
                }

                const std::string text = FloatingPointToString(*value);
                outPayload.assign(text.begin(), text.end());
                return true;
            }

            if (typeToken == "std::string")
            {
                const std::string* value = variant.AnyCast<std::string>();
                if (value == nullptr)
                {
                    return false;
                }

                outPayload.assign(value->begin(), value->end());
                return true;
            }

            if (typeToken == "binary")
            {
                const GB_ByteBuffer* value = variant.AnyCast<GB_ByteBuffer>();
                if (value == nullptr)
                {
                    return false;
                }

                outPayload = *value;
                return true;
            }
        }
        catch (...)
        {
            outPayload.clear();
            return false;
        }

        return false;
    }

    bool DeserializeBuiltInValue(const std::string& typeToken,
        const GB_ByteBuffer& payload,
        GB_Variant& outValue) noexcept
    {
        try
        {
            const std::string payloadText = payload.empty()
                ? std::string()
                : std::string(reinterpret_cast<const char*>(payload.data()), payload.size());

            if (typeToken == "bool")
            {
                if (payload.size() != 1)
                {
                    return false;
                }

                outValue = GB_Variant(payload[0] != 0);
                return true;
            }

            if (typeToken == "char")
            {
                int parsedValue = 0;
                if (!TryParseSignedInteger<int>(payloadText, parsedValue)
                    || parsedValue < static_cast<int>(std::numeric_limits<char>::min())
                    || parsedValue > static_cast<int>(std::numeric_limits<char>::max()))
                {
                    return false;
                }

                outValue = GB_Variant(static_cast<char>(parsedValue));
                return true;
            }

            if (typeToken == "signed char")
            {
                int parsedValue = 0;
                if (!TryParseSignedInteger<int>(payloadText, parsedValue)
                    || parsedValue < static_cast<int>(std::numeric_limits<signed char>::min())
                    || parsedValue > static_cast<int>(std::numeric_limits<signed char>::max()))
                {
                    return false;
                }

                outValue = GB_Variant(static_cast<signed char>(parsedValue));
                return true;
            }

            if (typeToken == "unsigned char")
            {
                unsigned int parsedValue = 0;
                if (!TryParseUnsignedInteger<unsigned int>(payloadText, parsedValue)
                    || parsedValue > static_cast<unsigned int>(std::numeric_limits<unsigned char>::max()))
                {
                    return false;
                }

                outValue = GB_Variant(static_cast<unsigned char>(parsedValue));
                return true;
            }

            if (typeToken == "short")
            {
                short parsedValue = 0;
                if (!TryParseSignedInteger<short>(payloadText, parsedValue))
                {
                    return false;
                }

                outValue = GB_Variant(parsedValue);
                return true;
            }

            if (typeToken == "unsigned short")
            {
                unsigned short parsedValue = 0;
                if (!TryParseUnsignedInteger<unsigned short>(payloadText, parsedValue))
                {
                    return false;
                }

                outValue = GB_Variant(parsedValue);
                return true;
            }

            if (typeToken == "int")
            {
                int parsedValue = 0;
                if (!TryParseSignedInteger<int>(payloadText, parsedValue))
                {
                    return false;
                }

                outValue = GB_Variant(parsedValue);
                return true;
            }

            if (typeToken == "unsigned int")
            {
                unsigned int parsedValue = 0;
                if (!TryParseUnsignedInteger<unsigned int>(payloadText, parsedValue))
                {
                    return false;
                }

                outValue = GB_Variant(parsedValue);
                return true;
            }

            if (typeToken == "long")
            {
                long long parsedValue = 0;
                if (!TryParseSignedInteger<long long>(payloadText, parsedValue))
                {
                    return false;
                }

                if (parsedValue < static_cast<long long>(std::numeric_limits<long>::min())
                    || parsedValue > static_cast<long long>(std::numeric_limits<long>::max()))
                {
                    return false;
                }

                outValue = GB_Variant(static_cast<long>(parsedValue));
                return true;
            }

            if (typeToken == "unsigned long")
            {
                unsigned long long parsedValue = 0;
                if (!TryParseUnsignedInteger<unsigned long long>(payloadText, parsedValue)
                    || parsedValue > static_cast<unsigned long long>(std::numeric_limits<unsigned long>::max()))
                {
                    return false;
                }

                outValue = GB_Variant(static_cast<unsigned long>(parsedValue));
                return true;
            }

            if (typeToken == "long long")
            {
                long long parsedValue = 0;
                if (!TryParseSignedInteger<long long>(payloadText, parsedValue))
                {
                    return false;
                }

                outValue = GB_Variant(parsedValue);
                return true;
            }

            if (typeToken == "unsigned long long")
            {
                unsigned long long parsedValue = 0;
                if (!TryParseUnsignedInteger<unsigned long long>(payloadText, parsedValue))
                {
                    return false;
                }

                outValue = GB_Variant(parsedValue);
                return true;
            }

            if (typeToken == "float")
            {
                float parsedValue = 0.0f;
                if (!TryParseFloatingPoint<float>(payloadText, parsedValue))
                {
                    return false;
                }

                outValue = GB_Variant(parsedValue);
                return true;
            }

            if (typeToken == "double")
            {
                double parsedValue = 0.0;
                if (!TryParseFloatingPoint<double>(payloadText, parsedValue))
                {
                    return false;
                }

                outValue = GB_Variant(parsedValue);
                return true;
            }

            if (typeToken == "long double")
            {
                long double parsedValue = 0.0L;
                if (!TryParseFloatingPoint<long double>(payloadText, parsedValue))
                {
                    return false;
                }

                outValue = GB_Variant(parsedValue);
                return true;
            }

            if (typeToken == "std::string")
            {
                outValue = GB_Variant(payloadText);
                return true;
            }

            if (typeToken == "binary")
            {
                outValue = GB_Variant(payload);
                return true;
            }
        }
        catch (...)
        {
            return false;
        }

        return false;
    }

    std::size_t CombineHash(const std::size_t seed, const std::size_t value) noexcept
    {
        return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    }

    bool TrySerializeForComparison(const GB_Variant& value, GB_ByteBuffer& outData) noexcept
    {
        return value.Serialize(outData);
    }

}

GB_Variant::GB_Variant()
{
}

GB_Variant::GB_Variant(std::nullptr_t)
{
}

GB_Variant::GB_Variant(const char* value)
{
    if (value != nullptr)
    {
        holder_.reset(new Holder<std::string>(std::string(value)));
    }
}

GB_Variant::GB_Variant(char* value)
{
    if (value != nullptr)
    {
        holder_.reset(new Holder<std::string>(std::string(value)));
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
{
    if (other.holder_ != nullptr)
    {
        holder_ = other.holder_->Clone();
    }
}

GB_Variant::GB_Variant(GB_Variant&& other) noexcept
    : holder_(std::move(other.holder_))
{
}

GB_Variant::~GB_Variant()
{
}

GB_Variant& GB_Variant::operator=(const GB_Variant& other)
{
    if (this == &other)
    {
        return *this;
    }

    if (other.holder_ == nullptr)
    {
        holder_.reset();
        return *this;
    }

    holder_ = other.holder_->Clone();
    return *this;
}

GB_Variant& GB_Variant::operator=(GB_Variant&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    holder_ = std::move(other.holder_);
    return *this;
}

bool GB_Variant::operator==(const GB_Variant& other) const noexcept
{
    if (holder_ == nullptr || other.holder_ == nullptr)
    {
        return holder_ == nullptr && other.holder_ == nullptr;
    }

    if (holder_->GetTypeInfo() != other.holder_->GetTypeInfo())
    {
        return false;
    }

    bool equalResult = false;
    if (TryInternalEquals(*this, other, equalResult))
    {
        return equalResult;
    }

    GB_ByteBuffer leftData;
    GB_ByteBuffer rightData;
    if (TrySerializeForComparison(*this, leftData) && TrySerializeForComparison(other, rightData))
    {
        return leftData == rightData;
    }

    return false;
}

bool GB_Variant::operator!=(const GB_Variant& other) const noexcept
{
    return !(*this == other);
}

bool GB_Variant::IsEmpty() const noexcept
{
    return holder_ == nullptr;
}

GB_VariantType GB_Variant::Type() const noexcept
{
    return holder_ == nullptr ? GB_VariantType::Empty : holder_->GetVariantType();
}

const std::type_info& GB_Variant::TypeInfo() const noexcept
{
    return holder_ == nullptr ? typeid(void) : holder_->GetTypeInfo();
}

std::string GB_Variant::TypeName() const
{
    if (holder_ == nullptr)
    {
        return "empty";
    }

    const std::string builtInTypeToken = GetBuiltInTypeToken(holder_->GetTypeInfo());
    if (!builtInTypeToken.empty())
    {
        return builtInTypeToken;
    }

    const std::type_index typeIndex(holder_->GetTypeInfo());
    std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
    const std::map<std::type_index, CustomTypeRegistration>& registryByType = GetCustomTypeRegistryByType();
    const auto foundIter = registryByType.find(typeIndex);
    if (foundIter != registryByType.end())
    {
        return foundIter->second.typeName;
    }

    return holder_->GetTypeInfo().name();
}

void GB_Variant::Reset() noexcept
{
    holder_.reset();
}

bool GB_Variant::CanCast(GB_VariantType targetType) const noexcept
{
    bool ok = false;

    switch (targetType)
    {
    case GB_VariantType::Empty:
        return IsEmpty();

    case GB_VariantType::Bool:
        static_cast<void>(ToBool(&ok));
        return ok;

    case GB_VariantType::Int8:
        static_cast<void>(ToInt8(&ok));
        return ok;

    case GB_VariantType::UInt8:
        static_cast<void>(ToUInt8(&ok));
        return ok;

    case GB_VariantType::Int16:
        static_cast<void>(ToInt16(&ok));
        return ok;

    case GB_VariantType::UInt16:
        static_cast<void>(ToUInt16(&ok));
        return ok;

    case GB_VariantType::Int32:
        static_cast<void>(ToInt(&ok));
        return ok;

    case GB_VariantType::UInt32:
        static_cast<void>(ToUInt(&ok));
        return ok;

    case GB_VariantType::Int64:
        static_cast<void>(ToInt64(&ok));
        return ok;

    case GB_VariantType::UInt64:
        static_cast<void>(ToUInt64(&ok));
        return ok;

    case GB_VariantType::Float:
        static_cast<void>(ToFloat(&ok));
        return ok;

    case GB_VariantType::Double:
        static_cast<void>(ToDouble(&ok));
        return ok;

    case GB_VariantType::String:
        static_cast<void>(ToString(&ok));
        return ok;

    case GB_VariantType::Binary:
        return Is<GB_ByteBuffer>();

    case GB_VariantType::Custom:
        return Type() == GB_VariantType::Custom;
    }

    return false;
}

bool GB_Variant::ToBool(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    if (holder_ == nullptr)
    {
        return false;
    }

    if (const bool* value = AnyCast<bool>())
    {
        SetSuccessFlag(ok, true);
        return *value;
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

    if (const std::string* value = AnyCast<std::string>())
    {
        bool parsedValue = false;
        if (TryParseBool(*value, parsedValue))
        {
            SetSuccessFlag(ok, true);
            return parsedValue;
        }
    }

    return false;
}

std::int8_t GB_Variant::ToInt8(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    std::int8_t result = 0;
    long long signedValue = 0;
    if (TryGetSignedValue(*this, signedValue) && ConvertSignedIntegerToSignedInteger<std::int8_t>(signedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    unsigned long long unsignedValue = 0;
    if (TryGetUnsignedValue(*this, unsignedValue) && ConvertUnsignedIntegerToSignedInteger<std::int8_t>(unsignedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(*this, floatingValue) && CheckedFloatToInteger<std::int8_t>(floatingValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    if (const std::string* value = AnyCast<std::string>())
    {
        if (TryParseSignedInteger<std::int8_t>(*value, result))
        {
            SetSuccessFlag(ok, true);
            return result;
        }
    }

    return 0;
}

std::uint8_t GB_Variant::ToUInt8(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    std::uint8_t result = 0;
    unsigned long long unsignedValue = 0;
    if (TryGetUnsignedValue(*this, unsignedValue) && ConvertUnsignedIntegerToUnsignedInteger<std::uint8_t>(unsignedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long long signedValue = 0;
    if (TryGetSignedValue(*this, signedValue) && ConvertSignedIntegerToUnsignedInteger<std::uint8_t>(signedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(*this, floatingValue) && CheckedFloatToInteger<std::uint8_t>(floatingValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    if (const std::string* value = AnyCast<std::string>())
    {
        if (TryParseUnsignedInteger<std::uint8_t>(*value, result))
        {
            SetSuccessFlag(ok, true);
            return result;
        }
    }

    return 0;
}

std::int16_t GB_Variant::ToInt16(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    std::int16_t result = 0;
    long long signedValue = 0;
    if (TryGetSignedValue(*this, signedValue) && ConvertSignedIntegerToSignedInteger<std::int16_t>(signedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    unsigned long long unsignedValue = 0;
    if (TryGetUnsignedValue(*this, unsignedValue) && ConvertUnsignedIntegerToSignedInteger<std::int16_t>(unsignedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(*this, floatingValue) && CheckedFloatToInteger<std::int16_t>(floatingValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    if (const std::string* value = AnyCast<std::string>())
    {
        if (TryParseSignedInteger<std::int16_t>(*value, result))
        {
            SetSuccessFlag(ok, true);
            return result;
        }
    }

    return 0;
}

std::uint16_t GB_Variant::ToUInt16(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    std::uint16_t result = 0;
    unsigned long long unsignedValue = 0;
    if (TryGetUnsignedValue(*this, unsignedValue) && ConvertUnsignedIntegerToUnsignedInteger<std::uint16_t>(unsignedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long long signedValue = 0;
    if (TryGetSignedValue(*this, signedValue) && ConvertSignedIntegerToUnsignedInteger<std::uint16_t>(signedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(*this, floatingValue) && CheckedFloatToInteger<std::uint16_t>(floatingValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    if (const std::string* value = AnyCast<std::string>())
    {
        if (TryParseUnsignedInteger<std::uint16_t>(*value, result))
        {
            SetSuccessFlag(ok, true);
            return result;
        }
    }

    return 0;
}

int GB_Variant::ToInt(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    int result = 0;
    long long signedValue = 0;
    if (TryGetSignedValue(*this, signedValue) && ConvertSignedIntegerToSignedInteger<int>(signedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    unsigned long long unsignedValue = 0;
    if (TryGetUnsignedValue(*this, unsignedValue) && ConvertUnsignedIntegerToSignedInteger<int>(unsignedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(*this, floatingValue) && CheckedFloatToInteger<int>(floatingValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    if (const std::string* value = AnyCast<std::string>())
    {
        if (TryParseSignedInteger<int>(*value, result))
        {
            SetSuccessFlag(ok, true);
            return result;
        }
    }

    return 0;
}

unsigned int GB_Variant::ToUInt(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    unsigned int result = 0;
    unsigned long long unsignedValue = 0;
    if (TryGetUnsignedValue(*this, unsignedValue) && ConvertUnsignedIntegerToUnsignedInteger<unsigned int>(unsignedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long long signedValue = 0;
    if (TryGetSignedValue(*this, signedValue) && ConvertSignedIntegerToUnsignedInteger<unsigned int>(signedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(*this, floatingValue) && CheckedFloatToInteger<unsigned int>(floatingValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    if (const std::string* value = AnyCast<std::string>())
    {
        if (TryParseUnsignedInteger<unsigned int>(*value, result))
        {
            SetSuccessFlag(ok, true);
            return result;
        }
    }

    return 0U;
}

long long GB_Variant::ToInt64(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    long long result = 0;
    if (TryGetSignedValue(*this, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    unsigned long long unsignedValue = 0;
    if (TryGetUnsignedValue(*this, unsignedValue) && ConvertUnsignedIntegerToSignedInteger<long long>(unsignedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(*this, floatingValue) && CheckedFloatToInteger<long long>(floatingValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    if (const std::string* value = AnyCast<std::string>())
    {
        if (TryParseSignedInteger<long long>(*value, result))
        {
            SetSuccessFlag(ok, true);
            return result;
        }
    }

    return 0;
}

unsigned long long GB_Variant::ToUInt64(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    unsigned long long result = 0;
    if (TryGetUnsignedValue(*this, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long long signedValue = 0;
    if (TryGetSignedValue(*this, signedValue) && ConvertSignedIntegerToUnsignedInteger<unsigned long long>(signedValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(*this, floatingValue) && CheckedFloatToInteger<unsigned long long>(floatingValue, result))
    {
        SetSuccessFlag(ok, true);
        return result;
    }

    if (const std::string* value = AnyCast<std::string>())
    {
        if (TryParseUnsignedInteger<unsigned long long>(*value, result))
        {
            SetSuccessFlag(ok, true);
            return result;
        }
    }

    return 0;
}

std::size_t GB_Variant::ToSizeT(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    const unsigned long long result = ToUInt64(ok);
    if (ok != nullptr && !*ok)
    {
        return 0;
    }

    if (result > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
    {
        SetSuccessFlag(ok, false);
        return 0;
    }

    SetSuccessFlag(ok, true);
    return static_cast<std::size_t>(result);
}

float GB_Variant::ToFloat(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(*this, floatingValue))
    {
        if (floatingValue < -static_cast<long double>(std::numeric_limits<float>::max())
            || floatingValue > static_cast<long double>(std::numeric_limits<float>::max()))
        {
            return 0.0f;
        }

        SetSuccessFlag(ok, true);
        return static_cast<float>(floatingValue);
    }

    if (const std::string* value = AnyCast<std::string>())
    {
        float parsedValue = 0.0f;
        if (TryParseFloatingPoint<float>(*value, parsedValue))
        {
            SetSuccessFlag(ok, true);
            return parsedValue;
        }
    }

    return 0.0f;
}

double GB_Variant::ToDouble(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(*this, floatingValue))
    {
        if (floatingValue < -static_cast<long double>(std::numeric_limits<double>::max())
            || floatingValue > static_cast<long double>(std::numeric_limits<double>::max()))
        {
            return 0.0;
        }

        SetSuccessFlag(ok, true);
        return static_cast<double>(floatingValue);
    }

    if (const std::string* value = AnyCast<std::string>())
    {
        double parsedValue = 0.0;
        if (TryParseFloatingPoint<double>(*value, parsedValue))
        {
            SetSuccessFlag(ok, true);
            return parsedValue;
        }
    }

    return 0.0;
}

std::string GB_Variant::ToString(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    if (holder_ == nullptr)
    {
        return std::string();
    }

    try
    {
        if (const std::string* value = AnyCast<std::string>())
        {
            SetSuccessFlag(ok, true);
            return *value;
        }

        if (const bool* value = AnyCast<bool>())
        {
            SetSuccessFlag(ok, true);
            return *value ? "true" : "false";
        }

        if (const char* value = AnyCast<char>())
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(static_cast<int>(*value));
        }

        if (const signed char* value = AnyCast<signed char>())
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(static_cast<int>(*value));
        }

        if (const unsigned char* value = AnyCast<unsigned char>())
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(static_cast<unsigned int>(*value));
        }

        if (const short* value = AnyCast<short>())
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*value);
        }

        if (const unsigned short* value = AnyCast<unsigned short>())
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*value);
        }

        if (const int* value = AnyCast<int>())
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*value);
        }

        if (const unsigned int* value = AnyCast<unsigned int>())
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*value);
        }

        if (const long* value = AnyCast<long>())
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*value);
        }

        if (const unsigned long* value = AnyCast<unsigned long>())
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*value);
        }

        if (const long long* value = AnyCast<long long>())
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*value);
        }

        if (const unsigned long long* value = AnyCast<unsigned long long>())
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*value);
        }

        if (const float* value = AnyCast<float>())
        {
            SetSuccessFlag(ok, true);
            return FloatingPointToString(*value);
        }

        if (const double* value = AnyCast<double>())
        {
            SetSuccessFlag(ok, true);
            return FloatingPointToString(*value);
        }

        if (const long double* value = AnyCast<long double>())
        {
            SetSuccessFlag(ok, true);
            return FloatingPointToString(*value);
        }

        if (const GB_ByteBuffer* value = AnyCast<GB_ByteBuffer>())
        {
            SetSuccessFlag(ok, true);
            return BinaryToHexString(*value);
        }
    }
    catch (...)
    {
        SetSuccessFlag(ok, false);
        return std::string();
    }

    return std::string();
}

bool GB_Variant::Serialize(GB_ByteBuffer& outData) const noexcept
{
    outData.clear();

    try
    {
        if (holder_ == nullptr)
        {
            outData.reserve(4 + 2 + 4 + 8);
            outData.push_back(kMagic0);
            outData.push_back(kMagic1);
            outData.push_back(kMagic2);
            outData.push_back(kMagic3);
            WriteUInt16(outData, kCurrentVersion);
            WriteUInt32(outData, 0);
            WriteUInt64(outData, 0);
            return true;
        }

        GB_ByteBuffer payload;
        std::string typeToken = GetBuiltInTypeToken(holder_->GetTypeInfo());
        if (!typeToken.empty())
        {
            if (!SerializeBuiltInValue(*this, typeToken, payload))
            {
                return false;
            }
        }
        else
        {
            const std::type_index typeIndex(holder_->GetTypeInfo());
            std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
            const std::map<std::type_index, CustomTypeRegistration>& registryByType = GetCustomTypeRegistryByType();
            const auto foundIter = registryByType.find(typeIndex);
            if (foundIter == registryByType.end())
            {
                return false;
            }

            typeToken = foundIter->second.typeName;
            if (typeToken.empty() || !foundIter->second.serializeFunc(holder_->GetConstPtr(), payload))
            {
                return false;
            }
        }

        if (typeToken.size() > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max()))
        {
            return false;
        }

        outData.reserve(4 + 2 + 4 + 8 + typeToken.size() + payload.size());
        outData.push_back(kMagic0);
        outData.push_back(kMagic1);
        outData.push_back(kMagic2);
        outData.push_back(kMagic3);
        WriteUInt16(outData, kCurrentVersion);
        WriteUInt32(outData, static_cast<unsigned int>(typeToken.size()));
        WriteUInt64(outData, static_cast<unsigned long long>(payload.size()));
        outData.insert(outData.end(), typeToken.begin(), typeToken.end());
        outData.insert(outData.end(), payload.begin(), payload.end());
        return true;
    }
    catch (...)
    {
        outData.clear();
        return false;
    }
}

GB_ByteBuffer GB_Variant::Serialize() const noexcept
{
    GB_ByteBuffer result;
    static_cast<void>(Serialize(result));
    return result;
}

bool GB_Variant::DeserializeFromBinary(const GB_ByteBuffer& data) noexcept
{
    return Deserialize(data, *this);
}

bool GB_Variant::Deserialize(const GB_ByteBuffer& data, GB_Variant& outValue) noexcept
{
    outValue.Reset();

    std::size_t offset = 0;
    unsigned short version = 0;
    unsigned int typeTokenLength = 0;
    unsigned long long payloadLength = 0;
    std::string typeToken;
    GB_ByteBuffer payload;

    if (data.size() < 4 + 2 + 4 + 8)
    {
        return false;
    }

    if (data[offset++] != kMagic0
        || data[offset++] != kMagic1
        || data[offset++] != kMagic2
        || data[offset++] != kMagic3)
    {
        return false;
    }

    if (!ReadUInt16(data, offset, version))
    {
        return false;
    }

    if (version != kCurrentVersion)
    {
        return false;
    }

    if (!ReadUInt32(data, offset, typeTokenLength))
    {
        return false;
    }

    if (!ReadUInt64(data, offset, payloadLength))
    {
        return false;
    }

    if (!ReadString(data, offset, static_cast<std::size_t>(typeTokenLength), typeToken))
    {
        return false;
    }

    if (!ReadBytes(data, offset, static_cast<std::size_t>(payloadLength), payload))
    {
        return false;
    }

    if (offset != data.size())
    {
        return false;
    }

    if (typeToken.empty())
    {
        return payload.empty();
    }

    if (DeserializeBuiltInValue(typeToken, payload, outValue))
    {
        return true;
    }

    std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
    const std::map<std::string, CustomTypeRegistration>& registryByName = GetCustomTypeRegistryByName();
    const auto foundIter = registryByName.find(typeToken);
    if (foundIter == registryByName.end())
    {
        return false;
    }

    std::unique_ptr<HolderBase> holder = foundIter->second.deserializeFunc(payload);
    if (holder == nullptr)
    {
        return false;
    }

    outValue.holder_ = std::move(holder);
    return true;
}

bool GB_Variant::RegisterCustomType(
    const std::type_index& typeIndex,
    const std::string& typeName,
    std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFunc,
    std::function<std::unique_ptr<HolderBase>(const GB_ByteBuffer& data)> deserializeFunc)
{
    if (typeName.empty() || !serializeFunc || !deserializeFunc)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
    std::map<std::type_index, CustomTypeRegistration>& registryByType = GetCustomTypeRegistryByType();
    std::map<std::string, CustomTypeRegistration>& registryByName = GetCustomTypeRegistryByName();

    const auto foundByType = registryByType.find(typeIndex);
    if (foundByType != registryByType.end() && foundByType->second.typeName != typeName)
    {
        return false;
    }

    const auto foundByName = registryByName.find(typeName);
    if (foundByName != registryByName.end() && foundByType == registryByType.end())
    {
        return false;
    }

    CustomTypeRegistration registration;
    registration.typeName = typeName;
    registration.serializeFunc = std::move(serializeFunc);
    registration.deserializeFunc = std::move(deserializeFunc);

    registryByType[typeIndex] = registration;
    registryByName[typeName] = registration;
    return true;
}

std::mutex& GB_Variant::GetCustomTypeRegistryMutex()
{
    static std::mutex registryMutex;
    return registryMutex;
}

std::map<std::type_index, GB_Variant::CustomTypeRegistration>& GB_Variant::GetCustomTypeRegistryByType()
{
    static std::map<std::type_index, CustomTypeRegistration> registryByType;
    return registryByType;
}

std::map<std::string, GB_Variant::CustomTypeRegistration>& GB_Variant::GetCustomTypeRegistryByName()
{
    static std::map<std::string, CustomTypeRegistration> registryByName;
    return registryByName;
}


bool GB_Variant::TryInternalEquals(const GB_Variant& left, const GB_Variant& right, bool& outResult) noexcept
{
    const HolderBase* leftHolder = left.GetHolder();
    const HolderBase* rightHolder = right.GetHolder();
    if (leftHolder == nullptr || rightHolder == nullptr)
    {
        return false;
    }

    return leftHolder->TryEquals(*rightHolder, outResult);
}

bool GB_Variant::TryInternalLess(const GB_Variant& left, const GB_Variant& right, bool& outResult) noexcept
{
    const HolderBase* leftHolder = left.GetHolder();
    const HolderBase* rightHolder = right.GetHolder();
    if (leftHolder == nullptr || rightHolder == nullptr)
    {
        return false;
    }

    return leftHolder->TryLess(*rightHolder, outResult);
}

bool GB_Variant::TryInternalHash(const GB_Variant& value, std::size_t& outHash) noexcept
{
    const HolderBase* holder = value.GetHolder();
    if (holder == nullptr)
    {
        return false;
    }

    return holder->TryHash(outHash);
}

const GB_Variant::HolderBase* GB_Variant::GetHolder() const noexcept
{
    return holder_.get();
}

GB_Variant::HolderBase* GB_Variant::GetHolder() noexcept
{
    return holder_.get();
}

bool GB_VariantLess::operator()(const GB_Variant& left, const GB_Variant& right) const noexcept
{
    if (left.IsEmpty() || right.IsEmpty())
    {
        return left.IsEmpty() && !right.IsEmpty();
    }

    if (left.Type() != right.Type())
    {
        return static_cast<int>(left.Type()) < static_cast<int>(right.Type());
    }

    const std::type_index leftTypeIndex(left.TypeInfo());
    const std::type_index rightTypeIndex(right.TypeInfo());
    if (leftTypeIndex != rightTypeIndex)
    {
        return leftTypeIndex < rightTypeIndex;
    }

    bool lessResult = false;
    if (GB_Variant::TryInternalLess(left, right, lessResult))
    {
        return lessResult;
    }

    GB_ByteBuffer leftData;
    GB_ByteBuffer rightData;
    if (TrySerializeForComparison(left, leftData) && TrySerializeForComparison(right, rightData))
    {
        return std::lexicographical_compare(leftData.begin(), leftData.end(), rightData.begin(), rightData.end());
    }

    return false;
}

bool GB_VariantEqual::operator()(const GB_Variant& left, const GB_Variant& right) const noexcept
{
    return left == right;
}

std::size_t GB_VariantHash::operator()(const GB_Variant& value) const noexcept
{
    if (value.IsEmpty())
    {
        return 0x6f9d5b0aULL;
    }

    const std::size_t typeHash = std::hash<std::type_index>()(std::type_index(value.TypeInfo()));

    std::size_t valueHash = 0;
    if (GB_Variant::TryInternalHash(value, valueHash))
    {
        return CombineHash(typeHash, valueHash);
    }

    GB_ByteBuffer serializedData;
    if (TrySerializeForComparison(value, serializedData))
    {
        std::size_t bufferHash = 0;
        for (unsigned char byteValue : serializedData)
        {
            bufferHash = CombineHash(bufferHash, static_cast<std::size_t>(byteValue));
        }

        return CombineHash(typeHash, bufferHash);
    }

    return typeHash;
}
