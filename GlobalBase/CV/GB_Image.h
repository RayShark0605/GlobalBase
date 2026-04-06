#ifndef GLOBALBASE_IMAGE_H_H
#define GLOBALBASE_IMAGE_H_H

#include "../GlobalBasePort.h"
#include "../GB_BaseTypes.h"
#include "GB_ColorRGBA.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace cv
{
    class Mat;
}

enum class GB_ImageDepth
{
    Unknown = 0,
    UInt8,
    Int8,
    UInt16,
    Int16,
    Int32,
    Float32,
    Float64
};

enum class GB_ImageCopyMode
{
    ShallowCopy = 0,
    DeepCopy
};

enum class GB_ImageInterpolation
{
    Nearest = 0,
    Linear,
    Cubic,
    Area,
    Lanczos4
};

enum class GB_ImageColorMode
{
    Unchanged = 0,
    Gray,
    BGR,
    BGRA
};

enum class GB_ImageColorConversion
{
    GrayToBgr = 0,
    GrayToBgra,
    BgrToGray,
    BgrToBgra,
    BgraToGray,
    BgraToBgr,
    BgrToRgb,
    RgbToBgr,
    BgraToRgba,
    RgbaToBgra,
    GrayToRgb,
    GrayToRgba,
    RgbToGray,
    RgbaToGray,
    RgbToBgra,
    RgbaToBgr
};

struct GB_ImageLoadOptions
{
    // 解码后的目标颜色模式。
    // - Unchanged：尽量保持源图像原始通道数/位深。
    // - Gray / BGR：按目标通道数读取；若 preserveBitDepth 为 true，则尽量保留 16bit/32bit 深度。
    // - BGRA：先按 Unchanged 解码，再在内存中转换为 BGRA。
    GB_ImageColorMode colorMode = GB_ImageColorMode::Unchanged;

    // 是否尽量保留源图像位深。
    // 说明：当 colorMode 为 Unchanged 或 BGRA 时，内部会优先保留源位深。
    bool preserveBitDepth = true;

    // 是否忽略 EXIF 方向。
    // 仅当 OpenCV 的相应解码路径支持该标志时生效。
    bool ignoreExifOrientation = false;

    // 多页图像/动画图像时读取的页索引，0 表示第一页。
    size_t pageIndex = 0;
};

struct GB_ImageSaveOptions
{
    // 目标文件已存在时是否允许覆盖。
    bool overwrite = true;

    // JPEG 质量：[0, 100]，越大通常质量越高、文件也越大。
    int jpegQuality = 95;

    // PNG 压缩等级：[0, 9]，越大压缩越强，通常编码更慢。
    int pngCompression = 3;

    // WebP 质量：[1, 100]。
    int webpQuality = 95;
};

/**
 * @brief 基于 OpenCV 的内存图像对象。
 *
 * 设计目标：
 * - 只描述“已经完整在内存中的图像”；
 * - 头文件不 include 任何第三方库；
 * - 默认拷贝语义为浅拷贝（与 cv::Mat 一致，O(1) 共享底层像素缓冲区）；
 * - 通过 Clone()/Detach()/显式 DeepCopy 构造实现深拷贝；
 * - 像素坐标系与 OpenCV 保持一致，统一使用 (row, col)。
 *
 * 说明：
 * - 对 3 通道/4 通道图像，本类默认遵循 OpenCV 的常见存储顺序：BGR / BGRA。
 * - 与 GB_ColorRGBA 的交互接口，会自动在“逻辑 RGBA”与“底层 BGR/BGRA”之间转换。
 */
class GLOBALBASE_PORT GB_Image
{
public:
    GB_Image();

    explicit GB_Image(const std::string& filePathUtf8, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());
    explicit GB_Image(const GB_ByteBuffer& encodedBytes, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());
    GB_Image(const void* encodedData, size_t encodedSize, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());

    // 创建指定尺寸/位深/通道数的图像。
    // zeroInitialize=true 时会清零（例如 8UC3 时得到全黑图像）。
    GB_Image(size_t rows, size_t cols, GB_ImageDepth depth, int channels, bool zeroInitialize = true);

    // 默认拷贝：浅拷贝（O(1)）
    GB_Image(const GB_Image& other);

    // 可显式指定浅拷贝/深拷贝。
    GB_Image(const GB_Image& other, GB_ImageCopyMode copyMode);

    GB_Image(GB_Image&& other) noexcept;

