#ifndef GB_VARIANT_H
#define GB_VARIANT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>

#include "GB_BaseTypes.h"
#include "GlobalBasePort.h"

enum class GB_VariantType
{
    Empty = 0,
    Bool,
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
    GB_Variant(TValue&& value)
        : holder_(new Holder<TDecayed>(std::forward<TValue>(value)))
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

        return &static_cast<Holder<ValueType>*>(holder_)->value;
    }

    template<typename TValue>
    const TValue* AnyCast() const noexcept
    {
        typedef typename std::decay<TValue>::type ValueType;
        if (!Is<ValueType>())
        {
            return nullptr;
        }

        return &static_cast<const Holder<ValueType>*>(holder_)->value;
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

    bool ToBool(bool* ok = nullptr) const noexcept;
    int ToInt(bool* ok = nullptr) const noexcept;
    unsigned int ToUInt(bool* ok = nullptr) const noexcept;
    long long ToInt64(bool* ok = nullptr) const noexcept;
    unsigned long long ToUInt64(bool* ok = nullptr) const noexcept;
    std::size_t ToSizeT(bool* ok = nullptr) const noexcept;
    float ToFloat(bool* ok = nullptr) const noexcept;
    double ToDouble(bool* ok = nullptr) const noexcept;
    std::string ToString(bool* ok = nullptr) const noexcept;
    GB_ByteBuffer ToBinary(bool* ok = nullptr) const noexcept;

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

        return RegisterCustomType(std::type_index(typeid(TValue)),
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
            [deserializeFunc](const GB_ByteBuffer& data) -> HolderBase*
            {
                try
                {
                    TValue value;
                    if (!deserializeFunc(data, value))
                    {
                        return nullptr;
                    }

                    return new Holder<TValue>(std::move(value));
                }
                catch (...)
                {
                    return nullptr;
                }
            });
    }

