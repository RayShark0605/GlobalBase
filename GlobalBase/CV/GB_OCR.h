#ifndef GLOBALBASE_OCR_H_H
#define GLOBALBASE_OCR_H_H

#include "../GlobalBasePort.h"
#include "../GB_Network.h"
#include "../Geometry/GB_Point2d.h"
#include "../Geometry/GB_Rectangle.h"
#include "GB_Image.h"

#include <memory>
#include <string>
#include <vector>

/**
 * @brief OCR 后端类型。
 *
 * 当前已实现 PP-OCRv5_mobile_det + PP-OCRv5_mobile_rec + ONNX Runtime CPU/CUDA，以及百度 OCR API。
 * Auto 会优先尝试 CUDA 后端，CUDA 不可用时自动退回 CPU 后端。
 */
enum class GB_OCRBackend
{
	/**
	 * @brief 自动选择当前环境中可用的后端。
	 */
	Auto = 0,

	/**
	 * @brief 使用 PP-OCRv5 mobile ONNX 模型，并通过 ONNX Runtime CPU 执行推理。
	 *
	 * 该后端只使用 ONNX Runtime 默认 CPU Execution Provider，不追加 CUDA/TensorRT EP，
	 * 也不会主动加载、探测或预热 CUDA/cuDNN/TensorRT 相关 DLL。
	 */
	PPOCRv5MobileOnnxRuntimeCpu,

	/**
	 * @brief 使用 PP-OCRv5 mobile ONNX 模型，并通过 ONNX Runtime CUDA 执行推理。
	 *
	 * 需要使用带 CUDA Execution Provider 的 ONNX Runtime GPU 包。
	 * Windows 下会优先尝试从 $(exe所在目录)/GlobalBaseDependencies 预加载 CUDA/cuDNN 相关运行时 DLL。
	 */
	PPOCRv5MobileOnnxRuntimeCuda,

	/**
	 * @brief 调用百度 OCR API。
	 */
	BaiduApi
};

/**
 * @brief 百度 OCR API 类型。
 */
enum class GB_BaiduOCRApiType
{
	/**
	 * @brief 通用文字识别（标准版），不返回文字位置信息。
	 */
	GeneralBasic = 0,

	/**
	 * @brief 通用文字识别（标准含位置版）。
	 */
	GeneralWithLocation,

	/**
	 * @brief 通用文字识别（高精度版），不返回文字位置信息。
	 */
	AccurateBasic,

	/**
	 * @brief 通用文字识别（高精度含位置版）。
	 */
	AccurateWithLocation
};

/**
 * @brief 百度 OCR API 选项。
 */
struct GB_BaiduOCROptions
{
	/**
	 * @brief 百度 OCR API 类型。
	 *
	 * 默认选择标准含位置版，以便尽量填充 GB_OCRTextBlock 的位置字段。
	 */
	GB_BaiduOCRApiType apiType = GB_BaiduOCRApiType::GeneralWithLocation;

	/**
	 * @brief 百度智能云应用的 API Key，UTF-8 编码。
	 *
	 * accessTokenUtf8 为空时，会使用 apiKeyUtf8 + secretKeyUtf8 自动获取 access_token。
	 */
	std::string apiKeyUtf8 = "";

	/**
	 * @brief 百度智能云应用的 Secret Key，UTF-8 编码。
	 */
	std::string secretKeyUtf8 = "";

	/**
	 * @brief 已经获取好的百度 access_token，UTF-8 编码。
	 *
	 * 非空时优先使用该 token，不再通过 apiKeyUtf8 + secretKeyUtf8 换取 token。
	 */
	std::string accessTokenUtf8 = "";

	/**
	 * @brief 识别语言类型。
	 *
	 * 常用值为 CHN_ENG。为空时不向百度 API 传递 language_type，让服务端使用默认值。
	 */
	std::string languageTypeUtf8 = "CHN_ENG";

