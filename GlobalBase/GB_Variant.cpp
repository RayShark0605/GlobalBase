#include "GB_Variant.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace
{
    const unsigned char kMagic0 = 'G';
    const unsigned char kMagic1 = 'B';
    const unsigned char kMagic2 = 'V';
    const unsigned char kMagic3 = 'R';
    const unsigned short kCurrentVersion = 1;

    void SetSuccessFlag(bool* ok, const bool value) noexcept
    {
        if (ok != nullptr)
        {
            *ok = value;
        }
    }

    template<typename TValue>
    const TValue* TryGetStoredValue(const std::type_info& storedTypeInfo, const void* storedValuePtr) noexcept
    {
        typedef typename std::decay<TValue>::type ValueType;
        if (storedValuePtr == nullptr || storedTypeInfo != typeid(ValueType))
        {
            return nullptr;
        }

        return static_cast<const ValueType*>(storedValuePtr);
    }

    int CompareByteSequence(const unsigned char* const leftData,
        const std::size_t leftSize,
        const unsigned char* const rightData,
        const std::size_t rightSize) noexcept
    {
        const std::size_t sharedSize = leftSize < rightSize ? leftSize : rightSize;
        for (std::size_t index = 0; index < sharedSize; index++)
        {
            if (leftData[index] < rightData[index])
            {
                return -1;
            }

            if (leftData[index] > rightData[index])
            {
                return 1;
            }
        }

        if (leftSize < rightSize)
        {
            return -1;
        }

        if (leftSize > rightSize)
        {
            return 1;
        }

        return 0;
    }

    std::size_t HashBytes(const unsigned char* const data, const std::size_t size) noexcept
    {
        if (data == nullptr || size == 0)
        {
            return static_cast<std::size_t>(0);
        }

        if (sizeof(std::size_t) >= sizeof(std::uint64_t))
        {
            std::uint64_t hashValue = 14695981039346656037ull;
            for (std::size_t index = 0; index < size; index++)
            {
                hashValue ^= static_cast<std::uint64_t>(data[index]);
                hashValue *= 1099511628211ull;
            }

            return static_cast<std::size_t>(hashValue);
        }

        std::uint32_t hashValue = 2166136261u;
        for (std::size_t index = 0; index < size; index++)
        {
            hashValue ^= static_cast<std::uint32_t>(data[index]);
            hashValue *= 16777619u;
        }

        return static_cast<std::size_t>(hashValue);
    }

    void HashCombine(std::size_t& seed, const std::size_t value) noexcept
    {
        seed ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (seed << 6) + (seed >> 2);
    }

    template<typename TValue>
    int CompareOrderedValues(const TValue& leftValue, const TValue& rightValue) noexcept
    {
        if (leftValue < rightValue)
        {
            return -1;
        }

        if (rightValue < leftValue)
        {
            return 1;
        }

        return 0;
    }

    template<typename TValue>
    int CompareBitwiseValues(const TValue& leftValue, const TValue& rightValue) noexcept
    {
        return CompareByteSequence(reinterpret_cast<const unsigned char*>(&leftValue),
            sizeof(TValue),
            reinterpret_cast<const unsigned char*>(&rightValue),
            sizeof(TValue));
    }

    template<typename TValue>
    std::size_t HashValueBytes(const TValue& value) noexcept
    {
        return HashBytes(reinterpret_cast<const unsigned char*>(&value), sizeof(TValue));
    }

    std::size_t HashStringBytes(const std::string& value) noexcept
    {
        if (value.empty())
        {
            return static_cast<std::size_t>(0);
        }

        return HashBytes(reinterpret_cast<const unsigned char*>(value.data()), value.size());
    }

    std::size_t HashByteBufferBytes(const GB_ByteBuffer& value) noexcept
    {
        if (value.empty())
        {
            return static_cast<std::size_t>(0);
        }

        return HashBytes(value.data(), value.size());
    }

    bool TryCompareExactStoredValue(const std::type_info& storedTypeInfo,
        const void* const leftValuePtr,
        const void* const rightValuePtr,
        int& outCompare) noexcept
    {
        if (leftValuePtr == nullptr || rightValuePtr == nullptr)
        {
            return false;
        }

        if (const bool* const leftValue = TryGetStoredValue<bool>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const bool*>(rightValuePtr));
            return true;
        }

        if (const char* const leftValue = TryGetStoredValue<char>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const char*>(rightValuePtr));
            return true;
        }

        if (const signed char* const leftValue = TryGetStoredValue<signed char>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const signed char*>(rightValuePtr));
            return true;
        }

        if (const unsigned char* const leftValue = TryGetStoredValue<unsigned char>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const unsigned char*>(rightValuePtr));
            return true;
        }

        if (const short* const leftValue = TryGetStoredValue<short>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const short*>(rightValuePtr));
            return true;
        }

        if (const unsigned short* const leftValue = TryGetStoredValue<unsigned short>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const unsigned short*>(rightValuePtr));
            return true;
        }

        if (const int* const leftValue = TryGetStoredValue<int>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const int*>(rightValuePtr));
            return true;
        }

        if (const unsigned int* const leftValue = TryGetStoredValue<unsigned int>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const unsigned int*>(rightValuePtr));
            return true;
        }

        if (const long* const leftValue = TryGetStoredValue<long>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const long*>(rightValuePtr));
            return true;
        }

        if (const unsigned long* const leftValue = TryGetStoredValue<unsigned long>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const unsigned long*>(rightValuePtr));
            return true;
        }

        if (const long long* const leftValue = TryGetStoredValue<long long>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const long long*>(rightValuePtr));
            return true;
        }

        if (const unsigned long long* const leftValue = TryGetStoredValue<unsigned long long>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const unsigned long long*>(rightValuePtr));
            return true;
        }

        if (const float* const leftValue = TryGetStoredValue<float>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareBitwiseValues(*leftValue, *static_cast<const float*>(rightValuePtr));
            return true;
        }

        if (const double* const leftValue = TryGetStoredValue<double>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareBitwiseValues(*leftValue, *static_cast<const double*>(rightValuePtr));
            return true;
        }

        if (const long double* const leftValue = TryGetStoredValue<long double>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareBitwiseValues(*leftValue, *static_cast<const long double*>(rightValuePtr));
            return true;
        }

        if (const std::string* const leftValue = TryGetStoredValue<std::string>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const std::string*>(rightValuePtr));
            return true;
        }

        if (const GB_ByteBuffer* const leftValue = TryGetStoredValue<GB_ByteBuffer>(storedTypeInfo, leftValuePtr))
        {
            outCompare = CompareOrderedValues(*leftValue, *static_cast<const GB_ByteBuffer*>(rightValuePtr));
            return true;
        }

        return false;
    }

    bool TryHashExactStoredValue(const std::type_info& storedTypeInfo,
        const void* const valuePtr,
        std::size_t& outHash) noexcept
    {
        if (valuePtr == nullptr)
        {
            return false;
        }

        if (const bool* const value = TryGetStoredValue<bool>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const char* const value = TryGetStoredValue<char>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const signed char* const value = TryGetStoredValue<signed char>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const unsigned char* const value = TryGetStoredValue<unsigned char>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const short* const value = TryGetStoredValue<short>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const unsigned short* const value = TryGetStoredValue<unsigned short>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const int* const value = TryGetStoredValue<int>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const unsigned int* const value = TryGetStoredValue<unsigned int>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const long* const value = TryGetStoredValue<long>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const unsigned long* const value = TryGetStoredValue<unsigned long>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const long long* const value = TryGetStoredValue<long long>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const unsigned long long* const value = TryGetStoredValue<unsigned long long>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const float* const value = TryGetStoredValue<float>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const double* const value = TryGetStoredValue<double>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const long double* const value = TryGetStoredValue<long double>(storedTypeInfo, valuePtr))
        {
            outHash = HashValueBytes(*value);
            return true;
        }

        if (const std::string* const value = TryGetStoredValue<std::string>(storedTypeInfo, valuePtr))
        {
            outHash = HashStringBytes(*value);
            return true;
        }

        if (const GB_ByteBuffer* const value = TryGetStoredValue<GB_ByteBuffer>(storedTypeInfo, valuePtr))
        {
            outHash = HashByteBufferBytes(*value);
            return true;
        }

        return false;
    }

    bool IsValidVariantType(const GB_VariantType type) noexcept
    {
        return type == GB_VariantType::Empty
            || type == GB_VariantType::Bool
            || type == GB_VariantType::Int32
            || type == GB_VariantType::UInt32
            || type == GB_VariantType::Int64
            || type == GB_VariantType::UInt64
            || type == GB_VariantType::Float
            || type == GB_VariantType::Double
            || type == GB_VariantType::String
            || type == GB_VariantType::Binary
            || type == GB_VariantType::Custom;
    }


    bool IsIntegralVariantType(const GB_VariantType type) noexcept
    {
        return type == GB_VariantType::Int32
            || type == GB_VariantType::UInt32
            || type == GB_VariantType::Int64
            || type == GB_VariantType::UInt64;
    }

    bool IsFloatingVariantType(const GB_VariantType type) noexcept
    {
        return type == GB_VariantType::Float
            || type == GB_VariantType::Double;
    }

    bool TryGetStoredFloatingValue(const GB_Variant& variantValue, long double& outValue) noexcept
    {
        if (const float* const floatValue = variantValue.AnyCast<float>())
        {
            outValue = static_cast<long double>(*floatValue);
            return true;
        }

        if (const double* const doubleValue = variantValue.AnyCast<double>())
        {
            outValue = static_cast<long double>(*doubleValue);
            return true;
        }

        if (const long double* const longDoubleValue = variantValue.AnyCast<long double>())
        {
            outValue = *longDoubleValue;
            return true;
        }

        return false;
    }

    int CompareIntegralVariantValues(const GB_Variant& leftValue, const GB_Variant& rightValue) noexcept
    {
        const GB_VariantType leftType = leftValue.Type();
        const GB_VariantType rightType = rightValue.Type();
        const bool leftIsUnsigned = leftType == GB_VariantType::UInt32 || leftType == GB_VariantType::UInt64;
        const bool rightIsUnsigned = rightType == GB_VariantType::UInt32 || rightType == GB_VariantType::UInt64;

        if (!leftIsUnsigned && !rightIsUnsigned)
        {
            bool leftOk = false;
            bool rightOk = false;
            const long long leftIntegerValue = leftValue.ToInt64(&leftOk);
            const long long rightIntegerValue = rightValue.ToInt64(&rightOk);
            if (!leftOk || !rightOk)
            {
                return static_cast<int>(leftType) < static_cast<int>(rightType) ? -1 : 1;
            }

            return CompareOrderedValues(leftIntegerValue, rightIntegerValue);
        }

        if (leftIsUnsigned && rightIsUnsigned)
        {
            bool leftOk = false;
            bool rightOk = false;
            const unsigned long long leftIntegerValue = leftValue.ToUInt64(&leftOk);
            const unsigned long long rightIntegerValue = rightValue.ToUInt64(&rightOk);
            if (!leftOk || !rightOk)
            {
                return static_cast<int>(leftType) < static_cast<int>(rightType) ? -1 : 1;
            }

            return CompareOrderedValues(leftIntegerValue, rightIntegerValue);
        }

        if (!leftIsUnsigned)
        {
            bool leftOk = false;
            bool rightOk = false;
            const long long leftIntegerValue = leftValue.ToInt64(&leftOk);
            const unsigned long long rightIntegerValue = rightValue.ToUInt64(&rightOk);
            if (!leftOk || !rightOk)
            {
                return static_cast<int>(leftType) < static_cast<int>(rightType) ? -1 : 1;
            }

            if (leftIntegerValue < 0)
            {
                return -1;
            }

            return CompareOrderedValues(static_cast<unsigned long long>(leftIntegerValue), rightIntegerValue);
        }

        const int inverseCompareResult = CompareIntegralVariantValues(rightValue, leftValue);
        if (inverseCompareResult < 0)
        {
            return 1;
        }

        if (inverseCompareResult > 0)
        {
            return -1;
        }

        return 0;
    }

    int CompareFloatingVariantValues(const GB_Variant& leftValue, const GB_Variant& rightValue) noexcept
    {
        long double leftFloatingValue = 0.0L;
        long double rightFloatingValue = 0.0L;
        if (!TryGetStoredFloatingValue(leftValue, leftFloatingValue)
            || !TryGetStoredFloatingValue(rightValue, rightFloatingValue))
        {
            const GB_VariantType leftType = leftValue.Type();
            const GB_VariantType rightType = rightValue.Type();
            return static_cast<int>(leftType) < static_cast<int>(rightType) ? -1 : 1;
        }

        const bool leftIsNan = std::isnan(leftFloatingValue);
        const bool rightIsNan = std::isnan(rightFloatingValue);
        if (leftIsNan || rightIsNan)
        {
            if (leftIsNan && rightIsNan)
            {
                return 0;
            }

            return leftIsNan ? 1 : -1;
        }

        if (leftFloatingValue == rightFloatingValue)
        {
            return 0;
        }

        return leftFloatingValue < rightFloatingValue ? -1 : 1;
    }

    std::size_t HashNormalizedIntegralVariantValue(const GB_Variant& variantValue) noexcept
    {
        const GB_VariantType variantType = variantValue.Type();
        const bool isUnsigned = variantType == GB_VariantType::UInt32 || variantType == GB_VariantType::UInt64;
        if (!isUnsigned)
        {
            bool signedOk = false;
            const long long signedValue = variantValue.ToInt64(&signedOk);
            if (signedOk)
            {
                if (signedValue >= 0)
                {
                    return HashValueBytes(static_cast<unsigned long long>(signedValue));
                }

                std::size_t hashValue = static_cast<std::size_t>(0x4E454754u);
                HashCombine(hashValue, HashValueBytes(signedValue));
                return hashValue;
            }
        }

        bool unsignedOk = false;
        const unsigned long long unsignedValue = variantValue.ToUInt64(&unsignedOk);
        if (unsignedOk)
        {
            return HashValueBytes(unsignedValue);
        }

        return static_cast<std::size_t>(0);
    }

    std::size_t HashNormalizedFloatingVariantValue(const GB_Variant& variantValue) noexcept
    {
        long double floatingValue = 0.0L;
        if (!TryGetStoredFloatingValue(variantValue, floatingValue))
        {
            return static_cast<std::size_t>(0);
        }

        if (std::isnan(floatingValue))
        {
            return static_cast<std::size_t>(0x7FF80000u);
        }

        if (floatingValue == 0.0L)
        {
            floatingValue = 0.0L;
        }

        const int precision = std::numeric_limits<long double>::max_digits10;
        char buffer[64];
        const int length = std::snprintf(buffer, sizeof(buffer), "%.*Lg", precision, floatingValue);
        if (length <= 0 || length >= static_cast<int>(sizeof(buffer)))
        {
            return static_cast<std::size_t>(0);
        }

        return HashBytes(reinterpret_cast<const unsigned char*>(buffer), static_cast<std::size_t>(length));
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
        return std::to_string(value);
    }

    template<typename TValue>
    std::string FloatingPointToString(const TValue value)
    {
        const int precision = std::numeric_limits<TValue>::max_digits10;
        char buffer[64];
        const int length = std::snprintf(buffer, sizeof(buffer), "%.*Lg", precision, static_cast<long double>(value));
        if (length < 0 || length >= static_cast<int>(sizeof(buffer)))
        {
            return std::string();
        }

        return std::string(buffer, static_cast<std::size_t>(length));
    }

    template<typename TValue>
    bool TryParseSignedInteger(const std::string& text, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value && std::is_signed<TValue>::value,
            "TValue must be a signed integer type.");

        try
        {
            const std::string trimmedText = TrimAscii(text);
            if (trimmedText.empty())
            {
                return false;
            }

            errno = 0;
            char* endPtr = nullptr;
            const long long parsedValue = std::strtoll(trimmedText.c_str(), &endPtr, 0);
            if (errno == ERANGE || endPtr == trimmedText.c_str())
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
        catch (...)
        {
            return false;
        }
    }

    template<typename TValue>
    bool TryParseUnsignedInteger(const std::string& text, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value && !std::is_signed<TValue>::value,
            "TValue must be an unsigned integer type.");

        try
        {
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
            if (errno == ERANGE || endPtr == trimmedText.c_str())
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
        catch (...)
        {
            return false;
        }
    }

    template<typename TValue>
    bool TryParseFloatingPoint(const std::string& text, TValue& outValue) noexcept
    {
        static_assert(std::is_floating_point<TValue>::value, "TValue must be a floating-point type.");

        try
        {
            const std::string trimmedText = TrimAscii(text);
            if (trimmedText.empty())
            {
                return false;
            }

            errno = 0;
            char* endPtr = nullptr;
            const long double parsedValue = std::strtold(trimmedText.c_str(), &endPtr);
            if (errno == ERANGE || endPtr == trimmedText.c_str())
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
        catch (...)
        {
            return false;
        }
    }

    template<typename TValue>
    bool ConvertFloatingPointToSignedInteger(const TValue value, long long& outValue) noexcept
    {
        if (!std::isfinite(static_cast<long double>(value)))
        {
            return false;
        }

        const long double truncatedValue = std::trunc(static_cast<long double>(value));
        static const long double kMinSignedLongLongValue = -9223372036854775808.0L;
        static const long double kSignedLongLongUpperBound = 9223372036854775808.0L;
        if (truncatedValue < kMinSignedLongLongValue
            || truncatedValue >= kSignedLongLongUpperBound)
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
        static const long double kUnsignedLongLongUpperBound = 18446744073709551616.0L;
        if (truncatedValue < 0.0L
            || truncatedValue >= kUnsignedLongLongUpperBound)
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

    template<typename TValue>
    bool ConvertSignedIntegerToInteger(const long long value, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value, "TValue must be an integer type.");

        if (std::is_signed<TValue>::value)
        {
            if (value < static_cast<long long>(std::numeric_limits<TValue>::min())
                || value > static_cast<long long>(std::numeric_limits<TValue>::max()))
            {
                return false;
            }
        }
        else
        {
            if (value < 0 || static_cast<unsigned long long>(value) > static_cast<unsigned long long>(std::numeric_limits<TValue>::max()))
            {
                return false;
            }
        }

        outValue = static_cast<TValue>(value);
        return true;
    }

    template<typename TValue>
    bool ConvertUnsignedIntegerToInteger(const unsigned long long value, TValue& outValue) noexcept
    {
        static_assert(std::is_integral<TValue>::value, "TValue must be an integer type.");

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
        if (offset > data.size() || data.size() - offset < 2)
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
        if (offset > data.size() || data.size() - offset < 4)
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
        if (offset > data.size() || data.size() - offset < 8)
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
        if (offset > data.size() || length > data.size() - offset)
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
        if (offset > data.size() || length > data.size() - offset)
        {
            return false;
        }

        if (length == 0)
        {
            outText.clear();
            return true;
        }

        try
        {
            outText.assign(reinterpret_cast<const char*>(data.data() + offset), length);
        }
        catch (...)
        {
            return false;
        }

        offset += length;
        return true;
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

    bool ReadExactSignedInteger(const GB_ByteBuffer& payload,
        const std::size_t expectedSize,
        long long& outValue) noexcept
    {
        if (expectedSize == 0 || expectedSize > sizeof(unsigned long long))
        {
            return false;
        }

        unsigned long long rawValue = 0;
        if (!ReadExactInteger(payload, expectedSize, rawValue))
        {
            return false;
        }

        if (expectedSize < sizeof(unsigned long long))
        {
            const unsigned int bitCount = static_cast<unsigned int>(expectedSize * 8);
            const unsigned long long signBit = 1ULL << (bitCount - 1);
            if ((rawValue & signBit) != 0)
            {
                rawValue |= (~0ULL) << bitCount;
            }
        }

        std::memcpy(&outValue, &rawValue, sizeof(outValue));
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
    const GB_VariantType variantType,
    const GB_ByteBuffer& payload,
    HolderBase*& outHolder) noexcept
{
    outHolder = nullptr;

    try
    {
        if (stableTypeName == "bool")
        {
            static_assert(sizeof(bool) == 1, "GB_Variant serialization assumes sizeof(bool) == 1.");
            if (variantType != GB_VariantType::Bool || payload.size() != 1)
            {
                return false;
            }

            outHolder = new Holder<bool>(payload[0] != 0);
            return true;
        }

        if (stableTypeName == "char")
        {
            if (payload.size() != 1)
            {
                return false;
            }

            char value = 0;
            if (variantType == GB_VariantType::Int32)
            {
                long long signedValue = 0;
                if (!ReadExactSignedInteger(payload, 1, signedValue)
                    || !ConvertSignedIntegerToInteger(signedValue, value))
                {
                    return false;
                }
            }
            else if (variantType == GB_VariantType::UInt32)
            {
                unsigned long long unsignedValue = 0;
                if (!ReadExactInteger(payload, 1, unsignedValue)
                    || !ConvertUnsignedIntegerToInteger(unsignedValue, value))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            outHolder = new Holder<char>(value);
            return true;
        }

        if (stableTypeName == "signed char")
        {
            if (variantType != GB_VariantType::Int32)
            {
                return false;
            }

            long long value = 0;
            if (!ReadExactSignedInteger(payload, 1, value))
            {
                return false;
            }

            signed char resultValue = 0;
            if (!ConvertSignedIntegerToInteger(value, resultValue))
            {
                return false;
            }

            outHolder = new Holder<signed char>(resultValue);
            return true;
        }

        if (stableTypeName == "unsigned char")
        {
            if (variantType != GB_VariantType::UInt32)
            {
                return false;
            }

            unsigned long long value = 0;
            if (!ReadExactInteger(payload, 1, value))
            {
                return false;
            }

            unsigned char resultValue = 0;
            if (!ConvertUnsignedIntegerToInteger(value, resultValue))
            {
                return false;
            }

            outHolder = new Holder<unsigned char>(resultValue);
            return true;
        }

        if (stableTypeName == "short")
        {
            if (variantType != GB_VariantType::Int32)
            {
                return false;
            }

            long long value = 0;
            if (!ReadExactSignedInteger(payload, 2, value))
            {
                return false;
            }

            short resultValue = 0;
            if (!ConvertSignedIntegerToInteger(value, resultValue))
            {
                return false;
            }

            outHolder = new Holder<short>(resultValue);
            return true;
        }

        if (stableTypeName == "unsigned short")
        {
            if (variantType != GB_VariantType::UInt32)
            {
                return false;
            }

            unsigned long long value = 0;
            if (!ReadExactInteger(payload, 2, value))
            {
                return false;
            }

            unsigned short resultValue = 0;
            if (!ConvertUnsignedIntegerToInteger(value, resultValue))
            {
                return false;
            }

            outHolder = new Holder<unsigned short>(resultValue);
            return true;
        }

        if (stableTypeName == "int")
        {
            if (variantType != GB_VariantType::Int32)
            {
                return false;
            }

            long long value = 0;
            if (!ReadExactSignedInteger(payload, 4, value))
            {
                return false;
            }

            int resultValue = 0;
            if (!ConvertSignedIntegerToInteger(value, resultValue))
            {
                return false;
            }

            outHolder = new Holder<int>(resultValue);
            return true;
        }

        if (stableTypeName == "unsigned int")
        {
            if (variantType != GB_VariantType::UInt32)
            {
                return false;
            }

            unsigned long long value = 0;
            if (!ReadExactInteger(payload, 4, value))
            {
                return false;
            }

            unsigned int resultValue = 0;
            if (!ConvertUnsignedIntegerToInteger(value, resultValue))
            {
                return false;
            }

            outHolder = new Holder<unsigned int>(resultValue);
            return true;
        }

        if (stableTypeName == "long")
        {
            if (variantType != GB_VariantType::Int32 && variantType != GB_VariantType::Int64)
            {
                return false;
            }

            const std::size_t expectedSize = variantType == GB_VariantType::Int32 ? 4 : 8;
            long long value = 0;
            if (!ReadExactSignedInteger(payload, expectedSize, value))
            {
                return false;
            }

            long resultValue = 0;
            if (!ConvertSignedIntegerToInteger(value, resultValue))
            {
                return false;
            }

            outHolder = new Holder<long>(resultValue);
            return true;
        }

        if (stableTypeName == "unsigned long")
        {
            if (variantType != GB_VariantType::UInt32 && variantType != GB_VariantType::UInt64)
            {
                return false;
            }

            const std::size_t expectedSize = variantType == GB_VariantType::UInt32 ? 4 : 8;
            unsigned long long value = 0;
            if (!ReadExactInteger(payload, expectedSize, value))
            {
                return false;
            }

            unsigned long resultValue = 0;
            if (!ConvertUnsignedIntegerToInteger(value, resultValue))
            {
                return false;
            }

            outHolder = new Holder<unsigned long>(resultValue);
            return true;
        }

        if (stableTypeName == "long long")
        {
            if (variantType != GB_VariantType::Int64)
            {
                return false;
            }

            long long value = 0;
            if (!ReadExactSignedInteger(payload, 8, value))
            {
                return false;
            }

            outHolder = new Holder<long long>(value);
            return true;
        }

        if (stableTypeName == "unsigned long long")
        {
            if (variantType != GB_VariantType::UInt64)
            {
                return false;
            }

            unsigned long long value = 0;
            if (!ReadExactInteger(payload, 8, value))
            {
                return false;
            }

            outHolder = new Holder<unsigned long long>(value);
            return true;
        }

        if (stableTypeName == "float")
        {
            if (variantType != GB_VariantType::Float)
            {
                return false;
            }

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
            if (variantType != GB_VariantType::Double)
            {
                return false;
            }

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
            if (variantType != GB_VariantType::Double)
            {
                return false;
            }

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
            if (variantType != GB_VariantType::String)
            {
                return false;
            }

            outHolder = new Holder<std::string>(GB_ByteBufferToString(payload));
            return true;
        }

        if (stableTypeName == "GB_ByteBuffer")
        {
            if (variantType != GB_VariantType::Binary)
            {
                return false;
            }

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

bool GB_Variant::IsBuiltinStableTypeName(const std::string& typeName)
{
    return typeName == "bool"
        || typeName == "char"
        || typeName == "signed char"
        || typeName == "unsigned char"
        || typeName == "short"
        || typeName == "unsigned short"
        || typeName == "int"
        || typeName == "unsigned int"
        || typeName == "long"
        || typeName == "unsigned long"
        || typeName == "long long"
        || typeName == "unsigned long long"
        || typeName == "float"
        || typeName == "double"
        || typeName == "long double"
        || typeName == "std::string"
        || typeName == "GB_ByteBuffer";
}

bool GB_Variant::IsReservedTypeName(const std::string& typeName)
{
    return typeName == "Empty" || IsBuiltinStableTypeName(typeName);
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
    : holder_(new Holder<std::string>(value != nullptr ? std::string(value) : std::string()))
{
}

GB_Variant::GB_Variant(char* value)
    : holder_(new Holder<std::string>(value != nullptr ? std::string(value) : std::string()))
{
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

int GB_Variant::CompareForOrdering(const GB_Variant& other) const noexcept
{
    if (holder_ == other.holder_)
    {
        return 0;
    }

    const GB_VariantType leftType = Type();
    const GB_VariantType rightType = other.Type();

    if (IsIntegralVariantType(leftType) && IsIntegralVariantType(rightType))
    {
        return CompareIntegralVariantValues(*this, other);
    }

    if (IsFloatingVariantType(leftType) && IsFloatingVariantType(rightType))
    {
        return CompareFloatingVariantValues(*this, other);
    }

    if (leftType != rightType)
    {
        return static_cast<int>(leftType) < static_cast<int>(rightType) ? -1 : 1;
    }

    if (holder_ == nullptr)
    {
        return 0;
    }

    const std::type_index leftTypeIndex(holder_->GetTypeInfo());
    const std::type_index rightTypeIndex(other.holder_->GetTypeInfo());
    if (leftTypeIndex != rightTypeIndex)
    {
        return leftTypeIndex < rightTypeIndex ? -1 : 1;
    }

    int compareResult = 0;
    if (TryCompareExactStoredValue(holder_->GetTypeInfo(), holder_->GetConstPtr(), other.holder_->GetConstPtr(), compareResult))
    {
        return compareResult;
    }

    GB_ByteBuffer leftPayload;
    GB_ByteBuffer rightPayload;
    const bool leftSerialized = holder_->SerializePayload(leftPayload);
    const bool rightSerialized = other.holder_->SerializePayload(rightPayload);
    if (leftSerialized != rightSerialized)
    {
        return leftSerialized ? 1 : -1;
    }

    if (leftSerialized)
    {
        return CompareByteSequence(leftPayload.empty() ? nullptr : leftPayload.data(),
            leftPayload.size(),
            rightPayload.empty() ? nullptr : rightPayload.data(),
            rightPayload.size());
    }

    const std::uintptr_t leftAddress = reinterpret_cast<std::uintptr_t>(holder_->GetConstPtr());
    const std::uintptr_t rightAddress = reinterpret_cast<std::uintptr_t>(other.holder_->GetConstPtr());
    if (leftAddress < rightAddress)
    {
        return -1;
    }

    if (leftAddress > rightAddress)
    {
        return 1;
    }

    return 0;
}

bool GB_Variant::operator==(const GB_Variant& other) const noexcept
{
    return CompareForOrdering(other) == 0;
}

bool GB_Variant::operator!=(const GB_Variant& other) const noexcept
{
    return CompareForOrdering(other) != 0;
}

bool GB_Variant::operator<(const GB_Variant& other) const noexcept
{
    return CompareForOrdering(other) < 0;
}

bool GB_Variant::operator>(const GB_Variant& other) const noexcept
{
    return CompareForOrdering(other) > 0;
}

bool GB_Variant::operator<=(const GB_Variant& other) const noexcept
{
    return CompareForOrdering(other) <= 0;
}

bool GB_Variant::operator>=(const GB_Variant& other) const noexcept
{
    return CompareForOrdering(other) >= 0;
}

std::size_t GB_Variant::Hash() const noexcept
{
    const GB_VariantType variantType = Type();
    if (holder_ == nullptr)
    {
        return HashValueBytes(variantType);
    }

    if (IsIntegralVariantType(variantType))
    {
        std::size_t hashValue = static_cast<std::size_t>(0x494E5447u);
        HashCombine(hashValue, HashNormalizedIntegralVariantValue(*this));
        return hashValue;
    }

    if (IsFloatingVariantType(variantType))
    {
        std::size_t hashValue = static_cast<std::size_t>(0x464C5447u);
        HashCombine(hashValue, HashNormalizedFloatingVariantValue(*this));
        return hashValue;
    }

    std::size_t hashValue = HashValueBytes(variantType);
    HashCombine(hashValue, std::hash<std::type_index>()(std::type_index(holder_->GetTypeInfo())));

    std::size_t payloadHash = 0;
    if (TryHashExactStoredValue(holder_->GetTypeInfo(), holder_->GetConstPtr(), payloadHash))
    {
        HashCombine(hashValue, payloadHash);
        return hashValue;
    }

    GB_ByteBuffer payload;
    if (holder_->SerializePayload(payload))
    {
        HashCombine(hashValue, HashByteBufferBytes(payload));
        return hashValue;
    }

    HashCombine(hashValue, static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(holder_->GetConstPtr())));
    return hashValue;
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

    std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
    const std::map<std::type_index, CustomTypeRegistration>& registryByType = GetCustomTypeRegistryByType();
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

bool GB_Variant::TryGetSignedValue(long long& outValue) const noexcept
{
    const HolderBase* const holder = GetHolder();
    if (holder == nullptr)
    {
        return false;
    }

    const std::type_info& typeInfo = holder->GetTypeInfo();
    const void* const valuePtr = holder->GetConstPtr();

    if (const bool* const boolValue = TryGetStoredValue<bool>(typeInfo, valuePtr))
    {
        outValue = *boolValue ? 1LL : 0LL;
        return true;
    }

    if (const char* const charValue = TryGetStoredValue<char>(typeInfo, valuePtr))
    {
        outValue = static_cast<long long>(*charValue);
        return true;
    }

    if (const signed char* const signedCharValue = TryGetStoredValue<signed char>(typeInfo, valuePtr))
    {
        outValue = static_cast<long long>(*signedCharValue);
        return true;
    }

    if (const short* const shortValue = TryGetStoredValue<short>(typeInfo, valuePtr))
    {
        outValue = static_cast<long long>(*shortValue);
        return true;
    }

    if (const int* const intValue = TryGetStoredValue<int>(typeInfo, valuePtr))
    {
        outValue = static_cast<long long>(*intValue);
        return true;
    }

    if (const long* const longValue = TryGetStoredValue<long>(typeInfo, valuePtr))
    {
        outValue = static_cast<long long>(*longValue);
        return true;
    }

    if (const long long* const longLongValue = TryGetStoredValue<long long>(typeInfo, valuePtr))
    {
        outValue = *longLongValue;
        return true;
    }

    if (const unsigned char* const unsignedCharValue = TryGetStoredValue<unsigned char>(typeInfo, valuePtr))
    {
        outValue = static_cast<long long>(*unsignedCharValue);
        return true;
    }

    if (const unsigned short* const unsignedShortValue = TryGetStoredValue<unsigned short>(typeInfo, valuePtr))
    {
        outValue = static_cast<long long>(*unsignedShortValue);
        return true;
    }

    if (const unsigned int* const unsignedIntValue = TryGetStoredValue<unsigned int>(typeInfo, valuePtr))
    {
        if (static_cast<unsigned long long>(*unsignedIntValue)
        > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
        {
            return false;
        }

        outValue = static_cast<long long>(*unsignedIntValue);
        return true;
    }

    if (const unsigned long* const unsignedLongValue = TryGetStoredValue<unsigned long>(typeInfo, valuePtr))
    {
        if (static_cast<unsigned long long>(*unsignedLongValue)
        > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
        {
            return false;
        }

        outValue = static_cast<long long>(*unsignedLongValue);
        return true;
    }

    if (const unsigned long long* const unsignedLongLongValue = TryGetStoredValue<unsigned long long>(typeInfo, valuePtr))
    {
        if (*unsignedLongLongValue > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
        {
            return false;
        }

        outValue = static_cast<long long>(*unsignedLongLongValue);
        return true;
    }

    if (const float* const floatValue = TryGetStoredValue<float>(typeInfo, valuePtr))
    {
        return ConvertFloatingPointToSignedInteger(*floatValue, outValue);
    }

    if (const double* const doubleValue = TryGetStoredValue<double>(typeInfo, valuePtr))
    {
        return ConvertFloatingPointToSignedInteger(*doubleValue, outValue);
    }

    if (const long double* const longDoubleValue = TryGetStoredValue<long double>(typeInfo, valuePtr))
    {
        return ConvertFloatingPointToSignedInteger(*longDoubleValue, outValue);
    }

    if (const std::string* const stringValue = TryGetStoredValue<std::string>(typeInfo, valuePtr))
    {
        return TryParseSignedInteger(*stringValue, outValue);
    }

    return false;
}

bool GB_Variant::TryGetUnsignedValue(unsigned long long& outValue) const noexcept
{
    const HolderBase* const holder = GetHolder();
    if (holder == nullptr)
    {
        return false;
    }

    const std::type_info& typeInfo = holder->GetTypeInfo();
    const void* const valuePtr = holder->GetConstPtr();

    if (const bool* const boolValue = TryGetStoredValue<bool>(typeInfo, valuePtr))
    {
        outValue = *boolValue ? 1ULL : 0ULL;
        return true;
    }

    if (const unsigned char* const unsignedCharValue = TryGetStoredValue<unsigned char>(typeInfo, valuePtr))
    {
        outValue = static_cast<unsigned long long>(*unsignedCharValue);
        return true;
    }

    if (const unsigned short* const unsignedShortValue = TryGetStoredValue<unsigned short>(typeInfo, valuePtr))
    {
        outValue = static_cast<unsigned long long>(*unsignedShortValue);
        return true;
    }

    if (const unsigned int* const unsignedIntValue = TryGetStoredValue<unsigned int>(typeInfo, valuePtr))
    {
        outValue = static_cast<unsigned long long>(*unsignedIntValue);
        return true;
    }

    if (const unsigned long* const unsignedLongValue = TryGetStoredValue<unsigned long>(typeInfo, valuePtr))
    {
        outValue = static_cast<unsigned long long>(*unsignedLongValue);
        return true;
    }

    if (const unsigned long long* const unsignedLongLongValue = TryGetStoredValue<unsigned long long>(typeInfo, valuePtr))
    {
        outValue = *unsignedLongLongValue;
        return true;
    }

    if (const char* const charValue = TryGetStoredValue<char>(typeInfo, valuePtr))
    {
        if (static_cast<int>(*charValue) < 0)
        {
            return false;
        }

        outValue = static_cast<unsigned long long>(*charValue);
        return true;
    }

    if (const signed char* const signedCharValue = TryGetStoredValue<signed char>(typeInfo, valuePtr))
    {
        if (*signedCharValue < 0)
        {
            return false;
        }

        outValue = static_cast<unsigned long long>(*signedCharValue);
        return true;
    }

    if (const short* const shortValue = TryGetStoredValue<short>(typeInfo, valuePtr))
    {
        if (*shortValue < 0)
        {
            return false;
        }

        outValue = static_cast<unsigned long long>(*shortValue);
        return true;
    }

    if (const int* const intValue = TryGetStoredValue<int>(typeInfo, valuePtr))
    {
        if (*intValue < 0)
        {
            return false;
        }

        outValue = static_cast<unsigned long long>(*intValue);
        return true;
    }

    if (const long* const longValue = TryGetStoredValue<long>(typeInfo, valuePtr))
    {
        if (*longValue < 0)
        {
            return false;
        }

        outValue = static_cast<unsigned long long>(*longValue);
        return true;
    }

    if (const long long* const longLongValue = TryGetStoredValue<long long>(typeInfo, valuePtr))
    {
        if (*longLongValue < 0)
        {
            return false;
        }

        outValue = static_cast<unsigned long long>(*longLongValue);
        return true;
    }

    if (const float* const floatValue = TryGetStoredValue<float>(typeInfo, valuePtr))
    {
        return ConvertFloatingPointToUnsignedInteger(*floatValue, outValue);
    }

    if (const double* const doubleValue = TryGetStoredValue<double>(typeInfo, valuePtr))
    {
        return ConvertFloatingPointToUnsignedInteger(*doubleValue, outValue);
    }

    if (const long double* const longDoubleValue = TryGetStoredValue<long double>(typeInfo, valuePtr))
    {
        return ConvertFloatingPointToUnsignedInteger(*longDoubleValue, outValue);
    }

    if (const std::string* const stringValue = TryGetStoredValue<std::string>(typeInfo, valuePtr))
    {
        return TryParseUnsignedInteger(*stringValue, outValue);
    }

    return false;
}

bool GB_Variant::TryGetFloatingValue(long double& outValue) const noexcept
{
    const HolderBase* const holder = GetHolder();
    if (holder == nullptr)
    {
        return false;
    }

    const std::type_info& typeInfo = holder->GetTypeInfo();
    const void* const valuePtr = holder->GetConstPtr();

    if (const float* const floatValue = TryGetStoredValue<float>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*floatValue);
        return true;
    }

    if (const double* const doubleValue = TryGetStoredValue<double>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*doubleValue);
        return true;
    }

    if (const long double* const longDoubleValue = TryGetStoredValue<long double>(typeInfo, valuePtr))
    {
        outValue = *longDoubleValue;
        return true;
    }

    if (const bool* const boolValue = TryGetStoredValue<bool>(typeInfo, valuePtr))
    {
        outValue = *boolValue ? 1.0L : 0.0L;
        return true;
    }

    if (const char* const charValue = TryGetStoredValue<char>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*charValue);
        return true;
    }

    if (const signed char* const signedCharValue = TryGetStoredValue<signed char>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*signedCharValue);
        return true;
    }

    if (const unsigned char* const unsignedCharValue = TryGetStoredValue<unsigned char>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*unsignedCharValue);
        return true;
    }

    if (const short* const shortValue = TryGetStoredValue<short>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*shortValue);
        return true;
    }

    if (const unsigned short* const unsignedShortValue = TryGetStoredValue<unsigned short>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*unsignedShortValue);
        return true;
    }

    if (const int* const intValue = TryGetStoredValue<int>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*intValue);
        return true;
    }

    if (const unsigned int* const unsignedIntValue = TryGetStoredValue<unsigned int>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*unsignedIntValue);
        return true;
    }

    if (const long* const longValue = TryGetStoredValue<long>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*longValue);
        return true;
    }

    if (const unsigned long* const unsignedLongValue = TryGetStoredValue<unsigned long>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*unsignedLongValue);
        return true;
    }

    if (const long long* const longLongValue = TryGetStoredValue<long long>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*longLongValue);
        return true;
    }

    if (const unsigned long long* const unsignedLongLongValue = TryGetStoredValue<unsigned long long>(typeInfo, valuePtr))
    {
        outValue = static_cast<long double>(*unsignedLongLongValue);
        return true;
    }

    if (const std::string* const stringValue = TryGetStoredValue<std::string>(typeInfo, valuePtr))
    {
        return TryParseFloatingPoint(*stringValue, outValue);
    }

    return false;
}

