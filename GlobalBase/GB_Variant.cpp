#include "GB_Variant.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
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

        if (!trimmedText.empty() && trimmedText[0] == '-')
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

        if (parsedValue < -std::numeric_limits<TValue>::max() || parsedValue > std::numeric_limits<TValue>::max())
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

        if (std::is_signed<TValue>::value)
        {
            if (value < static_cast<long double>(std::numeric_limits<TValue>::min())
                || value > static_cast<long double>(std::numeric_limits<TValue>::max()))
            {
                return false;
            }
        }
        else
        {
            if (value < 0.0L || value > static_cast<long double>(std::numeric_limits<TValue>::max()))
            {
                return false;
            }
        }

        outValue = static_cast<TValue>(value);
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
                if (!TryParseSignedInteger<long long>(payloadText, parsedValue)
                    || parsedValue < static_cast<long long>(std::numeric_limits<long>::min())
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
}

struct GB_Variant::CustomTypeRegistration
{
    std::string typeName;
    std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFunc;
    std::function<std::unique_ptr<HolderBase>(const GB_ByteBuffer& data)> deserializeFunc;
};

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

    holder_ = other.holder_ != nullptr ? other.holder_->Clone() : std::unique_ptr<HolderBase>();
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

bool GB_Variant::IsEmpty() const noexcept
{
    return holder_ == nullptr;
}

GB_VariantType GB_Variant::Type() const noexcept
{
    return holder_ != nullptr ? holder_->GetVariantType() : GB_VariantType::Empty;
}

const std::type_info& GB_Variant::TypeInfo() const noexcept
{
    return holder_ != nullptr ? holder_->GetTypeInfo() : typeid(void);
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

    const std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
    const std::map<std::type_index, CustomTypeRegistration>& registryByType = GetCustomTypeRegistryByType();
    const auto iterator = registryByType.find(std::type_index(holder_->GetTypeInfo()));
    if (iterator != registryByType.end())
    {
        return iterator->second.typeName;
    }

    return holder_->GetTypeInfo().name();
}

void GB_Variant::Reset() noexcept
{
    holder_.reset();
}

bool GB_Variant::ToBool(bool* ok) const noexcept
{
    try
    {
        if (holder_ == nullptr)
        {
            SetSuccessFlag(ok, false);
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
            bool result = false;
            const bool success = TryParseBool(*value, result);
            SetSuccessFlag(ok, success);
            return success ? result : false;
        }
    }
    catch (...)
    {
    }

    SetSuccessFlag(ok, false);
    return false;
}

