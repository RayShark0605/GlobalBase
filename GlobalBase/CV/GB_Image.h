#ifndef GLOBALBASE_IMAGE_H_H
#define GLOBALBASE_IMAGE_H_H

#include "../GlobalBasePort.h"
#include "../GB_BaseTypes.h"
#include "../Geometry/GB_Point2d.h"
#include "../Geometry/GB_Polygon.h"
#include "../Geometry/GB_Rectangle.h"
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
 * @brief 图像旋转后的空白区域填充值模式。
 */
enum class GB_ImageRotateBackgroundMode
{
    /**
     * @brief 使用黑色背景。
     */
    Black = 0,

    /**
     * @brief 使用白色背景。
     */
    White,

    /**
     * @brief 使用全透明背景。
     *
     * 当源图像不带 Alpha 且当前旋转需要透明背景时，结果图像会先提升为 4 通道，
     * 以保证旋转后新增的空白区域能够保持真实透明，而不是退化为数值为 0 的黑色背景。
     */
    Transparent,

    /**
     * @brief 使用 backgroundColor 指定的自定义背景色。
     */
    Custom
};

/**
 * @brief 图像旋转选项。
 */
struct GB_ImageRotateOptions
{
    /**
     * @brief 是否自动扩展输出尺寸以完整容纳旋转后的图像。
     *
     * - false：输出尺寸保持与原图一致，旋转后可能发生裁剪；
     * - true：输出尺寸会自动扩大到足以容纳旋转后的完整内容。
     */
    bool expandOutput = true;

    /**
     * @brief 旋转采样时使用的插值方式。
     */
    GB_ImageInterpolation interpolation = GB_ImageInterpolation::Linear;

    /**
     * @brief 空白区域的填充值模式。
     */
    GB_ImageRotateBackgroundMode backgroundMode = GB_ImageRotateBackgroundMode::Transparent;

    /**
     * @brief 自定义空白填充值。
     *
     * 仅当 backgroundMode 为 Custom 时使用。
     */
    GB_ColorRGBA backgroundColor = GB_ColorRGBA::Black;
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
 * @brief 图像绘制对象超出当前图像范围时的处理策略。
 */
enum class GB_ImageDrawOutOfBoundsPolicy
{
    /**
     * @brief 保持当前图像尺寸不变，只绘制落在当前图像范围内的部分。
     */
    ClipToImage = 0,

    /**
     * @brief 自动外扩当前图像，使原图像与待绘制对象都能被完整容纳。
     *
     * 当外扩发生在左侧或上侧时，原图像内容会整体平移到新画布中的对应位置，
     * 待绘制对象也会使用相同平移量，以保持二者在同一像素坐标系下的相对位置不变。
     */
    ExpandImage
};

/**
 * @brief 图像多边形绘制参数。
 */
struct GB_ImageDrawPolygonOptions
{
    /** @brief 多边形边界颜色，按 source-over 规则与目标图像 Alpha 混合。 */
    GB_ColorRGBA boundaryColor = GB_ColorRGBA::Red;

    /** @brief 多边形边界线宽，单位为图像像素。小于等于 0 表示不绘制边界。 */
    int boundaryThickness = 2;

    /** @brief 是否填充多边形内部。 */
    bool fill = false;

    /** @brief 多边形填充颜色，按 source-over 规则与目标图像 Alpha 混合。 */
    GB_ColorRGBA fillColor = GB_ColorRGBA(255, 0, 0, 64);

    /** @brief 是否启用抗锯齿。 */
    bool antialias = true;

    /** @brief 多边形超出当前图像范围时的处理策略。 */
    GB_ImageDrawOutOfBoundsPolicy outOfBoundsPolicy = GB_ImageDrawOutOfBoundsPolicy::ClipToImage;

    /**
     * @brief 自动外扩图像时，新扩展区域的背景色。
     *
     * 仅当 outOfBoundsPolicy 为 ExpandImage 且实际发生外扩时使用。
     */
    GB_ColorRGBA expandBackgroundColor = GB_ColorRGBA::Transparent;
};

/**
 * @brief 图像叠加绘制参数。
 */
struct GB_ImageDrawImageOptions
{
    /**
     * @brief 源图像绘制到当前图像像素坐标系中的目标区域。
     *
     * minX / maxX 对应列方向边界，minY / maxY 对应行方向边界。
     */
    GB_Rectangle imageRectangle;