bool GB_Variant::ToBool(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    const HolderBase* const holder = GetHolder();
    if (holder == nullptr)
    {
        return false;
    }

    const std::type_info& typeInfo = holder->GetTypeInfo();
    const void* const valuePtr = holder->GetConstPtr();

    if (const bool* const boolValue = TryGetStoredValue<bool>(typeInfo, valuePtr))
    {
        SetSuccessFlag(ok, true);
        return *boolValue;
    }

    if (const std::string* const stringValue = TryGetStoredValue<std::string>(typeInfo, valuePtr))
    {
        try
        {
            const std::string normalized = ToLowerAscii(TrimAscii(*stringValue));
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
        catch (...)
        {
        }

        long long parsedSignedValue = 0;
        if (TryParseSignedInteger(*stringValue, parsedSignedValue))
        {
            SetSuccessFlag(ok, true);
            return parsedSignedValue != 0;
        }

        long double parsedFloatingValue = 0.0L;
        if (TryParseFloatingPoint(*stringValue, parsedFloatingValue))
        {
            if (std::isnan(parsedFloatingValue))
            {
                return false;
            }

            SetSuccessFlag(ok, true);
            return parsedFloatingValue != 0.0L;
        }

        return false;
    }

    long long signedValue = 0;
    if (TryGetSignedValue(signedValue))
    {
        SetSuccessFlag(ok, true);
        return signedValue != 0;
    }

    unsigned long long unsignedValue = 0;
    if (TryGetUnsignedValue(unsignedValue))
    {
        SetSuccessFlag(ok, true);
        return unsignedValue != 0;
    }

    long double floatingValue = 0.0L;
    if (TryGetFloatingValue(floatingValue))
    {
        if (std::isnan(floatingValue))
        {
            return false;
        }

        SetSuccessFlag(ok, true);
        return floatingValue != 0.0L;
    }

    return false;
}

int GB_Variant::ToInt(bool* ok) const noexcept
{
    SetSuccessFlag(ok, false);

    long long value = 0;
    if (!TryGetSignedValue(value))
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
    if (!TryGetUnsignedValue(value))
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
    if (!TryGetSignedValue(value))
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
    if (!TryGetUnsignedValue(value))
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
    if (!TryGetUnsignedValue(value))
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
    if (!TryGetFloatingValue(value))
    {
        return 0.0f;
    }

    if (!std::isfinite(value)
        || value < -static_cast<long double>(std::numeric_limits<float>::max())
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
    if (!TryGetFloatingValue(value))
    {
        return 0.0;
    }

    if (!std::isfinite(value)
        || value < -static_cast<long double>(std::numeric_limits<double>::max())
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

    const HolderBase* const holder = GetHolder();
    if (holder == nullptr)
    {
        return std::string();
    }

    try
    {
        const std::type_info& typeInfo = holder->GetTypeInfo();
        const void* const valuePtr = holder->GetConstPtr();

        if (const std::string* const stringValue = TryGetStoredValue<std::string>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return *stringValue;
        }

        if (const bool* const boolValue = TryGetStoredValue<bool>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return *boolValue ? "true" : "false";
        }

        if (const char* const charValue = TryGetStoredValue<char>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(static_cast<int>(*charValue));
        }

        if (const signed char* const signedCharValue = TryGetStoredValue<signed char>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(static_cast<int>(*signedCharValue));
        }

        if (const unsigned char* const unsignedCharValue = TryGetStoredValue<unsigned char>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(static_cast<unsigned int>(*unsignedCharValue));
        }

        if (const short* const shortValue = TryGetStoredValue<short>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*shortValue);
        }

        if (const unsigned short* const unsignedShortValue = TryGetStoredValue<unsigned short>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*unsignedShortValue);
        }

        if (const int* const intValue = TryGetStoredValue<int>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*intValue);
        }

        if (const unsigned int* const unsignedIntValue = TryGetStoredValue<unsigned int>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*unsignedIntValue);
        }

        if (const long* const longValue = TryGetStoredValue<long>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*longValue);
        }

        if (const unsigned long* const unsignedLongValue = TryGetStoredValue<unsigned long>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*unsignedLongValue);
        }

        if (const long long* const longLongValue = TryGetStoredValue<long long>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*longLongValue);
        }

        if (const unsigned long long* const unsignedLongLongValue = TryGetStoredValue<unsigned long long>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return IntegerToString(*unsignedLongLongValue);
        }

        if (const float* const floatValue = TryGetStoredValue<float>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return FloatingPointToString(*floatValue);
        }

        if (const double* const doubleValue = TryGetStoredValue<double>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return FloatingPointToString(*doubleValue);
        }

        if (const long double* const longDoubleValue = TryGetStoredValue<long double>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return FloatingPointToString(*longDoubleValue);
        }

        if (const GB_ByteBuffer* const binaryValue = TryGetStoredValue<GB_ByteBuffer>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return BinaryToHexString(*binaryValue);
        }
    }
    catch (...)
    {
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

    try
    {
        const std::type_info& typeInfo = holder_->GetTypeInfo();
        const void* const valuePtr = holder_->GetConstPtr();

        if (const GB_ByteBuffer* const binaryValue = TryGetStoredValue<GB_ByteBuffer>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return *binaryValue;
        }

        if (const std::string* const stringValue = TryGetStoredValue<std::string>(typeInfo, valuePtr))
        {
            SetSuccessFlag(ok, true);
            return GB_StringToByteBuffer(*stringValue);
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
    }
    catch (...)
    {
    }

    return GB_ByteBuffer();
}

bool GB_Variant::Serialize(GB_ByteBuffer& outData) const noexcept
{
    outData.clear();

    try
    {
        outData.reserve(64);
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
                std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFuncCopy;
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
                    serializeFuncCopy = iter->second.serializeFunc;
                }

                if (!serializeFuncCopy(holder_->GetConstPtr(), payload))
                {
                    outData.clear();
                    return false;
                }
            }
            else
            {
                if (!holder_->SerializePayload(payload))
                {
                    outData.clear();
                    return false;
                }
            }
        }

        if (stableTypeName.size() > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max()))
        {
            outData.clear();
            return false;
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

    const GB_VariantType variantType = static_cast<GB_VariantType>(variantTypeValue);
    if (!IsValidVariantType(variantType))
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

    HolderBase* newHolder = nullptr;
    if (variantType == GB_VariantType::Empty)
    {
        if (!stableTypeName.empty() || !payload.empty())
        {
            return false;
        }
    }
    else if (IsBuiltinStableTypeName(stableTypeName))
    {
        if (!DeserializeBuiltinValue(stableTypeName, variantType, payload, newHolder))
        {
            return false;
        }
    }
    else
    {
        if (variantType != GB_VariantType::Custom)
        {
            return false;
        }

        std::function<HolderBase* (const GB_ByteBuffer& data)> deserializeFuncCopy;
        {
            std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
            const std::map<std::string, const CustomTypeRegistration*>& registryByName = GetCustomTypeRegistryByName();
            const std::map<std::string, const CustomTypeRegistration*>::const_iterator iter = registryByName.find(stableTypeName);
            if (iter == registryByName.end())
            {
                return false;
            }

            deserializeFuncCopy = iter->second->deserializeFunc;
        }

        try
        {
            newHolder = deserializeFuncCopy(payload);
        }
        catch (...)
        {
            return false;
        }

        if (newHolder == nullptr || newHolder->GetVariantType() != GB_VariantType::Custom)
        {
            delete newHolder;
            return false;
        }
    }

    if (variantType == GB_VariantType::Empty)
    {
        outValue.Reset();
        return true;
    }

    delete outValue.holder_;
    outValue.holder_ = newHolder;
    return true;
}

bool GB_Variant::RegisterCustomType(const std::type_index& typeIndex,
    const std::string& typeName,
    std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFunc,
    std::function<HolderBase* (const GB_ByteBuffer& data)> deserializeFunc)
{
    if (typeName.empty() || IsReservedTypeName(typeName) || !serializeFunc || !deserializeFunc)
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

    try
    {
        const std::pair<std::map<std::type_index, CustomTypeRegistration>::iterator, bool> insertResult =
            registryByType.insert(std::make_pair(typeIndex, std::move(registration)));
        if (!insertResult.second)
        {
            return false;
        }

        try
        {
            registryByName.insert(std::make_pair(typeName, &insertResult.first->second));
        }
        catch (...)
        {
            registryByType.erase(insertResult.first);
            return false;
        }

        return true;
    }
    catch (...)
    {
        return false;
    }
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

