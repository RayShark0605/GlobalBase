#ifndef GB_VARIANT_H
#define GB_VARIANT_H

#include <cstddef>
#include <cstdint>
#include <cstring>
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

/**
 * @brief GB_Variant 当前支持的逻辑类型分类。
 *
 * 说明：
 * - 这里的枚举值用于描述 GB_Variant 的"逻辑大类"，便于比较、转换与序列化。
 * - 对于具体的 C++ 存储类型（例如 char、short、long、std::string、自定义类型等），
 *   仍应结合 TypeInfo() / TypeName() 一起判断。
 */
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

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief 万能值容器。
 *
 * 设计目标：
 * - 像一个轻量级 any / variant 一样，统一承载常见标量、字符串、二进制以及已注册的自定义类型。
 * - 提供稳健的类型查询、按需转换、可排序比较、哈希、二进制序列化/反序列化能力。
 * - 对内建类型尽量维持直接存取与低额外开销；对自定义类型则通过注册表提供扩展点。
 *
 * 说明：
 * - 空对象由 holder_ == nullptr 表示，其 Type() 返回 GB_VariantType::Empty。
 * - 该类本身不是线程安全容器；并发读写同一个实例仍需外部同步。
 * - 自定义类型注册表由内部静态互斥量保护，可安全进行并发注册/查询。
 */
class GLOBALBASE_PORT GB_Variant
{
public:
    /**
     * @brief 构造一个空 Variant。
     *
     * 此时对象不持有任何值，Type() 为 GB_VariantType::Empty。
     */
    GB_Variant();

    /**
     * @brief 使用 nullptr 构造空 Variant。
     *
     * 该重载与默认构造等价，便于写出更直观的空值语义。
     */
    GB_Variant(std::nullptr_t);

    /**
     * @brief 使用 C 风格字符串构造。
     *
     * 说明：
     * - 按字符串语义存储，内部实际持有 std::string。
     * - 当 @p value 为 nullptr 时，按空字符串处理。
     */
    GB_Variant(const char* value);

    /**
     * @brief 使用可写 C 风格字符串构造。
     *
     * 该重载主要用于兼容 char* 实参，行为与 const char* 版本一致。
     */
    GB_Variant(char* value);

    /**
     * @brief 以拷贝方式存储 std::string。
     */
    GB_Variant(const std::string& value);

    /**
     * @brief 以移动方式存储 std::string。
     */
    GB_Variant(std::string&& value);

    /**
     * @brief 以拷贝方式存储二进制缓冲区。
     */
    GB_Variant(const GB_ByteBuffer& value);

    /**
     * @brief 以移动方式存储二进制缓冲区。
     */
    GB_Variant(GB_ByteBuffer&& value);

    /**
     * @brief 拷贝构造。
     *
     * 若 @p other 非空，会克隆其内部 Holder。
     */
    GB_Variant(const GB_Variant& other);

    /**
     * @brief 移动构造。
     *
     * 采用所有权转移语义，源对象会被置为空。
     */
    GB_Variant(GB_Variant&& other) noexcept;

    /**
     * @brief 以模板方式构造任意受支持值。
     *
     * 说明：
     * - 该重载会把值按衰减后的类型 TValueDecay 直接存入 Holder。
     * - GB_Variant / nullptr / C 字符串 / std::string / GB_ByteBuffer 等已有专门重载的类型会被排除，
     *   以避免重载歧义并保留它们的定制语义。
     */
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

    /**
     * @brief 析构函数。
     */
    ~GB_Variant();

    /**
     * @brief 拷贝赋值。
     * @return 当前对象引用。
     */
    GB_Variant& operator=(const GB_Variant& other);

    /**
     * @brief 移动赋值。
     * @return 当前对象引用。
     */
    GB_Variant& operator=(GB_Variant&& other) noexcept;

    /**
     * @brief 判断两个 Variant 是否等价。
     *
     * 等价关系与 CompareForOrdering() 的返回值为 0 保持一致。
     */
    bool operator==(const GB_Variant& other) const noexcept;

    /**
     * @brief 判断两个 Variant 是否不等价。
     */
    bool operator!=(const GB_Variant& other) const noexcept;

