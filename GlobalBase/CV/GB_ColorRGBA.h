#ifndef GLOBALBASE_COLOR_RGBA_H_H
#define GLOBALBASE_COLOR_RGBA_H_H

#include "../GlobalBasePort.h"
#include "../GB_BaseTypes.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

struct GB_ColorHSV;
struct GB_ColorHSL;
struct GB_ColorLinearRGBA;

/**
 * @brief 8 位 RGBA 像素颜色。
 *
 * 设计目标：
 * - 仅描述最常见、最实用的 8-bit RGBA 像素；
 * - 保持标准布局、平凡可复制、sizeof == 4，便于 memcpy / GPU 上传；
 * - R/G/B/A 公开存储，方便直接访问与批量处理；
 * - 提供常用颜色运算、插值、Alpha 混合、颜色空间转换等能力。
 */
struct GLOBALBASE_PORT GB_ColorRGBA
{
public:
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    static const GB_ColorRGBA Transparent;
    static const GB_ColorRGBA Black;
    static const GB_ColorRGBA White;
    static const GB_ColorRGBA Red;
    static const GB_ColorRGBA Green;
    static const GB_ColorRGBA Blue;
    static const GB_ColorRGBA Yellow;
    static const GB_ColorRGBA Cyan;
    static const GB_ColorRGBA Magenta;
    static const GB_ColorRGBA Gray;
    static const GB_ColorRGBA LightGray;
    static const GB_ColorRGBA DarkGray;
    static const GB_ColorRGBA Silver;
    static const GB_ColorRGBA Orange;
    static const GB_ColorRGBA Gold;
    static const GB_ColorRGBA Pink;
    static const GB_ColorRGBA Purple;
    static const GB_ColorRGBA Brown;

    /**
     * @brief 默认构造：不透明黑色。
     */
    constexpr GB_ColorRGBA() noexcept : r(0), g(0), b(0), a(255)
    {
    }

    /**
     * @brief 以灰度值构造。
     */
    constexpr explicit GB_ColorRGBA(uint8_t gray, uint8_t alpha = 255) noexcept : r(gray), g(gray), b(gray), a(alpha)
    {
    }

    /**
     * @brief 以 RGBA 四通道构造。
     */
    constexpr GB_ColorRGBA(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) noexcept : r(red), g(green), b(blue), a(alpha)
    {
    }

    /**
     * @brief 以 RGB 三通道构造，Alpha 默认为 255。
     */
    constexpr GB_ColorRGBA(uint8_t red, uint8_t green, uint8_t blue) noexcept : r(red), g(green), b(blue), a(255)
    {
    }

    /**
     * @brief 从 4 字节数组构造。
     */
    constexpr explicit GB_ColorRGBA(const std::array<uint8_t, 4>& rgbaArray) noexcept : r(rgbaArray[0]), g(rgbaArray[1]), b(rgbaArray[2]), a(rgbaArray[3])
    {
    }

    /**
     * @brief 获取原始字节指针，顺序固定为 RGBA。
     */
    uint8_t* Data() noexcept
    {
        return reinterpret_cast<uint8_t*>(this);
    }

    /**
     * @brief 获取原始字节只读指针，顺序固定为 RGBA。
     */
    const uint8_t* Data() const noexcept
    {
        return reinterpret_cast<const uint8_t*>(this);
    }

    /**
     * @brief 下标访问通道。
     * @note 下标范围必须为 [0, 3]，依次对应 r/g/b/a。
     */
    constexpr uint8_t& operator[](size_t channelIndex) noexcept
    {
        assert(channelIndex < 4);
        return channelIndex == 0 ? r : (channelIndex == 1 ? g : (channelIndex == 2 ? b : a));
    }

    /**
     * @brief 下标访问通道（只读）。
     * @note 下标范围必须为 [0, 3]，依次对应 r/g/b/a。
     */
    constexpr const uint8_t& operator[](size_t channelIndex) const noexcept
    {
        assert(channelIndex < 4);
        return channelIndex == 0 ? r : (channelIndex == 1 ? g : (channelIndex == 2 ? b : a));
    }

    /**
     * @brief 重新设置 RGBA。
     */
    constexpr void Set(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255) noexcept
    {
        r = red;
        g = green;
        b = blue;
        a = alpha;
    }

    /**
     * @brief 重新设置为灰度颜色。
     */
    constexpr void SetGray(uint8_t gray, uint8_t alpha = 255) noexcept
    {
        r = gray;
        g = gray;
        b = gray;
        a = alpha;
    }

