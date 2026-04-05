#include "GB_Image.h"
#include "../GB_FileSystem.h"
#include "../GB_IO.h"
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace gbImageInternal
{
    static bool TryConvertSizeToInt(size_t value, int& intValue)
    {
        if (value > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        intValue = static_cast<int>(value);
        return true;
    }

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

    static uint8_t RgbaToGray8(const GB_ColorRGBA& pixelColor)
    {
        const int grayValue = (299 * static_cast<int>(pixelColor.r) + 587 * static_cast<int>(pixelColor.g) + 114 * static_cast<int>(pixelColor.b) + 500) / 1000;
        return ClampToByte(grayValue);
    }

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

    static bool TryGetCvType(GB_ImageDepth imageDepth, int channels, int& cvType)
    {
        if (channels <= 0 || channels > 4)
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
            if (imageMat.channels() == 4)
            {
                return true;
            }

            if (imageMat.channels() == 3)
            {
                cv::Mat convertedImage;
                cv::cvtColor(imageMat, convertedImage, cv::COLOR_BGR2BGRA);
                imageMat = std::move(convertedImage);
                return true;
            }

            if (imageMat.channels() == 1)
            {
                cv::Mat convertedImage;
                cv::cvtColor(imageMat, convertedImage, cv::COLOR_GRAY2BGRA);
                imageMat = std::move(convertedImage);
                return true;
            }
        }
        catch (...)
        {
            return false;
        }

        return false;
    }

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
            const unsigned char ch = static_cast<unsigned char>(normalizedExt[i]);
            if (ch >= static_cast<unsigned char>('A') && ch <= static_cast<unsigned char>('Z'))
            {
                normalizedExt[i] = static_cast<char>(ch - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
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
}

class GB_Image::Impl
{
public:
    cv::Mat imageMat;
};

GB_Image::GB_Image() : imageImpl(new Impl())
{
}

GB_Image::GB_Image(const std::string& filePathUtf8, const GB_ImageLoadOptions& loadOptions) : imageImpl(new Impl())
{
    (void)LoadFromFile(filePathUtf8, loadOptions);
}

GB_Image::GB_Image(const GB_ByteBuffer& encodedBytes, const GB_ImageLoadOptions& loadOptions) : imageImpl(new Impl())
{
    (void)LoadFromMemory(encodedBytes, loadOptions);
}

GB_Image::GB_Image(const void* encodedData, size_t encodedSize, const GB_ImageLoadOptions& loadOptions) : imageImpl(new Impl())
{
    (void)LoadFromMemory(encodedData, encodedSize, loadOptions);
}

GB_Image::GB_Image(size_t rows, size_t cols, GB_ImageDepth depth, int channels, bool zeroInitialize) : imageImpl(new Impl())
{
    (void)Create(rows, cols, depth, channels, zeroInitialize);
}

GB_Image::GB_Image(const GB_Image& other) : imageImpl(new Impl())
{
    if (other.imageImpl != nullptr)
    {
        imageImpl->imageMat = other.imageImpl->imageMat;
    }
}

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
}

GB_Image::GB_Image(GB_Image&& other) noexcept : imageImpl(new Impl())
{
    if (other.imageImpl != nullptr)
    {
        imageImpl->imageMat = std::move(other.imageImpl->imageMat);
    }
}

GB_Image::GB_Image(const cv::Mat& imageMat, GB_ImageCopyMode copyMode) : imageImpl(new Impl())
{
    (void)SetFromCvMat(imageMat, copyMode);
}

GB_Image::~GB_Image()
{
    delete imageImpl;
    imageImpl = nullptr;
}

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

    imageImpl->imageMat = other.imageImpl->imageMat;
    return *this;
}

GB_Image& GB_Image::operator=(GB_Image&& other) noexcept
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

    imageImpl->imageMat = std::move(other.imageImpl->imageMat);
    return *this;
}

void GB_Image::Swap(GB_Image& other) noexcept
{
    if (this == &other)
    {
        return;
    }

    imageImpl->imageMat.swap(other.imageImpl->imageMat);
}

