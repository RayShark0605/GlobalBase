#include "GB_ColorRGBA.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr static inline bool IsAsciiWhitespace(char character) noexcept
    {
        return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' || character == '\v';
    }

    static std::string TrimAsciiWhitespace(const std::string& text)
    {
        size_t beginIndex = 0;
        while (beginIndex < text.size() && IsAsciiWhitespace(text[beginIndex]))
        {
            beginIndex++;
        }

        size_t endIndex = text.size();
        while (endIndex > beginIndex && IsAsciiWhitespace(text[endIndex - 1]))
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    static inline int HexCharToValue(char character) noexcept
    {
        if (character >= '0' && character <= '9')
        {
            return character - '0';
        }

        if (character >= 'a' && character <= 'f')
        {
            return character - 'a' + 10;
        }

        if (character >= 'A' && character <= 'F')
        {
            return character - 'A' + 10;
        }

        return -1;
    }

    static inline bool ParseHexByte(char highCharacter, char lowCharacter, uint8_t& outValue) noexcept
    {
        const int highValue = HexCharToValue(highCharacter);
        const int lowValue = HexCharToValue(lowCharacter);
        if (highValue < 0 || lowValue < 0)
        {
            return false;
        }

        outValue = static_cast<uint8_t>((highValue << 4) | lowValue);
        return true;
    }

    static inline float ClampUnitFloat(float value) noexcept
    {
        return value <= 0.0f ? 0.0f : (value >= 1.0f ? 1.0f : value);
    }

    static inline float WrapHue360(float hue) noexcept
    {
        if (!std::isfinite(hue))
        {
            return 0.0f;
        }

        float wrappedHue = std::fmod(hue, 360.0f);
        if (wrappedHue < 0.0f)
        {
            wrappedHue += 360.0f;
        }

        if (wrappedHue >= 360.0f)
        {
            wrappedHue -= 360.0f;
        }

        return wrappedHue;
    }

    static inline float SrgbToLinear(float srgbValue) noexcept
    {
        const float clampedValue = ClampUnitFloat(srgbValue);
        if (clampedValue <= 0.04045f)
        {
            return clampedValue / 12.92f;
        }

        return std::pow((clampedValue + 0.055f) / 1.055f, 2.4f);
    }

    static inline float LinearToSrgb(float linearValue) noexcept
    {
        const float clampedValue = ClampUnitFloat(linearValue);
        if (clampedValue <= 0.0031308f)
        {
            return 12.92f * clampedValue;
        }

        return 1.055f * std::pow(clampedValue, 1.0f / 2.4f) - 0.055f;
    }

    static inline float HueToRgb(float pValue, float qValue, float hue) noexcept
    {
        float adjustedHue = hue;
        if (adjustedHue < 0.0f)
        {
            adjustedHue += 1.0f;
        }
        if (adjustedHue > 1.0f)
        {
            adjustedHue -= 1.0f;
        }

        if (adjustedHue < 1.0f / 6.0f)
        {
            return pValue + (qValue - pValue) * 6.0f * adjustedHue;
        }

        if (adjustedHue < 1.0f / 2.0f)
        {
            return qValue;
        }

        if (adjustedHue < 2.0f / 3.0f)
        {
            return pValue + (qValue - pValue) * (2.0f / 3.0f - adjustedHue) * 6.0f;
        }

        return pValue;
    }
}

const GB_ColorRGBA GB_ColorRGBA::Transparent(0, 0, 0, 0);
const GB_ColorRGBA GB_ColorRGBA::Black(0, 0, 0, 255);
const GB_ColorRGBA GB_ColorRGBA::White(255, 255, 255, 255);
const GB_ColorRGBA GB_ColorRGBA::Red(255, 0, 0, 255);
const GB_ColorRGBA GB_ColorRGBA::Green(0, 255, 0, 255);
const GB_ColorRGBA GB_ColorRGBA::Blue(0, 0, 255, 255);
const GB_ColorRGBA GB_ColorRGBA::Yellow(255, 255, 0, 255);
const GB_ColorRGBA GB_ColorRGBA::Cyan(0, 255, 255, 255);
const GB_ColorRGBA GB_ColorRGBA::Magenta(255, 0, 255, 255);
const GB_ColorRGBA GB_ColorRGBA::Gray(128, 128, 128, 255);
const GB_ColorRGBA GB_ColorRGBA::LightGray(211, 211, 211, 255);
const GB_ColorRGBA GB_ColorRGBA::DarkGray(64, 64, 64, 255);
const GB_ColorRGBA GB_ColorRGBA::Silver(192, 192, 192, 255);
const GB_ColorRGBA GB_ColorRGBA::Orange(255, 165, 0, 255);
const GB_ColorRGBA GB_ColorRGBA::Gold(255, 215, 0, 255);
const GB_ColorRGBA GB_ColorRGBA::Pink(255, 192, 203, 255);
const GB_ColorRGBA GB_ColorRGBA::Purple(128, 0, 128, 255);
const GB_ColorRGBA GB_ColorRGBA::Brown(165, 42, 42, 255);

