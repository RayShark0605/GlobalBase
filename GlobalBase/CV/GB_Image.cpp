#include "GB_Image.h"
#include "../GB_FileSystem.h"
#include "../GB_IO.h"
#include "../Geometry/GB_GeometryInterface.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4819)
#endif

#include <opencv2/core.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

namespace GBImage_Internal
{
    /**
     * @brief 将 OpenCV 日志级别一次性收敛到 Error。
     *
     * OpenCV 在首次进入部分编解码或图像处理路径时，可能会输出并行后端插件探测
     * 的 INFO 日志。这里在 GB_Image 模块内统一将日志级别压到 ERROR，避免污染
     * 调用方的控制台输出。
     */
    static void EnsureOpenCvErrorLogOnly()
    {
        static std::once_flag configureLogLevelOnce;
        std::call_once(configureLogLevelOnce, []()
            {
                try
                {
                    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
                }
                catch (...)
                {
                }
            });
    }

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
     *
     * 这里使用通道混排接口一次性完成 2 -> 4 通道映射，
     * 避免 split/merge 带来的额外中间矩阵分配与数据搬运。
     */
    static bool ConvertGrayAlphaToBgra(const cv::Mat& sourceImage, cv::Mat& targetImage)
    {
        if (sourceImage.empty() || sourceImage.channels() != 2)
        {
            return false;
        }

        try
        {
            cv::Mat convertedImage(sourceImage.rows, sourceImage.cols, CV_MAKETYPE(sourceImage.depth(), 4));

            const cv::Mat sourceImages[] = { sourceImage };
            cv::Mat targetImages[] = { convertedImage };
            const int fromTo[] =
            {
                0, 0,
                0, 1,
                0, 2,
                1, 3
            };

            cv::mixChannels(sourceImages, 1, targetImages, 1, fromTo, 4);

            targetImage = std::move(convertedImage);
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
     * @brief 获取 8 位图像指定像素的首字节指针。
     */
    static unsigned char* GetPixelPtr(cv::Mat& imageMat, size_t row, size_t col)
    {
        return const_cast<unsigned char*>(GetPixelPtr(static_cast<const cv::Mat&>(imageMat), row, col));
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

    /**
     * @brief 安全计算两个 size_t 的乘积。
     */
    static bool TryMultiplySize(size_t leftValue, size_t rightValue, size_t& resultValue)
    {
        if (leftValue != 0 && rightValue > std::numeric_limits<size_t>::max() / leftValue)
        {
            return false;
        }

        resultValue = leftValue * rightValue;
        return true;
    }

    /**
     * @brief 将 uint64_t 安全转换为 size_t。
     */
    static bool TryConvertUInt64ToSize(uint64_t value, size_t& sizeValue)
    {
        if (value > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        {
            return false;
        }

        sizeValue = static_cast<size_t>(value);
        return true;
    }

    /**
     * @brief 判断当前图像是否支持与 GB_ColorRGBA 稳定互转。
     */
    static bool IsColorMatrixSupported(const cv::Mat& imageMat, ImageChannelLayout channelLayout)
    {
        if (imageMat.empty() || imageMat.depth() != CV_8U)
        {
            return false;
        }

        switch (channelLayout)
        {
        case ImageChannelLayout::Gray:
            return imageMat.channels() == 1;

        case ImageChannelLayout::Bgr:
        case ImageChannelLayout::Rgb:
            return imageMat.channels() == 3;

        case ImageChannelLayout::Bgra:
        case ImageChannelLayout::Rgba:
            return imageMat.channels() == 4;

        default:
            break;
        }

        return false;
    }

    /**
     * @brief 判断通道排列信息是否与通道数相容。
     */
    static bool IsChannelLayoutValidForChannels(int channels, ImageChannelLayout channelLayout, bool isEmptyImage)
    {
        if (isEmptyImage)
        {
            return channels == 0 && channelLayout == ImageChannelLayout::Empty;
        }

        switch (channelLayout)
        {
        case ImageChannelLayout::Gray:
            return channels == 1;

        case ImageChannelLayout::Bgr:
        case ImageChannelLayout::Rgb:
            return channels == 3;

        case ImageChannelLayout::Bgra:
        case ImageChannelLayout::Rgba:
            return channels == 4;

        case ImageChannelLayout::Other:
            return channels > 0;

        default:
            break;
        }

        return false;
    }

    /**
     * @brief 计算紧凑序列化时的像素字节数。
     */
    static bool GetCompactPixelByteSize(size_t rows, size_t cols, size_t bytesPerPixel, size_t& totalByteSize)
    {
        size_t pixelCount = 0;
        if (!TryMultiplySize(rows, cols, pixelCount))
        {
            return false;
        }

        return TryMultiplySize(pixelCount, bytesPerPixel, totalByteSize);
    }

    /**
     * @brief 追加原始字节到缓冲区尾部。
     */
    static void AppendRawBytes(GB_ByteBuffer& buffer, const unsigned char* rawData, size_t byteCount)
    {
        if (rawData == nullptr || byteCount == 0)
        {
            return;
        }

        buffer.insert(buffer.end(), rawData, rawData + byteCount);
    }

    /**
     * @brief 将图像按“逐行紧凑”方式写入字节缓冲区。
     *
     * 无论底层是否连续，写入的数据都只覆盖当前逻辑图像区域，且不包含行间 padding。
     */
    static bool CopyMatToCompactBytes(const cv::Mat& imageMat, GB_ByteBuffer& buffer)
    {
        if (imageMat.empty())
        {
            return true;
        }

        const size_t rowByteSize = static_cast<size_t>(imageMat.cols) * imageMat.elemSize();
        for (int rowIndex = 0; rowIndex < imageMat.rows; rowIndex++)
        {
            const unsigned char* rowData = imageMat.ptr<unsigned char>(rowIndex);
            if (rowData == nullptr && rowByteSize != 0)
            {
                return false;
            }

            AppendRawBytes(buffer, rowData, rowByteSize);
        }

        return true;
    }

    /**
     * @brief 将紧凑像素字节流逐行拷贝回图像。
     */
    static bool CopyCompactBytesToMat(const GB_ByteBuffer& buffer, size_t dataOffset, cv::Mat& imageMat)
    {
        if (imageMat.empty())
        {
            return dataOffset == buffer.size();
        }

        const size_t rowByteSize = static_cast<size_t>(imageMat.cols) * imageMat.elemSize();
        size_t offset = dataOffset;

        for (int rowIndex = 0; rowIndex < imageMat.rows; rowIndex++)
        {
            if (offset + rowByteSize > buffer.size())
            {
                return false;
            }

            unsigned char* rowData = imageMat.ptr<unsigned char>(rowIndex);
            if (rowData == nullptr && rowByteSize != 0)
            {
                return false;
            }

            if (rowByteSize != 0)
            {
                std::memcpy(rowData, buffer.data() + offset, rowByteSize);
            }
            offset += rowByteSize;
        }

        return offset == buffer.size();
    }

    /**
     * @brief 将角度规范化到 [0, 360) 区间。
     */
    static double NormalizeAngleDegrees(double angleDegrees)
    {
        double normalizedAngle = std::fmod(angleDegrees, 360.0);
        if (normalizedAngle < 0.0)
        {
            normalizedAngle += 360.0;
        }

        if (normalizedAngle >= 360.0)
        {
            normalizedAngle -= 360.0;
        }

        return normalizedAngle;
    }

    /**
     * @brief 判断两个双精度数是否近似相等。
     */
    static bool IsNearlyEqual(double leftValue, double rightValue, double epsilon = 1e-10)
    {
        return std::fabs(leftValue - rightValue) <= epsilon;
    }

    /**
     * @brief 判断颜色是否可视为全 0。
     */
    static bool IsZeroColor(const GB_ColorRGBA& pixelColor)
    {
        return pixelColor.r == 0 && pixelColor.g == 0 && pixelColor.b == 0 && pixelColor.a == 0;
    }

    /**
     * @brief 根据旋转选项解析背景色。
     */
    static GB_ColorRGBA ResolveRotateBackgroundColor(const GB_ImageRotateOptions& rotateOptions)
    {
        switch (rotateOptions.backgroundMode)
        {
        case GB_ImageRotateBackgroundMode::Black:
            return GB_ColorRGBA::Black;
        case GB_ImageRotateBackgroundMode::White:
            return GB_ColorRGBA::White;
        case GB_ImageRotateBackgroundMode::Transparent:
            return GB_ColorRGBA::Transparent;
        case GB_ImageRotateBackgroundMode::Custom:
            return rotateOptions.backgroundColor;
        default:
            break;
        }

        return GB_ColorRGBA::Black;
    }

    /**
     * @brief 判断当前旋转是否需要先为源图像补出 Alpha 通道。
     *
     * 当请求 Transparent 背景而源图像本身不带 Alpha 时，
     * 需要先提升为 4 通道，否则旋转后新增的空白区域只能得到数值为 0 的颜色，
     * 无法表达真实透明。
     */
    static bool ShouldPromoteRotateSourceToFourChannels(const cv::Mat& sourceImage,
        ImageChannelLayout channelLayout,
        const GB_ImageRotateOptions& rotateOptions)
    {
        if (sourceImage.empty() || rotateOptions.backgroundMode != GB_ImageRotateBackgroundMode::Transparent)
        {
            return false;
        }

        switch (channelLayout)
        {
        case ImageChannelLayout::Gray:
            return sourceImage.channels() == 1;

        case ImageChannelLayout::Bgr:
        case ImageChannelLayout::Rgb:
            return sourceImage.channels() == 3;

        default:
            break;
        }

        return false;
    }

    /**
     * @brief 为旋转准备实际参与运算的源图像。
     *
     * 当需要透明背景但源图像不带 Alpha 时，这里会先转换为 4 通道图像，
     * 以保证旋转结果中的空白区域能够保持透明。
     */
    static bool PrepareRotateSourceImage(const cv::Mat& sourceImage,
        ImageChannelLayout channelLayout,
        const GB_ImageRotateOptions& rotateOptions,
        cv::Mat& rotateSourceImage,
        ImageChannelLayout& rotateSourceLayout)
    {
        rotateSourceImage.release();
        rotateSourceLayout = channelLayout;

        if (sourceImage.empty())
        {
            return false;
        }

        if (!ShouldPromoteRotateSourceToFourChannels(sourceImage, channelLayout, rotateOptions))
        {
            rotateSourceImage = sourceImage;
            return true;
        }

        try
        {
            switch (channelLayout)
            {
            case ImageChannelLayout::Gray:
                cv::cvtColor(sourceImage, rotateSourceImage, cv::COLOR_GRAY2BGRA);
                rotateSourceLayout = ImageChannelLayout::Bgra;
                return !rotateSourceImage.empty();

            case ImageChannelLayout::Bgr:
                cv::cvtColor(sourceImage, rotateSourceImage, cv::COLOR_BGR2BGRA);
                rotateSourceLayout = ImageChannelLayout::Bgra;
                return !rotateSourceImage.empty();

            case ImageChannelLayout::Rgb:
                cv::cvtColor(sourceImage, rotateSourceImage, cv::COLOR_RGB2RGBA);
                rotateSourceLayout = ImageChannelLayout::Rgba;
                return !rotateSourceImage.empty();

            default:
                break;
            }
        }
        catch (...)
        {
            rotateSourceImage.release();
            rotateSourceLayout = channelLayout;
            return false;
        }

        rotateSourceImage.release();
        rotateSourceLayout = channelLayout;
        return false;
    }

    /**
     * @brief 将旋转背景色映射为可用于 setTo 的标量值。
     *
     * 对于 2 通道图像，这里按 Gray + Alpha 解释背景值。
     */
    static bool TryGetRotateFillScalar(int channels, ImageChannelLayout channelLayout, const GB_ColorRGBA& backgroundColor, cv::Scalar& fillScalar)
    {
        switch (channels)
        {
        case 1:
            fillScalar = cv::Scalar(RgbaToGray8(backgroundColor));
            return true;

        case 2:
            fillScalar = cv::Scalar(RgbaToGray8(backgroundColor), backgroundColor.a);
            return true;

        case 3:
            switch (channelLayout)
            {
            case ImageChannelLayout::Bgr:
                fillScalar = cv::Scalar(backgroundColor.b, backgroundColor.g, backgroundColor.r);
                return true;
            case ImageChannelLayout::Rgb:
                fillScalar = cv::Scalar(backgroundColor.r, backgroundColor.g, backgroundColor.b);
                return true;
            default:
                break;
            }
            break;

        case 4:
            switch (channelLayout)
            {
            case ImageChannelLayout::Bgra:
                fillScalar = cv::Scalar(backgroundColor.b, backgroundColor.g, backgroundColor.r, backgroundColor.a);
                return true;
            case ImageChannelLayout::Rgba:
                fillScalar = cv::Scalar(backgroundColor.r, backgroundColor.g, backgroundColor.b, backgroundColor.a);
                return true;
            default:
                break;
            }
            break;

        default:
            break;
        }

        fillScalar = cv::Scalar();
        return false;
    }

    /**
     * @brief 创建并预填充旋转结果图像。
     *
     * 传入的 sourceImage 可以是原始图像，也可以是为了支持透明背景而临时提升后的 4 通道图像。
     * 对超过 4 通道的图像，当前仅稳定支持数值全 0 的背景。
     */
    static bool CreatePrefilledRotateDestination(const cv::Mat& sourceImage,
        ImageChannelLayout channelLayout,
        const GB_ImageRotateOptions& rotateOptions,
        int dstRows,
        int dstCols,
        cv::Mat& destinationImage)
    {
        destinationImage.release();

        if (sourceImage.empty() || dstRows <= 0 || dstCols <= 0)
        {
            return false;
        }

        try
        {
            const GB_ColorRGBA backgroundColor = ResolveRotateBackgroundColor(rotateOptions);
            const int channels = sourceImage.channels();

            if (channels > 4)
            {
                if (backgroundColor != GB_ColorRGBA::Black && !IsZeroColor(backgroundColor))
                {
                    return false;
                }

                destinationImage = cv::Mat::zeros(dstRows, dstCols, sourceImage.type());
                return !destinationImage.empty();
            }

            destinationImage.create(dstRows, dstCols, sourceImage.type());
            if (destinationImage.empty())
            {
                return false;
            }

            cv::Scalar fillScalar;
            if (!TryGetRotateFillScalar(channels, channelLayout, backgroundColor, fillScalar))
            {
                destinationImage.release();
                return false;
            }

            destinationImage.setTo(fillScalar);
            return true;
        }
        catch (...)
        {
            destinationImage.release();
            return false;
        }
    }

    /**
     * @brief 获取图像旋转中心。
     *
     * 使用 (cols - 1) / 2 与 (rows - 1) / 2，能够与像素中心坐标对齐，
     * 避免偶数尺寸图像在旋转时产生额外的半像素偏移。
     */
    static cv::Point2d GetRotationCenter(const cv::Mat& imageMat)
    {
        return cv::Point2d((static_cast<double>(imageMat.cols) - 1.0) * 0.5, (static_cast<double>(imageMat.rows) - 1.0) * 0.5);
    }

    /**
     * @brief 用 2x3 仿射矩阵变换一个点。
     */
    static cv::Point2d TransformAffinePoint(const cv::Mat& affineMatrix, const cv::Point2d& sourcePoint)
    {
        const double x = affineMatrix.at<double>(0, 0) * sourcePoint.x + affineMatrix.at<double>(0, 1) * sourcePoint.y + affineMatrix.at<double>(0, 2);
        const double y = affineMatrix.at<double>(1, 0) * sourcePoint.x + affineMatrix.at<double>(1, 1) * sourcePoint.y + affineMatrix.at<double>(1, 2);
        return cv::Point2d(x, y);
    }

    /**
     * @brief 在 expandOutput=true 时，计算完整包围旋转结果所需的尺寸，并同步修正平移量。
     */
    static bool AdjustRotationMatrixForExpandedOutput(const cv::Mat& sourceImage, cv::Mat& affineMatrix, int& dstRows, int& dstCols)
    {
        if (sourceImage.empty() || affineMatrix.rows != 2 || affineMatrix.cols != 3)
        {
            return false;
        }

        const std::vector<cv::Point2d> sourceCorners =
        {
            cv::Point2d(-0.5, -0.5),
            cv::Point2d(static_cast<double>(sourceImage.cols) - 0.5, -0.5),
            cv::Point2d(static_cast<double>(sourceImage.cols) - 0.5, static_cast<double>(sourceImage.rows) - 0.5),
            cv::Point2d(-0.5, static_cast<double>(sourceImage.rows) - 0.5)
        };

        double minX = 0.0;
        double minY = 0.0;
        double maxX = 0.0;
        double maxY = 0.0;
        bool firstPoint = true;

        for (size_t i = 0; i < sourceCorners.size(); i++)
        {
            const cv::Point2d transformedPoint = TransformAffinePoint(affineMatrix, sourceCorners[i]);
            if (firstPoint)
            {
                minX = transformedPoint.x;
                maxX = transformedPoint.x;
                minY = transformedPoint.y;
                maxY = transformedPoint.y;
                firstPoint = false;
            }
            else
            {
                if (transformedPoint.x < minX)
                {
                    minX = transformedPoint.x;
                }
                if (transformedPoint.x > maxX)
                {
                    maxX = transformedPoint.x;
                }
                if (transformedPoint.y < minY)
                {
                    minY = transformedPoint.y;
                }
                if (transformedPoint.y > maxY)
                {
                    maxY = transformedPoint.y;
                }
            }
        }

        const double dstWidthDouble = std::ceil(maxX) - std::floor(minX);
        const double dstHeightDouble = std::ceil(maxY) - std::floor(minY);
        if (dstWidthDouble <= 0.0 || dstHeightDouble <= 0.0)
        {
            return false;
        }

        const size_t dstWidthSize = static_cast<size_t>(dstWidthDouble);
        const size_t dstHeightSize = static_cast<size_t>(dstHeightDouble);
        if (!TryConvertSizeToInt(dstWidthSize, dstCols) || !TryConvertSizeToInt(dstHeightSize, dstRows))
        {
            return false;
        }

        affineMatrix.at<double>(0, 2) -= std::floor(minX);
        affineMatrix.at<double>(1, 2) -= std::floor(minY);
        return true;
    }

    /**
     * @brief 判断是否是 90 度整数倍旋转，并返回可直接使用的 cv::rotate 代码。
     */
    static bool TryGetCvRotateCode(double normalizedAngleDegrees, int& rotateCode)
    {
        if (IsNearlyEqual(normalizedAngleDegrees, 90.0))
        {
            rotateCode = cv::ROTATE_90_COUNTERCLOCKWISE;
            return true;
        }

        if (IsNearlyEqual(normalizedAngleDegrees, 180.0))
        {
            rotateCode = cv::ROTATE_180;
            return true;
        }

        if (IsNearlyEqual(normalizedAngleDegrees, 270.0))
        {
            rotateCode = cv::ROTATE_90_CLOCKWISE;
            return true;
        }

        rotateCode = -1;
        return false;
    }
}

/**
 * @brief GB_Image 的内部实现。
 */
class GB_Image::Impl
{
public:
    cv::Mat imageMat;
    GBImage_Internal::ImageChannelLayout channelLayout = GBImage_Internal::ImageChannelLayout::Empty;
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
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
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
        imageImpl->channelLayout = GBImage_Internal::ImageChannelLayout::Empty;
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
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
    if (rows == 0 || cols == 0)
    {
        return false;
    }

    int cvRows = 0;
    int cvCols = 0;
    int cvType = 0;
    if (!GBImage_Internal::TryConvertSizeToInt(rows, cvRows) || !GBImage_Internal::TryConvertSizeToInt(cols, cvCols) || !GBImage_Internal::TryGetCvType(depth, channels, cvType))
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
        imageImpl->channelLayout = GBImage_Internal::InferChannelLayout(imageImpl->imageMat);
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
    const GB_ByteBuffer encodedBytes = GB_ReadBinaryFromFile(filePathUtf8);
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
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
    if (encodedData == nullptr || encodedSize == 0)
    {
        return false;
    }

    int cvEncodedSize = 0;
    if (!GBImage_Internal::TryConvertSizeToInt(encodedSize, cvEncodedSize))
    {
        return false;
    }

    try
    {
        const cv::Mat encodedView(1, cvEncodedSize, CV_8UC1, const_cast<void*>(encodedData));
        const int imreadFlags = GBImage_Internal::GetImreadFlags(loadOptions);

        cv::Mat decodedImage;
        if (loadOptions.pageIndex == 0)
        {
            decodedImage = cv::imdecode(encodedView, imreadFlags);
        }
        else
        {
            int beginIndex = 0;
            if (!GBImage_Internal::TryConvertSizeToInt(loadOptions.pageIndex, beginIndex))
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

        if (!GBImage_Internal::ApplyLoadPostProcess(decodedImage, loadOptions))
        {
            return false;
        }

        if (!EnsureImageImpl())
        {
            return false;
        }

        imageImpl->imageMat = std::move(decodedImage);
        imageImpl->channelLayout = GBImage_Internal::InferChannelLayout(imageImpl->imageMat);
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

    const std::string fileExt = GBImage_Internal::NormalizeFileExtension(GB_GetFileExt(filePathUtf8));
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
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
    encodedBytes.clear();

    if (IsEmpty())
    {
        return false;
    }

    const std::string normalizedExt = GBImage_Internal::NormalizeFileExtension(fileExt);
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
        GBImage_Internal::BuildImwriteParams(normalizedExt, saveOptions, imwriteParams);

        cv::Mat imageForEncode;
        if (!GBImage_Internal::PrepareImageForEncoding(imageImpl->imageMat, imageImpl->channelLayout, imageForEncode))
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
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
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
        imageImpl->channelLayout = GBImage_Internal::InferChannelLayout(imageImpl->imageMat);
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
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
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
 * @brief 返回当前类类型字符串。
 */
const std::string& GB_Image::GetClassType() const
{
    static const std::string classType = "GB_Image";
    return classType;
}

/**
 * @brief 返回当前类类型 Id。
 */
uint64_t GB_Image::GetClassTypeId() const
{
    static const uint64_t classTypeId = GB_GenerateClassTypeId(GetClassType());
    return classTypeId;
}

/**
 * @brief 导出为逻辑 RGBA 二维矩阵。
 */
bool GB_Image::ToColorMatrix(std::vector<std::vector<GB_ColorRGBA>>& colorMatrix) const
{
    colorMatrix.clear();

    if (IsEmpty() || !GBImage_Internal::IsColorMatrixSupported(imageImpl->imageMat, imageImpl->channelLayout))
    {
        return false;
    }

    const size_t rows = GetRows();
    const size_t cols = GetCols();

    try
    {
        std::vector<std::vector<GB_ColorRGBA>> newColorMatrix;
        newColorMatrix.resize(rows);

        for (size_t rowIndex = 0; rowIndex < rows; rowIndex++)
        {
            std::vector<GB_ColorRGBA>& colorRow = newColorMatrix[rowIndex];
            colorRow.resize(cols);

            const unsigned char* rowData = GetRowData(rowIndex);
            if (rowData == nullptr)
            {
                return false;
            }

            switch (imageImpl->channelLayout)
            {
            case GBImage_Internal::ImageChannelLayout::Gray:
                for (size_t colIndex = 0; colIndex < cols; colIndex++)
                {
                    const uint8_t grayValue = rowData[colIndex];
                    colorRow[colIndex] = GB_ColorRGBA(grayValue, grayValue, grayValue, 255);
                }
                break;

            case GBImage_Internal::ImageChannelLayout::Bgr:
                for (size_t colIndex = 0; colIndex < cols; colIndex++)
                {
                    const unsigned char* pixelData = rowData + colIndex * 3;
                    colorRow[colIndex] = GB_ColorRGBA(pixelData[2], pixelData[1], pixelData[0], 255);
                }
                break;

            case GBImage_Internal::ImageChannelLayout::Bgra:
                for (size_t colIndex = 0; colIndex < cols; colIndex++)
                {
                    const unsigned char* pixelData = rowData + colIndex * 4;
                    colorRow[colIndex] = GB_ColorRGBA(pixelData[2], pixelData[1], pixelData[0], pixelData[3]);
                }
                break;

            case GBImage_Internal::ImageChannelLayout::Rgb:
                for (size_t colIndex = 0; colIndex < cols; colIndex++)
                {
                    const unsigned char* pixelData = rowData + colIndex * 3;
                    colorRow[colIndex] = GB_ColorRGBA(pixelData[0], pixelData[1], pixelData[2], 255);
                }
                break;

            case GBImage_Internal::ImageChannelLayout::Rgba:
                for (size_t colIndex = 0; colIndex < cols; colIndex++)
                {
                    const unsigned char* pixelData = rowData + colIndex * 4;
                    colorRow[colIndex] = GB_ColorRGBA(pixelData[0], pixelData[1], pixelData[2], pixelData[3]);
                }
                break;

            default:
                return false;
            }
        }

        colorMatrix.swap(newColorMatrix);
        return true;
    }
    catch (...)
    {
        colorMatrix.clear();
        return false;
    }
}

/**
 * @brief 根据逻辑 RGBA 二维矩阵设置当前图像。
 */
bool GB_Image::SetFromColorMatrix(const std::vector<std::vector<GB_ColorRGBA>>& colorMatrix)
{
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
    if (colorMatrix.empty() || colorMatrix[0].empty())
    {
        return false;
    }

    const size_t rows = colorMatrix.size();
    const size_t cols = colorMatrix[0].size();
    for (size_t rowIndex = 1; rowIndex < rows; rowIndex++)
    {
        if (colorMatrix[rowIndex].size() != cols)
        {
            return false;
        }
    }

    int cvRows = 0;
    int cvCols = 0;
    if (!GBImage_Internal::TryConvertSizeToInt(rows, cvRows) || !GBImage_Internal::TryConvertSizeToInt(cols, cvCols))
    {
        return false;
    }

    try
    {
        cv::Mat newImageMat(cvRows, cvCols, CV_8UC4);
        if (newImageMat.empty())
        {
            return false;
        }

        for (size_t rowIndex = 0; rowIndex < rows; rowIndex++)
        {
            unsigned char* rowData = newImageMat.ptr<unsigned char>(static_cast<int>(rowIndex));
            if (rowData == nullptr)
            {
                return false;
            }

            const std::vector<GB_ColorRGBA>& colorRow = colorMatrix[rowIndex];
            for (size_t colIndex = 0; colIndex < cols; colIndex++)
            {
                const GB_ColorRGBA& pixelColor = colorRow[colIndex];
                unsigned char* pixelData = rowData + colIndex * 4;
                pixelData[0] = pixelColor.b;
                pixelData[1] = pixelColor.g;
                pixelData[2] = pixelColor.r;
                pixelData[3] = pixelColor.a;
            }
        }

        if (!EnsureImageImpl())
        {
            return false;
        }

        imageImpl->imageMat = std::move(newImageMat);
        imageImpl->channelLayout = GBImage_Internal::ImageChannelLayout::Bgra;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

/**
 * @brief 序列化为便于人类阅读的文本字符串。
 */
std::string GB_Image::SerializeToString() const
{
    if (IsEmpty())
    {
        return std::string("(") + GetClassType() + " empty)";
    }

    if (!GBImage_Internal::IsColorMatrixSupported(imageImpl->imageMat, imageImpl->channelLayout))
    {
        return std::string();
    }

    try
    {
        std::ostringstream oss;
        oss.imbue(std::locale::classic());
        oss << "(" << GetClassType();
        oss << " rows=" << GetRows();
        oss << " cols=" << GetCols();
        oss << " pixels=[";

        for (size_t rowIndex = 0; rowIndex < GetRows(); rowIndex++)
        {
            if (rowIndex != 0)
            {
                oss << ",";
            }

            oss << "[";
            for (size_t colIndex = 0; colIndex < GetCols(); colIndex++)
            {
                if (colIndex != 0)
                {
                    oss << ",";
                }

                GB_ColorRGBA pixelColor;
                if (!GetPixelColor(rowIndex, colIndex, pixelColor))
                {
                    return std::string();
                }

                oss << "(GB_ColorRGBA "
                    << static_cast<unsigned int>(pixelColor.r) << ","
                    << static_cast<unsigned int>(pixelColor.g) << ","
                    << static_cast<unsigned int>(pixelColor.b) << ","
                    << static_cast<unsigned int>(pixelColor.a) << ")";
            }
            oss << "]";
        }

        oss << "])";
        return oss.str();
    }
    catch (...)
    {
        return std::string();
    }
}

/**
 * @brief 序列化为二进制缓冲区。
 */
GB_ByteBuffer GB_Image::SerializeToBinary() const
{
    constexpr static uint16_t payloadVersion = 1;
    constexpr static size_t headerByteSize = 48;

    const size_t rows = GetRows();
    const size_t cols = GetCols();
    const size_t bytesPerPixel = GetBytesPerPixel();
    const bool isEmptyImage = IsEmpty();

    size_t compactPixelByteSize = 0;
    if (!isEmptyImage)
    {
        if (!GBImage_Internal::GetCompactPixelByteSize(rows, cols, bytesPerPixel, compactPixelByteSize))
        {
            return GB_ByteBuffer();
        }
    }

    GB_ByteBuffer buffer;
    try
    {
        buffer.reserve(headerByteSize + compactPixelByteSize);

        GB_ByteBufferIO::AppendUInt32LE(buffer, GB_ClassMagicNumber);
        GB_ByteBufferIO::AppendUInt64LE(buffer, GetClassTypeId());
        GB_ByteBufferIO::AppendUInt16LE(buffer, payloadVersion);
        GB_ByteBufferIO::AppendUInt16LE(buffer, 0);

        GB_ByteBufferIO::AppendUInt64LE(buffer, static_cast<uint64_t>(rows));
        GB_ByteBufferIO::AppendUInt64LE(buffer, static_cast<uint64_t>(cols));
        GB_ByteBufferIO::AppendUInt16LE(buffer, static_cast<uint16_t>(GetDepth()));
        GB_ByteBufferIO::AppendUInt16LE(buffer, static_cast<uint16_t>(GetChannels()));
        GB_ByteBufferIO::AppendUInt16LE(buffer, static_cast<uint16_t>(isEmptyImage ? GBImage_Internal::ImageChannelLayout::Empty : imageImpl->channelLayout));
        GB_ByteBufferIO::AppendUInt16LE(buffer, 0);
        GB_ByteBufferIO::AppendUInt64LE(buffer, static_cast<uint64_t>(compactPixelByteSize));

        if (!isEmptyImage && !GBImage_Internal::CopyMatToCompactBytes(imageImpl->imageMat, buffer))
        {
            return GB_ByteBuffer();
        }

        return buffer;
    }
    catch (...)
    {
        return GB_ByteBuffer();
    }
}

namespace
{
    static void SkipAsciiWhitespace(const std::string& text, size_t& offset)
    {
        while (offset < text.size() && std::isspace(static_cast<unsigned char>(text[offset])) != 0)
        {
            offset++;
        }
    }

    static bool ConsumeChar(const std::string& text, size_t& offset, char expectedChar)
    {
        SkipAsciiWhitespace(text, offset);
        if (offset >= text.size() || text[offset] != expectedChar)
        {
            return false;
        }

        offset++;
        return true;
    }

    static bool ConsumeLiteral(const std::string& text, size_t& offset, const char* literalText)
    {
        if (literalText == nullptr)
        {
            return false;
        }

        SkipAsciiWhitespace(text, offset);
        const size_t literalLength = std::strlen(literalText);
        if (offset + literalLength > text.size())
        {
            return false;
        }

        if (text.compare(offset, literalLength, literalText) != 0)
        {
            return false;
        }

        offset += literalLength;
        return true;
    }

    static bool ParseUIntComponent(const std::string& text, size_t& offset, unsigned int& componentValue)
    {
        SkipAsciiWhitespace(text, offset);
        if (offset >= text.size() || !std::isdigit(static_cast<unsigned char>(text[offset])))
        {
            return false;
        }

        unsigned int parsedValue = 0;
        while (offset < text.size() && std::isdigit(static_cast<unsigned char>(text[offset])) != 0)
        {
            const unsigned int digitValue = static_cast<unsigned int>(text[offset] - '0');
            if (parsedValue > (std::numeric_limits<unsigned int>::max() - digitValue) / 10u)
            {
                return false;
            }

            parsedValue = parsedValue * 10u + digitValue;
            offset++;
        }

        componentValue = parsedValue;
        return true;
    }

    static bool ParseSizeValue(const std::string& text, size_t& offset, size_t& sizeValue)
    {
        SkipAsciiWhitespace(text, offset);
        if (offset >= text.size() || !std::isdigit(static_cast<unsigned char>(text[offset])))
        {
            return false;
        }

        size_t parsedValue = 0;
        while (offset < text.size() && std::isdigit(static_cast<unsigned char>(text[offset])) != 0)
        {
            const size_t digitValue = static_cast<size_t>(text[offset] - '0');
            if (parsedValue > (std::numeric_limits<size_t>::max() - digitValue) / static_cast<size_t>(10))
            {
                return false;
            }

            parsedValue = parsedValue * static_cast<size_t>(10) + digitValue;
            offset++;
        }

        sizeValue = parsedValue;
        return true;
    }

    static bool ParseColorText(const std::string& text, size_t& offset, GB_ColorRGBA& pixelColor)
    {
        size_t localOffset = offset;
        unsigned int red = 0;
        unsigned int green = 0;
        unsigned int blue = 0;
        unsigned int alpha = 0;

        if (!ConsumeChar(text, localOffset, '(')
            || !ConsumeLiteral(text, localOffset, "GB_ColorRGBA")
            || !ParseUIntComponent(text, localOffset, red)
            || !ConsumeChar(text, localOffset, ',')
            || !ParseUIntComponent(text, localOffset, green)
            || !ConsumeChar(text, localOffset, ',')
            || !ParseUIntComponent(text, localOffset, blue)
            || !ConsumeChar(text, localOffset, ',')
            || !ParseUIntComponent(text, localOffset, alpha)
            || !ConsumeChar(text, localOffset, ')'))
        {
            return false;
        }

        if (red > 255u || green > 255u || blue > 255u || alpha > 255u)
        {
            return false;
        }

        pixelColor = GB_ColorRGBA(static_cast<uint8_t>(red), static_cast<uint8_t>(green), static_cast<uint8_t>(blue), static_cast<uint8_t>(alpha));
        offset = localOffset;
        return true;
    }
}

/**
 * @brief 从文本字符串反序列化。
 */
bool GB_Image::Deserialize(const std::string& data)
{
    size_t offset = 0;
    size_t rows = 0;
    size_t cols = 0;

    if (!ConsumeChar(data, offset, '(') || !ConsumeLiteral(data, offset, GetClassType().c_str()))
    {
        return false;
    }

    if (ConsumeLiteral(data, offset, "empty"))
    {
        if (!ConsumeChar(data, offset, ')'))
        {
            return false;
        }

        SkipAsciiWhitespace(data, offset);
        if (offset != data.size())
        {
            return false;
        }

        Clear();
        return true;
    }

    if (!ConsumeLiteral(data, offset, "rows=")
        || !ParseSizeValue(data, offset, rows)
        || !ConsumeLiteral(data, offset, "cols=")
        || !ParseSizeValue(data, offset, cols)
        || !ConsumeLiteral(data, offset, "pixels=")
        || !ConsumeChar(data, offset, '['))
    {
        return false;
    }

    if (rows == 0 || cols == 0)
    {
        return false;
    }

    int cvRows = 0;
    int cvCols = 0;
    if (!GBImage_Internal::TryConvertSizeToInt(rows, cvRows)
        || !GBImage_Internal::TryConvertSizeToInt(cols, cvCols))
    {
        return false;
    }

    try
    {
        cv::Mat newImageMat(cvRows, cvCols, CV_8UC4);
        if (newImageMat.empty())
        {
            return false;
        }

        for (size_t rowIndex = 0; rowIndex < rows; rowIndex++)
        {
            if (rowIndex != 0 && !ConsumeChar(data, offset, ','))
            {
                return false;
            }

            if (!ConsumeChar(data, offset, '['))
            {
                return false;
            }

            unsigned char* rowData = newImageMat.ptr<unsigned char>(static_cast<int>(rowIndex));
            if (rowData == nullptr)
            {
                return false;
            }

            for (size_t colIndex = 0; colIndex < cols; colIndex++)
            {
                if (colIndex != 0 && !ConsumeChar(data, offset, ','))
                {
                    return false;
                }

                GB_ColorRGBA pixelColor;
                if (!ParseColorText(data, offset, pixelColor))
                {
                    return false;
                }

                unsigned char* pixelData = rowData + colIndex * 4;
                pixelData[0] = pixelColor.b;
                pixelData[1] = pixelColor.g;
                pixelData[2] = pixelColor.r;
                pixelData[3] = pixelColor.a;
            }

            if (!ConsumeChar(data, offset, ']'))
            {
                return false;
            }
        }

        if (!ConsumeChar(data, offset, ']') || !ConsumeChar(data, offset, ')'))
        {
            return false;
        }

        SkipAsciiWhitespace(data, offset);
        if (offset != data.size())
        {
            return false;
        }

        GB_Image newImage;
        newImage.imageImpl->imageMat = std::move(newImageMat);
        newImage.imageImpl->channelLayout = GBImage_Internal::ImageChannelLayout::Bgra;
        Swap(newImage);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

/**
 * @brief 从二进制缓冲区反序列化。
 */
bool GB_Image::Deserialize(const GB_ByteBuffer& data)
{
    constexpr static uint16_t expectedPayloadVersion = 1;
    constexpr static size_t minByteSize = 48;

    if (data.size() < minByteSize)
    {
        return false;
    }

    size_t offset = 0;
    uint32_t magic = 0;
    uint64_t typeId = 0;
    uint16_t payloadVersion = 0;
    uint16_t reserved0 = 0;
    uint64_t rowsValue = 0;
    uint64_t colsValue = 0;
    uint16_t depthValue = 0;
    uint16_t channelsValue = 0;
    uint16_t channelLayoutValue = 0;
    uint16_t reserved1 = 0;
    uint64_t rawDataByteSizeValue = 0;

    if (!GB_ByteBufferIO::ReadUInt32LE(data, offset, magic)
        || !GB_ByteBufferIO::ReadUInt64LE(data, offset, typeId)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, payloadVersion)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, reserved0)
        || !GB_ByteBufferIO::ReadUInt64LE(data, offset, rowsValue)
        || !GB_ByteBufferIO::ReadUInt64LE(data, offset, colsValue)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, depthValue)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, channelsValue)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, channelLayoutValue)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, reserved1)
        || !GB_ByteBufferIO::ReadUInt64LE(data, offset, rawDataByteSizeValue))
    {
        return false;
    }

    if (magic != GB_ClassMagicNumber
        || typeId != GetClassTypeId()
        || payloadVersion != expectedPayloadVersion
        || reserved0 != 0
        || reserved1 != 0)
    {
        return false;
    }

    size_t rows = 0;
    size_t cols = 0;
    size_t rawDataByteSize = 0;
    if (!GBImage_Internal::TryConvertUInt64ToSize(rowsValue, rows)
        || !GBImage_Internal::TryConvertUInt64ToSize(colsValue, cols)
        || !GBImage_Internal::TryConvertUInt64ToSize(rawDataByteSizeValue, rawDataByteSize))
    {
        return false;
    }

    const GB_ImageDepth imageDepth = static_cast<GB_ImageDepth>(depthValue);
    const int channels = static_cast<int>(channelsValue);
    const GBImage_Internal::ImageChannelLayout channelLayout = static_cast<GBImage_Internal::ImageChannelLayout>(channelLayoutValue);

    const bool isEmptyImage = rows == 0 || cols == 0 || channels == 0 || imageDepth == GB_ImageDepth::Unknown;
    if (!GBImage_Internal::IsChannelLayoutValidForChannels(channels, channelLayout, isEmptyImage))
    {
        return false;
    }

    if (isEmptyImage)
    {
        if (rows != 0 || cols != 0 || channels != 0 || imageDepth != GB_ImageDepth::Unknown || channelLayout != GBImage_Internal::ImageChannelLayout::Empty || rawDataByteSize != 0 || offset != data.size())
        {
            return false;
        }

        Clear();
        return true;
    }

    int cvRows = 0;
    int cvCols = 0;
    int cvType = 0;
    if (!GBImage_Internal::TryConvertSizeToInt(rows, cvRows)
        || !GBImage_Internal::TryConvertSizeToInt(cols, cvCols)
        || !GBImage_Internal::TryGetCvType(imageDepth, channels, cvType))
    {
        return false;
    }

    const size_t bytesPerPixel = GBImage_Internal::GetBytesPerChannel(imageDepth) * static_cast<size_t>(channels);
    size_t expectedRawDataByteSize = 0;
    if (!GBImage_Internal::GetCompactPixelByteSize(rows, cols, bytesPerPixel, expectedRawDataByteSize))
    {
        return false;
    }

    if (rawDataByteSize != expectedRawDataByteSize || offset + rawDataByteSize != data.size())
    {
        return false;
    }

    try
    {
        cv::Mat newImageMat(cvRows, cvCols, cvType);
        if (newImageMat.empty())
        {
            return false;
        }

        if (!GBImage_Internal::CopyCompactBytesToMat(data, offset, newImageMat))
        {
            return false;
        }

        if (!EnsureImageImpl())
        {
            return false;
        }

        imageImpl->imageMat = std::move(newImageMat);
        imageImpl->channelLayout = channelLayout;
        return true;
    }
    catch (...)
    {
        return false;
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

    return GBImage_Internal::CvDepthToImageDepth(imageImpl->imageMat.depth());
}

size_t GB_Image::GetBitDepth() const
{
    return GetBytesPerChannel() * 8;
}

size_t GB_Image::GetBytesPerChannel() const
{
    if (IsEmpty())
    {
        return 0;
    }
    return imageImpl->imageMat.elemSize1();
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

    const unsigned char* pixelPtr = GBImage_Internal::GetPixelPtr(imageImpl->imageMat, row, col);
    if (pixelPtr == nullptr)
    {
        return false;
    }

    return GBImage_Internal::ReadPixelColor(pixelPtr, imageImpl->channelLayout, pixelColor);
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

    unsigned char* pixelPtr = GBImage_Internal::GetPixelPtr(imageImpl->imageMat, row, col);
    if (pixelPtr == nullptr)
    {
        return false;
    }

    return GBImage_Internal::WritePixelColor(pixelPtr, imageImpl->channelLayout, pixelColor);
}

/**
 * @brief 用指定颜色填充整幅图像。
 */
bool GB_Image::Fill(const GB_ColorRGBA& pixelColor)
{
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
    if (IsEmpty() || imageImpl->imageMat.depth() != CV_8U)
    {
        return false;
    }

    try
    {
        cv::Scalar fillScalar;
        if (!GBImage_Internal::GetFillScalar(pixelColor, imageImpl->channelLayout, fillScalar))
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
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
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
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
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
 * @brief 转换像素位深，并附带线性变换。
 */
GB_Image GB_Image::ConvertTo(GB_ImageDepth targetDepth, double scale, double shift) const
{
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
    GB_Image resultImage;
    if (IsEmpty())
    {
        return resultImage;
    }

    int cvType = 0;
    if (!GBImage_Internal::TryGetCvType(targetDepth, GetChannels(), cvType))
    {
        return resultImage;
    }

    try
    {
        imageImpl->imageMat.convertTo(resultImage.imageImpl->imageMat, cvType, scale, shift);
        resultImage.imageImpl->channelLayout = imageImpl->channelLayout;
    }
    catch (...)
    {
        resultImage.Clear();
    }

    return resultImage;
}

/**
 * @brief 原地转换像素位深，并附带线性变换。
 */
bool GB_Image::ConvertToInPlace(GB_ImageDepth targetDepth, double scale, double shift)
{
    if (IsEmpty())
    {
        return false;
    }

    GB_Image convertedImage = ConvertTo(targetDepth, scale, shift);
    if (convertedImage.IsEmpty())
    {
        return false;
    }

    Swap(convertedImage);
    return true;
}

/**
 * @brief 翻转图像。
 */
GB_Image GB_Image::Flip(bool horizontalFlip, bool verticalFlip) const
{
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
    GB_Image resultImage;
    if (IsEmpty())
    {
        return resultImage;
    }

    if (!horizontalFlip && !verticalFlip)
    {
        return Clone();
    }

    const int flipCode = horizontalFlip ? (verticalFlip ? -1 : 1) : 0;

    try
    {
        cv::flip(imageImpl->imageMat, resultImage.imageImpl->imageMat, flipCode);
        resultImage.imageImpl->channelLayout = imageImpl->channelLayout;
    }
    catch (...)
    {
        resultImage.Clear();
    }

    return resultImage;
}

/**
 * @brief 原地翻转图像。
 */
bool GB_Image::FlipInPlace(bool horizontalFlip, bool verticalFlip)
{
    if (IsEmpty())
    {
        return false;
    }

    if (!horizontalFlip && !verticalFlip)
    {
        return true;
    }

    GB_Image flippedImage = Flip(horizontalFlip, verticalFlip);
    if (flippedImage.IsEmpty())
    {
        return false;
    }

    Swap(flippedImage);
    return true;
}

/**
 * @brief 旋转图像。
 */
GB_Image GB_Image::Rotate(double angleDegrees, const GB_ImageRotateOptions& rotateOptions) const
{
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
    GB_Image resultImage;
    if (IsEmpty())
    {
        return resultImage;
    }

    cv::Mat rotateSourceImage;
    GBImage_Internal::ImageChannelLayout rotateSourceLayout = GBImage_Internal::ImageChannelLayout::Empty;
    if (!GBImage_Internal::PrepareRotateSourceImage(imageImpl->imageMat, imageImpl->channelLayout, rotateOptions, rotateSourceImage, rotateSourceLayout))
    {
        return resultImage;
    }

    const double normalizedAngle = GBImage_Internal::NormalizeAngleDegrees(angleDegrees);
    if (GBImage_Internal::IsNearlyEqual(normalizedAngle, 0.0))
    {
        try
        {
            resultImage.imageImpl->imageMat = rotateSourceImage.clone();
            resultImage.imageImpl->channelLayout = rotateSourceLayout;
        }
        catch (...)
        {
            resultImage.Clear();
        }

        return resultImage;
    }

    int cvRotateCode = -1;
    const bool isQuarterTurn = GBImage_Internal::TryGetCvRotateCode(normalizedAngle, cvRotateCode);
    if (isQuarterTurn && (rotateOptions.expandOutput || cvRotateCode == cv::ROTATE_180))
    {
        try
        {
            cv::rotate(rotateSourceImage, resultImage.imageImpl->imageMat, cvRotateCode);
            resultImage.imageImpl->channelLayout = rotateSourceLayout;
        }
        catch (...)
        {
            resultImage.Clear();
        }

        return resultImage;
    }

    int dstRows = rotateSourceImage.rows;
    int dstCols = rotateSourceImage.cols;

    try
    {
        cv::Mat rotationMatrix = cv::getRotationMatrix2D(GBImage_Internal::GetRotationCenter(rotateSourceImage), angleDegrees, 1.0);
        if (rotationMatrix.empty())
        {
            return resultImage;
        }

        if (rotateOptions.expandOutput)
        {
            if (!GBImage_Internal::AdjustRotationMatrixForExpandedOutput(rotateSourceImage, rotationMatrix, dstRows, dstCols))
            {
                return resultImage;
            }
        }

        if (!GBImage_Internal::CreatePrefilledRotateDestination(rotateSourceImage, rotateSourceLayout, rotateOptions,
            dstRows, dstCols, resultImage.imageImpl->imageMat))
        {
            return resultImage;
        }

        cv::warpAffine(rotateSourceImage, resultImage.imageImpl->imageMat, rotationMatrix, cv::Size(dstCols, dstRows),
            GBImage_Internal::ToCvInterpolation(rotateOptions.interpolation), cv::BORDER_TRANSPARENT);

        resultImage.imageImpl->channelLayout = rotateSourceLayout;
    }
    catch (...)
    {
        resultImage.Clear();
    }

    return resultImage;
}

/**
 * @brief 原地旋转图像。
 */
bool GB_Image::RotateInPlace(double angleDegrees, const GB_ImageRotateOptions& rotateOptions)
{
    if (IsEmpty())
    {
        return false;
    }

    GB_Image rotatedImage = Rotate(angleDegrees, rotateOptions);
    if (rotatedImage.IsEmpty())
    {
        return false;
    }

    Swap(rotatedImage);
    return true;
}

/**
 * @brief 生成缩放后的新图像。
 */
GB_Image GB_Image::Resize(size_t newRows, size_t newCols, GB_ImageInterpolation interpolation) const
{
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
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
    if (!GBImage_Internal::TryConvertSizeToInt(newRows, cvRows) || !GBImage_Internal::TryConvertSizeToInt(newCols, cvCols))
    {
        return resultImage;
    }

    try
    {
        cv::resize(imageImpl->imageMat, resultImage.imageImpl->imageMat, cv::Size(cvCols, cvRows), 0.0, 0.0, GBImage_Internal::ToCvInterpolation(interpolation));
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
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
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
    if (!GBImage_Internal::TryConvertSizeToInt(row, cvRow) || !GBImage_Internal::TryConvertSizeToInt(col, cvCol) || !GBImage_Internal::TryConvertSizeToInt(cropRows, cvCropRows) || !GBImage_Internal::TryConvertSizeToInt(cropCols, cvCropCols))
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
    GBImage_Internal::EnsureOpenCvErrorLogOnly();
    GB_Image resultImage;
    if (IsEmpty())
    {
        return resultImage;
    }

    int cvColorCode = -1;
    if (!GBImage_Internal::TryGetCvColorCode(conversion, cvColorCode))
    {
        return resultImage;
    }

    if (!GBImage_Internal::IsConversionSourceLayoutCompatible(conversion, imageImpl->channelLayout))
    {
        return resultImage;
    }

    try
    {
        cv::cvtColor(imageImpl->imageMat, resultImage.imageImpl->imageMat, cvColorCode);
        resultImage.imageImpl->channelLayout = GBImage_Internal::GetConvertedChannelLayout(conversion);
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
