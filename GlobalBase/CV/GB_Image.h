#ifndef GLOBALBASE_IMAGE_H_H
#define GLOBALBASE_IMAGE_H_H

#include "../GlobalBasePort.h"
#include "../GB_BaseTypes.h"
#include "GB_ColorRGBA.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cv
{
    class Mat;
}

/**
 * @brief 像素单通道的数据类型。
 */
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

/**
 * @brief 图像拷贝方式。
 */
enum class GB_ImageCopyMode
{
    ShallowCopy = 0,
    DeepCopy
};

/**
 * @brief 图像缩放插值方式。
 */
enum class GB_ImageInterpolation
{
    Nearest = 0,
    Linear,
    Cubic,
    Area,
    Lanczos4
};

/**
 * @brief 图像读取后的目标颜色模式。
 */
enum class GB_ImageColorMode
{
    Unchanged = 0,
    Gray,
    BGR,
    BGRA
};

/**
 * @brief 颜色空间或通道顺序转换类型。
 */
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

/**
 * @brief 图像读取选项。
 */
struct GB_ImageLoadOptions
{
    /**
     * @brief 解码后的目标颜色模式。
     *
     * - Unchanged：尽量保持源图像原始通道数与位深。
     * - Gray / BGR：按目标通道数读取；当 preserveBitDepth 为 true 时，尽量保留源位深。
     * - BGRA：先按原始通道数读取，再在内存中转换为 BGRA。
     */
    GB_ImageColorMode colorMode = GB_ImageColorMode::Unchanged;

    /**
     * @brief 是否尽量保留源图像位深。
     *
     * 当 colorMode 为 Unchanged 或 BGRA 时，会优先保持原位深。
     */
    bool preserveBitDepth = true;

    /**
     * @brief 是否忽略图像方向元数据。
     *
     * 仅当底层解码路径支持该选项时生效。
     */
    bool ignoreExifOrientation = false;

    /**
     * @brief 多页图像或动画图像时要读取的页索引，0 表示第一页。
     */
    size_t pageIndex = 0;
};

/**
 * @brief 图像保存选项。
 */
struct GB_ImageSaveOptions
{
    /**
     * @brief 目标文件已存在时是否允许覆盖。
     */
    bool overwrite = true;

    /**
     * @brief JPEG 质量，范围 [0, 100]，越大通常质量越高、文件也越大。
     */
    int jpegQuality = 95;

    /**
     * @brief PNG 压缩等级，范围 [0, 9]，越大压缩越强，通常编码更慢。
     */
    int pngCompression = 3;

    /**
     * @brief WebP 质量，范围 [1, 100]。
     */
    int webpQuality = 95;
};

/**
 * @brief 已完整驻留在内存中的图像对象。
 *
 * 设计原则：
 * - 只描述“整幅图像都已在内存中”的场景；
 * - 头文件不引入第三方头文件；
 * - 默认拷贝为浅拷贝，多个对象可共享同一块像素缓冲区；
 * - 可通过 Clone()、Detach() 或显式 DeepCopy 获取独立副本；
 * - 统一使用 (row, col) 表示像素坐标；
 * - 对 3 通道 / 4 通道图像，常规顺序默认为 BGR / BGRA；
 * - 当执行颜色转换后，内部也可以处于 RGB / RGBA 顺序，对象会同步记录当前实际通道顺序。
 *
 * 约定：
 * - Create()、LoadFromFile()、LoadFromMemory()、SetFromCvMat() 这类会修改内容的接口，
 *   都采用“构造成功后再提交”的语义；若中途失败，当前对象保持原有内容不变；
 * - 与 GB_ColorRGBA 交互时，会根据当前实际通道顺序自动完成逻辑 RGBA 与底层像素排列之间的映射；
 * - 保存或编码时，若当前内部为 RGB / RGBA 顺序，会先转换成适合写出的通道顺序。
 */
class GLOBALBASE_PORT GB_Image
{
public:
    /**
     * @brief 构造空图像。
     */
    GB_Image();