    /**
     * @brief 按统一排序语义判断当前值是否小于另一个值。
     *
     * 该顺序可用于 std::map、std::set 等有序容器。
     */
    bool operator<(const GB_Variant& other) const noexcept;

    /**
     * @brief 按统一排序语义判断当前值是否大于另一个值。
     */
    bool operator>(const GB_Variant& other) const noexcept;

    /**
     * @brief 按统一排序语义判断当前值是否小于等于另一个值。
     */
    bool operator<=(const GB_Variant& other) const noexcept;

    /**
     * @brief 按统一排序语义判断当前值是否大于等于另一个值。
     */
    bool operator>=(const GB_Variant& other) const noexcept;

    /**
     * @brief 计算哈希值。
     *
     * 可与 operator== 配合，用于 std::unordered_map / std::unordered_set。
     */
    std::size_t Hash() const noexcept;

    /**
     * @brief 以模板方式赋值任意受支持值。
     *
     * 内部采用先构造临时对象、再移动赋值的写法，以复用构造逻辑。
     */
    template<typename TValue,
        typename TDecayed = typename std::decay<TValue>::type,
        typename std::enable_if<!std::is_same<TDecayed, GB_Variant>::value, int>::type = 0>
    GB_Variant& operator=(TValue&& value)
    {
        GB_Variant newValue(std::forward<TValue>(value));
        *this = std::move(newValue);
        return *this;
    }

    /**
     * @brief 判断当前是否为空。
     */
    bool IsEmpty() const noexcept;

    /**
     * @brief 获取逻辑类型分类。
     */
    GB_VariantType Type() const noexcept;

    /**
     * @brief 获取当前实际存储的 C++ 类型信息。
     *
     * 为空时返回 typeid(void)。
     */
    const std::type_info& TypeInfo() const noexcept;

    /**
     * @brief 获取类型名称。
     *
     * 规则：
     * - 内建类型返回稳定名字（如 int、std::string、GB_ByteBuffer）。
     * - 已注册的自定义类型返回注册名。
     * - 未注册且无法提供稳定名时，退化为编译器提供的 type_info::name()。
     */
    std::string TypeName() const;

    /**
     * @brief 清空当前值并恢复为空状态。
     */
    void Reset() noexcept;

    /**
     * @brief 判断当前值的实际存储类型是否恰好为 TValue。
     *
     * 这里进行的是"精确类型匹配"，不会做数值可转换判断。
     */
    template<typename TValue>
    bool Is() const noexcept
    {
        if (holder_ == nullptr)
        {
            return false;
        }

        return holder_->GetTypeInfo() == typeid(typename std::decay<TValue>::type);
    }

    /**
     * @brief 尝试取得可写的实际存储对象指针。
     *
     * @return 类型匹配时返回内部对象地址，否则返回 nullptr。
     */
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

    /**
     * @brief 尝试取得只读的实际存储对象指针。
     *
     * @return 类型匹配时返回内部对象地址，否则返回 nullptr。
     */
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

    /**
     * @brief 尝试把内部对象按精确类型拷贝到输出参数。
     *
     * @param outValue 输出值。
     * @return 成功返回 true；类型不匹配返回 false。
     */
    template<typename TValue>
    bool AnyCast(TValue& outValue) const
    {
        const TValue* value = AnyCast<TValue>();
        if (value == nullptr)
        {
            return false;
        }

        outValue = *value;
        return true;
    }

    /**
     * @brief 尝试转换为 bool。
     *
     * 支持从布尔、数值以及部分字符串文本（如 true/false/1/0/yes/no/on/off）转换。
     * @param ok 若非空，用于返回转换是否成功。
     */
    bool ToBool(bool* ok = nullptr) const noexcept;

    /**
     * @brief 尝试转换为 int。
     * @param ok 若非空，用于返回转换是否成功。
     */
    int ToInt(bool* ok = nullptr) const noexcept;

    /**
     * @brief 尝试转换为 unsigned int。
     * @param ok 若非空，用于返回转换是否成功。
     */
    unsigned int ToUInt(bool* ok = nullptr) const noexcept;