private:
    struct HolderBase
    {
        virtual ~HolderBase()
        {
        }

        virtual const std::type_info& GetTypeInfo() const noexcept = 0;
        virtual GB_VariantType GetVariantType() const noexcept = 0;
        virtual HolderBase* Clone() const = 0;
        virtual const void* GetConstPtr() const noexcept = 0;
        virtual void* GetPtr() noexcept = 0;
        virtual std::string GetStableTypeName() const = 0;
        virtual bool SerializePayload(GB_ByteBuffer& outData) const noexcept = 0;
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

        HolderBase* Clone() const override
        {
            return new Holder<ValueType>(value);
        }

        const void* GetConstPtr() const noexcept override
        {
            return &value;
        }

        void* GetPtr() noexcept override
        {
            return &value;
        }

        std::string GetStableTypeName() const override
        {
            return GetStableBuiltinTypeName<ValueType>();
        }

        bool SerializePayload(GB_ByteBuffer& outData) const noexcept override
        {
            return SerializeValue(value, outData);
        }

        ValueType value;
    };

    struct CustomTypeRegistration
    {
        std::string typeName;
        std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFunc;
        std::function<HolderBase* (const GB_ByteBuffer& data)> deserializeFunc;
    };

    template<typename TValue>
    static GB_VariantType DeduceVariantType() noexcept
    {
        typedef typename std::decay<TValue>::type ValueType;

        if (std::is_same<ValueType, bool>::value)
        {
            return GB_VariantType::Bool;
        }

        if (std::is_same<ValueType, std::string>::value)
        {
            return GB_VariantType::String;
        }

        if (std::is_same<ValueType, GB_ByteBuffer>::value)
        {
            return GB_VariantType::Binary;
        }

        if (std::is_same<ValueType, float>::value)
        {
            return GB_VariantType::Float;
        }

        if (std::is_same<ValueType, double>::value || std::is_same<ValueType, long double>::value)
        {
            return GB_VariantType::Double;
        }

        if (std::is_integral<ValueType>::value)
        {
            if (std::is_same<ValueType, bool>::value)
            {
                return GB_VariantType::Bool;
            }

            if (std::is_signed<ValueType>::value)
            {
                return sizeof(ValueType) <= 4 ? GB_VariantType::Int32 : GB_VariantType::Int64;
            }

            return sizeof(ValueType) <= 4 ? GB_VariantType::UInt32 : GB_VariantType::UInt64;
        }

        return GB_VariantType::Custom;
    }

    template<typename TValue>
    static std::string GetStableBuiltinTypeName()
    {
        typedef typename std::decay<TValue>::type ValueType;

        if (std::is_same<ValueType, bool>::value)
        {
            return "bool";
        }

        if (std::is_same<ValueType, char>::value)
        {
            return "char";
        }

        if (std::is_same<ValueType, signed char>::value)
        {
            return "signed char";
        }

        if (std::is_same<ValueType, unsigned char>::value)
        {
            return "unsigned char";
        }

        if (std::is_same<ValueType, short>::value)
        {
            return "short";
        }

        if (std::is_same<ValueType, unsigned short>::value)
        {
            return "unsigned short";
        }

        if (std::is_same<ValueType, int>::value)
        {
            return "int";
        }

        if (std::is_same<ValueType, unsigned int>::value)
        {
            return "unsigned int";
        }

        if (std::is_same<ValueType, long>::value)
        {
            return "long";
        }

        if (std::is_same<ValueType, unsigned long>::value)
        {
            return "unsigned long";
        }

        if (std::is_same<ValueType, long long>::value)
        {
            return "long long";
        }

        if (std::is_same<ValueType, unsigned long long>::value)
        {
            return "unsigned long long";
        }

        if (std::is_same<ValueType, float>::value)
        {
            return "float";
        }

        if (std::is_same<ValueType, double>::value)
        {
            return "double";
        }

        if (std::is_same<ValueType, long double>::value)
        {
            return "long double";
        }

        if (std::is_same<ValueType, std::string>::value)
        {
            return "std::string";
        }

        if (std::is_same<ValueType, GB_ByteBuffer>::value)
        {
            return "GB_ByteBuffer";
        }

        return std::string();
    }

    template<typename TValue, bool IsIntegral, bool IsFloatingPoint, bool IsString, bool IsBinary>
    struct SerializeValueHelper;

    template<typename TValue>
    static bool SerializeValue(const TValue& value, GB_ByteBuffer& outData) noexcept
    {
        typedef typename std::decay<TValue>::type ValueType;
        return SerializeValueHelper<ValueType,
            std::is_integral<ValueType>::value,
            std::is_floating_point<ValueType>::value,
            std::is_same<ValueType, std::string>::value,
            std::is_same<ValueType, GB_ByteBuffer>::value>::Do(value, outData);
    }

    template<typename TValue>
    static typename std::enable_if<std::is_integral<typename std::decay<TValue>::type>::value, bool>::type
        SerializeBuiltinValue(const TValue& value, GB_ByteBuffer& outData) noexcept;

    template<typename TValue>
    static typename std::enable_if<std::is_floating_point<typename std::decay<TValue>::type>::value, bool>::type
        SerializeBuiltinValue(const TValue& value, GB_ByteBuffer& outData) noexcept;

    static bool SerializeBuiltinValue(const std::string& value, GB_ByteBuffer& outData) noexcept;
    static bool SerializeBuiltinValue(const GB_ByteBuffer& value, GB_ByteBuffer& outData) noexcept;
    static bool DeserializeBuiltinValue(const std::string& stableTypeName,
        const GB_ByteBuffer& payload,
        HolderBase*& outHolder) noexcept;

    template<typename TValue>
    struct SerializeValueHelper<TValue, true, false, false, false>
    {
        static bool Do(const TValue& value, GB_ByteBuffer& outData) noexcept
        {
            return SerializeBuiltinValue(value, outData);
        }
    };

    template<typename TValue>
    struct SerializeValueHelper<TValue, false, true, false, false>
    {
        static bool Do(const TValue& value, GB_ByteBuffer& outData) noexcept
        {
            return SerializeBuiltinValue(value, outData);
        }
    };

    template<typename TValue>
    struct SerializeValueHelper<TValue, false, false, true, false>
    {
        static bool Do(const TValue& value, GB_ByteBuffer& outData) noexcept
        {
            return SerializeBuiltinValue(value, outData);
        }
    };

    template<typename TValue>
    struct SerializeValueHelper<TValue, false, false, false, true>
    {
        static bool Do(const TValue& value, GB_ByteBuffer& outData) noexcept
        {
            return SerializeBuiltinValue(value, outData);
        }
    };

    template<typename TValue>
    struct SerializeValueHelper<TValue, false, false, false, false>
    {
        static bool Do(const TValue& value, GB_ByteBuffer& outData) noexcept
        {
            std::lock_guard<std::mutex> lock(GetCustomTypeRegistryMutex());
            const std::map<std::type_index, CustomTypeRegistration>& registryByType = GetCustomTypeRegistryByType();
            const typename std::map<std::type_index, CustomTypeRegistration>::const_iterator iter =
                registryByType.find(std::type_index(typeid(TValue)));
            if (iter == registryByType.end())
            {
                outData.clear();
                return false;
            }

            return iter->second.serializeFunc(&value, outData);
        }
    };

    static bool RegisterCustomType(const std::type_index& typeIndex,
        const std::string& typeName,
        std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFunc,
        std::function<HolderBase* (const GB_ByteBuffer& data)> deserializeFunc);

    static std::mutex& GetCustomTypeRegistryMutex();
    static std::map<std::type_index, CustomTypeRegistration>& GetCustomTypeRegistryByType();
    static std::map<std::string, const CustomTypeRegistration*>& GetCustomTypeRegistryByName();

    const HolderBase* GetHolder() const noexcept;
    HolderBase* GetHolder() noexcept;

    HolderBase* holder_;
};

#endif // GB_VARIANT_H