    /**
     * @brief 转为 4 字节数组，顺序为 RGBA。
     */
    constexpr std::array<uint8_t, 4> ToArray() const noexcept
    {
        return std::array<uint8_t, 4>{ r, g, b, a };
    }

    /**
     * @brief 转为归一化浮点数组，范围 [0, 1]，顺序为 RGBA。
     */
    constexpr std::array<float, 4> ToFloatArray() const noexcept
    {
        return std::array<float, 4>{static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f};
    }

    /**
     * @brief 从归一化 RGBA 构造。
     * @note NaN 与负无穷按 0 处理，正无穷按 1 处理。
     */
    static constexpr GB_ColorRGBA FromFloatArray(const std::array<float, 4>& rgbaArray) noexcept
    {
        return GB_ColorRGBA(ClampUnitFloatToByte(rgbaArray[0]), ClampUnitFloatToByte(rgbaArray[1]), ClampUnitFloatToByte(rgbaArray[2]), ClampUnitFloatToByte(rgbaArray[3]));
    }

    /**
     * @brief 从归一化 RGBA 构造。
     * @note NaN 与负无穷按 0 处理，正无穷按 1 处理。
     */
    static constexpr GB_ColorRGBA FromFloat(float red, float green, float blue, float alpha = 1.0f) noexcept
    {
        return GB_ColorRGBA(ClampUnitFloatToByte(red), ClampUnitFloatToByte(green), ClampUnitFloatToByte(blue), ClampUnitFloatToByte(alpha));
    }

    /**
     * @brief 转为 #RRGGBB 或 #RRGGBBAA 形式的十六进制字符串。
     *
     * @param includeAlpha 是否把 Alpha 也编码进字符串。
     * @param upperCase 是否使用大写十六进制字符。
     */
    std::string ToHexString(bool includeAlpha = false, bool upperCase = true) const;

    /**
     * @brief 解析十六进制颜色字符串。
     *
     * 支持以下形式：
     * - #RGB
     * - #RGBA
     * - #RRGGBB
     * - #RRGGBBAA
     * - 去掉前导 '#' 的对应形式
     *
     * @return true 表示解析成功；false 表示输入非法，outColor 保持不变。
     */
    static bool TryParseHexString(const std::string& colorText, GB_ColorRGBA& outColor);

    /**
     * @brief 转为显式定义字节顺序的 32 位打包值：0xRRGGBBAA。
     */
    constexpr uint32_t ToPackedRGBA() const noexcept
    {
        return (static_cast<uint32_t>(r) << 24) | (static_cast<uint32_t>(g) << 16) | (static_cast<uint32_t>(b) << 8) | static_cast<uint32_t>(a);
    }

    /**
     * @brief 转为显式定义字节顺序的 32 位打包值：0xAARRGGBB。
     */
    constexpr uint32_t ToPackedARGB() const noexcept
    {
        return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }

    /**
     * @brief 转为显式定义字节顺序的 32 位打包值：0xBBGGRRAA。
     */
    constexpr uint32_t ToPackedBGRA() const noexcept
    {
        return (static_cast<uint32_t>(b) << 24) | (static_cast<uint32_t>(g) << 16) | (static_cast<uint32_t>(r) << 8) | static_cast<uint32_t>(a);
    }

    /**
     * @brief 转为显式定义字节顺序的 32 位打包值：0xAABBGGRR。
     */
    constexpr uint32_t ToPackedABGR() const noexcept
    {
        return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
    }

    /**
     * @brief 从 0xRRGGBBAA 形式构造。
     */
    static constexpr GB_ColorRGBA FromPackedRGBA(uint32_t packedRgba) noexcept
    {
        return GB_ColorRGBA(static_cast<uint8_t>((packedRgba >> 24) & 0xFFu), static_cast<uint8_t>((packedRgba >> 16) & 0xFFu), static_cast<uint8_t>((packedRgba >> 8) & 0xFFu), static_cast<uint8_t>(packedRgba & 0xFFu));
    }

    /**
     * @brief 从 0xAARRGGBB 形式构造。
     */
    static constexpr GB_ColorRGBA FromPackedARGB(uint32_t packedArgb) noexcept
    {
        return GB_ColorRGBA(static_cast<uint8_t>((packedArgb >> 16) & 0xFFu), static_cast<uint8_t>((packedArgb >> 8) & 0xFFu), static_cast<uint8_t>(packedArgb & 0xFFu), static_cast<uint8_t>((packedArgb >> 24) & 0xFFu));
    }