    /**
     * @brief 从文件读取图像。
     *
     * 若读取失败，对象保持原有内容不变。
     */
    explicit GB_Image(const std::string& filePathUtf8, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());

    /**
     * @brief 从已编码的内存字节流读取图像。
     *
     * 若读取失败，对象保持原有内容不变。
     */
    explicit GB_Image(const GB_ByteBuffer& encodedBytes, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());

    /**
     * @brief 从已编码的内存块读取图像。
     *
     * 若读取失败，对象保持原有内容不变。
     */
    GB_Image(const void* encodedData, size_t encodedSize, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());

    /**
     * @brief 创建指定尺寸、位深、通道数的图像。
     *
     * @param rows 图像行数。
     * @param cols 图像列数。
     * @param depth 像素位深。
     * @param channels 通道数，必须大于 0，且处于底层支持范围内。
     * @param zeroInitialize 是否在创建后立即清零。
     */
    GB_Image(size_t rows, size_t cols, GB_ImageDepth depth, int channels, bool zeroInitialize = true);

    /**
     * @brief 默认拷贝：浅拷贝，O(1) 共享底层像素缓冲区。
     */
    GB_Image(const GB_Image& other);

    /**
     * @brief 可显式指定浅拷贝或深拷贝。
     */
    GB_Image(const GB_Image& other, GB_ImageCopyMode copyMode);

    GB_Image(GB_Image&& other) noexcept;

    /**
     * @brief 从外部矩阵对象构造图像。
     */
    explicit GB_Image(const cv::Mat& imageMat, GB_ImageCopyMode copyMode = GB_ImageCopyMode::ShallowCopy);

    ~GB_Image();

    GB_Image& operator=(const GB_Image& other);
    GB_Image& operator=(GB_Image&& other) noexcept;

    /**
     * @brief 交换两个图像对象的内部状态。
     */
    void Swap(GB_Image& other) noexcept;

    /**
     * @brief 清空当前图像内容。
     *
     * 清空后对象仍保持可继续复用。
     */
    void Clear();

    /**
     * @brief 创建指定尺寸、位深、通道数的图像。
     *
     * 创建失败时，对象保持原有内容不变。
     */
    bool Create(size_t rows, size_t cols, GB_ImageDepth depth, int channels, bool zeroInitialize = true);

    /**
     * @brief 从文件读取图像。
     *
     * 读取失败时，对象保持原有内容不变。
     */
    bool LoadFromFile(const std::string& filePathUtf8, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());

    /**
     * @brief 从已编码字节流读取图像。
     *
     * 读取失败时，对象保持原有内容不变。
     */
    bool LoadFromMemory(const GB_ByteBuffer& encodedBytes, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());

    /**
     * @brief 从已编码内存块读取图像。
     *
     * 读取失败时，对象保持原有内容不变。
     */
    bool LoadFromMemory(const void* encodedData, size_t encodedSize, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());

    /**
     * @brief 保存到文件。
     *
     * 保存失败时，不会对目标文件做额外保证；是否允许覆盖由 saveOptions.overwrite 控制。
     */
    bool SaveToFile(const std::string& filePathUtf8, const GB_ImageSaveOptions& saveOptions = GB_ImageSaveOptions()) const;

    /**
     * @brief 编码到内存。
     *
     * @param encodedBytes 输出的编码字节流。
     * @param fileExt 目标格式扩展名，可传入 ".png"、".jpg"、"png"、"jpg" 等。
     */
    bool EncodeToMemory(GB_ByteBuffer& encodedBytes, const std::string& fileExt, const GB_ImageSaveOptions& saveOptions = GB_ImageSaveOptions()) const;

    /**
     * @brief 从外部矩阵对象设置当前图像。
     *
     * 设置失败时，对象保持原有内容不变。
     */
    bool SetFromCvMat(const cv::Mat& imageMat, GB_ImageCopyMode copyMode = GB_ImageCopyMode::ShallowCopy);