std::string GB_ColorRGBA::ToHexString(bool includeAlpha, bool upperCase) const
{
    static const char* upperDigits = "0123456789ABCDEF";
    static const char* lowerDigits = "0123456789abcdef";
    const char* digits = upperCase ? upperDigits : lowerDigits;

    std::string result;
    result.resize(includeAlpha ? 9 : 7);
    result[0] = '#';

    const auto appendByte = [&](size_t offset, uint8_t value)
        {
            result[offset] = digits[(value >> 4) & 0x0F];
            result[offset + 1] = digits[value & 0x0F];
        };

    appendByte(1, r);
    appendByte(3, g);
    appendByte(5, b);
    if (includeAlpha)
    {
        appendByte(7, a);
    }

    return result;
}

bool GB_ColorRGBA::TryParseHexString(const std::string& colorText, GB_ColorRGBA& outColor)
{
    const std::string trimmedText = TrimAsciiWhitespace(colorText);
    if (trimmedText.empty())
    {
        return false;
    }

    size_t beginIndex = 0;
    if (trimmedText[0] == '#')
    {
        beginIndex = 1;
    }

    const size_t hexLength = trimmedText.size() - beginIndex;
    const char* hexText = trimmedText.data() + beginIndex;

    GB_ColorRGBA parsedColor;
    if (hexLength == 3 || hexLength == 4)
    {
        const int redValue = HexCharToValue(hexText[0]);
        const int greenValue = HexCharToValue(hexText[1]);
        const int blueValue = HexCharToValue(hexText[2]);
        const int alphaValue = hexLength == 4 ? HexCharToValue(hexText[3]) : 15;
        if (redValue < 0 || greenValue < 0 || blueValue < 0 || alphaValue < 0)
        {
            return false;
        }

        parsedColor.r = static_cast<uint8_t>((redValue << 4) | redValue);
        parsedColor.g = static_cast<uint8_t>((greenValue << 4) | greenValue);
        parsedColor.b = static_cast<uint8_t>((blueValue << 4) | blueValue);
        parsedColor.a = static_cast<uint8_t>((alphaValue << 4) | alphaValue);
        outColor = parsedColor;
        return true;
    }

    if (hexLength == 6 || hexLength == 8)
    {
        if (!ParseHexByte(hexText[0], hexText[1], parsedColor.r) || !ParseHexByte(hexText[2], hexText[3], parsedColor.g) || !ParseHexByte(hexText[4], hexText[5], parsedColor.b))
        {
            return false;
        }

        parsedColor.a = 255;
        if (hexLength == 8 && !ParseHexByte(hexText[6], hexText[7], parsedColor.a))
        {
            return false;
        }

        outColor = parsedColor;
        return true;
    }

    return false;
}

GB_ColorHSV GB_ColorRGBA::ToHsv() const noexcept
{
    const float red = static_cast<float>(r) / 255.0f;
    const float green = static_cast<float>(g) / 255.0f;
    const float blue = static_cast<float>(b) / 255.0f;

    const float maxValue = std::max(red, std::max(green, blue));
    const float minValue = std::min(red, std::min(green, blue));
    const float delta = maxValue - minValue;

    GB_ColorHSV hsvColor;
    hsvColor.v = maxValue;
    hsvColor.a = static_cast<float>(a) / 255.0f;

    if (maxValue <= 0.0f)
    {
        hsvColor.h = 0.0f;
        hsvColor.s = 0.0f;
        return hsvColor;
    }

    hsvColor.s = delta / maxValue;
    if (delta <= 0.0f)
    {
        hsvColor.h = 0.0f;
        return hsvColor;
    }

    if (maxValue == red)
    {
        hsvColor.h = 60.0f * std::fmod(((green - blue) / delta), 6.0f);
    }
    else if (maxValue == green)
    {
        hsvColor.h = 60.0f * (((blue - red) / delta) + 2.0f);
    }
    else
    {
        hsvColor.h = 60.0f * (((red - green) / delta) + 4.0f);
    }

    hsvColor.h = WrapHue360(hsvColor.h);
    return hsvColor;
}

