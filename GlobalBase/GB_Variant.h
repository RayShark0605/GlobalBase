#ifndef GB_VARIANT_H
#define GB_VARIANT_H

#include <cstddef>
#include <cstdint>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include "GB_BaseTypes.h"
#include "GlobalBasePort.h"

enum class GB_VariantType
{
    Empty = 0,
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
    String,
    Binary,
    Custom
};

class GLOBALBASE_PORT GB_Variant
{
public:
    GB_Variant();
    GB_Variant(std::nullptr_t);
    GB_Variant(const char* value);
    GB_Variant(char* value);
    GB_Variant(const std::string& value);
    GB_Variant(std::string&& value);
    GB_Variant(const GB_ByteBuffer& value);
    GB_Variant(GB_ByteBuffer&& value);
    GB_Variant(const GB_Variant& other);
    GB_Variant(GB_Variant&& other) noexcept;

    template<typename TValue,
        typename TDecayed = typename std::decay<TValue>::type,
        typename std::enable_if<!std::is_same<TDecayed, GB_Variant>::value
        && !std::is_same<TDecayed, std::nullptr_t>::value
        && !std::is_same<TDecayed, const char*>::value
        && !std::is_same<TDecayed, char*>::value
        && !std::is_same<TDecayed, std::string>::value
        && !std::is_same<TDecayed, GB_ByteBuffer>::value,
        int>::type = 0>
    GB_Variant(TValue&& value): holder_(new Holder<TDecayed>(std::forward<TValue>(value)))
    {
    }

    ~GB_Variant();

    GB_Variant& operator=(const GB_Variant& other);
    GB_Variant& operator=(GB_Variant&& other) noexcept;

    template<typename TValue,
        typename TDecayed = typename std::decay<TValue>::type,
        typename std::enable_if<!std::is_same<TDecayed, GB_Variant>::value, int>::type = 0>
    GB_Variant& operator=(TValue&& value)
    {
        GB_Variant newValue(std::forward<TValue>(value));
        *this = std::move(newValue);
        return *this;
    }

    bool operator==(const GB_Variant& other) const noexcept;
    bool operator!=(const GB_Variant& other) const noexcept;

    bool IsEmpty() const noexcept;
    GB_VariantType Type() const noexcept;
    const std::type_info& TypeInfo() const noexcept;
    std::string TypeName() const;
    void Reset() noexcept;

    template<typename TValue>
    bool Is() const noexcept
    {
        if (holder_ == nullptr)
        {
            return false;
        }

        return holder_->GetTypeInfo() == typeid(typename std::decay<TValue>::type);
    }

    template<typename TValue>
    TValue* AnyCast() noexcept
    {
        typedef typename std::decay<TValue>::type ValueType;

        if (!Is<ValueType>())
        {
            return nullptr;
        }

        return &static_cast<Holder<ValueType>*>(holder_.get())->value;
    }

    template<typename TValue>
    const TValue* AnyCast() const noexcept
    {
        typedef typename std::decay<TValue>::type ValueType;

        if (!Is<ValueType>())
        {
            return nullptr;
        }

        return &static_cast<const Holder<ValueType>*>(holder_.get())->value;
    }

    template<typename TValue>
    bool AnyCast(TValue& outValue) const noexcept
    {
        const TValue* value = AnyCast<TValue>();

        if (value == nullptr)
        {
            return false;
        }

        outValue = *value;
        return true;
    }

    bool CanCast(GB_VariantType targetType) const noexcept;

    template<typename TValue>
    bool CanCast() const noexcept
    {
        typedef typename std::decay<TValue>::type ValueType;

        if constexpr (std::is_same_v<ValueType, std::string>)
        {
            return CanCast(GB_VariantType::String);
        }
        else if constexpr (std::is_same_v<ValueType, GB_ByteBuffer>)
        {
            return Is<GB_ByteBuffer>();
        }
        else if constexpr (std::is_same_v<ValueType, bool>)
        {
            return CanCast(GB_VariantType::Bool);
        }
        else if constexpr (std::is_same_v<ValueType, float>)
        {
            return CanCast(GB_VariantType::Float);
        }
        else if constexpr (std::is_same_v<ValueType, double> || std::is_same_v<ValueType, long double>)
        {
            return CanCast(GB_VariantType::Double);
        }
        else if constexpr (std::is_integral_v<ValueType>)
        {
            return CanCast(DeduceVariantType<ValueType>());
        }
        else
        {
            return Is<ValueType>();
        }
    }