	/**
	 * @brief 上传给百度 API 前的图像编码格式。
	 *
	 * 支持常见 OpenCV imencode 扩展名，例如 ".png"、".jpg"、".jpeg"、".bmp"。
	 */
	std::string imageFileExtUtf8 = ".png";

	/**
	 * @brief 图像编码选项。
	 */
	GB_ImageSaveOptions imageSaveOptions;

	/**
	 * @brief 是否检测文字朝向。
	 */
	bool detectDirection = false;

	/**
	 * @brief 是否检测语言。
	 */
	bool detectLanguage = false;

	/**
	 * @brief 是否输出段落信息。
	 */
	bool paragraph = false;

	/**
	 * @brief 是否请求百度 API 返回置信度信息。
	 */
	bool probability = true;

	/**
	 * @brief 含位置版接口是否按小粒度识别。
	 *
	 * true 时传递 recognize_granularity=small；false 时不传递该参数。
	 */
	bool recognizeGranularitySmall = false;

	/**
	 * @brief 含位置版接口是否请求四顶点位置信息。
	 */
	bool vertexesLocation = false;

	/**
	 * @brief 高精度接口是否开启行级别多方向文字识别。
	 */
	bool multidirectionalRecognize = false;

	/**
	 * @brief 百度 API 失败时，是否自动退回 PP-OCRv5 mobile Auto 后端。
	 *
	 * 该回退会沿用当前 PP-OCRv5 选项，并按 Auto 机制优先尝试 CUDA，失败时退回 CPU。
	 */
	bool fallbackToPPOCRv5MobileOnFailure = true;

	/**
	 * @brief access_token 过期前提前刷新的秒数。
	 */
	unsigned int accessTokenRefreshAdvanceSeconds = 300;

	/**
	 * @brief 百度 API 网络请求选项。
	 */
	GB_NetworkRequestOptions networkRequestOptions;
};

/**
 * @brief DB 文本检测框置信度计算方式。
 */
enum class GB_OCRTextDetectionScoreMode
{
	/**
	 * @brief 使用文本框轴对齐外包区域内的概率均值，速度较快，适合常规场景。
	 */
	Fast = 0,

	/**
	 * @brief 使用原始轮廓区域内的概率均值，过滤低质量框更精确，但会略微增加后处理耗时。
	 */
	Slow
};

/**
 * @brief ONNX Runtime 日志严重程度。
 *
 * ONNX Runtime 在 CUDA 后端中可能会把少量形状推导类节点放到 CPU 上执行，
 * 并在 Warning 级别输出提示。OCR 正常使用时通常不需要显示这类内部调度提示，
 * 因此 GB_OCROptions 默认使用 Error 级别；调试模型节点分配时可改为 Verbose 或 Info。
 */
enum class GB_OCROnnxRuntimeLogSeverityLevel
{
	/**
	 * @brief 输出详细诊断日志。
	 */
	Verbose = 0,

	/**
	 * @brief 输出信息、警告和错误日志。
	 */
	Info = 1,

	/**
	 * @brief 输出警告和错误日志。
	 */
	Warning = 2,

	/**
	 * @brief 仅输出错误和致命错误日志。
	 */
	Error = 3,

	/**
	 * @brief 仅输出致命错误日志。
	 */
	Fatal = 4
};

/**
 * @brief OCR 文字区域。
 *
 * polygonPoints 使用图像像素坐标，通常为顺时针四点文本框；boundingRectangle 为其轴对齐外包矩形。
 */
struct GB_OCRTextBlock
{
	/**
	 * @brief 识别出的 UTF-8 文本。
	 */
	std::string text = "";

	/**
	 * @brief 文本区域多边形顶点，坐标单位为输入图像像素。
	 */
	std::vector<GB_Point2d> polygonPoints;

	/**
	 * @brief 文本区域轴对齐外包矩形，坐标单位为输入图像像素。
	 */
	GB_Rectangle boundingRectangle;

	/**
	 * @brief 最终置信度，通常为 detectionConfidence 与 recognitionConfidence 的乘积。
	 */
	double confidence = 0;

