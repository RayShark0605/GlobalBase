#include "GB_Image.h"
#include "../GB_FileSystem.h"
#include "../GB_IO.h"

#include <limits>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4819)
#endif

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

namespace gbImageInternal
{
    /**
     * @brief 将 size_t 安全转换为 int。
     *
     * 底层矩阵接口大量使用 int 作为尺寸类型；这里统一做边界检查，
     * 避免超范围尺寸在后续构造时发生截断。
     */
    static bool TryConvertSizeToInt(size_t value, int& intValue)
    {
        if (value > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        intValue = static_cast<int>(value);
        return true;
    }

    /**
     * @brief 将整数限制到给定闭区间。
     */
    static int ClampInt(int value, int minValue, int maxValue)
    {
        if (value < minValue)
        {
            return minValue;
        }
        if (value > maxValue)
        {
            return maxValue;
        }
        return value;
    }

    /**
     * @brief 将整数限制到 8 位无符号字节范围。
     */
    static uint8_t ClampToByte(int value)
    {
        if (value <= 0)
        {
            return 0;
        }
        if (value >= 255)
        {
            return 255;
        }
        return static_cast<uint8_t>(value);
    }

    /**
     * @brief 将 RGBA 颜色按常用亮度权重转换为 8 位灰度值。
     *
     * Alpha 不参与灰度计算。
     */
    static uint8_t RgbaToGray8(const GB_ColorRGBA& pixelColor)
    {
        const int grayValue = (299 * static_cast<int>(pixelColor.r) + 587 * static_cast<int>(pixelColor.g) + 114 * static_cast<int>(pixelColor.b) + 500) / 1000;
        return ClampToByte(grayValue);
    }

    /**
     * @brief 将 GB_ImageDepth 映射为底层深度枚举。
     */
    static bool TryGetCvDepth(GB_ImageDepth imageDepth, int& cvDepth)
    {
        switch (imageDepth)
        {
        case GB_ImageDepth::UInt8:
            cvDepth = CV_8U;
            return true;
        case GB_ImageDepth::Int8:
            cvDepth = CV_8S;
            return true;
        case GB_ImageDepth::UInt16:
            cvDepth = CV_16U;
            return true;
        case GB_ImageDepth::Int16:
            cvDepth = CV_16S;
            return true;
        case GB_ImageDepth::Int32:
            cvDepth = CV_32S;
            return true;
        case GB_ImageDepth::Float32:
            cvDepth = CV_32F;
            return true;
        case GB_ImageDepth::Float64:
            cvDepth = CV_64F;
            return true;
        default:
            break;
        }

        cvDepth = -1;
        return false;
    }

    /**
     * @brief 将底层深度枚举映射回 GB_ImageDepth。
     */
    static GB_ImageDepth CvDepthToImageDepth(int cvDepth)
    {
        switch (cvDepth)
        {
        case CV_8U:
            return GB_ImageDepth::UInt8;
        case CV_8S:
            return GB_ImageDepth::Int8;
        case CV_16U:
            return GB_ImageDepth::UInt16;
        case CV_16S:
            return GB_ImageDepth::Int16;
        case CV_32S:
            return GB_ImageDepth::Int32;
        case CV_32F:
            return GB_ImageDepth::Float32;
        case CV_64F:
            return GB_ImageDepth::Float64;
        default:
            break;
        }

        return GB_ImageDepth::Unknown;
    }

    /**
     * @brief 获取单通道字节数。
     */
    static size_t GetBytesPerChannel(GB_ImageDepth imageDepth)
    {
        switch (imageDepth)
        {
        case GB_ImageDepth::UInt8:
        case GB_ImageDepth::Int8:
            return 1;
        case GB_ImageDepth::UInt16:
        case GB_ImageDepth::Int16:
            return 2;
        case GB_ImageDepth::Int32:
        case GB_ImageDepth::Float32:
            return 4;
        case GB_ImageDepth::Float64:
            return 8;
        default:
            break;
        }

        return 0;
    }

    /**
     * @brief 组合得到底层图像类型。
     *
     * 这里不把通道数限制为 1/2/3/4，而是放宽到当前底层支持的上限；
     * 这样 Create() 能够创建更通用的多通道图像。
     * 像素读写、Fill() 等接口仍只对 8 位 1/3/4 通道提供稳定支持。
     */
    static bool TryGetCvType(GB_ImageDepth imageDepth, int channels, int& cvType)
    {
        if (channels <= 0 || channels > CV_CN_MAX)
        {
            return false;
        }

        int cvDepth = -1;
        if (!TryGetCvDepth(imageDepth, cvDepth))
        {
            return false;
        }

        cvType = CV_MAKETYPE(cvDepth, channels);
        return true;
    }

    /**
     * @brief 根据读取选项拼装解码标志。
     */
    static int GetImreadFlags(const GB_ImageLoadOptions& loadOptions)
    {
        int imreadFlags = 0;

        switch (loadOptions.colorMode)
        {
        case GB_ImageColorMode::Gray:
            imreadFlags = cv::IMREAD_GRAYSCALE;
            if (loadOptions.preserveBitDepth)
            {
                imreadFlags |= cv::IMREAD_ANYDEPTH;
            }
            break;

        case GB_ImageColorMode::BGR:
            imreadFlags = cv::IMREAD_COLOR;
            if (loadOptions.preserveBitDepth)
            {
                imreadFlags |= cv::IMREAD_ANYDEPTH;
            }
            break;

        case GB_ImageColorMode::BGRA:
        case GB_ImageColorMode::Unchanged:
        default:
            imreadFlags = cv::IMREAD_UNCHANGED;
            break;
        }

        if (loadOptions.ignoreExifOrientation)
        {
            imreadFlags |= cv::IMREAD_IGNORE_ORIENTATION;
        }

        return imreadFlags;
    }

    /**
     * @brief 将双通道灰度+Alpha 图像扩展为 4 通道 BGRA。
     *
     * 约定 sourceImage 的两个通道分别为 Gray、Alpha。
     */
    static bool ConvertGrayAlphaToBgra(const cv::Mat& sourceImage, cv::Mat& targetImage)
    {
        if (sourceImage.empty() || sourceImage.channels() != 2)
        {
            return false;
        }

        try
        {
            std::vector<cv::Mat> sourceChannels;
            cv::split(sourceImage, sourceChannels);
            if (sourceChannels.size() != 2 || sourceChannels[0].empty() || sourceChannels[1].empty())
            {
                return false;
            }

            std::vector<cv::Mat> targetChannels;
            targetChannels.reserve(4);
            targetChannels.push_back(sourceChannels[0]);
            targetChannels.push_back(sourceChannels[0]);
            targetChannels.push_back(sourceChannels[0]);
            targetChannels.push_back(sourceChannels[1]);

            cv::merge(targetChannels, targetImage);
            return !targetImage.empty();
        }
        catch (...)
        {
            targetImage.release();
            return false;
        }
    }

    /**
     * @brief 对刚解码出的图像做读入后的规范化处理。
     *
     * 当前只在请求 BGRA 时做额外转换；其余模式直接保留解码结果。
     */
    static bool ApplyLoadPostProcess(cv::Mat& imageMat, const GB_ImageLoadOptions& loadOptions)
    {
        if (imageMat.empty())
        {
            return false;
        }

        if (loadOptions.colorMode != GB_ImageColorMode::BGRA)
        {
            return true;
        }

        try
        {
            switch (imageMat.channels())
            {
            case 4:
                return true;

            case 3:
            {
                cv::Mat convertedImage;
                cv::cvtColor(imageMat, convertedImage, cv::COLOR_BGR2BGRA);
                imageMat = std::move(convertedImage);
                return true;
            }

            case 2:
            {
                cv::Mat convertedImage;
                if (!ConvertGrayAlphaToBgra(imageMat, convertedImage))
                {
                    return false;
                }

                imageMat = std::move(convertedImage);
                return true;
            }

            case 1:
            {
                cv::Mat convertedImage;
                cv::cvtColor(imageMat, convertedImage, cv::COLOR_GRAY2BGRA);
                imageMat = std::move(convertedImage);
                return true;
            }

            default:
                break;
            }
        }
        catch (...)
        {
            return false;
        }

        return false;
    }

    /**
     * @brief 规范化文件扩展名。
     *
     * 结果统一带前导点，并转换为小写。
     */
    static std::string NormalizeFileExtension(const std::string& fileExt)
    {
        if (fileExt.empty())
        {
            return std::string();
        }

        std::string normalizedExt = fileExt;
        if (normalizedExt[0] != '.')
        {
            normalizedExt.insert(normalizedExt.begin(), '.');
        }

        for (size_t i = 0; i < normalizedExt.size(); i++)
        {
            const unsigned char character = static_cast<unsigned char>(normalizedExt[i]);
            if (character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('Z'))
            {
                normalizedExt[i] = static_cast<char>(character - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
            }
        }

        return normalizedExt;
    }

    static bool IsJpegExtension(const std::string& fileExt)
    {
        return fileExt == ".jpg" || fileExt == ".jpeg" || fileExt == ".jpe";
    }

    static bool IsPngExtension(const std::string& fileExt)
    {
        return fileExt == ".png";
    }

    static bool IsWebpExtension(const std::string& fileExt)
    {
        return fileExt == ".webp";
    }

    /**
     * @brief 按扩展名生成编码参数。
     */
    static void BuildImwriteParams(const std::string& fileExt, const GB_ImageSaveOptions& saveOptions, std::vector<int>& imwriteParams)
    {
        imwriteParams.clear();

        if (IsJpegExtension(fileExt))
        {
            imwriteParams.push_back(cv::IMWRITE_JPEG_QUALITY);
            imwriteParams.push_back(ClampInt(saveOptions.jpegQuality, 0, 100));
            return;
        }

        if (IsPngExtension(fileExt))
        {
            imwriteParams.push_back(cv::IMWRITE_PNG_COMPRESSION);
            imwriteParams.push_back(ClampInt(saveOptions.pngCompression, 0, 9));
            return;
        }

        if (IsWebpExtension(fileExt))
        {
            imwriteParams.push_back(cv::IMWRITE_WEBP_QUALITY);
            imwriteParams.push_back(ClampInt(saveOptions.webpQuality, 1, 100));
            return;
        }
    }

    /**
     * @brief 将缩放插值枚举映射为底层插值类型。
     */
    static int ToCvInterpolation(GB_ImageInterpolation interpolation)
    {
        switch (interpolation)
        {
        case GB_ImageInterpolation::Nearest:
            return cv::INTER_NEAREST;
        case GB_ImageInterpolation::Linear:
            return cv::INTER_LINEAR;
        case GB_ImageInterpolation::Cubic:
            return cv::INTER_CUBIC;
        case GB_ImageInterpolation::Area:
            return cv::INTER_AREA;
        case GB_ImageInterpolation::Lanczos4:
            return cv::INTER_LANCZOS4;
        default:
            break;
        }

        return cv::INTER_LINEAR;
    }

    /**
     * @brief 记录当前图像内部真实的通道排列方式。
     *
     * 这与“通道个数”不同：3 通道既可能是 BGR，也可能是 RGB；
     * 4 通道既可能是 BGRA，也可能是 RGBA。
     */
    enum class ImageChannelLayout
    {
        Empty = 0,
        Gray,
        Bgr,
        Bgra,
        Rgb,
        Rgba,
        Other
    };

    /**
     * @brief 将颜色转换枚举映射为底层颜色转换码。
     */
    static bool TryGetCvColorCode(GB_ImageColorConversion conversion, int& cvColorCode)
    {
        switch (conversion)
        {
        case GB_ImageColorConversion::GrayToBgr:
            cvColorCode = cv::COLOR_GRAY2BGR;
            return true;
        case GB_ImageColorConversion::GrayToBgra:
            cvColorCode = cv::COLOR_GRAY2BGRA;
            return true;
        case GB_ImageColorConversion::BgrToGray:
            cvColorCode = cv::COLOR_BGR2GRAY;
            return true;
        case GB_ImageColorConversion::BgrToBgra:
            cvColorCode = cv::COLOR_BGR2BGRA;
            return true;
        case GB_ImageColorConversion::BgraToGray:
            cvColorCode = cv::COLOR_BGRA2GRAY;
            return true;
        case GB_ImageColorConversion::BgraToBgr:
            cvColorCode = cv::COLOR_BGRA2BGR;
            return true;
        case GB_ImageColorConversion::BgrToRgb:
            cvColorCode = cv::COLOR_BGR2RGB;
            return true;
        case GB_ImageColorConversion::RgbToBgr:
            cvColorCode = cv::COLOR_RGB2BGR;
            return true;
        case GB_ImageColorConversion::BgraToRgba:
            cvColorCode = cv::COLOR_BGRA2RGBA;
            return true;
        case GB_ImageColorConversion::RgbaToBgra:
            cvColorCode = cv::COLOR_RGBA2BGRA;
            return true;
        case GB_ImageColorConversion::GrayToRgb:
            cvColorCode = cv::COLOR_GRAY2RGB;
            return true;
        case GB_ImageColorConversion::GrayToRgba:
            cvColorCode = cv::COLOR_GRAY2RGBA;
            return true;
        case GB_ImageColorConversion::RgbToGray:
            cvColorCode = cv::COLOR_RGB2GRAY;
            return true;
        case GB_ImageColorConversion::RgbaToGray:
            cvColorCode = cv::COLOR_RGBA2GRAY;
            return true;
        case GB_ImageColorConversion::RgbToBgra:
            cvColorCode = cv::COLOR_RGB2BGRA;
            return true;
        case GB_ImageColorConversion::RgbaToBgr:
            cvColorCode = cv::COLOR_RGBA2BGR;
            return true;
        default:
            break;
        }

        cvColorCode = -1;
        return false;
    }

    /**
     * @brief 根据图像来源推断默认通道排列。
     *
     * 对常见的 1/3/4 通道图像分别推断为 Gray、Bgr、Bgra；
     * 其它情况统一记为 Other。
     */
    static ImageChannelLayout InferChannelLayout(const cv::Mat& imageMat)
    {
        if (imageMat.empty())
        {
            return ImageChannelLayout::Empty;
        }

        switch (imageMat.channels())
        {
        case 1:
            return ImageChannelLayout::Gray;
        case 3:
            return ImageChannelLayout::Bgr;
        case 4:
            return ImageChannelLayout::Bgra;
        default:
            break;
        }

        return ImageChannelLayout::Other;
    }

    /**
     * @brief 预估颜色转换后的通道排列。
     */
    static ImageChannelLayout GetConvertedChannelLayout(GB_ImageColorConversion conversion)
    {
        switch (conversion)
        {
        case GB_ImageColorConversion::GrayToBgr:
        case GB_ImageColorConversion::BgraToBgr:
        case GB_ImageColorConversion::RgbToBgr:
        case GB_ImageColorConversion::RgbaToBgr:
            return ImageChannelLayout::Bgr;

        case GB_ImageColorConversion::GrayToBgra:
        case GB_ImageColorConversion::BgrToBgra:
        case GB_ImageColorConversion::RgbaToBgra:
        case GB_ImageColorConversion::RgbToBgra:
            return ImageChannelLayout::Bgra;

        case GB_ImageColorConversion::BgrToGray:
        case GB_ImageColorConversion::BgraToGray:
        case GB_ImageColorConversion::RgbToGray:
        case GB_ImageColorConversion::RgbaToGray:
            return ImageChannelLayout::Gray;

        case GB_ImageColorConversion::BgrToRgb:
        case GB_ImageColorConversion::GrayToRgb:
            return ImageChannelLayout::Rgb;

        case GB_ImageColorConversion::BgraToRgba:
        case GB_ImageColorConversion::GrayToRgba:
            return ImageChannelLayout::Rgba;

        default:
            break;
        }

        return ImageChannelLayout::Other;
    }

    /**
     * @brief 判断当前通道排列是否允许执行指定转换。
     *
     * 例如 BgrToGray 只能用于当前真实排列为 BGR 的 3 通道图像，
     * 不能拿 RGB 图像直接套用同一个转换码。
     */
    static bool IsConversionSourceLayoutCompatible(GB_ImageColorConversion conversion, ImageChannelLayout channelLayout)
    {
        switch (conversion)
        {
        case GB_ImageColorConversion::GrayToBgr:
        case GB_ImageColorConversion::GrayToBgra:
        case GB_ImageColorConversion::GrayToRgb:
        case GB_ImageColorConversion::GrayToRgba:
            return channelLayout == ImageChannelLayout::Gray;

        case GB_ImageColorConversion::BgrToGray:
        case GB_ImageColorConversion::BgrToBgra:
        case GB_ImageColorConversion::BgrToRgb:
            return channelLayout == ImageChannelLayout::Bgr;

        case GB_ImageColorConversion::BgraToGray:
        case GB_ImageColorConversion::BgraToBgr:
        case GB_ImageColorConversion::BgraToRgba:
            return channelLayout == ImageChannelLayout::Bgra;

        case GB_ImageColorConversion::RgbToBgr:
        case GB_ImageColorConversion::RgbToGray:
        case GB_ImageColorConversion::RgbToBgra:
            return channelLayout == ImageChannelLayout::Rgb;

        case GB_ImageColorConversion::RgbaToBgra:
        case GB_ImageColorConversion::RgbaToGray:
        case GB_ImageColorConversion::RgbaToBgr:
            return channelLayout == ImageChannelLayout::Rgba;

        default:
            break;
        }

        return false;
    }

    /**
     * @brief 获取 8 位图像指定像素的首字节指针。
     */
    static unsigned char* GetPixelPtr(cv::Mat& imageMat, size_t row, size_t col)
    {
        if (imageMat.empty() || imageMat.depth() != CV_8U)
        {
            return nullptr;
        }

        if (row >= static_cast<size_t>(imageMat.rows) || col >= static_cast<size_t>(imageMat.cols))
        {
            return nullptr;
        }

        const size_t pixelSize = imageMat.elemSize();
        return imageMat.ptr<unsigned char>(static_cast<int>(row)) + col * pixelSize;
    }

    /**
     * @brief 获取 8 位图像指定像素的只读首字节指针。
     */
    static const unsigned char* GetPixelPtr(const cv::Mat& imageMat, size_t row, size_t col)
    {
        if (imageMat.empty() || imageMat.depth() != CV_8U)
        {
            return nullptr;
        }

        if (row >= static_cast<size_t>(imageMat.rows) || col >= static_cast<size_t>(imageMat.cols))
        {
            return nullptr;
        }

        const size_t pixelSize = imageMat.elemSize();
        return imageMat.ptr<unsigned char>(static_cast<int>(row)) + col * pixelSize;
    }

    /**
     * @brief 按当前通道排列读取单个像素，并转换为逻辑 RGBA。
     */
    static bool ReadPixelColor(const unsigned char* pixelPtr, ImageChannelLayout channelLayout, GB_ColorRGBA& pixelColor)
    {
        if (pixelPtr == nullptr)
        {
            return false;
        }

        switch (channelLayout)
        {
        case ImageChannelLayout::Gray:
            pixelColor.r = pixelPtr[0];
            pixelColor.g = pixelPtr[0];
            pixelColor.b = pixelPtr[0];
            pixelColor.a = 255;
            return true;

        case ImageChannelLayout::Bgr:
            pixelColor.r = pixelPtr[2];
            pixelColor.g = pixelPtr[1];
            pixelColor.b = pixelPtr[0];
            pixelColor.a = 255;
            return true;

        case ImageChannelLayout::Bgra:
            pixelColor.r = pixelPtr[2];
            pixelColor.g = pixelPtr[1];
            pixelColor.b = pixelPtr[0];
            pixelColor.a = pixelPtr[3];
            return true;

        case ImageChannelLayout::Rgb:
            pixelColor.r = pixelPtr[0];
            pixelColor.g = pixelPtr[1];
            pixelColor.b = pixelPtr[2];
            pixelColor.a = 255;
            return true;

        case ImageChannelLayout::Rgba:
            pixelColor.r = pixelPtr[0];
            pixelColor.g = pixelPtr[1];
            pixelColor.b = pixelPtr[2];
            pixelColor.a = pixelPtr[3];
            return true;

        default:
            break;
        }

        return false;
    }

    /**
     * @brief 按当前通道排列写入单个逻辑 RGBA 像素。
     */
    static bool WritePixelColor(unsigned char* pixelPtr, ImageChannelLayout channelLayout, const GB_ColorRGBA& pixelColor)
    {
        if (pixelPtr == nullptr)
        {
            return false;
        }

        switch (channelLayout)
        {
        case ImageChannelLayout::Gray:
            pixelPtr[0] = RgbaToGray8(pixelColor);
            return true;

        case ImageChannelLayout::Bgr:
            pixelPtr[0] = pixelColor.b;
            pixelPtr[1] = pixelColor.g;
            pixelPtr[2] = pixelColor.r;
            return true;

        case ImageChannelLayout::Bgra:
            pixelPtr[0] = pixelColor.b;
            pixelPtr[1] = pixelColor.g;
            pixelPtr[2] = pixelColor.r;
            pixelPtr[3] = pixelColor.a;
            return true;

        case ImageChannelLayout::Rgb:
            pixelPtr[0] = pixelColor.r;
            pixelPtr[1] = pixelColor.g;
            pixelPtr[2] = pixelColor.b;
            return true;

        case ImageChannelLayout::Rgba:
            pixelPtr[0] = pixelColor.r;
            pixelPtr[1] = pixelColor.g;
            pixelPtr[2] = pixelColor.b;
            pixelPtr[3] = pixelColor.a;
            return true;

        default:
            break;
        }

        return false;
    }

    /**
     * @brief 为整幅填充构造与当前通道排列匹配的标量值。
     */
    static bool GetFillScalar(const GB_ColorRGBA& pixelColor, ImageChannelLayout channelLayout, cv::Scalar& fillScalar)
    {
        switch (channelLayout)
        {
        case ImageChannelLayout::Gray:
            fillScalar = cv::Scalar(RgbaToGray8(pixelColor));
            return true;

        case ImageChannelLayout::Bgr:
            fillScalar = cv::Scalar(pixelColor.b, pixelColor.g, pixelColor.r);
            return true;

        case ImageChannelLayout::Bgra:
            fillScalar = cv::Scalar(pixelColor.b, pixelColor.g, pixelColor.r, pixelColor.a);
            return true;

        case ImageChannelLayout::Rgb:
            fillScalar = cv::Scalar(pixelColor.r, pixelColor.g, pixelColor.b);
            return true;

        case ImageChannelLayout::Rgba:
            fillScalar = cv::Scalar(pixelColor.r, pixelColor.g, pixelColor.b, pixelColor.a);
            return true;

        default:
            break;
        }

        return false;
    }

    /**
     * @brief 在编码前将图像整理成写出端更容易接受的通道顺序。
     *
     * 当前只在内部真实排列为 RGB / RGBA 时做显式转换；
     * 其它情况直接沿用原图像。
     */
    static bool PrepareImageForEncoding(const cv::Mat& sourceImage, ImageChannelLayout channelLayout, cv::Mat& encodedImage)
    {
        if (sourceImage.empty())
        {
            return false;
        }

        try
        {
            switch (channelLayout)
            {
            case ImageChannelLayout::Rgb:
                cv::cvtColor(sourceImage, encodedImage, cv::COLOR_RGB2BGR);
                return !encodedImage.empty();

            case ImageChannelLayout::Rgba:
                cv::cvtColor(sourceImage, encodedImage, cv::COLOR_RGBA2BGRA);
                return !encodedImage.empty();

            default:
                encodedImage = sourceImage;
                return true;
            }
        }
        catch (...)
        {
            encodedImage.release();
            return false;
        }
    }
}

/**
 * @brief GB_Image 的内部实现。
 */
class GB_Image::Impl
{
public:
    cv::Mat imageMat;
    gbImageInternal::ImageChannelLayout channelLayout = gbImageInternal::ImageChannelLayout::Empty;
};

/**
 * @brief 构造空图像。
 */
GB_Image::GB_Image() : imageImpl(new Impl())
{
}

/**
 * @brief 构造并从文件读取图像。
 */
GB_Image::GB_Image(const std::string& filePathUtf8, const GB_ImageLoadOptions& loadOptions) : imageImpl(new Impl())
{
    (void)LoadFromFile(filePathUtf8, loadOptions);
}

/**
 * @brief 构造并从编码字节流读取图像。
 */
GB_Image::GB_Image(const GB_ByteBuffer& encodedBytes, const GB_ImageLoadOptions& loadOptions) : imageImpl(new Impl())
{
    (void)LoadFromMemory(encodedBytes, loadOptions);
}

/**
 * @brief 构造并从编码内存块读取图像。
 */
GB_Image::GB_Image(const void* encodedData, size_t encodedSize, const GB_ImageLoadOptions& loadOptions) : imageImpl(new Impl())
{
    (void)LoadFromMemory(encodedData, encodedSize, loadOptions);
}

/**
 * @brief 构造指定规格的图像。
 */
GB_Image::GB_Image(size_t rows, size_t cols, GB_ImageDepth depth, int channels, bool zeroInitialize) : imageImpl(new Impl())
{
    (void)Create(rows, cols, depth, channels, zeroInitialize);
}

/**
 * @brief 默认拷贝构造：浅拷贝共享底层像素缓冲区。
 */
GB_Image::GB_Image(const GB_Image& other) : imageImpl(new Impl())
{
    if (other.imageImpl != nullptr)
    {
        imageImpl->imageMat = other.imageImpl->imageMat;
        imageImpl->channelLayout = other.imageImpl->channelLayout;
    }
}

/**
 * @brief 按指定拷贝方式构造图像。
 */
GB_Image::GB_Image(const GB_Image& other, GB_ImageCopyMode copyMode) : imageImpl(new Impl())
{
    if (other.imageImpl == nullptr)
    {
        return;
    }

    if (copyMode == GB_ImageCopyMode::DeepCopy)
    {
        imageImpl->imageMat = other.imageImpl->imageMat.clone();
    }
    else
    {
        imageImpl->imageMat = other.imageImpl->imageMat;
    }

    imageImpl->channelLayout = other.imageImpl->channelLayout;
}

/**
 * @brief 移动构造。
 */
GB_Image::GB_Image(GB_Image&& other) noexcept : imageImpl(other.imageImpl)
{
    other.imageImpl = nullptr;
}

/**
 * @brief 从外部矩阵对象构造图像。
 */
GB_Image::GB_Image(const cv::Mat& imageMat, GB_ImageCopyMode copyMode) : imageImpl(new Impl())
{
    (void)SetFromCvMat(imageMat, copyMode);
}

/**
 * @brief 析构。
 */
GB_Image::~GB_Image()
{
    delete imageImpl;
    imageImpl = nullptr;
}

/**
 * @brief 拷贝赋值：浅拷贝共享底层像素缓冲区。
 */
GB_Image& GB_Image::operator=(const GB_Image& other)
{
    if (this == &other)
    {
        return *this;
    }

    if (other.imageImpl == nullptr)
    {
        Clear();
        return *this;
    }

    if (imageImpl == nullptr)
    {
        imageImpl = new Impl();
    }

    imageImpl->imageMat = other.imageImpl->imageMat;
    imageImpl->channelLayout = other.imageImpl->channelLayout;
    return *this;
}

/**
 * @brief 移动赋值。
 */
GB_Image& GB_Image::operator=(GB_Image&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    delete imageImpl;
    imageImpl = other.imageImpl;
    other.imageImpl = nullptr;
    return *this;
}

/**
 * @brief 交换两个对象的内部状态。
 */
void GB_Image::Swap(GB_Image& other) noexcept
{
    if (this == &other)
    {
        return;
    }

    std::swap(imageImpl, other.imageImpl);
}

/**
 * @brief 释放当前图像内容，但保留对象可复用。
 */
void GB_Image::Clear()
{
    if (imageImpl != nullptr)
    {
        imageImpl->imageMat.release();
        imageImpl->channelLayout = gbImageInternal::ImageChannelLayout::Empty;
    }
}

/**
 * @brief 确保内部实现对象存在。
 */
bool GB_Image::EnsureImageImpl()
{
    if (imageImpl != nullptr)
    {
        return true;
    }

    try
    {
        imageImpl = new Impl();
        return true;
    }
    catch (...)
    {
        imageImpl = nullptr;
        return false;
    }
}

/**
 * @brief 创建指定规格的新图像。
 *
 * 只有创建完全成功后，才会替换当前对象中的旧图像。
 */
bool GB_Image::Create(size_t rows, size_t cols, GB_ImageDepth depth, int channels, bool zeroInitialize)
{
    if (rows == 0 || cols == 0)
    {
        return false;
    }

    int cvRows = 0;
    int cvCols = 0;
    int cvType = 0;
    if (!gbImageInternal::TryConvertSizeToInt(rows, cvRows) || !gbImageInternal::TryConvertSizeToInt(cols, cvCols) || !gbImageInternal::TryGetCvType(depth, channels, cvType))
    {
        return false;
    }

    try
    {
        cv::Mat newImageMat;
        if (zeroInitialize)
        {
            newImageMat = cv::Mat::zeros(cvRows, cvCols, cvType);
        }
        else
        {
            newImageMat.create(cvRows, cvCols, cvType);
        }

        if (newImageMat.empty())
        {
            return false;
        }

        if (!EnsureImageImpl())
        {
            return false;
        }

        imageImpl->imageMat = std::move(newImageMat);
        imageImpl->channelLayout = gbImageInternal::InferChannelLayout(imageImpl->imageMat);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

/**
 * @brief 从文件读取图像。
 *
 * 读取失败时，当前对象保持原状。
 */
bool GB_Image::LoadFromFile(const std::string& filePathUtf8, const GB_ImageLoadOptions& loadOptions)
{
    const GB_ByteBuffer encodedBytes = GB_ReadFileToBinary(filePathUtf8);
    if (encodedBytes.empty())
    {
        return false;
    }

    return LoadFromMemory(encodedBytes, loadOptions);
}

/**
 * @brief 从编码字节流读取图像。
 */
bool GB_Image::LoadFromMemory(const GB_ByteBuffer& encodedBytes, const GB_ImageLoadOptions& loadOptions)
{
    if (encodedBytes.empty())
    {
        return false;
    }

    return LoadFromMemory(encodedBytes.data(), encodedBytes.size(), loadOptions);
}

/**
 * @brief 从编码内存块读取图像。
 *
 * 读取失败时，当前对象保持原状。
 */
bool GB_Image::LoadFromMemory(const void* encodedData, size_t encodedSize, const GB_ImageLoadOptions& loadOptions)
{
    if (encodedData == nullptr || encodedSize == 0)
    {
        return false;
    }

    int cvEncodedSize = 0;
    if (!gbImageInternal::TryConvertSizeToInt(encodedSize, cvEncodedSize))
    {
        return false;
    }

    try
    {
        const cv::Mat encodedView(1, cvEncodedSize, CV_8UC1, const_cast<void*>(encodedData));
        const int imreadFlags = gbImageInternal::GetImreadFlags(loadOptions);

        cv::Mat decodedImage;
        if (loadOptions.pageIndex == 0)
        {
            decodedImage = cv::imdecode(encodedView, imreadFlags);
        }
        else
        {
            int beginIndex = 0;
            if (!gbImageInternal::TryConvertSizeToInt(loadOptions.pageIndex, beginIndex))
            {
                return false;
            }

            if (beginIndex == std::numeric_limits<int>::max())
            {
                return false;
            }

            const int endIndex = beginIndex + 1;
            std::vector<cv::Mat> decodedPages;
            if (!cv::imdecodemulti(encodedView, imreadFlags, decodedPages, cv::Range(beginIndex, endIndex)) || decodedPages.empty())
            {
                return false;
            }

            decodedImage = std::move(decodedPages[0]);
        }

        if (decodedImage.empty())
        {
            return false;
        }

        if (!gbImageInternal::ApplyLoadPostProcess(decodedImage, loadOptions))
        {
            return false;
        }

        if (!EnsureImageImpl())
        {
            return false;
        }

        imageImpl->imageMat = std::move(decodedImage);
        imageImpl->channelLayout = gbImageInternal::InferChannelLayout(imageImpl->imageMat);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

/**
 * @brief 将当前图像编码后写入文件。
 */
bool GB_Image::SaveToFile(const std::string& filePathUtf8, const GB_ImageSaveOptions& saveOptions) const
{
    if (IsEmpty() || filePathUtf8.empty())
    {
        return false;
    }

    if (!saveOptions.overwrite && GB_IsFileExists(filePathUtf8))
    {
        return false;
    }

    const std::string fileExt = gbImageInternal::NormalizeFileExtension(GB_GetFileExt(filePathUtf8));
    if (fileExt.empty())
    {
        return false;
    }

    GB_ByteBuffer encodedBytes;
    if (!EncodeToMemory(encodedBytes, fileExt, saveOptions))
    {
        return false;
    }

    return GB_WriteBinaryToFile(encodedBytes, filePathUtf8);
}

/**
 * @brief 将当前图像编码到内存字节流。
 */
bool GB_Image::EncodeToMemory(GB_ByteBuffer& encodedBytes, const std::string& fileExt, const GB_ImageSaveOptions& saveOptions) const
{
    encodedBytes.clear();

    if (IsEmpty())
    {
        return false;
    }

    const std::string normalizedExt = gbImageInternal::NormalizeFileExtension(fileExt);
    if (normalizedExt.empty())
    {
        return false;
    }

    try
    {
        if (!cv::haveImageWriter(normalizedExt))
        {
            return false;
        }

        std::vector<int> imwriteParams;
        gbImageInternal::BuildImwriteParams(normalizedExt, saveOptions, imwriteParams);

        cv::Mat imageForEncode;
        if (!gbImageInternal::PrepareImageForEncoding(imageImpl->imageMat, imageImpl->channelLayout, imageForEncode))
        {
            return false;
        }

        std::vector<unsigned char> cvEncodedBytes;
        if (!cv::imencode(normalizedExt, imageForEncode, cvEncodedBytes, imwriteParams))
        {
            return false;
        }

        encodedBytes.swap(cvEncodedBytes);
        return !encodedBytes.empty();
    }
    catch (...)
    {
        encodedBytes.clear();
        return false;
    }
}

/**
 * @brief 用外部矩阵对象替换当前图像内容。
 *
 * 设置失败时，当前对象保持原状。
 */
bool GB_Image::SetFromCvMat(const cv::Mat& imageMat, GB_ImageCopyMode copyMode)
{
    if (imageMat.empty())
    {
        return false;
    }

    try
    {
        cv::Mat newImageMat;
        if (copyMode == GB_ImageCopyMode::DeepCopy)
        {
            newImageMat = imageMat.clone();
        }
        else
        {
            newImageMat = imageMat;
        }

        if (newImageMat.empty())
        {
            return false;
        }

        if (!EnsureImageImpl())
        {
            return false;
        }

        imageImpl->imageMat = std::move(newImageMat);
        imageImpl->channelLayout = gbImageInternal::InferChannelLayout(imageImpl->imageMat);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

/**
 * @brief 导出为外部矩阵对象。
 */
cv::Mat GB_Image::ToCvMat(GB_ImageCopyMode copyMode) const
{
    if (IsEmpty())
    {
        return cv::Mat();
    }

    try
    {
        if (copyMode == GB_ImageCopyMode::DeepCopy)
        {
            return imageImpl->imageMat.clone();
        }

        return imageImpl->imageMat;
    }
    catch (...)
    {
        return cv::Mat();
    }
}

/**
 * @brief 当前是否为空图像。
 */
bool GB_Image::IsEmpty() const
{
    return imageImpl == nullptr || imageImpl->imageMat.empty();
}

size_t GB_Image::GetWidth() const
{
    if (IsEmpty())
    {
        return 0;
    }

    return static_cast<size_t>(imageImpl->imageMat.cols);
}

size_t GB_Image::GetHeight() const
{
    if (IsEmpty())
    {
        return 0;
    }

    return static_cast<size_t>(imageImpl->imageMat.rows);
}

size_t GB_Image::GetRows() const
{
    return GetHeight();
}

size_t GB_Image::GetCols() const
{
    return GetWidth();
}

int GB_Image::GetChannels() const
{
    if (IsEmpty())
    {
        return 0;
    }

    return imageImpl->imageMat.channels();
}

GB_ImageDepth GB_Image::GetDepth() const
{
    if (IsEmpty())
    {
        return GB_ImageDepth::Unknown;
    }

    return gbImageInternal::CvDepthToImageDepth(imageImpl->imageMat.depth());
}

size_t GB_Image::GetBitDepth() const
{
    return GetBytesPerChannel() * 8;
}

size_t GB_Image::GetBytesPerChannel() const
{
    return gbImageInternal::GetBytesPerChannel(GetDepth());
}

size_t GB_Image::GetBytesPerPixel() const
{
    if (IsEmpty())
    {
        return 0;
    }

    return imageImpl->imageMat.elemSize();
}

size_t GB_Image::GetRowStrideBytes() const
{
    if (IsEmpty())
    {
        return 0;
    }

    return imageImpl->imageMat.step[0];
}

size_t GB_Image::GetTotalByteSize() const
{
    if (IsEmpty())
    {
        return 0;
    }

    return imageImpl->imageMat.total() * imageImpl->imageMat.elemSize();
}

bool GB_Image::IsContinuous() const
{
    if (IsEmpty())
    {
        return false;
    }

    return imageImpl->imageMat.isContinuous();
}

bool GB_Image::IsValidPixelCoordinate(size_t row, size_t col) const
{
    if (IsEmpty())
    {
        return false;
    }

    return row < static_cast<size_t>(imageImpl->imageMat.rows) && col < static_cast<size_t>(imageImpl->imageMat.cols);
}

unsigned char* GB_Image::GetData()
{
    if (IsEmpty())
    {
        return nullptr;
    }

    return imageImpl->imageMat.data;
}

const unsigned char* GB_Image::GetData() const
{
    if (IsEmpty())
    {
        return nullptr;
    }

    return imageImpl->imageMat.data;
}

unsigned char* GB_Image::GetRowData(size_t row)
{
    if (IsEmpty() || row >= static_cast<size_t>(imageImpl->imageMat.rows))
    {
        return nullptr;
    }

    return imageImpl->imageMat.ptr<unsigned char>(static_cast<int>(row));
}

const unsigned char* GB_Image::GetRowData(size_t row) const
{
    if (IsEmpty() || row >= static_cast<size_t>(imageImpl->imageMat.rows))
    {
        return nullptr;
    }

    return imageImpl->imageMat.ptr<unsigned char>(static_cast<int>(row));
}

/**
 * @brief 读取单个逻辑 RGBA 像素。
 */
bool GB_Image::GetPixelColor(size_t row, size_t col, GB_ColorRGBA& pixelColor) const
{
    if (IsEmpty())
    {
        return false;
    }

    const unsigned char* pixelPtr = gbImageInternal::GetPixelPtr(imageImpl->imageMat, row, col);
    if (pixelPtr == nullptr)
    {
        return false;
    }

    return gbImageInternal::ReadPixelColor(pixelPtr, imageImpl->channelLayout, pixelColor);
}

/**
 * @brief 写入单个逻辑 RGBA 像素。
 */
bool GB_Image::SetPixelColor(size_t row, size_t col, const GB_ColorRGBA& pixelColor)
{
    if (IsEmpty())
    {
        return false;
    }

    unsigned char* pixelPtr = gbImageInternal::GetPixelPtr(imageImpl->imageMat, row, col);
    if (pixelPtr == nullptr)
    {
        return false;
    }

    return gbImageInternal::WritePixelColor(pixelPtr, imageImpl->channelLayout, pixelColor);
}

/**
 * @brief 用指定颜色填充整幅图像。
 */
bool GB_Image::Fill(const GB_ColorRGBA& pixelColor)
{
    if (IsEmpty() || imageImpl->imageMat.depth() != CV_8U)
    {
        return false;
    }

    try
    {
        cv::Scalar fillScalar;
        if (!gbImageInternal::GetFillScalar(pixelColor, imageImpl->channelLayout, fillScalar))
        {
            return false;
        }

        imageImpl->imageMat.setTo(fillScalar);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

/**
 * @brief 生成当前图像的深拷贝副本。
 */
GB_Image GB_Image::Clone() const
{
    GB_Image resultImage;
    if (IsEmpty())
    {
        return resultImage;
    }

    try
    {
        resultImage.imageImpl->imageMat = imageImpl->imageMat.clone();
        resultImage.imageImpl->channelLayout = imageImpl->channelLayout;
    }
    catch (...)
    {
        resultImage.Clear();
    }

    return resultImage;
}

/**
 * @brief 若当前与其它对象共享像素缓冲区，则克隆出独立副本。
 */
bool GB_Image::Detach()
{
    if (IsEmpty())
    {
        return true;
    }

    try
    {
        imageImpl->imageMat = imageImpl->imageMat.clone();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

/**
 * @brief 生成缩放后的新图像。
 */
GB_Image GB_Image::Resize(size_t newRows, size_t newCols, GB_ImageInterpolation interpolation) const
{
    GB_Image resultImage;
    if (IsEmpty() || newRows == 0 || newCols == 0)
    {
        return resultImage;
    }

    if (newRows == GetRows() && newCols == GetCols())
    {
        return Clone();
    }

    int cvRows = 0;
    int cvCols = 0;
    if (!gbImageInternal::TryConvertSizeToInt(newRows, cvRows) || !gbImageInternal::TryConvertSizeToInt(newCols, cvCols))
    {
        return resultImage;
    }

    try
    {
        cv::resize(imageImpl->imageMat, resultImage.imageImpl->imageMat, cv::Size(cvCols, cvRows), 0.0, 0.0, gbImageInternal::ToCvInterpolation(interpolation));
        resultImage.imageImpl->channelLayout = imageImpl->channelLayout;
    }
    catch (...)
    {
        resultImage.Clear();
    }

    return resultImage;
}

/**
 * @brief 原地缩放图像。
 */
bool GB_Image::ResizeInPlace(size_t newRows, size_t newCols, GB_ImageInterpolation interpolation)
{
    if (IsEmpty())
    {
        return false;
    }

    if (newRows == GetRows() && newCols == GetCols())
    {
        return true;
    }

    GB_Image resizedImage = Resize(newRows, newCols, interpolation);
    if (resizedImage.IsEmpty())
    {
        return false;
    }

    Swap(resizedImage);
    return true;
}

/**
 * @brief 裁剪子图。
 */
GB_Image GB_Image::Crop(size_t row, size_t col, size_t cropRows, size_t cropCols, GB_ImageCopyMode copyMode) const
{
    GB_Image resultImage;
    if (IsEmpty() || cropRows == 0 || cropCols == 0)
    {
        return resultImage;
    }

    const size_t imageRows = static_cast<size_t>(imageImpl->imageMat.rows);
    const size_t imageCols = static_cast<size_t>(imageImpl->imageMat.cols);
    if (row >= imageRows || col >= imageCols)
    {
        return resultImage;
    }

    if (cropRows > imageRows - row || cropCols > imageCols - col)
    {
        return resultImage;
    }

    int cvRow = 0;
    int cvCol = 0;
    int cvCropRows = 0;
    int cvCropCols = 0;
    if (!gbImageInternal::TryConvertSizeToInt(row, cvRow) || !gbImageInternal::TryConvertSizeToInt(col, cvCol) || !gbImageInternal::TryConvertSizeToInt(cropRows, cvCropRows) || !gbImageInternal::TryConvertSizeToInt(cropCols, cvCropCols))
    {
        return resultImage;
    }

    try
    {
        const cv::Rect roiRect(cvCol, cvRow, cvCropCols, cvCropRows);
        const cv::Mat roiView = imageImpl->imageMat(roiRect);
        if (copyMode == GB_ImageCopyMode::DeepCopy)
        {
            resultImage.imageImpl->imageMat = roiView.clone();
        }
        else
        {
            resultImage.imageImpl->imageMat = roiView;
        }

        resultImage.imageImpl->channelLayout = imageImpl->channelLayout;
    }
    catch (...)
    {
        resultImage.Clear();
    }

    return resultImage;
}

/**
 * @brief 原地裁剪子图。
 */
bool GB_Image::CropInPlace(size_t row, size_t col, size_t cropRows, size_t cropCols, GB_ImageCopyMode copyMode)
{
    if (IsEmpty())
    {
        return false;
    }

    GB_Image croppedImage = Crop(row, col, cropRows, cropCols, copyMode);
    if (croppedImage.IsEmpty())
    {
        return false;
    }

    Swap(croppedImage);
    return true;
}

/**
 * @brief 生成颜色转换后的新图像。
 */
GB_Image GB_Image::ConvertColor(GB_ImageColorConversion conversion) const
{
    GB_Image resultImage;
    if (IsEmpty())
    {
        return resultImage;
    }

    int cvColorCode = -1;
    if (!gbImageInternal::TryGetCvColorCode(conversion, cvColorCode))
    {
        return resultImage;
    }

    if (!gbImageInternal::IsConversionSourceLayoutCompatible(conversion, imageImpl->channelLayout))
    {
        return resultImage;
    }

    try
    {
        cv::cvtColor(imageImpl->imageMat, resultImage.imageImpl->imageMat, cvColorCode);
        resultImage.imageImpl->channelLayout = gbImageInternal::GetConvertedChannelLayout(conversion);
    }
    catch (...)
    {
        resultImage.Clear();
    }

    return resultImage;
}

/**
 * @brief 原地执行颜色转换。
 */
bool GB_Image::ConvertColorInPlace(GB_ImageColorConversion conversion)
{
    if (IsEmpty())
    {
        return false;
    }

    GB_Image convertedImage = ConvertColor(conversion);
    if (convertedImage.IsEmpty())
    {
        return false;
    }

    Swap(convertedImage);
    return true;
}

/**
 * @brief 交换两个 GB_Image。
 */
void swap(GB_Image& leftImage, GB_Image& rightImage) noexcept
{
    leftImage.Swap(rightImage);
}