    /**
     * @brief 从 0xBBGGRRAA 形式构造。
     */
    static constexpr GB_ColorRGBA FromPackedBGRA(uint32_t packedBgra) noexcept
    {
        return GB_ColorRGBA(static_cast<uint8_t>((packedBgra >> 8) & 0xFFu), static_cast<uint8_t>((packedBgra >> 16) & 0xFFu), static_cast<uint8_t>((packedBgra >> 24) & 0xFFu), static_cast<uint8_t>(packedBgra & 0xFFu));
    }

    /**
     * @brief 从 0xAABBGGRR 形式构造。
     */
    static constexpr GB_ColorRGBA FromPackedABGR(uint32_t packedAbgr) noexcept
    {
        return GB_ColorRGBA(static_cast<uint8_t>(packedAbgr & 0xFFu), static_cast<uint8_t>((packedAbgr >> 8) & 0xFFu), static_cast<uint8_t>((packedAbgr >> 16) & 0xFFu), static_cast<uint8_t>((packedAbgr >> 24) & 0xFFu));
    }

    /**
     * @brief 是否完全不透明。
     */
    constexpr bool IsOpaque() const noexcept
    {
        return a == 255;
    }

    /**
     * @brief 是否完全透明。
     */
    constexpr bool IsTransparent() const noexcept
    {
        return a == 0;
    }

    /**
     * @brief 返回同 RGB、不同 Alpha 的颜色副本。
     */
    constexpr GB_ColorRGBA WithAlpha(uint8_t alpha) const noexcept
    {
        return GB_ColorRGBA(r, g, b, alpha);
    }

    /**
     * @brief 返回红蓝通道交换后的颜色（RGBA -> BGRA）。
     */
    constexpr GB_ColorRGBA BgraSwapped() const noexcept
    {
        return GB_ColorRGBA(b, g, r, a);
    }

    /**
     * @brief 计算 8 位灰度值。
     *
     * 采用常见的 BT.601 权重近似：
     * gray ≈ 0.299 * R + 0.587 * G + 0.114 * B
     */
    constexpr uint8_t GetGray() const noexcept
    {
        return static_cast<uint8_t>((77u * r + 150u * g + 29u * b + 128u) >> 8);
    }

    /**
     * @brief 转为灰度颜色，Alpha 保持不变。
     */
    constexpr GB_ColorRGBA ToGray() const noexcept
    {
        const uint8_t gray = GetGray();
        return GB_ColorRGBA(gray, gray, gray, a);
    }

    /**
     * @brief 反色。
     *
     * @param invertAlpha 是否同时反转 Alpha。
     */
    constexpr GB_ColorRGBA Inverted(bool invertAlpha = false) const noexcept
    {
        return GB_ColorRGBA(static_cast<uint8_t>(255u - r), static_cast<uint8_t>(255u - g), static_cast<uint8_t>(255u - b), invertAlpha ? static_cast<uint8_t>(255u - a) : a);
    }

    /**
     * @brief 返回 Alpha 预乘后的颜色。
     */
    constexpr GB_ColorRGBA PremultiplyAlpha() const noexcept
    {
        return GB_ColorRGBA(MultiplyChannel(r, a), MultiplyChannel(g, a), MultiplyChannel(b, a), a);
    }

    /**
     * @brief 返回从 Alpha 预乘格式恢复后的颜色。
     *
     * @note
     * - a == 0 时没有唯一逆，返回 {0, 0, 0, 0}。
     */
    constexpr GB_ColorRGBA UnpremultiplyAlpha() const noexcept
    {
        if (a == 0)
        {
            return GB_ColorRGBA(0, 0, 0, 0);
        }

        if (a == 255)
        {
            return *this;
        }

        return GB_ColorRGBA(ClampToByte((static_cast<int>(r) * 255 + static_cast<int>(a) / 2) / static_cast<int>(a)), ClampToByte((static_cast<int>(g) * 255 + static_cast<int>(a) / 2) / static_cast<int>(a)), ClampToByte((static_cast<int>(b) * 255 + static_cast<int>(a) / 2) / static_cast<int>(a)), a);
    }