    bool ToBool(bool* ok = nullptr) const noexcept;
    std::int8_t ToInt8(bool* ok = nullptr) const noexcept;
    std::uint8_t ToUInt8(bool* ok = nullptr) const noexcept;
    std::int16_t ToInt16(bool* ok = nullptr) const noexcept;
    std::uint16_t ToUInt16(bool* ok = nullptr) const noexcept;
    int ToInt(bool* ok = nullptr) const noexcept;
    unsigned int ToUInt(bool* ok = nullptr) const noexcept;
    long long ToInt64(bool* ok = nullptr) const noexcept;
    unsigned long long ToUInt64(bool* ok = nullptr) const noexcept;
    std::size_t ToSizeT(bool* ok = nullptr) const noexcept;
    float ToFloat(bool* ok = nullptr) const noexcept;
    double ToDouble(bool* ok = nullptr) const noexcept;
    std::string ToString(bool* ok = nullptr) const noexcept;

    bool Serialize(GB_ByteBuffer& outData) const noexcept;
    GB_ByteBuffer Serialize() const noexcept;
    bool DeserializeFromBinary(const GB_ByteBuffer& data) noexcept;
    static bool Deserialize(const GB_ByteBuffer& data, GB_Variant& outValue) noexcept;

    template<typename TValue>
    static bool RegisterType(const std::string& typeName,
        GB_ByteBuffer(*serializeFunc)(const TValue& value),
        bool (*deserializeFunc)(const GB_ByteBuffer& data, TValue& outValue))
    {
        if (typeName.empty() || serializeFunc == nullptr || deserializeFunc == nullptr)
        {
            return false;
        }

        return RegisterCustomType(
            std::type_index(typeid(TValue)),
            typeName,
            [serializeFunc](const void* object, GB_ByteBuffer& outData) -> bool
            {
                if (object == nullptr)
                {
                    return false;
                }

                try
                {
                    outData = serializeFunc(*static_cast<const TValue*>(object));
                    return true;
                }
                catch (...)
                {
                    outData.clear();
                    return false;
                }
            },
            [deserializeFunc](const GB_ByteBuffer& data) -> std::unique_ptr<struct HolderBase>
            {
                try
                {
                    TValue value;
                    if (!deserializeFunc(data, value))
                    {
                        return std::unique_ptr<HolderBase>();
                    }

                    return std::unique_ptr<HolderBase>(new Holder<TValue>(std::move(value)));
                }
                catch (...)
                {
                    return std::unique_ptr<HolderBase>();
                }
            });
    }

private:
    template<typename TValue, typename = void>
    struct HasEqualOperator : std::false_type
    {
    };

    template<typename TValue>
    struct HasEqualOperator<TValue,
        std::void_t<decltype(std::declval<const TValue&>() == std::declval<const TValue&>())>> : std::true_type
    {
    };

    template<typename TValue, typename = void>
    struct HasLessOperator : std::false_type
    {
    };

    template<typename TValue>
    struct HasLessOperator<TValue,
        std::void_t<decltype(std::declval<const TValue&>() < std::declval<const TValue&>())>> : std::true_type
    {
    };

    template<typename TValue, typename = void>
    struct HasStdHash : std::false_type
    {
    };

    template<typename TValue>
    struct HasStdHash<TValue,
        std::void_t<decltype(std::hash<TValue>()(std::declval<const TValue&>()))>> : std::true_type
    {
    };

    struct HolderBase
    {
        virtual ~HolderBase() {}
        virtual const std::type_info& GetTypeInfo() const noexcept = 0;
        virtual GB_VariantType GetVariantType() const noexcept = 0;
        virtual std::unique_ptr<HolderBase> Clone() const = 0;
        virtual const void* GetConstPtr() const noexcept = 0;
        virtual void* GetPtr() noexcept = 0;
        virtual bool TryEquals(const HolderBase& other, bool& outResult) const noexcept = 0;
        virtual bool TryLess(const HolderBase& other, bool& outResult) const noexcept = 0;
        virtual bool TryHash(std::size_t& outHash) const noexcept = 0;
    };

    template<typename TValue>
    struct Holder : public HolderBase
    {
        typedef typename std::decay<TValue>::type ValueType;

        explicit Holder(const ValueType& inputValue)
            : value(inputValue)
        {
        }

        explicit Holder(ValueType&& inputValue)
            : value(std::move(inputValue))
        {
        }

        const std::type_info& GetTypeInfo() const noexcept override
        {
            return typeid(ValueType);
        }

        GB_VariantType GetVariantType() const noexcept override
        {
            return DeduceVariantType<ValueType>();
        }

        std::unique_ptr<HolderBase> Clone() const override
        {
            return std::unique_ptr<HolderBase>(new Holder<ValueType>(value));
        }

        const void* GetConstPtr() const noexcept override
        {
            return &value;
        }

        void* GetPtr() noexcept override
        {
            return &value;
        }