	/**
	 * @brief 文本检测置信度。
	 */
	double detectionConfidence = 0;

	/**
	 * @brief 文本识别置信度。
	 */
	double recognitionConfidence = 0;
};

/**
 * @brief OCR 执行结果。
 */
struct GB_OCRResult
{
	/**
	 * @brief OCR 是否执行成功。
	 *
	 * success 为 true 时，textBlocks 也可能为空，表示正常执行但未检测到文字。
	 */
	bool success = false;

	/**
	 * @brief 实际使用的 OCR 后端。
	 */
	GB_OCRBackend backend = GB_OCRBackend::Auto;

	/**
	 * @brief 识别出的文字块列表。
	 */
	std::vector<GB_OCRTextBlock> textBlocks;

	/**
	 * @brief 错误信息，成功时通常为空。
	 */
	std::string errorMessage = "";
};

/**
 * @brief PP-OCRv5 mobile 模型路径配置。
 */
struct GB_PPOCRv5MobileModelPaths
{
	/**
	 * @brief 文本检测 ONNX 模型路径，UTF-8 编码。
	 */
	std::string detModelPathUtf8 = "";

	/**
	 * @brief 文本识别 ONNX 模型路径，UTF-8 编码。
	 */
	std::string recModelPathUtf8 = "";

	/**
	 * @brief 文字方向分类 ONNX 模型路径，UTF-8 编码。
	 *
	 * 该模型为可选模型，通常使用 PP-LCNet_x0_25_textline_ori 或 PP-LCNet_x1_0_textline_ori，
	 * 用于判断裁剪后的文字行是否需要旋转 180° 后再识别。
	 */
	std::string clsModelPathUtf8 = "";

	/**
	 * @brief 文本识别字典路径，UTF-8 编码。
	 */
	std::string dictPathUtf8 = "";
};

/**
 * @brief OCR 运行选项。
 */
struct GB_OCROptions
{
	/**
	 * @brief 期望使用的 OCR 后端。
	 */
	GB_OCRBackend backend = GB_OCRBackend::Auto;

	/**
	 * @brief PP-OCRv5 mobile 模型路径。
	 *
	 * 全部路径均为空时，会尝试自动查找；clsModelPathUtf8 为可选项。
	 */
	GB_PPOCRv5MobileModelPaths ppocrv5MobileModelPaths;

	/**
	 * @brief 百度 OCR API 选项。
	 */
	GB_BaiduOCROptions baiduApiOptions;

	/**
	 * @brief ONNX Runtime intra-op 线程数，0 表示使用 ONNX Runtime 默认值。
	 */
	int intraOpNumThreads = 0;

	/**
	 * @brief ONNX Runtime inter-op 线程数，0 表示使用 ONNX Runtime 默认值。
	 */
	int interOpNumThreads = 0;

	/**
	 * @brief ONNX Runtime 日志严重程度。
	 *
	 * 默认只输出 Error 及以上级别，避免 CUDA 后端在正常模型图优化和节点分配时输出非错误 warning。
	 * 若需要诊断 CUDA/CPU 节点分配情况，可临时设置为 Warning、Info 或 Verbose。
	 */
	GB_OCROnnxRuntimeLogSeverityLevel onnxRuntimeLogSeverityLevel = GB_OCROnnxRuntimeLogSeverityLevel::Error;

	/**
	 * @brief ONNX Runtime CUDA 设备编号。
	 *
	 * 仅在 PPOCRv5MobileOnnxRuntimeCuda 后端或 Auto 自动选择到 CUDA 后端时生效。
	 */
	int onnxRuntimeCudaDeviceId = 0;

	/**
	 * @brief ONNX Runtime CUDA 显存池上限，单位为字节。
	 *
	 * 0 表示使用 ONNX Runtime 默认值；该限制只作用于 CUDA Execution Provider 的 Arena，不代表进程总显存上限。
	 */
	unsigned long long onnxRuntimeCudaGpuMemLimitBytes = 0;