void GB_Image::Clear()
{
    imageImpl->imageMat.release();
}

bool GB_Image::Create(size_t rows, size_t cols, GB_ImageDepth depth, int channels, bool zeroInitialize)
{
    Clear();

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
        if (zeroInitialize)
        {
            imageImpl->imageMat = cv::Mat::zeros(cvRows, cvCols, cvType);
        }
        else
        {
            imageImpl->imageMat.create(cvRows, cvCols, cvType);
        }
    }
    catch (...)
    {
        Clear();
        return false;
    }

    return !imageImpl->imageMat.empty();
}

bool GB_Image::LoadFromFile(const std::string& filePathUtf8, const GB_ImageLoadOptions& loadOptions)
{
    const GB_ByteBuffer encodedBytes = GB_ReadFileToBinary(filePathUtf8);
    if (encodedBytes.empty())
    {
        Clear();
        return false;
    }

    return LoadFromMemory(encodedBytes, loadOptions);
}

bool GB_Image::LoadFromMemory(const GB_ByteBuffer& encodedBytes, const GB_ImageLoadOptions& loadOptions)
{
    if (encodedBytes.empty())
    {
        Clear();
        return false;
    }

    return LoadFromMemory(encodedBytes.data(), encodedBytes.size(), loadOptions);
}

bool GB_Image::LoadFromMemory(const void* encodedData, size_t encodedSize, const GB_ImageLoadOptions& loadOptions)
{
    Clear();

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
                Clear();
                return false;
            }

            if (beginIndex == std::numeric_limits<int>::max())
            {
                Clear();
                return false;
            }

            const int endIndex = beginIndex + 1;
            std::vector<cv::Mat> decodedPages;
            if (!cv::imdecodemulti(encodedView, imreadFlags, decodedPages, cv::Range(beginIndex, endIndex)) || decodedPages.empty())
            {
                Clear();
                return false;
            }
            decodedImage = std::move(decodedPages[0]);
        }

        if (decodedImage.empty())
        {
            Clear();
            return false;
        }

        if (!gbImageInternal::ApplyLoadPostProcess(decodedImage, loadOptions))
        {
            Clear();
            return false;
        }

        imageImpl->imageMat = std::move(decodedImage);
        return true;
    }
    catch (...)
    {
        Clear();
        return false;
    }
}

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

        std::vector<unsigned char> cvEncodedBytes;
        if (!cv::imencode(normalizedExt, imageImpl->imageMat, cvEncodedBytes, imwriteParams))
        {
            return false;
        }

        encodedBytes.assign(cvEncodedBytes.begin(), cvEncodedBytes.end());
        return !encodedBytes.empty();
    }
    catch (...)
    {
        encodedBytes.clear();
        return false;
    }
}

bool GB_Image::SetFromCvMat(const cv::Mat& imageMat, GB_ImageCopyMode copyMode)
{
    if (imageMat.empty())
    {
        Clear();
        return false;
    }

    try
    {
        if (copyMode == GB_ImageCopyMode::DeepCopy)
        {
            imageImpl->imageMat = imageMat.clone();
        }
        else
        {
            imageImpl->imageMat = imageMat;
        }
        return true;
    }
    catch (...)
    {
        Clear();
        return false;
    }
}

cv::Mat GB_Image::ToCvMat(GB_ImageCopyMode copyMode) const
{
    if (IsEmpty())
    {
        return cv::Mat();
    }

    if (copyMode == GB_ImageCopyMode::DeepCopy)
    {
        return imageImpl->imageMat.clone();
    }

    return imageImpl->imageMat;
}

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