    /** @brief 源图像缩放到目标区域时使用的插值方式。 */
    GB_ImageInterpolation interpolation = GB_ImageInterpolation::Linear;

    /** @brief 绘制区域超出当前图像范围时的处理策略。 */
    GB_ImageDrawOutOfBoundsPolicy outOfBoundsPolicy = GB_ImageDrawOutOfBoundsPolicy::ClipToImage;

    /**
     * @brief 自动外扩图像时，新扩展区域的背景色。
     *
     * 仅当 outOfBoundsPolicy 为 ExpandImage 且实际发生外扩时使用。
     */
    GB_ColorRGBA expandBackgroundColor = GB_ColorRGBA::Transparent;
};

/**
 * @brief 图像模板查找算法。
 */
enum class GB_ImageTemplateFindAlgorithm
{
    /**
     * @brief 自动策略：优先模板匹配，失败后依次尝试 ORB、SIFT。
     */
    Auto = 0,

    /**
     * @brief 模板匹配，适合模板与目标尺寸、角度、视角基本一致的场景。
     */
    TemplateMatching,

    /**
     * @brief ORB 特征匹配，速度较快，适合存在一定旋转、尺度变化的纹理目标。
     */
    ORB,

    /**
     * @brief SIFT 特征匹配，通常更稳定但计算成本更高。
     */
    SIFT
};

/**
 * @brief OpenCV 模板匹配方法。
 */
enum class GB_ImageTemplateMatchMethod
{
    SqDiff = 0,
    SqDiffNormed,
    CCorr,
    CCorrNormed,
    CCoeff,
    CCoeffNormed
};

/**
 * @brief 图像模板查找选项。
 */
struct GB_ImageTemplateFindOptions
{
    /**
     * @brief 查找算法。
     */
    GB_ImageTemplateFindAlgorithm algorithm = GB_ImageTemplateFindAlgorithm::Auto;

    /**
     * @brief 是否优先尝试 CUDA 加速。
     *
     * 仅在当前 OpenCV 构建包含 CUDA 模板匹配模块且运行时存在可用 CUDA 设备时生效；
     * 否则会自动回退到 CPU 路径。
     */
    bool preferCuda = true;

    /**
     * @brief 是否转换为灰度图后再执行匹配。
     *
     * 对模板匹配通常能减少计算量并降低通道顺序差异带来的干扰；
     * ORB / SIFT 特征匹配始终使用灰度图。
     */
    bool convertToGray = true;

    /**
     * @brief 模板匹配方法。
     */
    GB_ImageTemplateMatchMethod templateMatchMethod = GB_ImageTemplateMatchMethod::CCoeffNormed;

    /**
     * @brief 模板匹配最低得分阈值。
     *
     * 对归一化模板匹配方法，该值通常取 [0, 1]；默认 0.85 表示较严格匹配。
     */
    double minTemplateMatchScore = 0.85;

    /**
     * @brief 是否对模板图像启用多尺度金字塔搜索。
     *
     * 当模板与大图中的目标存在尺度差异时可开启；默认关闭以避免额外计算量。
     */
    bool useTemplateScalePyramid = false;

    /**
     * @brief 多尺度模板搜索的最小模板缩放比例。
     */
    double minTemplateScale = 0.75;

    /**
     * @brief 多尺度模板搜索的最大模板缩放比例。
     */
    double maxTemplateScale = 1.25;

    /**
     * @brief 多尺度模板搜索的缩放步长。
     */
    double templateScaleStep = 0.05;

    /**
     * @brief 多尺度模板搜索最多尝试的缩放数量。
     */
    int maxTemplateScaleCount = 32;

    /**
     * @brief 模板缩放时使用的插值方式。
     */
    GB_ImageInterpolation templateScaleInterpolation = GB_ImageInterpolation::Linear;

    /**
     * @brief ORB 最大特征点数量。
     */
    int orbMaxFeatures = 2000;

    /**
     * @brief ORB 图像金字塔缩放因子。
     */
    double orbScaleFactor = 1.2;

    /**
     * @brief ORB 图像金字塔层数。
     */
    int orbNumLevels = 8;

    /**
     * @brief ORB 边缘阈值。
     */
    int orbEdgeThreshold = 31;

    /**
     * @brief ORB 描述子 patch 尺寸。
     */
    int orbPatchSize = 31;