	/**
	 * @brief ONNX Runtime CUDA 是否允许使用 TF32。
	 *
	 * Ampere 及之后的 NVIDIA GPU 上，开启 TF32 通常能提升 float32 卷积和矩阵运算速度；
	 * OCR 推理场景通常可以保持默认开启。
	 */
	bool onnxRuntimeCudaUseTf32 = true;

	/**
	 * @brief 文本检测输入图像边长限制。
	 *
	 * 实际检测输入尺寸会按 PaddleOCR 常规流程调整为 32 的整数倍。
	 */
	int detLimitSideLen = 736;

	/**
	 * @brief 文本检测输入尺寸是否按长边限制。
	 *
	 * true：最长边不超过 detLimitSideLen；false：最短边不小于 detLimitSideLen。
	 * PP-OCRv5 对小字和截图类图像更依赖足够的检测分辨率，默认采用短边限制。
	 */
	bool detLimitByMaxSide = false;

	/**
	 * @brief 文本检测输入图像最大边长上限。
	 *
	 * 大于 0 时，即使 detLimitByMaxSide 为 false，也会限制最长边，避免超大图像导致耗时和内存占用过高。
	 */
	int detMaxSideLen = 1920;

	/**
	 * @brief 文本检测前给原图四周补充的白色边框像素数。
	 *
	 * 对贴边文字或裁剪过紧的截图，适当白边有助于检测模型保留边缘文字；输出坐标仍会映射回原图坐标。
	 */
	int detImagePadding = 8;

	/**
	 * @brief DB 文本检测概率图二值化阈值。
	 */
	double detDbThresh = 0.3;

	/**
	 * @brief DB 文本框平均置信度阈值。
	 */
	double detDbBoxThresh = 0.6;

	/**
	 * @brief DB 文本检测框置信度计算方式。
	 *
	 * Fast 与 PaddleOCR 默认后处理一致，速度较快；Slow 对复杂轮廓的置信度过滤更精确。
	 */
	GB_OCRTextDetectionScoreMode detScoreMode = GB_OCRTextDetectionScoreMode::Fast;

	/**
	 * @brief DB 文本框外扩比例。
	 */
	double detDbUnclipRatio = 1.5;

	/**
	 * @brief 是否对检测二值图做一次膨胀，低清晰度图像可尝试开启。
	 */
	bool detUseDilation = false;

	/**
	 * @brief 过滤过小文本框的最小边长。
	 */
	double detMinBoxSideLen = 3.0;

	/**
	 * @brief 单张图像最多保留的候选文本框数量。
	 */
	int maxCandidateTextBoxes = 1000;

	/**
	 * @brief 文本检测框非极大值抑制阈值。
	 *
	 * 大于 0 时，会按照检测置信度去除高度重叠的重复文本框；取值建议在 [0.1, 0.5]。
	 */
	double detBoxNmsThresh = 0.3;

	/**
	 * @brief 是否对超长图像自动分片检测。
	 *
	 * 对手机长截图、长票据、窄长扫描件等图像，整体缩放会导致短边被压得过小，容易漏检小字；
	 * 开启后会按长边分片检测并合并结果。普通比例图像不会触发分片。
	 */
	bool enableLongImageDetectionSlice = true;

	/**
	 * @brief 超长图像分片检测的相邻分片重叠像素数。
	 *
	 * 适当重叠可避免文字正好落在切片边界导致被截断；最终会通过检测框 NMS 合并重复结果。
	 */
	int detSliceOverlap = 64;

	/**
	 * @brief 识别模型输入高度。
	 */
	int recImageHeight = 48;

	/**
	 * @brief 识别模型输入宽度。
	 *
	 * 若模型输入宽度为固定正数，初始化时会优先使用模型自身宽度，避免选项与模型实际输入不一致。
	 * 若模型输入宽度为动态尺寸，本值作为最小识别宽度使用。
	 */
	int recImageWidth = 320;

	/**
	 * @brief 识别裁剪图像四周补充的像素边框。
	 *
	 * 对检测框略紧、文字笔画贴边的图像，少量边框有助于识别模型保留完整字形；0 表示不补边。
	 */
	int recImagePadding = 2;