    /**
     * @brief 应用额外不透明度，只调整 Alpha，不修改 RGB。
     * @note NaN 与负无穷按 0 处理，正无穷按饱和值处理。
     */
    constexpr GB_ColorRGBA ApplyOpacity(float opacity) const noexcept
    {
        return GB_ColorRGBA(r, g, b, MultiplyByScalar(a, opacity));
    }

    /**
     * @brief 线性插值。
     *
     * @param t 插值因子，超出 [0, 1] 会被钳制；NaN 按 0 处理。
     */
    static constexpr GB_ColorRGBA Lerp(const GB_ColorRGBA& firstColor, const GB_ColorRGBA& secondColor, float t) noexcept
    {
        const float clampedT = ClampUnitFloat(t);
        return GB_ColorRGBA(
            LerpChannel(firstColor.r, secondColor.r, clampedT),
            LerpChannel(firstColor.g, secondColor.g, clampedT),
            LerpChannel(firstColor.b, secondColor.b, clampedT),
            LerpChannel(firstColor.a, secondColor.a, clampedT));
    }

    /**
     * @brief 标准 source-over Alpha 混合（非预乘输入，非预乘输出）。
     *
     * 计算过程只在最终输出处进行一次四舍五入，避免分阶段除以 255 造成双重舍入误差。
     *
     * @param sourceColor 源颜色（覆盖层）。
     * @param destinationColor 目标颜色（背景层）。
     */
    static constexpr GB_ColorRGBA AlphaBlend(const GB_ColorRGBA& sourceColor, const GB_ColorRGBA& destinationColor) noexcept
    {
        if (sourceColor.a == 255)
        {
            return sourceColor;
        }

        if (sourceColor.a == 0)
        {
            return destinationColor;
        }

        const int sourceAlpha = static_cast<int>(sourceColor.a);
        const int destinationAlpha = static_cast<int>(destinationColor.a);
        const int invSourceAlpha = 255 - sourceAlpha;
        const int alphaNumerator = sourceAlpha * 255 + destinationAlpha * invSourceAlpha;
        if (alphaNumerator <= 0)
        {
            return GB_ColorRGBA(0, 0, 0, 0);
        }

        const int outAlpha = RoundDiv255(alphaNumerator);
        const int redNumerator = static_cast<int>(sourceColor.r) * sourceAlpha * 255 + static_cast<int>(destinationColor.r) * destinationAlpha * invSourceAlpha;
        const int greenNumerator = static_cast<int>(sourceColor.g) * sourceAlpha * 255 + static_cast<int>(destinationColor.g) * destinationAlpha * invSourceAlpha;
        const int blueNumerator = static_cast<int>(sourceColor.b) * sourceAlpha * 255 + static_cast<int>(destinationColor.b) * destinationAlpha * invSourceAlpha;

        return GB_ColorRGBA(
            ClampToByte(DivideRounded(redNumerator, alphaNumerator)),
            ClampToByte(DivideRounded(greenNumerator, alphaNumerator)),
            ClampToByte(DivideRounded(blueNumerator, alphaNumerator)),
            static_cast<uint8_t>(outAlpha));
    }

    /**
     * @brief 转 HSV。
     */
    GB_ColorHSV ToHsv() const noexcept;

    /**
     * @brief 从 HSV 构造 RGBA。
     */
    static GB_ColorRGBA FromHsv(const GB_ColorHSV& hsvColor) noexcept;

    /**
     * @brief 转 HSL。
     */
    GB_ColorHSL ToHsl() const noexcept;

    /**
     * @brief 从 HSL 构造 RGBA。
     */
    static GB_ColorRGBA FromHsl(const GB_ColorHSL& hslColor) noexcept;

    /**
     * @brief 转线性 RGB（sRGB -> Linear）。
     */
    GB_ColorLinearRGBA ToLinearRgba() const noexcept;

    /**
     * @brief 从线性 RGB 构造（Linear -> sRGB）。
     */
    static GB_ColorRGBA FromLinearRgba(const GB_ColorLinearRGBA& linearColor) noexcept;

    /**
     * @brief 返回当前类类型字符串。
     */
    const std::string& GetClassType() const;

    /**
     * @brief 返回当前类类型 Id。
     */
    uint64_t GetClassTypeId() const;

    /**
     * @brief 序列化为便于人类阅读的文本字符串。
     *
     * 文本格式为：
     * (GB_ColorRGBA r,g,b,a)
     */
    std::string SerializeToString() const;