    /**
     * @brief 导出为外部矩阵对象。
     *
     * DeepCopy 时返回独立副本；ShallowCopy 时返回共享底层缓冲区的视图。
     * 返回值的通道顺序与当前对象内部记录的实际顺序一致。
     */
    cv::Mat ToCvMat(GB_ImageCopyMode copyMode = GB_ImageCopyMode::ShallowCopy) const;

    /**
     * @brief 获取当前类的稳定类型字符串。
     */
    const std::string& GetClassType() const;

    /**
     * @brief 获取当前类的稳定类型 Id。
     */
    uint64_t GetClassTypeId() const;

    /**
     * @brief 导出为逻辑 RGBA 二维矩阵。
     *
     * 仅对 8 位、1 / 3 / 4 通道图像提供稳定支持。
     * 导出失败时，colorMatrix 会被清空。
     */
    bool ToColorMatrix(std::vector<std::vector<GB_ColorRGBA>>& colorMatrix) const;

    /**
     * @brief 根据逻辑 RGBA 二维矩阵设置当前图像。
     *
     * 输入矩阵必须为规则矩阵，且行列数都大于 0。
     * 设置成功后，内部会创建 8 位 4 通道图像，并按 BGRA 排列保存像素。
     * 设置失败时，对象保持原有内容不变。
     */
    bool SetFromColorMatrix(const std::vector<std::vector<GB_ColorRGBA>>& colorMatrix);

    /**
     * @brief 序列化为便于人类阅读的文本字符串。
     *
     * 当前文本格式逐像素输出逻辑 RGBA 颜色值，
     * 仅对 8 位、1 / 3 / 4 通道图像提供稳定支持。
     * 序列化失败时返回空字符串；空图像会输出 "(GB_Image empty)"。
     */
    std::string SerializeToString() const;

    /**
     * @brief 序列化为 GB_ByteBuffer。
     */
    GB_ByteBuffer SerializeToBinary() const;

    /**
     * @brief 从文本字符串反序列化。
     *
     * 当前仅支持由 SerializeToString() 生成的文本格式。
     * 反序列化成功后，内部会生成 8 位 4 通道 BGRA 图像。
     * 失败时，对象保持原有内容不变。
     */
    bool Deserialize(const std::string& data);

    /**
     * @brief 从 GB_ByteBuffer 反序列化。
     *
     * 失败时，对象保持原有内容不变。
     */
    bool Deserialize(const GB_ByteBuffer& data);

    /**
     * @brief 当前是否为空图像。
     */
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

    /**
     * @brief 返回当前逻辑图像区域的像素总字节数。
     *
     * 该值等于 rows * cols * elemSize。
     * 若当前对象只是某个更大图像的 ROI 或浅视图，则它不一定等于底层共享缓冲区的真实分配大小。
     */
    size_t GetTotalByteSize() const;

    /**
     * @brief 当前图像是否按单段连续内存存储。
     */
    bool IsContinuous() const;

    /**
     * @brief 判断给定像素坐标是否有效。
     */
    bool IsValidPixelCoordinate(size_t row, size_t col) const;

    /**
     * @brief 获取首像素地址。
     *
     * 对于非连续图像，只能保证返回首行首像素地址，不能据此假定整张图像可按一段连续内存处理。
     */
    unsigned char* GetData();

    /**
     * @brief 获取首像素只读地址。
     *
     * 对于非连续图像，只能保证返回首行首像素地址，不能据此假定整张图像可按一段连续内存处理。
     */
    const unsigned char* GetData() const;

    /**
     * @brief 获取指定行起始地址。
     */
    unsigned char* GetRowData(size_t row);

    /**
     * @brief 获取指定行起始地址（只读）。
     */
    const unsigned char* GetRowData(size_t row) const;

    /**
     * @brief 读取单个像素颜色。
     *
     * 仅对 8 位、1 / 3 / 4 通道图像提供稳定支持：
     * - 1 通道：读取时扩展为灰度 RGBA，A 固定为 255；
     * - 3 通道：根据当前实际通道顺序（BGR 或 RGB）转换为 RGBA；
     * - 4 通道：根据当前实际通道顺序（BGRA 或 RGBA）转换为 RGBA。
     */
    bool GetPixelColor(size_t row, size_t col, GB_ColorRGBA& pixelColor) const;