GB_ColorRGBA GB_ColorRGBA::FromHsv(const GB_ColorHSV& hsvColor) noexcept
{
    const float hue = WrapHue360(hsvColor.h);
    const float saturation = ClampUnitFloat(hsvColor.s);
    const float value = ClampUnitFloat(hsvColor.v);
    const float alpha = ClampUnitFloat(hsvColor.a);

    if (saturation <= 0.0f)
    {
        return GB_ColorRGBA::FromFloat(value, value, value, alpha);
    }

    const float chroma = value * saturation;
    const float hueSection = hue / 60.0f;
    const float secondComponent = chroma * (1.0f - std::fabs(std::fmod(hueSection, 2.0f) - 1.0f));
    const float matchValue = value - chroma;

    float redPrime = 0.0f;
    float greenPrime = 0.0f;
    float bluePrime = 0.0f;

    if (hueSection < 1.0f)
    {
        redPrime = chroma;
        greenPrime = secondComponent;
    }
    else if (hueSection < 2.0f)
    {
        redPrime = secondComponent;
        greenPrime = chroma;
    }
    else if (hueSection < 3.0f)
    {
        greenPrime = chroma;
        bluePrime = secondComponent;
    }
    else if (hueSection < 4.0f)
    {
        greenPrime = secondComponent;
        bluePrime = chroma;
    }
    else if (hueSection < 5.0f)
    {
        redPrime = secondComponent;
        bluePrime = chroma;
    }
    else
    {
        redPrime = chroma;
        bluePrime = secondComponent;
    }

    return GB_ColorRGBA::FromFloat(redPrime + matchValue, greenPrime + matchValue, bluePrime + matchValue, alpha);
}

GB_ColorHSL GB_ColorRGBA::ToHsl() const noexcept
{
    const float red = static_cast<float>(r) / 255.0f;
    const float green = static_cast<float>(g) / 255.0f;
    const float blue = static_cast<float>(b) / 255.0f;

    const float maxValue = std::max(red, std::max(green, blue));
    const float minValue = std::min(red, std::min(green, blue));
    const float delta = maxValue - minValue;

    GB_ColorHSL hslColor;
    hslColor.l = 0.5f * (maxValue + minValue);
    hslColor.a = static_cast<float>(a) / 255.0f;

    if (delta <= 0.0f)
    {
        hslColor.h = 0.0f;
        hslColor.s = 0.0f;
        return hslColor;
    }

    hslColor.s = delta / (1.0f - std::fabs(2.0f * hslColor.l - 1.0f));

    if (maxValue == red)
    {
        hslColor.h = 60.0f * std::fmod(((green - blue) / delta), 6.0f);
    }
    else if (maxValue == green)
    {
        hslColor.h = 60.0f * (((blue - red) / delta) + 2.0f);
    }
    else
    {
        hslColor.h = 60.0f * (((red - green) / delta) + 4.0f);
    }

    hslColor.h = WrapHue360(hslColor.h);
    return hslColor;
}

GB_ColorRGBA GB_ColorRGBA::FromHsl(const GB_ColorHSL& hslColor) noexcept
{
    const float hue = WrapHue360(hslColor.h) / 360.0f;
    const float saturation = ClampUnitFloat(hslColor.s);
    const float lightness = ClampUnitFloat(hslColor.l);
    const float alpha = ClampUnitFloat(hslColor.a);

    if (saturation <= 0.0f)
    {
        return GB_ColorRGBA::FromFloat(lightness, lightness, lightness, alpha);
    }

    const float qValue = lightness < 0.5f ? lightness * (1.0f + saturation) : lightness + saturation - lightness * saturation;

    const float pValue = 2.0f * lightness - qValue;

    return GB_ColorRGBA::FromFloat(HueToRgb(pValue, qValue, hue + 1.0f / 3.0f), HueToRgb(pValue, qValue, hue), HueToRgb(pValue, qValue, hue - 1.0f / 3.0f), alpha);
}

GB_ColorLinearRGBA GB_ColorRGBA::ToLinearRgba() const noexcept
{
    return GB_ColorLinearRGBA(SrgbToLinear(static_cast<float>(r) / 255.0f), SrgbToLinear(static_cast<float>(g) / 255.0f), SrgbToLinear(static_cast<float>(b) / 255.0f), static_cast<float>(a) / 255.0f);
}

GB_ColorRGBA GB_ColorRGBA::FromLinearRgba(const GB_ColorLinearRGBA& linearColor) noexcept
{
    return GB_ColorRGBA::FromFloat(LinearToSrgb(linearColor.r), LinearToSrgb(linearColor.g), LinearToSrgb(linearColor.b), ClampUnitFloat(linearColor.a));
}