    /**
     * @brief ORB FAST 角点阈值。
     */
    int orbFastThreshold = 20;

    /**
     * @brief SIFT 保留的最佳特征点数量，0 表示不限制。
     */
    int siftNumFeatures = 0;

    /**
     * @brief SIFT 每个 octave 的层数。
     */
    int siftNumOctaveLayers = 3;

    /**
     * @brief SIFT 对低对比度特征的过滤阈值。
     */
    double siftContrastThreshold = 0.04;

    /**
     * @brief SIFT 对边缘型特征的过滤阈值。
     */
    double siftEdgeThreshold = 10.0;

    /**
     * @brief SIFT 初始高斯模糊参数。
     */
    double siftSigma = 1.6;

    /**
     * @brief KNN 匹配后的 Lowe ratio test 阈值。
     */
    double featureRatioTest = 0.75;

    /**
     * @brief 特征匹配至少需要保留的优质匹配数量。
     */
    int minGoodMatches = 8;

    /**
     * @brief 是否使用 RANSAC 估计单应矩阵并剔除错误匹配。
     */
    bool useRansac = true;

    /**
     * @brief RANSAC 重投影误差阈值，单位为像素。
     */
    double ransacReprojThreshold = 3.0;

    /**
     * @brief 特征匹配至少需要保留的内点数量。
     */
    int minInlierMatches = 6;

    /**
     * @brief 特征匹配最低内点比例。
     */
    double minInlierRatio = 0.35;

    /**
     * @brief 单应矩阵估计前最多使用的优质匹配数量。
     */
    int maxFeatureMatches = 500;

    /**
     * @brief 是否检查结果区域大致落在大图范围内。
     */
    bool checkResultInsideSourceImage = true;

    /**
     * @brief 允许结果区域略微超出大图范围的像素容差。
     */
    double outsideTolerance = 3.0;
};

/**
 * @brief 图像模板查找结果。
 */
struct GB_ImageTemplateFindResult
{
    /**
     * @brief 是否成功找到模板区域。
     */
    bool found = false;

    /**
     * @brief 实际产生结果的算法。
     */
    GB_ImageTemplateFindAlgorithm algorithm = GB_ImageTemplateFindAlgorithm::Auto;

    /**
     * @brief 是否实际使用了 CUDA 路径。
     */
    bool usedCuda = false;

    /**
     * @brief 匹配得分。
     *
     * 模板匹配归一化方法通常位于 [0, 1]，越大越可靠；
     * 特征匹配中该值为内点比例。
     */
    double score = 0.0;

    /**
     * @brief 底层算法的原始得分。
     */
    double rawScore = 0.0;

    /**
     * @brief 模板匹配使用的模板缩放比例。
     */
    double templateScale = 1.0;

    /**
     * @brief 小图像在大图像中的区域。
     */
    GB_Polygon polygon;

    /**
     * @brief 小图像在大图像中的区域中心点。
     */
    GB_Point2d centerPoint;

    /**
     * @brief 结果区域的轴对齐包围盒。
     *
     * 图像坐标语义：minX / maxX 对应列方向边界，minY / maxY 对应行方向边界。
     */
    GB_Rectangle boundingBox;

    /**
     * @brief 模板图像特征点数量，仅特征匹配算法填写。
     */
    size_t templateKeyPointCount = 0;

    /**
     * @brief 大图像特征点数量，仅特征匹配算法填写。
     */
    size_t sourceKeyPointCount = 0;

    /**
     * @brief Lowe ratio test 后保留的优质匹配数量，仅特征匹配算法填写。
     */
    size_t goodMatchCount = 0;

    /**
     * @brief RANSAC 过滤后的内点数量，仅特征匹配算法填写。
     */
    size_t inlierMatchCount = 0;

    /**
     * @brief 失败原因或补充说明。
     */
    std::string message = "";
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
     * @brief 在当前图像上绘制一个多边形。
     *
     * @param polygon 顶点坐标位于当前图像像素坐标系中，x 对应列方向，y 对应行方向。
     * @param drawOptions 绘制参数。
     * @return true=绘制成功；false=输入非法、图像格式不支持或没有可绘制内容。
     *
     * 说明：
     * - 当前实现仅对 8 位、1 / 3 / 4 通道图像提供稳定支持；
     * - 颜色会按当前图像的真实通道顺序写入，并按 source-over 规则处理 Alpha；
     * - 当 outOfBoundsPolicy 为 ClipToImage 时，当前图像尺寸保持不变，超出部分被裁剪；
     * - 当 outOfBoundsPolicy 为 ExpandImage 时，当前图像会在需要时外扩到能同时容纳原图像与多边形绘制范围。
     */
    bool DrawPolygon(const GB_Polygon& polygon, const GB_ImageDrawPolygonOptions& drawOptions = GB_ImageDrawPolygonOptions());