	/**
	 * @brief 动态宽度识别模型允许使用的最大输入宽度。
	 *
	 * 仅当识别模型输入宽度为动态尺寸时生效；适当增大可提升长文本识别准确率，但会增加推理耗时。
	 */
	int recMaxDynamicImageWidth = 1280;

	/**
	 * @brief 识别阶段批处理数量。
	 *
	 * 仅当识别 ONNX 模型支持动态 batch 或固定 batch 大于 1 时生效；若模型固定 batch 为 1，会自动退化为逐条识别。
	 */
	int recBatchSize = 8;

	/**
	 * @brief 是否启用文字行方向分类。
	 *
	 * 仅当 clsModelPathUtf8 指向的模型文件存在时才会实际启用；若未配置该模型，将自动跳过本阶段。
	 * 该阶段主要用于识别并纠正 180° 倒置文字行。常规截图、票据和文档图片通常不需要该阶段，
	 * 默认关闭以避免额外的模型推理耗时；若存在倒置文字行，可显式开启以提升这类图像的识别准确率。
	 */
	bool useTextLineOrientationClassification = false;

	/**
	 * @brief 文字行方向分类模型输入高度。
	 */
	int clsImageHeight = 48;

	/**
	 * @brief 文字行方向分类模型输入宽度。
	 */
	int clsImageWidth = 192;

	/**
	 * @brief 文字行方向分类批处理数量。
	 */
	int clsBatchSize = 8;

	/**
	 * @brief 文字行方向分类置信度阈值。
	 *
	 * 当模型判断文字行是 180°，且置信度不低于该阈值时，才会将文字行旋转 180° 后送入识别模型。
	 */
	double clsConfidenceThresh = 0.9;

	/**
	 * @brief 是否在检测前进行局部对比度增强。
	 *
	 * 对光照不均、暗部文字、低对比度截图可能有帮助；对干净文档可能只增加耗时，因此默认关闭。
	 */
	bool enableDetectionContrastEnhancement = false;

	/**
	 * @brief 检测前 CLAHE 对比度增强的 clipLimit。
	 */
	double detClaheClipLimit = 2.0;

	/**
	 * @brief 检测前 CLAHE 对比度增强的网格大小。
	 */
	int detClaheTileGridSize = 8;

	/**
	 * @brief 首次检测未找到文字时，是否自动用 CLAHE 增强图像重试一次检测。
	 *
	 * 该选项默认开启，只在首次检测结果为空且未显式启用 enableDetectionContrastEnhancement 时触发，
	 * 对低对比度截图、浅色文字、局部光照不均图像有较明显收益，正常图片几乎不增加耗时。
	 */
	bool retryWithContrastEnhancementWhenNoTextDetected = true;

	/**
	 * @brief 识别前文字裁剪图锐化强度。
	 *
	 * 0 表示不锐化；建议取值 [0.1, 0.5]。过强锐化可能放大噪声并降低准确率。
	 */
	double recSharpenStrength = 0.0;

	/**
	 * @brief 是否允许在启用 OpenMP 的编译环境中并行处理裁剪图和输入张量。
	 */
	bool enableOpenMPParallel = true;

	/**
	 * @brief 遇到带 Alpha 通道的输入图像时，是否先按白色背景进行 Alpha 混合。
	 *
	 * OCR 场景中透明背景通常应视作白底；直接丢弃 Alpha 可能把透明区域的隐藏颜色带入检测模型。
	 */
	bool blendAlphaWithWhiteBackground = true;

	/**
	 * @brief 识别置信度阈值。
	 */
	double recConfidenceThresh = 0.8;

	/**
	 * @brief 是否按常规阅读顺序对结果排序。
	 */
	bool sortTextBlocks = true;
};

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief OCR 引擎。
 *
 * 同一个 GB_OCR 实例内部会串行化初始化、选项切换和识别调用；不同实例之间互不共享运行状态。
 */