int GB_Variant::ToInt(bool* ok) const noexcept
{
    try
    {
        long long signedValue = 0;
        if (TryGetSignedValue(*this, signedValue))
        {
            int result = 0;
            const bool success = ConvertSignedIntegerToSignedInteger<int>(signedValue, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0;
        }

        unsigned long long unsignedValue = 0;
        if (TryGetUnsignedValue(*this, unsignedValue))
        {
            int result = 0;
            const bool success = ConvertUnsignedIntegerToSignedInteger<int>(unsignedValue, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0;
        }

        long double floatingValue = 0.0L;
        if (TryGetFloatingValue(*this, floatingValue))
        {
            int result = 0;
            const bool success = CheckedFloatToInteger<int>(floatingValue, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0;
        }

        if (const std::string* value = AnyCast<std::string>())
        {
            int result = 0;
            const bool success = TryParseSignedInteger<int>(*value, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0;
        }
    }
    catch (...)
    {
    }

    SetSuccessFlag(ok, false);
    return 0;
}

unsigned int GB_Variant::ToUInt(bool* ok) const noexcept
{
    try
    {
        unsigned int result = 0;

        unsigned long long unsignedValue = 0;
        if (TryGetUnsignedValue(*this, unsignedValue))
        {
            const bool success = ConvertUnsignedIntegerToUnsignedInteger<unsigned int>(unsignedValue, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0U;
        }

        long long signedValue = 0;
        if (TryGetSignedValue(*this, signedValue))
        {
            const bool success = ConvertSignedIntegerToUnsignedInteger<unsigned int>(signedValue, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0U;
        }

        long double floatingValue = 0.0L;
        if (TryGetFloatingValue(*this, floatingValue))
        {
            const bool success = CheckedFloatToInteger<unsigned int>(floatingValue, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0U;
        }

        if (const std::string* value = AnyCast<std::string>())
        {
            const bool success = TryParseUnsignedInteger<unsigned int>(*value, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0U;
        }
    }
    catch (...)
    {
    }

    SetSuccessFlag(ok, false);
    return 0U;
}

long long GB_Variant::ToInt64(bool* ok) const noexcept
{
    try
    {
        long long signedValue = 0;
        if (TryGetSignedValue(*this, signedValue))
        {
            SetSuccessFlag(ok, true);
            return signedValue;
        }

        unsigned long long unsignedValue = 0;
        if (TryGetUnsignedValue(*this, unsignedValue))
        {
            long long result = 0;
            const bool success = ConvertUnsignedIntegerToSignedInteger<long long>(unsignedValue, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0LL;
        }

        long double floatingValue = 0.0L;
        if (TryGetFloatingValue(*this, floatingValue))
        {
            long long result = 0;
            const bool success = CheckedFloatToInteger<long long>(floatingValue, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0LL;
        }

        if (const std::string* value = AnyCast<std::string>())
        {
            long long result = 0;
            const bool success = TryParseSignedInteger<long long>(*value, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0LL;
        }
    }
    catch (...)
    {
    }

    SetSuccessFlag(ok, false);
    return 0LL;
}

unsigned long long GB_Variant::ToUInt64(bool* ok) const noexcept
{
    try
    {
        unsigned long long unsignedValue = 0;
        if (TryGetUnsignedValue(*this, unsignedValue))
        {
            SetSuccessFlag(ok, true);
            return unsignedValue;
        }

        long long signedValue = 0;
        if (TryGetSignedValue(*this, signedValue))
        {
            unsigned long long result = 0;
            const bool success = ConvertSignedIntegerToUnsignedInteger<unsigned long long>(signedValue, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0ULL;
        }

        long double floatingValue = 0.0L;
        if (TryGetFloatingValue(*this, floatingValue))
        {
            unsigned long long result = 0;
            const bool success = CheckedFloatToInteger<unsigned long long>(floatingValue, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0ULL;
        }

        if (const std::string* value = AnyCast<std::string>())
        {
            unsigned long long result = 0;
            const bool success = TryParseUnsignedInteger<unsigned long long>(*value, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0ULL;
        }
    }
    catch (...)
    {
    }

    SetSuccessFlag(ok, false);
    return 0ULL;
}

std::size_t GB_Variant::ToSizeT(bool* ok) const noexcept
{
    try
    {
        bool success = false;
        const unsigned long long value = ToUInt64(&success);
        if (!success || value > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        {
            SetSuccessFlag(ok, false);
            return static_cast<std::size_t>(0);
        }

        SetSuccessFlag(ok, true);
        return static_cast<std::size_t>(value);
    }
    catch (...)
    {
    }

    SetSuccessFlag(ok, false);
    return static_cast<std::size_t>(0);
}

float GB_Variant::ToFloat(bool* ok) const noexcept
{
    try
    {
        long double floatingValue = 0.0L;
        if (TryGetFloatingValue(*this, floatingValue))
        {
            if (floatingValue < -std::numeric_limits<float>::max() || floatingValue > std::numeric_limits<float>::max())
            {
                SetSuccessFlag(ok, false);
                return 0.0f;
            }

            SetSuccessFlag(ok, true);
            return static_cast<float>(floatingValue);
        }

        if (const std::string* value = AnyCast<std::string>())
        {
            float result = 0.0f;
            const bool success = TryParseFloatingPoint<float>(*value, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0.0f;
        }
    }
    catch (...)
    {
    }

    SetSuccessFlag(ok, false);
    return 0.0f;
}

double GB_Variant::ToDouble(bool* ok) const noexcept
{
    try
    {
        long double floatingValue = 0.0L;
        if (TryGetFloatingValue(*this, floatingValue))
        {
            if (floatingValue < -std::numeric_limits<double>::max() || floatingValue > std::numeric_limits<double>::max())
            {
                SetSuccessFlag(ok, false);
                return 0.0;
            }

            SetSuccessFlag(ok, true);
            return static_cast<double>(floatingValue);
        }

        if (const std::string* value = AnyCast<std::string>())
        {
            double result = 0.0;
            const bool success = TryParseFloatingPoint<double>(*value, result);
            SetSuccessFlag(ok, success);
            return success ? result : 0.0;
        }
    }
    catch (...)
    {
    }

    SetSuccessFlag(ok, false);
    return 0.0;
}

std::string GB_Variant::ToString(bool* ok) const noexcept
{
    try
    {
        if (holder_ == nullptr)
        {
            SetSuccessFlag(ok, false);
            return std::string();
        }

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
    }

    SetSuccessFlag(ok, false);
    return std::string();
}

GB_ByteBuffer GB_Variant::ToBinary(bool* ok) const noexcept
{
    GB_ByteBuffer result;

    try
    {
        if (holder_ == nullptr)
        {
            SetSuccessFlag(ok, false);
            return result;
        }

        if (const GB_ByteBuffer* value = AnyCast<GB_ByteBuffer>())
        {
            SetSuccessFlag(ok, true);
            return *value;
        }

        if (const std::string* value = AnyCast<std::string>())
        {
            result.assign(value->begin(), value->end());
            SetSuccessFlag(ok, true);
            return result;
        }

        if (const bool* value = AnyCast<bool>())
        {
            result.push_back(*value ? 1U : 0U);
            SetSuccessFlag(ok, true);
            return result;
        }

        const std::string text = ToString(ok);
        if (ok != nullptr && !*ok)
        {
            return GB_ByteBuffer();
        }

        result.assign(text.begin(), text.end());
        SetSuccessFlag(ok, true);
        return result;
    }
    catch (...)
    {
    }

    SetSuccessFlag(ok, false);
    return GB_ByteBuffer();
}

bool GB_Variant::Serialize(GB_ByteBuffer& outData) const noexcept
{
    try
    {
        outData.clear();
        outData.reserve(32);

        outData.push_back(kMagic0);
        outData.push_back(kMagic1);
        outData.push_back(kMagic2);
        outData.push_back(kMagic3);
        WriteUInt16(outData, kCurrentVersion);

        std::string typeName;
        GB_ByteBuffer payload;

        if (holder_ == nullptr)
        {
            typeName = "empty";
        }
        else
        {
            typeName = GetBuiltInTypeToken(holder_->GetTypeInfo());
            if (!typeName.empty())
            {
                if (!SerializeBuiltInValue(*this, typeName, payload))
                {
                    outData.clear();
                    return false;
                }
            }
            else
            {
                const std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
                const std::map<std::type_index, CustomTypeRegistration>& registryByType = GetCustomTypeRegistryByType();
                const auto iterator = registryByType.find(std::type_index(holder_->GetTypeInfo()));
                if (iterator == registryByType.end())
                {
                    outData.clear();
                    return false;
                }

                typeName = iterator->second.typeName;
                if (!iterator->second.serializeFunc(holder_->GetConstPtr(), payload))
                {
                    outData.clear();
                    return false;
                }
            }
        }

        WriteUInt32(outData, static_cast<unsigned int>(typeName.size()));
        outData.insert(outData.end(), typeName.begin(), typeName.end());
        WriteUInt64(outData, static_cast<unsigned long long>(payload.size()));
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
    try
    {
        outValue.Reset();

        if (data.size() < 6)
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

        unsigned int typeNameLength = 0;
        if (!ReadUInt32(data, offset, typeNameLength))
        {
            return false;
        }

        std::string typeName;
        if (!ReadString(data, offset, static_cast<std::size_t>(typeNameLength), typeName))
        {
            return false;
        }

        unsigned long long payloadLength = 0;
        if (!ReadUInt64(data, offset, payloadLength))
        {
            return false;
        }

        if (payloadLength > static_cast<unsigned long long>(data.size() - offset))
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

        if (typeName == "empty")
        {
            outValue.Reset();
            return payload.empty();
        }

        if (DeserializeBuiltInValue(typeName, payload, outValue))
        {
            return true;
        }

        const std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
        const std::map<std::string, CustomTypeRegistration>& registryByName = GetCustomTypeRegistryByName();
        const auto iterator = registryByName.find(typeName);
        if (iterator == registryByName.end())
        {
            return false;
        }

        std::unique_ptr<HolderBase> holder = iterator->second.deserializeFunc(payload);
        if (!holder)
        {
            return false;
        }

        outValue.holder_ = std::move(holder);
        return true;
    }
    catch (...)
    {
        outValue.Reset();
        return false;
    }
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

    const std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
    std::map<std::type_index, CustomTypeRegistration>& registryByType = GetCustomTypeRegistryByType();
    std::map<std::string, CustomTypeRegistration>& registryByName = GetCustomTypeRegistryByName();

    const auto typeIterator = registryByType.find(typeIndex);
    if (typeIterator != registryByType.end() && typeIterator->second.typeName != typeName)
    {
        return false;
    }

    const auto nameIterator = registryByName.find(typeName);
    if (nameIterator != registryByName.end() && registryByType.find(typeIndex) == registryByType.end())
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

const GB_Variant::HolderBase* GB_Variant::GetHolder() const noexcept
{
    return holder_.get();
}

GB_Variant::HolderBase* GB_Variant::GetHolder() noexcept
{
    return holder_.get();
}