        bool TryEquals(const HolderBase& other, bool& outResult) const noexcept override
        {
            const Holder<ValueType>* rightHolder = dynamic_cast<const Holder<ValueType>*>(&other);
            if (rightHolder == nullptr)
            {
                return false;
            }

            if constexpr (std::is_arithmetic_v<ValueType>
                || std::is_same_v<ValueType, std::string>
                || std::is_same_v<ValueType, GB_ByteBuffer>
                || HasEqualOperator<ValueType>::value)
            {
                try
                {
                    outResult = value == rightHolder->value;
                    return true;
                }
                catch (...)
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }

        bool TryLess(const HolderBase& other, bool& outResult) const noexcept override
        {
            const Holder<ValueType>* rightHolder = dynamic_cast<const Holder<ValueType>*>(&other);
            if (rightHolder == nullptr)
            {
                return false;
            }

            if constexpr (std::is_arithmetic_v<ValueType>
                || std::is_same_v<ValueType, std::string>
                || std::is_same_v<ValueType, GB_ByteBuffer>
                || HasLessOperator<ValueType>::value)
            {
                try
                {
                    outResult = value < rightHolder->value;
                    return true;
                }
                catch (...)
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }

        bool TryHash(std::size_t& outHash) const noexcept override
        {
            if constexpr (HasStdHash<ValueType>::value)
            {
                try
                {
                    outHash = std::hash<ValueType>()(value);
                    return true;
                }
                catch (...)
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }

        ValueType value;
    };

    struct CustomTypeRegistration
    {
        std::string typeName;
        std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFunc;
        std::function<std::unique_ptr<HolderBase>(const GB_ByteBuffer& data)> deserializeFunc;
    };

    template<typename TValue>
    static GB_VariantType DeduceVariantType() noexcept
    {
        typedef typename std::decay<TValue>::type ValueType;

        if constexpr (std::is_same_v<ValueType, bool>)
        {
            return GB_VariantType::Bool;
        }
        else if constexpr (std::is_same_v<ValueType, std::string>)
        {
            return GB_VariantType::String;
        }
        else if constexpr (std::is_same_v<ValueType, GB_ByteBuffer>)
        {
            return GB_VariantType::Binary;
        }
        else if constexpr (std::is_same_v<ValueType, float>)
        {
            return GB_VariantType::Float;
        }
        else if constexpr (std::is_same_v<ValueType, double> || std::is_same_v<ValueType, long double>)
        {
            return GB_VariantType::Double;
        }
        else if constexpr (std::is_integral_v<ValueType>)
        {
            if constexpr (std::is_signed_v<ValueType>)
            {
                if constexpr (sizeof(ValueType) <= 1)
                {
                    return GB_VariantType::Int8;
                }
                else if constexpr (sizeof(ValueType) <= 2)
                {
                    return GB_VariantType::Int16;
                }
                else if constexpr (sizeof(ValueType) <= 4)
                {
                    return GB_VariantType::Int32;
                }
                else
                {
                    return GB_VariantType::Int64;
                }
            }
            else
            {
                if constexpr (sizeof(ValueType) <= 1)
                {
                    return GB_VariantType::UInt8;
                }
                else if constexpr (sizeof(ValueType) <= 2)
                {
                    return GB_VariantType::UInt16;
                }
                else if constexpr (sizeof(ValueType) <= 4)
                {
                    return GB_VariantType::UInt32;
                }
                else
                {
                    return GB_VariantType::UInt64;
                }
            }
        }
        else
        {
            return GB_VariantType::Custom;
        }
    }

    static bool RegisterCustomType(
        const std::type_index& typeIndex,
        const std::string& typeName,
        std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFunc,
        std::function<std::unique_ptr<HolderBase>(const GB_ByteBuffer& data)> deserializeFunc);
    static std::mutex& GetCustomTypeRegistryMutex();
    static std::map<std::type_index, CustomTypeRegistration>& GetCustomTypeRegistryByType();
    static std::map<std::string, CustomTypeRegistration>& GetCustomTypeRegistryByName();

    const HolderBase* GetHolder() const noexcept;
    HolderBase* GetHolder() noexcept;

    static bool TryInternalEquals(const GB_Variant& left, const GB_Variant& right, bool& outResult) noexcept;
    static bool TryInternalLess(const GB_Variant& left, const GB_Variant& right, bool& outResult) noexcept;
    static bool TryInternalHash(const GB_Variant& value, std::size_t& outHash) noexcept;

    std::unique_ptr<HolderBase> holder_;

    friend struct GB_VariantLess;
    friend struct GB_VariantEqual;
    friend struct GB_VariantHash;
};

struct GLOBALBASE_PORT GB_VariantLess
{
    bool operator()(const GB_Variant& left, const GB_Variant& right) const noexcept;
};

struct GLOBALBASE_PORT GB_VariantEqual
{
    bool operator()(const GB_Variant& left, const GB_Variant& right) const noexcept;
};

struct GLOBALBASE_PORT GB_VariantHash
{
    std::size_t operator()(const GB_Variant& value) const noexcept;
};

#endif // GB_VARIANT_H