    /**
     * @brief 序列化为二进制缓冲区。
     */
    GB_ByteBuffer SerializeToBinary() const;

    /**
     * @brief 从文本字符串反序列化。
     */
    bool Deserialize(const std::string& data);

    /**
     * @brief 从二进制缓冲区反序列化。
     */
    bool Deserialize(const GB_ByteBuffer& data);

    /**
     * @brief 相等比较。
     */
    constexpr bool operator==(const GB_ColorRGBA& other) const noexcept
    {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }

    /**
     * @brief 不等比较。
     */
    constexpr bool operator!=(const GB_ColorRGBA& other) const noexcept
    {
        return !(*this == other);
    }

    /**
     * @brief 分量饱和加法。
     */
    constexpr GB_ColorRGBA operator+(const GB_ColorRGBA& other) const noexcept
    {
        return GB_ColorRGBA(
            SaturatingAdd(r, other.r),
            SaturatingAdd(g, other.g),
            SaturatingAdd(b, other.b),
            SaturatingAdd(a, other.a));
    }

    /**
     * @brief 分量饱和减法。
     */
    constexpr GB_ColorRGBA operator-(const GB_ColorRGBA& other) const noexcept
    {
        return GB_ColorRGBA(
            SaturatingSub(r, other.r),
            SaturatingSub(g, other.g),
            SaturatingSub(b, other.b),
            SaturatingSub(a, other.a));
    }

    /**
     * @brief 与另一个颜色做分量调制（component-wise multiply）。
     */
    constexpr GB_ColorRGBA operator*(const GB_ColorRGBA& other) const noexcept
    {
        return GB_ColorRGBA(
            MultiplyChannel(r, other.r),
            MultiplyChannel(g, other.g),
            MultiplyChannel(b, other.b),
            MultiplyChannel(a, other.a));
    }

    /**
     * @brief 按标量缩放 RGBA 四个分量。
     * @note NaN 与负无穷按 0 处理，正无穷按饱和值处理。
     */
    constexpr GB_ColorRGBA operator*(float scalar) const noexcept
    {
        return GB_ColorRGBA(
            MultiplyByScalar(r, scalar),
            MultiplyByScalar(g, scalar),
            MultiplyByScalar(b, scalar),
            MultiplyByScalar(a, scalar));
    }

    /**
     * @brief 分量饱和加法赋值。
     */
    constexpr GB_ColorRGBA& operator+=(const GB_ColorRGBA& other) noexcept
    {
        *this = *this + other;
        return *this;
    }

    /**
     * @brief 分量饱和减法赋值。
     */
    constexpr GB_ColorRGBA& operator-=(const GB_ColorRGBA& other) noexcept
    {
        *this = *this - other;
        return *this;
    }

    /**
     * @brief 分量调制赋值。
     */
    constexpr GB_ColorRGBA& operator*=(const GB_ColorRGBA& other) noexcept
    {
        *this = *this * other;
        return *this;
    }

    /**
     * @brief 标量缩放赋值。
     */
    constexpr GB_ColorRGBA& operator*=(float scalar) noexcept
    {
        *this = *this * scalar;
        return *this;
    }

private:
    static constexpr uint8_t ClampToByte(int value) noexcept
    {
        return value <= 0 ? 0 : (value >= 255 ? 255 : static_cast<uint8_t>(value));
    }

    static constexpr float ClampUnitFloat(float value) noexcept
    {
        return !(value > 0.0f) ? 0.0f : (value >= 1.0f ? 1.0f : value);
    }

    static constexpr uint8_t ClampUnitFloatToByte(float value) noexcept
    {
        const float clampedValue = ClampUnitFloat(value);
        return clampedValue >= 1.0f ? 255 : static_cast<uint8_t>(clampedValue * 255.0f + 0.5f);
    }

    static constexpr uint8_t SaturatingAdd(uint8_t firstValue, uint8_t secondValue) noexcept
    {
        return ClampToByte(static_cast<int>(firstValue) + static_cast<int>(secondValue));
    }

    static constexpr uint8_t SaturatingSub(uint8_t firstValue, uint8_t secondValue) noexcept
    {
        return ClampToByte(static_cast<int>(firstValue) - static_cast<int>(secondValue));
    }

    static constexpr int RoundDiv255(int value) noexcept
    {
        return (value + 127) / 255;
    }

    static constexpr int DivideRounded(int numerator, int denominator) noexcept
    {
        return (numerator + denominator / 2) / denominator;
    }