    /**
     * @brief 在当前图像上绘制一个多边形。
     */
    bool DrawPolygon(const GB_Polygon& polygon, const GB_ColorRGBA& boundaryColor, int boundaryThickness, bool fill, const GB_ColorRGBA& fillColor);

    /**
     * @brief 在当前图像上叠加绘制另一幅图像。
     *
     * @param image 要绘制的源图像。
     * @param drawOptions 绘制参数。
     * @return true=绘制成功；false=输入非法、图像格式不支持或没有可绘制内容。
     *
     * 说明：
     * - 当前目标图像仅对 8 位、1 / 3 / 4 通道提供稳定支持；
     * - 源图像会按逻辑 RGBA 读取，并按 source-over 规则叠加到当前图像；
     * - imageRectangle 可指定负坐标或超出当前图像大小的区域，具体处理由 outOfBoundsPolicy 决定。
     */
    bool DrawImage(const GB_Image& image, const GB_ImageDrawImageOptions& drawOptions);

    /**
     * @brief 在当前图像上叠加绘制另一幅图像。
     */
    bool DrawImage(const GB_Image& image, const GB_Rectangle& imageRectangle);


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
     * @brief 旋转图像。
     *
     * @param angleDegrees 旋转角度，单位为度。正值表示逆时针旋转，负值表示顺时针旋转。
     * @param rotateOptions 旋转选项。
     *
     * 说明：
     * - 当 expandOutput 为 false 时，输出尺寸与原图一致，旋转后可能产生裁剪；
     * - 当 expandOutput 为 true 时，会自动调整输出尺寸与平移量，尽量完整保留旋转结果；
     * - 当 backgroundMode 为 Transparent 且源图像本身不带 Alpha 时，结果图像会自动提升为 4 通道，
     *   以保证旋转后新增的空白区域保持透明；
     * - 对 1 / 2 / 3 / 4 通道图像可稳定指定背景值；对于超过 4 通道的图像，当前仅稳定支持零背景。
     */
    GB_Image Rotate(double angleDegrees, const GB_ImageRotateOptions& rotateOptions = GB_ImageRotateOptions()) const;

    /**
     * @brief 原地旋转图像。
     */
    bool RotateInPlace(double angleDegrees, const GB_ImageRotateOptions& rotateOptions = GB_ImageRotateOptions());

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

    /**
     * @brief 转换为 8 位直 Alpha BGRA 图像。
     *
     * 说明：
     * - 支持当前模块可识别的 Gray、Gray+Alpha、BGR、BGRA、RGB、RGBA 图像；
     * - 非 8 位输入会沿用 ConvertTo(UInt8) 的数值转换语义；
     * - 已经是 8 位 BGRA 的连续或非连续图像仍返回独立的 GB_Image 值对象，底层像素可按引用计数共享；
     * - 转换失败或源图像为空时返回空图像。
     */
    GB_Image ConvertToBgra8() const;

    /**
     * @brief 在当前图像中查找模板图像。
     *
     * @param templateImage 要查找的小图像。
     * @param findOptions 查找选项。
     * @return 查找结果。失败时 found 为 false。
     */
    GB_ImageTemplateFindResult FindTemplate(const GB_Image& templateImage, const GB_ImageTemplateFindOptions& findOptions = GB_ImageTemplateFindOptions()) const;

    /**
     * @brief 在 sourceImage 中查找 templateImage。
     *
     * @param sourceImage 大图像。
     * @param templateImage 要查找的小图像。
     * @param findOptions 查找选项。
     * @return 查找结果。失败时 found 为 false。
     */
    static GB_ImageTemplateFindResult FindTemplate(const GB_Image& sourceImage, const GB_Image& templateImage, const GB_ImageTemplateFindOptions& findOptions = GB_ImageTemplateFindOptions());

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