bool GB_Image::GetPixelColor(size_t row, size_t col, GB_ColorRGBA& pixelColor) const
{
    if (!IsValidPixelCoordinate(row, col))
    {
        return false;
    }

    if (imageImpl->imageMat.depth() != CV_8U)
    {
        return false;
    }

    const int channelCount = imageImpl->imageMat.channels();
    const unsigned char* pixelPtr = imageImpl->imageMat.ptr<unsigned char>(static_cast<int>(row)) + col * static_cast<size_t>(channelCount);

    if (channelCount == 1)
    {
        pixelColor.r = pixelPtr[0];
        pixelColor.g = pixelPtr[0];
        pixelColor.b = pixelPtr[0];
        pixelColor.a = 255;
        return true;
    }

    if (channelCount == 3)
    {
        pixelColor.r = pixelPtr[2];
        pixelColor.g = pixelPtr[1];
        pixelColor.b = pixelPtr[0];
        pixelColor.a = 255;
        return true;
    }

    if (channelCount == 4)
    {
        pixelColor.r = pixelPtr[2];
        pixelColor.g = pixelPtr[1];
        pixelColor.b = pixelPtr[0];
        pixelColor.a = pixelPtr[3];
        return true;
    }

    return false;
}

bool GB_Image::SetPixelColor(size_t row, size_t col, const GB_ColorRGBA& pixelColor)
{
    if (!IsValidPixelCoordinate(row, col))
    {
        return false;
    }

    if (imageImpl->imageMat.depth() != CV_8U)
    {
        return false;
    }

    const int channelCount = imageImpl->imageMat.channels();
    unsigned char* pixelPtr = imageImpl->imageMat.ptr<unsigned char>(static_cast<int>(row)) + col * static_cast<size_t>(channelCount);

    if (channelCount == 1)
    {
        pixelPtr[0] = gbImageInternal::RgbaToGray8(pixelColor);
        return true;
    }

    if (channelCount == 3)
    {
        pixelPtr[0] = pixelColor.b;
        pixelPtr[1] = pixelColor.g;
        pixelPtr[2] = pixelColor.r;
        return true;
    }

    if (channelCount == 4)
    {
        pixelPtr[0] = pixelColor.b;
        pixelPtr[1] = pixelColor.g;
        pixelPtr[2] = pixelColor.r;
        pixelPtr[3] = pixelColor.a;
        return true;
    }

    return false;
}

bool GB_Image::Fill(const GB_ColorRGBA& pixelColor)
{
    if (IsEmpty())
    {
        return false;
    }

    if (imageImpl->imageMat.depth() != CV_8U)
    {
        return false;
    }

    try
    {
        const int channelCount = imageImpl->imageMat.channels();
        if (channelCount == 1)
        {
            imageImpl->imageMat.setTo(cv::Scalar(gbImageInternal::RgbaToGray8(pixelColor)));
            return true;
        }

        if (channelCount == 3)
        {
            imageImpl->imageMat.setTo(cv::Scalar(pixelColor.b, pixelColor.g, pixelColor.r));
            return true;
        }

        if (channelCount == 4)
        {
            imageImpl->imageMat.setTo(cv::Scalar(pixelColor.b, pixelColor.g, pixelColor.r, pixelColor.a));
            return true;
        }
    }
    catch (...)
    {
        return false;
    }

    return false;
}

GB_Image GB_Image::Clone() const
{
    return GB_Image(*this, GB_ImageCopyMode::DeepCopy);
}

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

GB_Image GB_Image::Resize(size_t newRows, size_t newCols, GB_ImageInterpolation interpolation) const
{
    GB_Image resultImage;
    if (IsEmpty() || newRows == 0 || newCols == 0)
    {
        return resultImage;
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
    }
    catch (...)
    {
        resultImage.Clear();
    }

    return resultImage;
}

bool GB_Image::ResizeInPlace(size_t newRows, size_t newCols, GB_ImageInterpolation interpolation)
{
    if (IsEmpty())
    {
        return false;
    }

    GB_Image resizedImage = Resize(newRows, newCols, interpolation);
    if (resizedImage.IsEmpty())
    {
        return false;
    }

    Swap(resizedImage);
    return true;
}

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
    }
    catch (...)
    {
        resultImage.Clear();
    }

    return resultImage;
}

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

    try
    {
        cv::cvtColor(imageImpl->imageMat, resultImage.imageImpl->imageMat, cvColorCode);
    }
    catch (...)
    {
        resultImage.Clear();
    }

    return resultImage;
}

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

void swap(GB_Image& leftImage, GB_Image& rightImage) noexcept
{
    leftImage.Swap(rightImage);
}