    static constexpr uint8_t MultiplyChannel(uint8_t firstValue, uint8_t secondValue) noexcept
    {
        return static_cast<uint8_t>(RoundDiv255(static_cast<int>(firstValue) * static_cast<int>(secondValue)));
    }

    static constexpr uint8_t MultiplyByScalar(uint8_t channelValue, float scalar) noexcept
    {
        if (channelValue == 0 || !(scalar > 0.0f))
        {
            return 0;
        }

        const float saturationScalar = 255.0f / static_cast<float>(channelValue);
        if (scalar >= saturationScalar)
        {
            return 255;
        }

        return static_cast<uint8_t>(static_cast<float>(channelValue) * scalar + 0.5f);
    }

    static constexpr uint8_t LerpChannel(uint8_t firstValue, uint8_t secondValue, float t) noexcept
    {
        return ClampToByte(static_cast<int>(static_cast<float>(firstValue) + (static_cast<float>(secondValue) - static_cast<float>(firstValue)) * t + 0.5f));
    }
};

/**
 * @brief 支持 scalar * color 的写法。
 */
inline constexpr GB_ColorRGBA operator*(float scalar, const GB_ColorRGBA& color) noexcept
{
    return color * scalar;
}

static_assert(std::is_same<uint8_t, unsigned char>::value, "GB_ColorRGBA requires uint8_t to be unsigned char for raw byte access.");
static_assert(sizeof(GB_ColorRGBA) == 4, "GB_ColorRGBA must be exactly 4 bytes.");
static_assert(alignof(GB_ColorRGBA) == 1, "GB_ColorRGBA alignment must be 1.");
static_assert(offsetof(GB_ColorRGBA, r) == 0, "GB_ColorRGBA::r offset must be 0.");
static_assert(offsetof(GB_ColorRGBA, g) == 1, "GB_ColorRGBA::g offset must be 1.");
static_assert(offsetof(GB_ColorRGBA, b) == 2, "GB_ColorRGBA::b offset must be 2.");
static_assert(offsetof(GB_ColorRGBA, a) == 3, "GB_ColorRGBA::a offset must be 3.");
static_assert(std::is_standard_layout<GB_ColorRGBA>::value, "GB_ColorRGBA must be a standard-layout type.");
static_assert(std::is_trivially_copyable<GB_ColorRGBA>::value, "GB_ColorRGBA must be trivially copyable.");

/**
 * @brief HSV 颜色（浮点表示）。
 *
 * 约定：
 * - h：色相，范围 [0, 360)。
 * - s：饱和度，范围 [0, 1]。
 * - v：明度，范围 [0, 1]。
 * - a：Alpha，范围 [0, 1]。
 */
struct GB_ColorHSV
{
public:
    constexpr GB_ColorHSV() noexcept = default;

    constexpr GB_ColorHSV(float hue, float saturation, float value, float alpha = 1.0f) noexcept : h(hue), s(saturation), v(value), a(alpha)
    {
    }

public:
    float h = 0.0f;
    float s = 0.0f;
    float v = 0.0f;
    float a = 1.0f;
};

/**
 * @brief HSL 颜色（浮点表示）。
 *
 * 约定：
 * - h：色相，范围 [0, 360)。
 * - s：饱和度，范围 [0, 1]。
 * - l：亮度，范围 [0, 1]。
 * - a：Alpha，范围 [0, 1]。
 */
struct GB_ColorHSL
{
public:
    constexpr GB_ColorHSL() noexcept = default;

    constexpr GB_ColorHSL(float hue, float saturation, float lightness, float alpha = 1.0f) noexcept : h(hue), s(saturation), l(lightness), a(alpha)
    {
    }

public:
    float h = 0.0f;
    float s = 0.0f;
    float l = 0.0f;
    float a = 1.0f;
};

/**
 * @brief 线性空间 RGBA 颜色（浮点表示）。
 *
 * 约定：
 * - r/g/b：线性 RGB 分量，范围 [0, 1]。
 * - a：Alpha，范围 [0, 1]。
 */
struct GB_ColorLinearRGBA
{
public:
    constexpr GB_ColorLinearRGBA() noexcept = default;

    constexpr GB_ColorLinearRGBA(float red, float green, float blue, float alpha = 1.0f) noexcept
        : r(red), g(green), b(blue), a(alpha)
    {
    }

public:
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};
#endif