    /**
     * @brief 尝试转换为 long long。
     * @param ok 若非空，用于返回转换是否成功。
     */
    long long ToInt64(bool* ok = nullptr) const noexcept;

    /**
     * @brief 尝试转换为 unsigned long long。
     * @param ok 若非空，用于返回转换是否成功。
     */
    unsigned long long ToUInt64(bool* ok = nullptr) const noexcept;

    /**
     * @brief 尝试转换为 std::size_t。
     * @param ok 若非空，用于返回转换是否成功。
     */
    std::size_t ToSizeT(bool* ok = nullptr) const noexcept;

    /**
     * @brief 尝试转换为 float。
     * @param ok 若非空，用于返回转换是否成功。
     */
    float ToFloat(bool* ok = nullptr) const noexcept;

    /**
     * @brief 尝试转换为 double。
     * @param ok 若非空，用于返回转换是否成功。
     */
    double ToDouble(bool* ok = nullptr) const noexcept;

    /**
     * @brief 尝试转换为字符串。
     *
     * 对内建数值类型会输出可读文本；对二进制会输出十六进制字符串。
     * @param ok 若非空，用于返回转换是否成功。
     */
    std::string ToString(bool* ok = nullptr) const noexcept;

    /**
     * @brief 尝试转换为二进制缓冲区。
     *
     * 说明：
     * - 若当前本身就是 GB_ByteBuffer，则直接返回。
     * - 若当前是 std::string，则返回其字节内容。
     * - 若当前是已注册自定义类型，则返回其自定义序列化负载。
     */
    GB_ByteBuffer ToBinary(bool* ok = nullptr) const noexcept;

    /**
     * @brief 序列化到二进制缓冲区。
     *
     * 输出格式包含魔数、版本号、逻辑类型、稳定类型名以及有效载荷。
     */
    bool Serialize(GB_ByteBuffer& outData) const noexcept;

    /**
     * @brief 序列化并直接返回二进制结果。
     */
    GB_ByteBuffer Serialize() const noexcept;

    /**
     * @brief 从二进制缓冲区反序列化到当前对象。
     *
     * 仅当整个输入数据有效且反序列化成功时才会修改当前对象。
     */
    bool DeserializeFromBinary(const GB_ByteBuffer& data) noexcept;

    /**
     * @brief 从二进制缓冲区反序列化一个 Variant。
     *
     * @param data 输入数据。
     * @param outValue 输出对象。成功时被赋予新值，失败时保持原样。
     */
    static bool Deserialize(const GB_ByteBuffer& data, GB_Variant& outValue) noexcept;

    /**
     * @brief 注册一个自定义类型的序列化/反序列化规则。
     *
     * 约束：
     * - @p typeName 不能为空，且不能与内建保留类型名冲突。
     * - 同一个 C++ 类型或同一个注册名都只能注册一次。
     *
     * @tparam TValue 需要注册的自定义类型。
     * @param typeName 稳定类型名，会写入序列化结果。
     * @param serializeFunc 把 TValue 转为字节流的函数。
     * @param deserializeFunc 从字节流恢复 TValue 的函数。
     * @return 注册成功返回 true。
     */
    template<typename TValue>
    static bool RegisterType(const std::string& typeName,
        GB_ByteBuffer(*serializeFunc)(const TValue& value),
        bool (*deserializeFunc)(const GB_ByteBuffer& data, TValue& outValue))
    {
        if (typeName.empty() || IsReservedTypeName(typeName) || serializeFunc == nullptr || deserializeFunc == nullptr)
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
    /**
     * @brief 多态 Holder 的公共抽象基类。
     *
     * 负责屏蔽具体值类型差异，并提供克隆、类型查询与序列化入口。
     */
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

    /**
     * @brief 具体值类型的 Holder 实现。
     */
    template<typename TValue>
    struct Holder : public HolderBase
    {
        typedef typename std::decay<TValue>::type ValueType;

        explicit Holder(const ValueType& inputValue): value(inputValue)
        {
        }

        explicit Holder(ValueType&& inputValue): value(std::move(inputValue))
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

    /**
     * @brief 自定义类型注册信息。
     */
    struct CustomTypeRegistration
    {
        std::string typeName;
        std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFunc;
        std::function<HolderBase* (const GB_ByteBuffer& data)> deserializeFunc;
    };

    /**
     * @brief 根据实际 C++ 类型推导逻辑类型分类。
     *
     * 说明：
     * - 这里的推导结果用于 Type()、比较、转换与序列化头信息。
     * - 多个不同的 C++ 类型可能会映射到同一个逻辑类型，例如 char / short / int
     *   都可能归入 Int32；unsigned char / unsigned short / unsigned int 都可能归入 UInt32。
     * - 非内建已知类型统一归入 Custom。
     */
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
            if (std::is_signed<ValueType>::value)
            {
                return sizeof(ValueType) <= 4 ? GB_VariantType::Int32 : GB_VariantType::Int64;
            }

            return sizeof(ValueType) <= 4 ? GB_VariantType::UInt32 : GB_VariantType::UInt64;
        }

        return GB_VariantType::Custom;
    }