    /**
     * @brief 写入单个像素颜色。
     *
     * 仅对 8 位、1 / 3 / 4 通道图像提供稳定支持：
     * - 1 通道：按灰度写入；
     * - 3 通道：根据当前实际通道顺序（BGR 或 RGB）写入，忽略 alpha；
     * - 4 通道：根据当前实际通道顺序（BGRA 或 RGBA）写入。
     */
    bool SetPixelColor(size_t row, size_t col, const GB_ColorRGBA& pixelColor);

    /**
     * @brief 用指定颜色填充整幅图像。
     *
     * 当前实现仅对 8 位、1 / 3 / 4 通道图像提供稳定支持。
     * 对 3 / 4 通道图像，会按照当前实际通道顺序写入像素值。
     */
    bool Fill(const GB_ColorRGBA& pixelColor);

    /**
     * @brief 返回深拷贝图像。
     */
    GB_Image Clone() const;

    /**
     * @brief 让当前对象与共享源脱离，确保之后拥有独立的像素缓冲区。
     */
    bool Detach();

    /**
     * @brief 转换像素位深，并可附带线性变换。
     *
     * 结果图像的通道数与当前图像保持一致，通道排列信息也会被保留。
     * 计算公式与 OpenCV 的 convertTo 一致：dst = src * scale + shift。
     */
    GB_Image ConvertTo(GB_ImageDepth targetDepth, double scale = 1.0, double shift = 0.0) const;

    /**
     * @brief 原地转换像素位深，并可附带线性变换。
     */
    bool ConvertToInPlace(GB_ImageDepth targetDepth, double scale = 1.0, double shift = 0.0);

    /**
     * @brief 翻转图像。
     *
     * - 仅 horizontalFlip 为 true：左右翻转；
     * - 仅 verticalFlip 为 true：上下翻转；
     * - 两者都为 true：同时左右和上下翻转；
     * - 两者都为 false：返回深拷贝。
     */
    GB_Image Flip(bool horizontalFlip, bool verticalFlip) const;

    /**
     * @brief 原地翻转图像。
     */
    bool FlipInPlace(bool horizontalFlip, bool verticalFlip);

    /**
     * @brief 生成缩放后的新图像。
     */
    GB_Image Resize(size_t newRows, size_t newCols, GB_ImageInterpolation interpolation = GB_ImageInterpolation::Linear) const;

    /**
     * @brief 原地缩放图像。
     */
    bool ResizeInPlace(size_t newRows, size_t newCols, GB_ImageInterpolation interpolation = GB_ImageInterpolation::Linear);

    /**
     * @brief 裁剪子图。
     *
     * 参数顺序统一为 (row, col, rows, cols)。
     */
    GB_Image Crop(size_t row, size_t col, size_t cropRows, size_t cropCols, GB_ImageCopyMode copyMode = GB_ImageCopyMode::DeepCopy) const;

    /**
     * @brief 原地裁剪子图。
     *
     * 参数顺序统一为 (row, col, rows, cols)。
     */
    bool CropInPlace(size_t row, size_t col, size_t cropRows, size_t cropCols, GB_ImageCopyMode copyMode = GB_ImageCopyMode::DeepCopy);

    /**
     * @brief 返回颜色空间转换后的新图像。
     *
     * 转换结果会保留其真实通道顺序信息。
     */
    GB_Image ConvertColor(GB_ImageColorConversion conversion) const;

    /**
     * @brief 原地执行颜色空间转换。
     *
     * 转换后会同步更新当前对象记录的通道顺序信息。
     */
    bool ConvertColorInPlace(GB_ImageColorConversion conversion);

private:
    /**
     * @brief 确保内部实现对象已创建。
     */
    bool EnsureImageImpl();

    class Impl;
    Impl* imageImpl = nullptr;
};

GLOBALBASE_PORT void swap(GB_Image& leftImage, GB_Image& rightImage) noexcept;

#endif