    // 允许与 cv::Mat 互转，但头文件不 include OpenCV；
    // 使用这些接口的调用方，在自己的 .cpp 中自行 include 对应 OpenCV 头文件即可。
    explicit GB_Image(const cv::Mat& imageMat, GB_ImageCopyMode copyMode = GB_ImageCopyMode::ShallowCopy);

    ~GB_Image();

    GB_Image& operator=(const GB_Image& other);
    GB_Image& operator=(GB_Image&& other) noexcept;

    void Swap(GB_Image& other) noexcept;
    void Clear();

    bool Create(size_t rows, size_t cols, GB_ImageDepth depth, int channels, bool zeroInitialize = true);

    bool LoadFromFile(const std::string& filePathUtf8, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());
    bool LoadFromMemory(const GB_ByteBuffer& encodedBytes, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());
    bool LoadFromMemory(const void* encodedData, size_t encodedSize, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());

    bool SaveToFile(const std::string& filePathUtf8, const GB_ImageSaveOptions& saveOptions = GB_ImageSaveOptions()) const;

    // fileExt 可传入 ".png" / ".jpg" / "png" / "jpg" 等。
    bool EncodeToMemory(GB_ByteBuffer& encodedBytes, const std::string& fileExt, const GB_ImageSaveOptions& saveOptions = GB_ImageSaveOptions()) const;

    bool SetFromCvMat(const cv::Mat& imageMat, GB_ImageCopyMode copyMode = GB_ImageCopyMode::ShallowCopy);
    cv::Mat ToCvMat(GB_ImageCopyMode copyMode = GB_ImageCopyMode::ShallowCopy) const;

    bool IsEmpty() const;
    size_t GetWidth() const;
    size_t GetHeight() const;
    size_t GetRows() const;
    size_t GetCols() const;
    int GetChannels() const;
    GB_ImageDepth GetDepth() const;
    size_t GetBitDepth() const;
    size_t GetBytesPerChannel() const;
    size_t GetBytesPerPixel() const;
    size_t GetRowStrideBytes() const;

    // 返回当前“逻辑图像区域”的像素数据总字节数：rows * cols * elemSize。
    // 若当前对象只是某个更大图像的 ROI/浅视图，则该值不一定等于底层共享缓冲区的真实分配大小。
    size_t GetTotalByteSize() const;

    bool IsContinuous() const;
    bool IsValidPixelCoordinate(size_t row, size_t col) const;

    unsigned char* GetData();
    const unsigned char* GetData() const;
    unsigned char* GetRowData(size_t row);
    const unsigned char* GetRowData(size_t row) const;

    // 仅对 8 位、1/3/4 通道图像提供稳定的 GB_ColorRGBA 像素读写。
    // - 1 通道：读取时扩展为灰度 RGBA（A=255）；写入时按灰度写入。
    // - 3 通道：按 BGR <-> RGBA 转换；写入时忽略 alpha。
    // - 4 通道：按 BGRA <-> RGBA 转换。
    bool GetPixelColor(size_t row, size_t col, GB_ColorRGBA& pixelColor) const;
    bool SetPixelColor(size_t row, size_t col, const GB_ColorRGBA& pixelColor);
    bool Fill(const GB_ColorRGBA& pixelColor);

    // 返回深拷贝图像。
    GB_Image Clone() const;

    // 将当前对象“原地脱离共享”，确保之后与原共享对象不再共用像素缓冲区。
    bool Detach();

    GB_Image Resize(size_t newRows, size_t newCols, GB_ImageInterpolation interpolation = GB_ImageInterpolation::Linear) const;
    bool ResizeInPlace(size_t newRows, size_t newCols, GB_ImageInterpolation interpolation = GB_ImageInterpolation::Linear);

    // 注意参数顺序：统一使用 (row, col, rows, cols)。
    GB_Image Crop(size_t row, size_t col, size_t cropRows, size_t cropCols, GB_ImageCopyMode copyMode = GB_ImageCopyMode::DeepCopy) const;
    bool CropInPlace(size_t row, size_t col, size_t cropRows, size_t cropCols, GB_ImageCopyMode copyMode = GB_ImageCopyMode::DeepCopy);

    GB_Image ConvertColor(GB_ImageColorConversion conversion) const;
    bool ConvertColorInPlace(GB_ImageColorConversion conversion);

private:
    bool EnsureImageImpl();

    class Impl;
    Impl* imageImpl = nullptr;
};

GLOBALBASE_PORT void swap(GB_Image& leftImage, GB_Image& rightImage) noexcept;

#endif