    /**
     * @brief 获取内建类型的稳定类型名。
     *
     * 该名字会写入序列化结果，用于跨进程/跨模块/跨编译单元反序列化时识别具体内建类型。
     * 对未纳入内建稳定名体系的类型返回空字符串。
     */
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

    /**
     * @brief 按类型类别分发序列化逻辑的辅助模板。
     *
     * 四个布尔模板参数分别表示：是否整数、是否浮点、是否字符串、是否二进制。
     * 对不属于这些内建类别的类型，会走“自定义类型注册表”路径。
     */
    template<typename TValue, bool IsIntegral, bool IsFloatingPoint, bool IsString, bool IsBinary>
    struct SerializeValueHelper;

    /**
     * @brief 统一的值序列化入口。
     *
     * 该函数只负责把“单个值”编码成 payload，不负责写入 GB_Variant 外层头部信息。
     */
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

    /**
     * @brief 序列化内建整数类型。
     *
     * 采用固定 little-endian 字节序输出，避免依赖宿主机器的整数内存布局。
     */
    template<typename TValue>
    static typename std::enable_if<std::is_integral<typename std::decay<TValue>::type>::value, bool>::type
        SerializeBuiltinValue(const TValue& value, GB_ByteBuffer& outData) noexcept
    {
        typedef typename std::decay<TValue>::type ValueType;

        try
        {
            outData.clear();
            outData.reserve(sizeof(ValueType));
            const unsigned long long shiftedValue = static_cast<unsigned long long>(value);
            for (std::size_t index = 0; index < sizeof(ValueType); index++)
            {
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

    /**
     * @brief 序列化内建浮点类型。
     *
     * 直接按对象字节拷贝输出 payload，因此序列化结果与当前平台的浮点表示相关。
     * 该实现优先保证工程内同平台读写的一致性与效率。
     */
    template<typename TValue>
    static typename std::enable_if<std::is_floating_point<typename std::decay<TValue>::type>::value, bool>::type
        SerializeBuiltinValue(const TValue& value, GB_ByteBuffer& outData) noexcept
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

    /**
     * @brief 序列化字符串为原始字节序列。
     */
    static bool SerializeBuiltinValue(const std::string& value, GB_ByteBuffer& outData) noexcept;

    /**
     * @brief 序列化二进制缓冲区。
     */
    static bool SerializeBuiltinValue(const GB_ByteBuffer& value, GB_ByteBuffer& outData) noexcept;

    /**
     * @brief 根据稳定类型名、逻辑类型和 payload 恢复一个内建 Holder。
     */
    static bool DeserializeBuiltinValue(const std::string& stableTypeName,
        GB_VariantType variantType,
        const GB_ByteBuffer& payload,
        HolderBase*& outHolder) noexcept;

    /**
     * @brief 判断给定名字是否属于内建稳定类型名集合。
     */
    static bool IsBuiltinStableTypeName(const std::string& typeName);

    /**
     * @brief 判断给定名字是否为保留类型名。
     *
     * 当前包括所有内建稳定类型名以及空名等不能被自定义类型占用的名称。
     */
    static bool IsReservedTypeName(const std::string& typeName);

    /**
     * @brief 整数类型的 payload 序列化分发。
     */
    template<typename TValue>
    struct SerializeValueHelper<TValue, true, false, false, false>
    {
        static bool Do(const TValue& value, GB_ByteBuffer& outData) noexcept
        {
            return SerializeBuiltinValue(value, outData);
        }
    };

    /**
     * @brief 浮点类型的 payload 序列化分发。
     */
    template<typename TValue>
    struct SerializeValueHelper<TValue, false, true, false, false>
    {
        static bool Do(const TValue& value, GB_ByteBuffer& outData) noexcept
        {
            return SerializeBuiltinValue(value, outData);
        }
    };

    /**
     * @brief 字符串类型的 payload 序列化分发。
     */
    template<typename TValue>
    struct SerializeValueHelper<TValue, false, false, true, false>
    {
        static bool Do(const TValue& value, GB_ByteBuffer& outData) noexcept
        {
            return SerializeBuiltinValue(value, outData);
        }
    };

    /**
     * @brief 二进制缓冲区类型的 payload 序列化分发。
     */
    template<typename TValue>
    struct SerializeValueHelper<TValue, false, false, false, true>
    {
        static bool Do(const TValue& value, GB_ByteBuffer& outData) noexcept
        {
            return SerializeBuiltinValue(value, outData);
        }
    };

    /**
     * @brief 非内建类型的 payload 序列化分发。
     *
     * 该路径要求类型已通过 RegisterType() 注册；否则序列化失败。
     */
    template<typename TValue>
    struct SerializeValueHelper<TValue, false, false, false, false>
    {
        static bool Do(const TValue& value, GB_ByteBuffer& outData) noexcept
        {
            std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFuncCopy;
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

                serializeFuncCopy = iter->second.serializeFunc;
            }

            return serializeFuncCopy(&value, outData);
        }
    };

    // 尝试按"有符号整数"语义读取当前值。
    // 该接口会接受：有符号整数、非负无符号整数、有限浮点数（截断到整数）、可解析的字符串。
    bool TryGetSignedValue(long long& outValue) const noexcept;

    // 尝试按"无符号整数"语义读取当前值。
    // 该接口会拒绝负值，并对范围越界做显式检查。
    bool TryGetUnsignedValue(unsigned long long& outValue) const noexcept;

    // 尝试按"浮点数"语义读取当前值。
    // 该接口会接受各类数值以及可解析的字符串文本。
    bool TryGetFloatingValue(long double& outValue) const noexcept;

    // 实现所有比较运算符共享的统一排序逻辑。
    // 排序键大致为：空值状态 -> 逻辑类型 -> 实际类型 -> 值内容。
    int CompareForOrdering(const GB_Variant& other) const noexcept;

    /**
     * @brief 向内部注册表写入一条自定义类型记录。
     */
    static bool RegisterCustomType(const std::type_index& typeIndex,
        const std::string& typeName,
        std::function<bool(const void* object, GB_ByteBuffer& outData)> serializeFunc,
        std::function<HolderBase* (const GB_ByteBuffer& data)> deserializeFunc);

    /**
     * @brief 获取自定义类型注册表所使用的全局互斥量。
     */
    static std::mutex& GetCustomTypeRegistryMutex();

    /**
     * @brief 获取按 type_index 索引的注册表。
     */
    static std::map<std::type_index, CustomTypeRegistration>& GetCustomTypeRegistryByType();

    /**
     * @brief 获取按稳定类型名索引的注册表。
     *
     * value 为指向 registryByType 中节点值的指针，因此两张表需要协同维护。
     */
    static std::map<std::string, const CustomTypeRegistration*>& GetCustomTypeRegistryByName();

    /**
     * @brief 获取当前 Holder 的只读指针。
     */
    const HolderBase* GetHolder() const noexcept;

    /**
     * @brief 获取当前 Holder 的可写指针。
     */
    HolderBase* GetHolder() noexcept;

    HolderBase* holder_;
};

namespace std
{
    /**
     * @brief GB_Variant 的标准哈希特化。
     */
    template<>
    struct hash<GB_Variant>
    {
        std::size_t operator()(const GB_Variant& value) const noexcept
        {
            return value.Hash();
        }
    };
}

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GB_VARIANT_H