class GLOBALBASE_PORT GB_OCR
{
public:
	/**
	 * @brief 构造 OCR 引擎，使用默认选项。
	 */
	GB_OCR();

	/**
	 * @brief 构造 OCR 引擎，使用指定选项。
	 */
	explicit GB_OCR(const GB_OCROptions& options);

	~GB_OCR();

	GB_OCR(const GB_OCR& other) = delete;
	GB_OCR& operator=(const GB_OCR& other) = delete;

	GB_OCR(GB_OCR&& other) noexcept;
	GB_OCR& operator=(GB_OCR&& other) noexcept;

	/**
	 * @brief 设置 OCR 选项。
	 *
	 * 若引擎已经初始化，本函数会清理旧后端；后续调用 Recognize 时会按新选项重新初始化。
	 */
	void SetOptions(const GB_OCROptions& options);

	/**
	 * @brief 获取当前 OCR 选项。
	 */
	const GB_OCROptions& GetOptions() const;

	/**
	 * @brief 主动初始化 OCR 后端。
	 *
	 * 调用 Recognize 前无需手动调用；Recognize 会自动延迟初始化。
	 */
	bool Initialize();

	/**
	 * @brief 当前 OCR 后端是否已经初始化成功。
	 */
	bool IsInitialized() const;

	/**
	 * @brief 清理已经初始化的 OCR 后端。
	 */
	void Clear();

	/**
	 * @brief 对内存图像执行 OCR。
	 */
	GB_OCRResult Recognize(const GB_Image& image);

	/**
	 * @brief 对图像文件执行 OCR。
	 */
	GB_OCRResult Recognize(const std::string& imageFilePathUtf8);

	/**
	 * @brief 对内存图像执行 OCR。
	 *
	 * @param image 输入图像。
	 * @param textBlocks 输出文字块。
	 * @param errorMessage 可选错误信息输出。
	 * @return true 执行成功；false 执行失败。
	 */
	bool Recognize(const GB_Image& image, std::vector<GB_OCRTextBlock>& textBlocks, std::string* errorMessage = nullptr);

	/**
	 * @brief 对图像文件执行 OCR。
	 */
	bool Recognize(const std::string& imageFilePathUtf8, std::vector<GB_OCRTextBlock>& textBlocks, std::string* errorMessage = nullptr);

	/**
	 * @brief 获取最近一次错误信息。
	 */
	const std::string& GetLastErrorMessage() const;

	/**
	 * @brief 获取实际使用的 OCR 后端。
	 */
	GB_OCRBackend GetActualBackend() const;

	/**
	 * @brief 获取默认 PP-OCRv5 mobile 模型路径。
	 *
	 * 默认会在 $(exe所在目录)/GlobalBaseDependencies/OCRModels/PP-OCRv5_mobile 下按常见 ONNX 文件名组合查找。
	 * clsModelPathUtf8 为可选模型路径；默认模型是否齐全只要求 det、rec、dict 存在。
	 */
	static GB_PPOCRv5MobileModelPaths GetDefaultPPOCRv5MobileModelPaths();

	/**
	 * @brief 判断默认 PP-OCRv5 mobile 模型文件是否齐全。
	 */
	static bool IsDefaultPPOCRv5MobileModelAvailable();

	/**
	 * @brief 判断指定后端在当前环境下是否可用。
	 */
	static bool IsBackendAvailable(GB_OCRBackend backend);

private:
	class Impl;
	std::unique_ptr<Impl> ocrImpl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

/**
 * @brief 一次性 OCR 识别接口，适合简单场景。
 */
GLOBALBASE_PORT GB_OCRResult GB_RecognizeTextFromImage(const GB_Image& image, const GB_OCROptions& options = GB_OCROptions());

/**
 * @brief 一次性 OCR 识别接口，适合简单场景。
 */
GLOBALBASE_PORT GB_OCRResult GB_RecognizeTextFromImageFile(const std::string& imageFilePathUtf8, const GB_OCROptions& options = GB_OCROptions());

#endif
