#include "GB_OCR.h"

#include "../GB_Crypto.h"
#include "../GB_FileSystem.h"
#include "../GB_FormatParser.h"
#include "../GB_Utf8String.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <utility>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4819)
#endif
#include <opencv2/opencv.hpp>
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#include <onnxruntime_cxx_api.h>

#ifdef _MSC_VER
#pragma comment(lib, "onnxruntime.lib")
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

namespace
{
	class IGB_OCRBackend
	{
	public:
		virtual ~IGB_OCRBackend()
		{
		}

		virtual bool Initialize(std::string& errorMessage) = 0;
		virtual bool Recognize(const GB_Image& image, std::vector<GB_OCRTextBlock>& textBlocks, std::string& errorMessage) = 0;
		virtual GB_OCRBackend GetBackendType() const = 0;
		virtual bool CanFallbackOnFailure() const
		{
			return true;
		}
	};

	struct DetectedTextBox
	{
		std::vector<cv::Point2f> points;
		double score = 0.0;
	};

	struct RecognizedText
	{
		std::string text;
		double confidence = 0.0;
	};

	struct TextLineOrientation
	{
		int angle = 0;
		double confidence = 0.0;
	};

	enum class RecognitionOutputLayout
	{
		Unknown = 0,
		BatchTimeClass,
		TimeBatchClass,
		TimeClass
	};

	struct RecognitionOutputInfo
	{
		RecognitionOutputLayout layout = RecognitionOutputLayout::Unknown;
		size_t outputBatchSize = 0;
		int timeSteps = 0;
		int classCount = 0;
	};

	struct FileCloser
	{
		void operator()(FILE* file) const
		{
			if (file)
			{
				std::fclose(file);
			}
		}
	};

	typedef std::unique_ptr<FILE, FileCloser> FilePtr;

	int ClampInt(int value, int minValue, int maxValue)
	{
		return std::max(minValue, std::min(value, maxValue));
	}

	double ClampDouble(double value, double minValue, double maxValue)
	{
		return std::max(minValue, std::min(value, maxValue));
	}

	GB_OCROptions NormalizeOptions(const GB_OCROptions& inputOptions)
	{
		GB_OCROptions options = inputOptions;
		options.detLimitSideLen = std::max(32, options.detLimitSideLen);
		options.detDbThresh = ClampDouble(options.detDbThresh, 0.0, 1.0);
		options.detDbBoxThresh = ClampDouble(options.detDbBoxThresh, 0.0, 1.0);
		options.detDbUnclipRatio = std::max(0.0, options.detDbUnclipRatio);
		options.detMinBoxSideLen = std::max(1.0, options.detMinBoxSideLen);
		options.detMaxSideLen = options.detMaxSideLen > 0 ? std::max(32, options.detMaxSideLen) : 0;
		options.detImagePadding = std::max(0, options.detImagePadding);
		options.maxCandidateTextBoxes = options.maxCandidateTextBoxes < 0 ? 0 : options.maxCandidateTextBoxes;
		options.detBoxNmsThresh = ClampDouble(options.detBoxNmsThresh, 0.0, 1.0);
		options.detSliceOverlap = std::max(0, options.detSliceOverlap);
		options.recImageHeight = std::max(1, options.recImageHeight);
		options.recImageWidth = std::max(1, options.recImageWidth);
		options.recImagePadding = std::max(0, options.recImagePadding);
		options.recMaxDynamicImageWidth = std::max(options.recImageWidth, options.recMaxDynamicImageWidth);
		options.recBatchSize = std::max(1, options.recBatchSize);
		options.clsImageHeight = std::max(1, options.clsImageHeight);
		options.clsImageWidth = std::max(1, options.clsImageWidth);
		options.clsBatchSize = std::max(1, options.clsBatchSize);
		options.clsConfidenceThresh = ClampDouble(options.clsConfidenceThresh, 0.0, 1.0);
		options.detClaheClipLimit = std::max(0.1, options.detClaheClipLimit);
		options.detClaheTileGridSize = std::max(1, options.detClaheTileGridSize);
		options.recSharpenStrength = ClampDouble(options.recSharpenStrength, 0.0, 2.0);
		options.recConfidenceThresh = ClampDouble(options.recConfidenceThresh, 0.0, 1.0);
		if (options.baiduApiOptions.imageFileExtUtf8.empty())
		{
			options.baiduApiOptions.imageFileExtUtf8 = ".png";
		}
		else if (options.baiduApiOptions.imageFileExtUtf8[0] != '.')
		{
			options.baiduApiOptions.imageFileExtUtf8 = "." + options.baiduApiOptions.imageFileExtUtf8;
		}
		return options;
	}

	OrtLoggingLevel ToOrtLoggingLevel(GB_OCROnnxRuntimeLogSeverityLevel logSeverityLevel)
	{
		switch (logSeverityLevel)
		{
		case GB_OCROnnxRuntimeLogSeverityLevel::Verbose:
			return ORT_LOGGING_LEVEL_VERBOSE;
		case GB_OCROnnxRuntimeLogSeverityLevel::Info:
			return ORT_LOGGING_LEVEL_INFO;
		case GB_OCROnnxRuntimeLogSeverityLevel::Warning:
			return ORT_LOGGING_LEVEL_WARNING;
		case GB_OCROnnxRuntimeLogSeverityLevel::Error:
			return ORT_LOGGING_LEVEL_ERROR;
		case GB_OCROnnxRuntimeLogSeverityLevel::Fatal:
			return ORT_LOGGING_LEVEL_FATAL;
		default:
			return ORT_LOGGING_LEVEL_ERROR;
		}
	}

	Ort::RunOptions CreateOnnxRuntimeRunOptions(const GB_OCROptions& options)
	{
		Ort::RunOptions runOptions;
		runOptions.SetRunLogSeverityLevel(static_cast<int>(ToOrtLoggingLevel(options.onnxRuntimeLogSeverityLevel)));
		return runOptions;
	}

	std::string JoinPath(const std::string& leftPathUtf8, const std::string& rightPathUtf8)
	{
		if (leftPathUtf8.empty())
		{
			return rightPathUtf8;
		}

		const char lastChar = leftPathUtf8[leftPathUtf8.size() - 1];
		if (lastChar == '/' || lastChar == '\\')
		{
			return leftPathUtf8 + rightPathUtf8;
		}

		return leftPathUtf8 + "/" + rightPathUtf8;
	}

	std::string FindFirstExistingFilePath(const std::vector<std::string>& filePathCandidatesUtf8)
	{
		for (const std::string& filePathUtf8 : filePathCandidatesUtf8)
		{
			if (GB_IsFileExists(filePathUtf8))
			{
				return filePathUtf8;
			}
		}

		return filePathCandidatesUtf8.empty() ? std::string() : filePathCandidatesUtf8[0];
	}

#ifdef _WIN32
	std::wstring Utf8ToWideString(const std::string& textUtf8)
	{
		if (textUtf8.empty())
		{
			return std::wstring();
		}
		if (textUtf8.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
		{
			return std::wstring();
		}

		const int textLength = static_cast<int>(textUtf8.size());
		const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, textUtf8.c_str(), textLength, nullptr, 0);
		if (wideLength <= 0)
		{
			return std::wstring();
		}

		std::wstring wideText(static_cast<size_t>(wideLength), L'\0');
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, textUtf8.c_str(), textLength, &wideText[0], wideLength) != wideLength)
		{
			return std::wstring();
		}
		return wideText;
	}
#endif


	std::string BuildGlobalBaseDependencyDirectoryPath()
	{
		const std::string exeDirectory = GB_GetExeDirectory();
		if (exeDirectory.empty())
		{
			return std::string();
		}

		return GB_JoinPath(exeDirectory, "GlobalBaseDependencies");
	}

#ifdef _WIN32
	std::string BuildWin32ErrorMessage(const std::string& operationName, const std::string& filePathUtf8, DWORD errorCode)
	{
		std::string errorMessage = operationName;
		errorMessage += GB_STR("失败，Win32 错误码：");
		errorMessage += std::to_string(static_cast<unsigned long>(errorCode));
		if (!filePathUtf8.empty())
		{
			errorMessage += GB_STR("，路径：");
			errorMessage += filePathUtf8;
		}

		return errorMessage;
	}

	bool PreloadOnnxRuntimeCudaDependencyDlls(const std::string& dependencyDirectoryPathUtf8, std::string& errorMessage)
	{
		errorMessage.clear();

		if (dependencyDirectoryPathUtf8.empty())
		{
			errorMessage = GB_STR("依赖库目录为空。");
			return false;
		}

		const std::vector<std::string> dependencyDllNames =
		{
			"cudnn64_9.dll",
			"onnxruntime_providers_shared.dll",
			"onnxruntime_providers_cuda.dll"
		};

		std::vector<std::string> dependencyDllPathsUtf8;
		dependencyDllPathsUtf8.reserve(dependencyDllNames.size());
		for (const std::string& dllName : dependencyDllNames)
		{
			const std::string dllPathUtf8 = GB_JoinPath(dependencyDirectoryPathUtf8, dllName);
			if (!GB_IsFileExists(dllPathUtf8))
			{
				errorMessage = GB_STR("缺少 ONNX Runtime CUDA 运行时依赖库：") + dllPathUtf8;
				return false;
			}
			dependencyDllPathsUtf8.push_back(dllPathUtf8);
		}

		static std::mutex dependencyDllMutex;
		static std::string loadedDependencyDirectoryPathUtf8;
		static std::vector<HMODULE> loadedDependencyModules;

		std::lock_guard<std::mutex> lockGuard(dependencyDllMutex);
		if (!loadedDependencyModules.empty())
		{
			if (loadedDependencyDirectoryPathUtf8 == dependencyDirectoryPathUtf8)
			{
				return true;
			}

			errorMessage = GB_STR("当前进程已经从不同目录预加载过 ONNX Runtime CUDA 依赖库：") + loadedDependencyDirectoryPathUtf8;
			return false;
		}

		std::vector<HMODULE> newlyLoadedModules;
		newlyLoadedModules.reserve(dependencyDllPathsUtf8.size());
		for (const std::string& dllPathUtf8 : dependencyDllPathsUtf8)
		{
			const std::wstring wideDllPath = Utf8ToWideString(dllPathUtf8);
			if (wideDllPath.empty())
			{
				errorMessage = GB_STR("依赖库路径 UTF-8 转换失败：") + dllPathUtf8;
				for (std::vector<HMODULE>::reverse_iterator iterator = newlyLoadedModules.rbegin(); iterator != newlyLoadedModules.rend(); iterator++)
				{
					FreeLibrary(*iterator);
				}
				return false;
			}

			const HMODULE moduleHandle = LoadLibraryExW(wideDllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (moduleHandle == nullptr)
			{
				const DWORD errorCode = GetLastError();
				for (std::vector<HMODULE>::reverse_iterator iterator = newlyLoadedModules.rbegin(); iterator != newlyLoadedModules.rend(); iterator++)
				{
					FreeLibrary(*iterator);
				}
				errorMessage = BuildWin32ErrorMessage(GB_STR("预加载 ONNX Runtime CUDA 依赖库"), dllPathUtf8, errorCode);
				return false;
			}

			newlyLoadedModules.push_back(moduleHandle);
		}

		loadedDependencyDirectoryPathUtf8 = dependencyDirectoryPathUtf8;
		loadedDependencyModules = std::move(newlyLoadedModules);
		return true;
	}

	bool PrepareOnnxRuntimeCudaDependencyDlls(std::string& errorMessage)
	{
		errorMessage.clear();

		const std::string dependencyDirectoryPathUtf8 = BuildGlobalBaseDependencyDirectoryPath();
		if (dependencyDirectoryPathUtf8.empty())
		{
			errorMessage = GB_STR("获取 exe 所在目录失败，无法定位 GlobalBaseDependencies 目录。");
			return false;
		}

		if (!GB_IsDirectoryExists(dependencyDirectoryPathUtf8))
		{
			errorMessage = GB_STR("ONNX Runtime CUDA 依赖库目录不存在：") + dependencyDirectoryPathUtf8;
			return false;
		}

		return PreloadOnnxRuntimeCudaDependencyDlls(dependencyDirectoryPathUtf8, errorMessage);
	}
#else
	bool PrepareOnnxRuntimeCudaDependencyDlls(std::string& errorMessage)
	{
		errorMessage.clear();
		return true;
	}
#endif

	std::basic_string<ORTCHAR_T> ToOrtPath(const std::string& filePathUtf8)
	{
#ifdef _WIN32
		return Utf8ToWideString(filePathUtf8);
#else
		return filePathUtf8;
#endif
	}

	struct CudaProviderOptionsDeleter
	{
		void operator()(OrtCUDAProviderOptionsV2* cudaProviderOptions) const
		{
			if (cudaProviderOptions)
			{
				Ort::GetApi().ReleaseCUDAProviderOptions(cudaProviderOptions);
			}
		}
	};

	bool AppendCudaExecutionProvider(Ort::SessionOptions& sessionOptions, const GB_OCROptions& options, std::string& errorMessage)
	{
		errorMessage.clear();

		try
		{
			const OrtApi& ortApi = Ort::GetApi();
			OrtCUDAProviderOptionsV2* rawCudaProviderOptions = nullptr;
			Ort::ThrowOnError(ortApi.CreateCUDAProviderOptions(&rawCudaProviderOptions));
			std::unique_ptr<OrtCUDAProviderOptionsV2, CudaProviderOptionsDeleter> cudaProviderOptions(rawCudaProviderOptions);

			std::vector<std::string> optionKeys;
			std::vector<std::string> optionValues;
			optionKeys.reserve(4);
			optionValues.reserve(4);

			optionKeys.push_back("device_id");
			optionValues.push_back(std::to_string(std::max(0, options.onnxRuntimeCudaDeviceId)));

			optionKeys.push_back("do_copy_in_default_stream");
			optionValues.push_back("1");

			optionKeys.push_back("use_tf32");
			optionValues.push_back(options.onnxRuntimeCudaUseTf32 ? "1" : "0");

			if (options.onnxRuntimeCudaGpuMemLimitBytes > 0)
			{
				optionKeys.push_back("gpu_mem_limit");
				optionValues.push_back(std::to_string(options.onnxRuntimeCudaGpuMemLimitBytes));
			}

			std::vector<const char*> optionKeyPointers;
			std::vector<const char*> optionValuePointers;
			optionKeyPointers.reserve(optionKeys.size());
			optionValuePointers.reserve(optionValues.size());
			for (size_t optionIndex = 0; optionIndex < optionKeys.size(); optionIndex++)
			{
				optionKeyPointers.push_back(optionKeys[optionIndex].c_str());
				optionValuePointers.push_back(optionValues[optionIndex].c_str());
			}

			Ort::ThrowOnError(ortApi.UpdateCUDAProviderOptions(cudaProviderOptions.get(), optionKeyPointers.data(), optionValuePointers.data(), optionKeyPointers.size()));
			Ort::ThrowOnError(ortApi.SessionOptionsAppendExecutionProvider_CUDA_V2(static_cast<OrtSessionOptions*>(sessionOptions), cudaProviderOptions.get()));
		}
		catch (const Ort::Exception& exception)
		{
			errorMessage = GB_STR("ONNX Runtime CUDA Execution Provider 初始化失败：") + exception.what();
			return false;
		}
		catch (const std::exception& exception)
		{
			errorMessage = GB_STR("ONNX Runtime CUDA Execution Provider 初始化失败：") + exception.what();
			return false;
		}

		return true;
	}

	bool AppendCpuExecutionProvider(Ort::SessionOptions& sessionOptions, std::string& errorMessage)
	{
		(void)sessionOptions;
		errorMessage.clear();

		// ONNX Runtime 的 CPU Execution Provider 是默认后端。
		// 部分 ONNX Runtime 版本的 C++ 头文件提供了 AppendExecutionProvider_CPU 包装函数，
		// 但实际发行包中的 C API 头文件未必导出 OrtSessionOptionsAppendExecutionProvider_CPU 符号，
		// 直接调用该包装函数可能导致编译失败。这里保留函数入口，仅依赖默认 CPU EP。
		return true;
	}

	void ConfigureOnnxRuntimeSessionOptions(Ort::SessionOptions& sessionOptions, const GB_OCROptions& options, GB_OCRBackend backendType)
	{
		const OrtLoggingLevel ortLogSeverityLevel = ToOrtLoggingLevel(options.onnxRuntimeLogSeverityLevel);
		sessionOptions.SetLogSeverityLevel(static_cast<int>(ortLogSeverityLevel));
		if (options.intraOpNumThreads > 0)
		{
			sessionOptions.SetIntraOpNumThreads(options.intraOpNumThreads);
		}
		if (options.interOpNumThreads > 0)
		{
			sessionOptions.SetInterOpNumThreads(options.interOpNumThreads);
		}

		// PP-OCRv5 mobile 的检测、方向分类、识别模型通常不是多分支图，ORT_SEQUENTIAL 可避免额外 inter-op 调度开销。
		// ORT_ENABLE_ALL 会启用 ONNX Runtime 支持的图优化；CPU Arena 与 MemPattern 对重复推理时的内存复用更友好。
		sessionOptions.SetExecutionMode(ORT_SEQUENTIAL);
		sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
		sessionOptions.EnableCpuMemArena();
		sessionOptions.EnableMemPattern();
		sessionOptions.AddConfigEntry("session.dynamic_block_base", "4");

		if (backendType == GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCpu)
		{
			// CPU 后端必须保持纯 CPU 路径：不追加 CUDA/TensorRT EP，不主动加载、探测或预热 CUDA 相关 DLL。
			// 这里仅显式固定 CPU SessionOptions，避免 GPU 包存在时误以为需要进行 provider 探测。
			sessionOptions.AddConfigEntry("session.intra_op.allow_spinning", "1");
		}
	}

	bool IsOnnxRuntimeCudaExecutionProviderAvailable(const GB_OCROptions& options)
	{
		try
		{
			std::string errorMessage;
			if (!PrepareOnnxRuntimeCudaDependencyDlls(errorMessage))
			{
				return false;
			}

			Ort::SessionOptions sessionOptions;
			return AppendCudaExecutionProvider(sessionOptions, NormalizeOptions(options), errorMessage);
		}
		catch (...)
		{
			return false;
		}
	}

	FILE* OpenFileUtf8(const std::string& filePathUtf8, const char* mode)
	{
		if (filePathUtf8.empty() || mode == nullptr || *mode == '\0')
		{
			return nullptr;
		}

#ifdef _WIN32
		const std::wstring widePath = Utf8ToWideString(filePathUtf8);
		if (widePath.empty())
		{
			return nullptr;
		}

		std::wstring wideMode;
		for (const char* currentChar = mode; *currentChar != '\0'; currentChar++)
		{
			wideMode.push_back(static_cast<wchar_t>(*currentChar));
		}

		FILE* file = nullptr;
		const errno_t openError = _wfopen_s(&file, widePath.c_str(), wideMode.c_str());
		if (openError != 0)
		{
			return nullptr;
		}

		return file;
#else
		return std::fopen(filePathUtf8.c_str(), mode);
#endif
	}

	bool ReadAllBytesUtf8(const std::string& filePathUtf8, std::vector<unsigned char>& bytes)
	{
		bytes.clear();
		FilePtr file(OpenFileUtf8(filePathUtf8, "rb"));
		if (!file)
		{
			return false;
		}

#ifdef _WIN32
		if (_fseeki64(file.get(), 0, SEEK_END) != 0)
		{
			return false;
		}

		const __int64 fileSize = _ftelli64(file.get());
		if (fileSize < 0 || static_cast<unsigned long long>(fileSize) > static_cast<unsigned long long>((std::numeric_limits<size_t>::max)()))
		{
			return false;
		}

		if (_fseeki64(file.get(), 0, SEEK_SET) != 0)
		{
			return false;
		}
#else
		if (std::fseek(file.get(), 0, SEEK_END) != 0)
		{
			return false;
		}

		const long fileSize = std::ftell(file.get());
		if (fileSize < 0)
		{
			return false;
		}

		if (std::fseek(file.get(), 0, SEEK_SET) != 0)
		{
			return false;
		}
#endif

		bytes.resize(static_cast<size_t>(fileSize));
		if (!bytes.empty())
		{
			const size_t readCount = std::fread(&bytes[0], 1, bytes.size(), file.get());
			if (readCount != bytes.size())
			{
				bytes.clear();
				return false;
			}
		}

		return true;
	}

	bool ReadTextLinesUtf8(const std::string& filePathUtf8, std::vector<std::string>& lines)
	{
		lines.clear();

		std::vector<unsigned char> bytes;
		if (!ReadAllBytesUtf8(filePathUtf8, bytes))
		{
			return false;
		}

		std::string content(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		size_t startPos = 0;
		while (startPos <= content.size())
		{
			const size_t endPos = content.find('\n', startPos);
			std::string line;
			if (endPos == std::string::npos)
			{
				line = content.substr(startPos);
				startPos = content.size() + 1;
			}
			else
			{
				line = content.substr(startPos, endPos - startPos);
				startPos = endPos + 1;
			}

			if (!line.empty() && line[line.size() - 1] == '\r')
			{
				line.pop_back();
			}

			if (lines.empty() && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
			{
				line.erase(0, 3);
			}

			if (!line.empty())
			{
				lines.push_back(line);
			}
		}

		return !lines.empty();
	}

	bool HasAllRequiredPPOCRv5ModelFiles(const GB_PPOCRv5MobileModelPaths& modelPaths)
	{
		return GB_IsFileExists(modelPaths.detModelPathUtf8) && GB_IsFileExists(modelPaths.recModelPathUtf8) && GB_IsFileExists(modelPaths.dictPathUtf8);
	}

	bool HasPPOCRv5TextLineOrientationModelFile(const GB_PPOCRv5MobileModelPaths& modelPaths)
	{
		return GB_IsFileExists(modelPaths.clsModelPathUtf8);
	}

	std::string BuildMissingPPOCRv5ModelFileMessage(const GB_PPOCRv5MobileModelPaths& modelPaths)
	{
		std::string message = GB_STR("PP-OCRv5 mobile 模型文件不完整。");
		if (!GB_IsFileExists(modelPaths.detModelPathUtf8))
		{
			message += GB_STR(" 缺少检测模型：") + modelPaths.detModelPathUtf8;
		}
		if (!GB_IsFileExists(modelPaths.recModelPathUtf8))
		{
			message += GB_STR(" 缺少识别模型：") + modelPaths.recModelPathUtf8;
		}
		if (!GB_IsFileExists(modelPaths.dictPathUtf8))
		{
			message += GB_STR(" 缺少识别字典：") + modelPaths.dictPathUtf8;
		}

		return message;
	}

	GB_PPOCRv5MobileModelPaths BuildDefaultPPOCRv5MobileModelPaths()
	{
		const std::string exeDirectory = GB_GetExeDirectory();
		const std::string modelRootDirectory = JoinPath(exeDirectory, "GlobalBaseDependencies/OCRModels/PP-OCRv5_mobile");

		GB_PPOCRv5MobileModelPaths modelPaths;
		modelPaths.detModelPathUtf8 = FindFirstExistingFilePath({
			JoinPath(modelRootDirectory, "PP-OCRv5_mobile_det_onnx/model.onnx"),
			JoinPath(modelRootDirectory, "PP-OCRv5_mobile_det_infer/model.onnx"),
			JoinPath(modelRootDirectory, "PP-OCRv5_mobile_det/model.onnx"),
			JoinPath(modelRootDirectory, "PP-OCRv5_mobile_det.onnx"),
			JoinPath(modelRootDirectory, "det.onnx")
			});
		modelPaths.recModelPathUtf8 = FindFirstExistingFilePath({
			JoinPath(modelRootDirectory, "PP-OCRv5_mobile_rec_onnx/model.onnx"),
			JoinPath(modelRootDirectory, "PP-OCRv5_mobile_rec_infer/model.onnx"),
			JoinPath(modelRootDirectory, "PP-OCRv5_mobile_rec/model.onnx"),
			JoinPath(modelRootDirectory, "PP-OCRv5_mobile_rec.onnx"),
			JoinPath(modelRootDirectory, "rec.onnx")
			});
		modelPaths.clsModelPathUtf8 = FindFirstExistingFilePath({
			JoinPath(modelRootDirectory, "PP-LCNet_x0_25_textline_ori_onnx/model.onnx"),
			JoinPath(modelRootDirectory, "PP-LCNet_x1_0_textline_ori_onnx/model.onnx"),
			JoinPath(modelRootDirectory, "PP-LCNet_x0_25_textline_ori_infer/model.onnx"),
			JoinPath(modelRootDirectory, "PP-LCNet_x1_0_textline_ori_infer/model.onnx"),
			JoinPath(modelRootDirectory, "PP-LCNet_x0_25_textline_ori/model.onnx"),
			JoinPath(modelRootDirectory, "PP-LCNet_x1_0_textline_ori/model.onnx"),
			JoinPath(modelRootDirectory, "textline_ori.onnx"),
			JoinPath(modelRootDirectory, "cls.onnx")
			});
		modelPaths.dictPathUtf8 = FindFirstExistingFilePath({
			JoinPath(modelRootDirectory, "ppocrv5_dict.txt"),
			JoinPath(modelRootDirectory, "ppocr_keys_v1.txt"),
			JoinPath(modelRootDirectory, "dict.txt")
			});
		return modelPaths;
	}

	GB_PPOCRv5MobileModelPaths NormalizeModelPaths(const GB_PPOCRv5MobileModelPaths& inputModelPaths)
	{
		GB_PPOCRv5MobileModelPaths modelPaths = inputModelPaths;
		if (modelPaths.detModelPathUtf8.empty() && modelPaths.recModelPathUtf8.empty() && modelPaths.clsModelPathUtf8.empty() && modelPaths.dictPathUtf8.empty())
		{
			modelPaths = BuildDefaultPPOCRv5MobileModelPaths();
		}
		else if (modelPaths.clsModelPathUtf8.empty())
		{
			modelPaths.clsModelPathUtf8 = BuildDefaultPPOCRv5MobileModelPaths().clsModelPathUtf8;
		}

		return modelPaths;
	}

	GB_Rectangle MakeBoundingRectangle(const std::vector<GB_Point2d>& points)
	{
		if (points.empty())
		{
			return GB_Rectangle::Invalid;
		}

		double minX = points[0].x;
		double minY = points[0].y;
		double maxX = points[0].x;
		double maxY = points[0].y;
		for (size_t pointIndex = 1; pointIndex < points.size(); pointIndex++)
		{
			minX = std::min(minX, points[pointIndex].x);
			minY = std::min(minY, points[pointIndex].y);
			maxX = std::max(maxX, points[pointIndex].x);
			maxY = std::max(maxY, points[pointIndex].y);
		}

		return GB_Rectangle(minX, minY, maxX, maxY);
	}

	double GetRectangleCenterY(const GB_Rectangle& rectangle)
	{
		return rectangle.IsValid() ? rectangle.minY * 0.5 + rectangle.maxY * 0.5 : std::numeric_limits<double>::infinity();
	}

	double GetRectangleCenterX(const GB_Rectangle& rectangle)
	{
		return rectangle.IsValid() ? rectangle.minX * 0.5 + rectangle.maxX * 0.5 : std::numeric_limits<double>::infinity();
	}

	struct OCRTextLine
	{
		std::vector<GB_OCRTextBlock> blocks;
		double centerY = 0.0;
		double averageHeight = 0.0;
	};

	bool CompareTextBlocksByVerticalPosition(const GB_OCRTextBlock& leftBlock, const GB_OCRTextBlock& rightBlock)
	{
		const double leftCenterY = GetRectangleCenterY(leftBlock.boundingRectangle);
		const double rightCenterY = GetRectangleCenterY(rightBlock.boundingRectangle);
		if (leftCenterY != rightCenterY)
		{
			return leftCenterY < rightCenterY;
		}

		const double leftCenterX = GetRectangleCenterX(leftBlock.boundingRectangle);
		const double rightCenterX = GetRectangleCenterX(rightBlock.boundingRectangle);
		if (leftCenterX != rightCenterX)
		{
			return leftCenterX < rightCenterX;
		}

		return leftBlock.text < rightBlock.text;
	}

	bool CompareTextBlocksByHorizontalPosition(const GB_OCRTextBlock& leftBlock, const GB_OCRTextBlock& rightBlock)
	{
		const double leftCenterX = GetRectangleCenterX(leftBlock.boundingRectangle);
		const double rightCenterX = GetRectangleCenterX(rightBlock.boundingRectangle);
		if (leftCenterX != rightCenterX)
		{
			return leftCenterX < rightCenterX;
		}

		const double leftCenterY = GetRectangleCenterY(leftBlock.boundingRectangle);
		const double rightCenterY = GetRectangleCenterY(rightBlock.boundingRectangle);
		if (leftCenterY != rightCenterY)
		{
			return leftCenterY < rightCenterY;
		}

		return leftBlock.text < rightBlock.text;
	}

	void SortTextBlocksReadingOrder(std::vector<GB_OCRTextBlock>& textBlocks)
	{
		if (textBlocks.size() < 2)
		{
			return;
		}

		std::stable_sort(textBlocks.begin(), textBlocks.end(), CompareTextBlocksByVerticalPosition);

		std::vector<OCRTextLine> textLines;
		textLines.reserve(textBlocks.size());
		for (GB_OCRTextBlock& textBlock : textBlocks)
		{
			const double blockCenterY = GetRectangleCenterY(textBlock.boundingRectangle);
			const double blockHeight = textBlock.boundingRectangle.IsValid() ? std::max(0.0, textBlock.boundingRectangle.Height()) : 0.0;
			bool appendToCurrentLine = false;
			if (!textLines.empty() && std::isfinite(blockCenterY))
			{
				const OCRTextLine& currentLine = textLines.back();
				const double lineTolerance = std::max(10.0, std::min(currentLine.averageHeight, blockHeight) * 0.5);
				appendToCurrentLine = std::isfinite(currentLine.centerY) && std::abs(blockCenterY - currentLine.centerY) <= lineTolerance;
			}

			if (!appendToCurrentLine)
			{
				OCRTextLine textLine;
				textLine.centerY = blockCenterY;
				textLine.averageHeight = blockHeight;
				textLine.blocks.push_back(std::move(textBlock));
				textLines.push_back(std::move(textLine));
				continue;
			}

			OCRTextLine& currentLine = textLines.back();
			const double oldBlockCount = static_cast<double>(currentLine.blocks.size());
			currentLine.centerY = (currentLine.centerY * oldBlockCount + blockCenterY) / (oldBlockCount + 1.0);
			currentLine.averageHeight = (currentLine.averageHeight * oldBlockCount + blockHeight) / (oldBlockCount + 1.0);
			currentLine.blocks.push_back(std::move(textBlock));
		}

		std::vector<GB_OCRTextBlock> sortedTextBlocks;
		sortedTextBlocks.reserve(textBlocks.size());
		for (OCRTextLine& textLine : textLines)
		{
			std::stable_sort(textLine.blocks.begin(), textLine.blocks.end(), CompareTextBlocksByHorizontalPosition);
			for (GB_OCRTextBlock& textBlock : textLine.blocks)
			{
				sortedTextBlocks.push_back(std::move(textBlock));
			}
		}

		textBlocks = std::move(sortedTextBlocks);
	}

	cv::Mat BlendBgraMatWithWhiteBackground(const cv::Mat& bgraImage)
	{
		if (bgraImage.empty() || bgraImage.channels() != 4 || bgraImage.depth() != CV_8U)
		{
			return cv::Mat();
		}

		cv::Mat bgrImage(bgraImage.rows, bgraImage.cols, CV_8UC3);
		if (bgrImage.empty())
		{
			return cv::Mat();
		}

		if (bgraImage.isContinuous() && bgrImage.isContinuous())
		{
			const size_t pixelCount = static_cast<size_t>(bgraImage.rows) * bgraImage.cols;
			const cv::Vec4b* sourceData = bgraImage.ptr<cv::Vec4b>(0);
			cv::Vec3b* targetData = bgrImage.ptr<cv::Vec3b>(0);
			for (size_t pixelIndex = 0; pixelIndex < pixelCount; pixelIndex++)
			{
				const cv::Vec4b& sourcePixel = sourceData[pixelIndex];
				const int alphaValue = sourcePixel[3];
				const int inverseAlphaValue = 255 - alphaValue;
				for (int channelIndex = 0; channelIndex < 3; channelIndex++)
				{
					targetData[pixelIndex][channelIndex] = static_cast<unsigned char>((static_cast<int>(sourcePixel[channelIndex]) * alphaValue + 255 * inverseAlphaValue + 127) / 255);
				}
			}

			return bgrImage;
		}

		for (int row = 0; row < bgraImage.rows; row++)
		{
			const cv::Vec4b* sourceRowData = bgraImage.ptr<cv::Vec4b>(row);
			cv::Vec3b* targetRowData = bgrImage.ptr<cv::Vec3b>(row);
			for (int col = 0; col < bgraImage.cols; col++)
			{
				const cv::Vec4b& sourcePixel = sourceRowData[col];
				const int alphaValue = sourcePixel[3];
				const int inverseAlphaValue = 255 - alphaValue;
				for (int channelIndex = 0; channelIndex < 3; channelIndex++)
				{
					targetRowData[col][channelIndex] = static_cast<unsigned char>((static_cast<int>(sourcePixel[channelIndex]) * alphaValue + 255 * inverseAlphaValue + 127) / 255);
				}
			}
		}

		return bgrImage;
	}

	cv::Mat ConvertGBImageColorIfPossible(const GB_Image& image, GB_ImageColorConversion conversion)
	{
		const GB_Image convertedImage = image.ConvertColor(conversion);
		if (convertedImage.IsEmpty())
		{
			return cv::Mat();
		}

		return convertedImage.ToCvMat(GB_ImageCopyMode::ShallowCopy);
	}

	cv::Mat NormalizeGBImageMatToOpenCvBgrLayout(const GB_Image& image, const cv::Mat& imageMat, bool blendAlphaWithWhiteBackground)
	{
		if (imageMat.empty())
		{
			return cv::Mat();
		}

		if (imageMat.channels() == 3)
		{
			const cv::Mat bgrMat = ConvertGBImageColorIfPossible(image, GB_ImageColorConversion::RgbToBgr);
			return bgrMat.empty() ? imageMat : bgrMat;
		}

		if (imageMat.channels() == 4)
		{
			const cv::Mat convertedMat = ConvertGBImageColorIfPossible(image, blendAlphaWithWhiteBackground ? GB_ImageColorConversion::RgbaToBgra : GB_ImageColorConversion::RgbaToBgr);
			return convertedMat.empty() ? imageMat : convertedMat;
		}

		return imageMat;
	}

	cv::Mat ConvertGBImageToBgrMat(const GB_Image& image, bool blendAlphaWithWhiteBackground)
	{
		if (image.IsEmpty() || image.GetWidth() == 0 || image.GetHeight() == 0)
		{
			return cv::Mat();
		}

		cv::Mat imageMat = image.ToCvMat(GB_ImageCopyMode::ShallowCopy);
		if (imageMat.empty())
		{
			return cv::Mat();
		}

		imageMat = NormalizeGBImageMatToOpenCvBgrLayout(image, imageMat, blendAlphaWithWhiteBackground);
		if (imageMat.empty())
		{
			return cv::Mat();
		}

		if (imageMat.depth() != CV_8U)
		{
			double minValue = 0.0;
			double maxValue = 0.0;
			cv::minMaxLoc(imageMat.reshape(1), &minValue, &maxValue);
			const double scale = maxValue > minValue ? 255.0 / (maxValue - minValue) : 1.0;
			const double shift = maxValue > minValue ? -minValue * scale : 0.0;
			imageMat.convertTo(imageMat, CV_8U, scale, shift);
		}

		cv::Mat bgrMat;
		if (imageMat.channels() == 1)
		{
			cv::cvtColor(imageMat, bgrMat, cv::COLOR_GRAY2BGR);
		}
		else if (imageMat.channels() == 3)
		{
			bgrMat = imageMat;
		}
		else if (imageMat.channels() == 4)
		{
			if (blendAlphaWithWhiteBackground)
			{
				bgrMat = BlendBgraMatWithWhiteBackground(imageMat);
			}
			else
			{
				cv::cvtColor(imageMat, bgrMat, cv::COLOR_BGRA2BGR);
			}
		}
		else
		{
			return cv::Mat();
		}

		return bgrMat;
	}

	long long GetCurrentUnixTimeSeconds()
	{
		const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
		return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
	}

	bool IsBaiduOCRApiWithLocation(GB_BaiduOCRApiType apiType)
	{
		return apiType == GB_BaiduOCRApiType::GeneralWithLocation || apiType == GB_BaiduOCRApiType::AccurateWithLocation;
	}

	bool IsBaiduOCRGeneralApi(GB_BaiduOCRApiType apiType)
	{
		return apiType == GB_BaiduOCRApiType::GeneralBasic || apiType == GB_BaiduOCRApiType::GeneralWithLocation;
	}

	bool IsBaiduOCRAccurateApi(GB_BaiduOCRApiType apiType)
	{
		return apiType == GB_BaiduOCRApiType::AccurateBasic || apiType == GB_BaiduOCRApiType::AccurateWithLocation;
	}

	std::string GetBaiduOCRApiUrl(GB_BaiduOCRApiType apiType)
	{
		switch (apiType)
		{
		case GB_BaiduOCRApiType::GeneralBasic:
			return "https://aip.baidubce.com/rest/2.0/ocr/v1/general_basic";
		case GB_BaiduOCRApiType::GeneralWithLocation:
			return "https://aip.baidubce.com/rest/2.0/ocr/v1/general";
		case GB_BaiduOCRApiType::AccurateBasic:
			return "https://aip.baidubce.com/rest/2.0/ocr/v1/accurate_basic";
		case GB_BaiduOCRApiType::AccurateWithLocation:
			return "https://aip.baidubce.com/rest/2.0/ocr/v1/accurate";
		default:
			return std::string();
		}
	}

	const GB_VariantMap* TryGetVariantMapField(const GB_VariantMap& valueMap, const std::string& keyUtf8)
	{
		const GB_VariantMap::const_iterator iter = valueMap.find(keyUtf8);
		if (iter == valueMap.end())
		{
			return nullptr;
		}

		return iter->second.AnyCast<GB_VariantMap>();
	}

	const GB_VariantList* TryGetVariantListField(const GB_VariantMap& valueMap, const std::string& keyUtf8)
	{
		const GB_VariantMap::const_iterator iter = valueMap.find(keyUtf8);
		if (iter == valueMap.end())
		{
			return nullptr;
		}

		return iter->second.AnyCast<GB_VariantList>();
	}

	bool TryGetVariantStringField(const GB_VariantMap& valueMap, const std::string& keyUtf8, std::string& outValueUtf8)
	{
		const GB_VariantMap::const_iterator iter = valueMap.find(keyUtf8);
		if (iter == valueMap.end())
		{
			return false;
		}

		bool ok = false;
		const std::string valueUtf8 = iter->second.ToString(&ok);
		if (!ok)
		{
			return false;
		}

		outValueUtf8 = valueUtf8;
		return true;
	}

	bool TryGetVariantDoubleField(const GB_VariantMap& valueMap, const std::string& keyUtf8, double& outValue)
	{
		const GB_VariantMap::const_iterator iter = valueMap.find(keyUtf8);
		if (iter == valueMap.end())
		{
			return false;
		}

		bool ok = false;
		const double value = iter->second.ToDouble(&ok);
		if (!ok)
		{
			return false;
		}

		outValue = value;
		return true;
	}

	bool TryGetBaiduResponseErrorMessage(const GB_VariantMap& responseMap, std::string& outErrorMessage, std::string* outErrorCodeText = nullptr)
	{
		std::string errorCodeText;
		std::string errorMessageText;
		if (TryGetVariantStringField(responseMap, "error_code", errorCodeText))
		{
			TryGetVariantStringField(responseMap, "error_msg", errorMessageText);
			outErrorMessage = GB_STR("百度 OCR API 返回错误 ") + errorCodeText + (errorMessageText.empty() ? std::string() : (": " + errorMessageText));
			if (outErrorCodeText)
			{
				*outErrorCodeText = errorCodeText;
			}
			return true;
		}

		if (TryGetVariantStringField(responseMap, "error", errorCodeText))
		{
			TryGetVariantStringField(responseMap, "error_description", errorMessageText);
			outErrorMessage = GB_STR("百度 OAuth 返回错误 ") + errorCodeText + (errorMessageText.empty() ? std::string() : (": " + errorMessageText));
			if (outErrorCodeText)
			{
				*outErrorCodeText = errorCodeText;
			}
			return true;
		}

		return false;
	}

	bool TryBuildBaiduPolygonFromVertexesLocation(const GB_VariantMap& wordMap, std::vector<GB_Point2d>& outPoints)
	{
		outPoints.clear();
		const GB_VariantList* const vertexList = TryGetVariantListField(wordMap, "vertexes_location");
		if (vertexList == nullptr || vertexList->size() < 3)
		{
			return false;
		}

		outPoints.reserve(vertexList->size());
		for (const GB_Variant& vertexValue : *vertexList)
		{
			const GB_VariantMap* const vertexMap = vertexValue.AnyCast<GB_VariantMap>();
			if (vertexMap == nullptr)
			{
				outPoints.clear();
				return false;
			}

			double x = 0.0;
			double y = 0.0;
			if (!TryGetVariantDoubleField(*vertexMap, "x", x) || !TryGetVariantDoubleField(*vertexMap, "y", y))
			{
				outPoints.clear();
				return false;
			}

			outPoints.push_back(GB_Point2d(x, y));
		}

		return true;
	}

	bool TryBuildBaiduPolygonFromLocation(const GB_VariantMap& wordMap, std::vector<GB_Point2d>& outPoints, GB_Rectangle& outRectangle)
	{
		outPoints.clear();
		outRectangle = GB_Rectangle::Invalid;

		const GB_VariantMap* const locationMap = TryGetVariantMapField(wordMap, "location");
		if (locationMap == nullptr)
		{
			return false;
		}

		double left = 0.0;
		double top = 0.0;
		double width = 0.0;
		double height = 0.0;
		if (!TryGetVariantDoubleField(*locationMap, "left", left) || !TryGetVariantDoubleField(*locationMap, "top", top) || !TryGetVariantDoubleField(*locationMap, "width", width) || !TryGetVariantDoubleField(*locationMap, "height", height))
		{
			return false;
		}

		const double right = left + std::max(0.0, width);
		const double bottom = top + std::max(0.0, height);
		outPoints.push_back(GB_Point2d(left, top));
		outPoints.push_back(GB_Point2d(right, top));
		outPoints.push_back(GB_Point2d(right, bottom));
		outPoints.push_back(GB_Point2d(left, bottom));
		outRectangle = GB_Rectangle(left, top, right, bottom);
		return true;
	}

	void AppendBaiduFormParameter(std::string& requestBody, const std::string& keyUtf8, const std::string& valueUtf8)
	{
		if (!requestBody.empty())
		{
			requestBody.push_back('&');
		}

		requestBody += GB_UrlOperator::UrlEncode(keyUtf8, GB_UrlOperator::UrlEncodingMode::FormUrlEncoded);
		requestBody.push_back('=');
		requestBody += GB_UrlOperator::UrlEncode(valueUtf8, GB_UrlOperator::UrlEncodingMode::FormUrlEncoded);
	}

	std::string BuildBaiduOCRRequestBody(const GB_BaiduOCROptions& baiduOptions, const std::string& imageBase64)
	{
		std::string requestBody;
		AppendBaiduFormParameter(requestBody, "image", imageBase64);
		if (!baiduOptions.languageTypeUtf8.empty())
		{
			AppendBaiduFormParameter(requestBody, "language_type", baiduOptions.languageTypeUtf8);
		}
		if (baiduOptions.detectDirection)
		{
			AppendBaiduFormParameter(requestBody, "detect_direction", "true");
		}
		if (IsBaiduOCRGeneralApi(baiduOptions.apiType) && baiduOptions.detectLanguage)
		{
			AppendBaiduFormParameter(requestBody, "detect_language", "true");
		}
		if (baiduOptions.paragraph)
		{
			AppendBaiduFormParameter(requestBody, "paragraph", "true");
		}
		if (baiduOptions.probability)
		{
			AppendBaiduFormParameter(requestBody, "probability", "true");
		}
		if (IsBaiduOCRApiWithLocation(baiduOptions.apiType) && baiduOptions.recognizeGranularitySmall)
		{
			AppendBaiduFormParameter(requestBody, "recognize_granularity", "small");
		}
		if (IsBaiduOCRApiWithLocation(baiduOptions.apiType) && baiduOptions.vertexesLocation)
		{
			AppendBaiduFormParameter(requestBody, "vertexes_location", "true");
		}
		if (IsBaiduOCRAccurateApi(baiduOptions.apiType) && baiduOptions.multidirectionalRecognize)
		{
			AppendBaiduFormParameter(requestBody, "multidirectional_recognize", "true");
		}
		return requestBody;
	}

	int RoundToMultiple(int value, int multiple)
	{
		if (value <= 0 || multiple <= 0)
		{
			return value;
		}

		return std::max(multiple, static_cast<int>(std::round(static_cast<double>(value) / multiple)) * multiple);
	}

	int RoundUpToMultiple(int value, int multiple)
	{
		if (value <= 0 || multiple <= 0)
		{
			return value;
		}

		return std::max(multiple, ((value + multiple - 1) / multiple) * multiple);
	}

	int FloorToMultiple(int value, int multiple)
	{
		if (value <= 0 || multiple <= 0)
		{
			return value;
		}

		return std::max(multiple, (value / multiple) * multiple);
	}

	bool TryMultiplySize(size_t leftValue, size_t rightValue, size_t& resultValue)
	{
		if (leftValue != 0 && rightValue > (std::numeric_limits<size_t>::max)() / leftValue)
		{
			return false;
		}

		resultValue = leftValue * rightValue;
		return true;
	}

	bool CanConvertInt64ToInt(int64_t value)
	{
		return value >= static_cast<int64_t>((std::numeric_limits<int>::min)()) && value <= static_cast<int64_t>((std::numeric_limits<int>::max)());
	}

	void ResizeForDetection(const cv::Mat& sourceImage, cv::Mat& resizedImage, int detLimitSideLen, bool detLimitByMaxSide, int detMaxSideLen)
	{
		const int imageHeight = sourceImage.rows;
		const int imageWidth = sourceImage.cols;
		if (imageHeight <= 0 || imageWidth <= 0)
		{
			resizedImage.release();
			return;
		}

		const int maxSide = std::max(imageHeight, imageWidth);
		const int minSide = std::min(imageHeight, imageWidth);
		double resizeRatio = 1.0;
		if (detLimitSideLen > 0)
		{
			if (detLimitByMaxSide && maxSide > detLimitSideLen)
			{
				resizeRatio = static_cast<double>(detLimitSideLen) / static_cast<double>(maxSide);
			}
			else if (!detLimitByMaxSide && minSide < detLimitSideLen)
			{
				resizeRatio = static_cast<double>(detLimitSideLen) / static_cast<double>(minSide);
			}
		}

		if (detMaxSideLen > 0 && static_cast<double>(maxSide) * resizeRatio > detMaxSideLen)
		{
			resizeRatio = static_cast<double>(detMaxSideLen) / static_cast<double>(maxSide);
		}

		int resizedHeight = std::max(32, RoundToMultiple(static_cast<int>(std::round(imageHeight * resizeRatio)), 32));
		int resizedWidth = std::max(32, RoundToMultiple(static_cast<int>(std::round(imageWidth * resizeRatio)), 32));
		if (detMaxSideLen > 0 && std::max(resizedHeight, resizedWidth) > detMaxSideLen)
		{
			const double maxLimitRatio = static_cast<double>(detMaxSideLen) / static_cast<double>(std::max(resizedHeight, resizedWidth));
			resizedHeight = std::max(32, FloorToMultiple(static_cast<int>(std::floor(resizedHeight * maxLimitRatio)), 32));
			resizedWidth = std::max(32, FloorToMultiple(static_cast<int>(std::floor(resizedWidth * maxLimitRatio)), 32));
		}

		if (resizedHeight <= 0 || resizedWidth <= 0)
		{
			resizedImage.release();
			return;
		}

		if (resizedHeight == imageHeight && resizedWidth == imageWidth)
		{
			resizedImage = sourceImage;
		}
		else
		{
			const int interpolationMode = (resizedHeight < imageHeight || resizedWidth < imageWidth) ? cv::INTER_AREA : cv::INTER_CUBIC;
			cv::resize(sourceImage, resizedImage, cv::Size(resizedWidth, resizedHeight), 0.0, 0.0, interpolationMode);
		}
	}

	double GetMinBoxSideLen(const std::vector<cv::Point2f>& points);

	cv::Mat AddWhiteBorderForDetection(const cv::Mat& sourceImage, int paddingSize)
	{
		if (sourceImage.empty() || paddingSize <= 0)
		{
			return sourceImage;
		}

		cv::Mat paddedImage;
		cv::copyMakeBorder(sourceImage, paddedImage, paddingSize, paddingSize, paddingSize, paddingSize, cv::BORDER_CONSTANT, cv::Scalar(255, 255, 255));
		return paddedImage.empty() ? sourceImage : paddedImage;
	}

	cv::Mat ApplyDetectionContrastEnhancement(const cv::Mat& sourceImage, double claheClipLimit, int claheTileGridSize)
	{
		if (sourceImage.empty() || sourceImage.channels() != 3)
		{
			return sourceImage;
		}

		cv::Mat labImage;
		cv::cvtColor(sourceImage, labImage, cv::COLOR_BGR2Lab);
		std::vector<cv::Mat> labChannels;
		cv::split(labImage, labChannels);
		if (labChannels.empty())
		{
			return sourceImage;
		}

		const int safeTileGridSize = std::max(1, claheTileGridSize);
		cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(std::max(0.1, claheClipLimit), cv::Size(safeTileGridSize, safeTileGridSize));
		clahe->apply(labChannels[0], labChannels[0]);
		cv::merge(labChannels, labImage);

		cv::Mat enhancedImage;
		cv::cvtColor(labImage, enhancedImage, cv::COLOR_Lab2BGR);
		return enhancedImage.empty() ? sourceImage : enhancedImage;
	}

	cv::Mat ApplyRecognitionSharpen(const cv::Mat& sourceImage, double sharpenStrength)
	{
		if (sourceImage.empty() || sharpenStrength <= 0.0)
		{
			return sourceImage;
		}

		cv::Mat blurredImage;
		cv::GaussianBlur(sourceImage, blurredImage, cv::Size(0, 0), 1.0);
		if (blurredImage.empty())
		{
			return sourceImage;
		}

		cv::Mat sharpenedImage;
		cv::addWeighted(sourceImage, 1.0 + sharpenStrength, blurredImage, -sharpenStrength, 0.0, sharpenedImage);
		return sharpenedImage.empty() ? sourceImage : sharpenedImage;
	}

	void ShiftDetectedTextBoxesToOriginalImage(std::vector<DetectedTextBox>& textBoxes, int paddingSize, int originalImageWidth, int originalImageHeight, double detMinBoxSideLen)
	{
		if (textBoxes.empty() || originalImageWidth <= 0 || originalImageHeight <= 0)
		{
			return;
		}

		for (DetectedTextBox& textBox : textBoxes)
		{
			for (cv::Point2f& point : textBox.points)
			{
				point.x = static_cast<float>(ClampDouble(static_cast<double>(point.x) - paddingSize, 0.0, static_cast<double>(originalImageWidth - 1)));
				point.y = static_cast<float>(ClampDouble(static_cast<double>(point.y) - paddingSize, 0.0, static_cast<double>(originalImageHeight - 1)));
			}
		}

		textBoxes.erase(std::remove_if(textBoxes.begin(), textBoxes.end(), [detMinBoxSideLen](const DetectedTextBox& textBox)
			{
				return textBox.points.size() != 4 || GetMinBoxSideLen(textBox.points) < detMinBoxSideLen;
			}), textBoxes.end());
	}

	bool FillBgrMatToDetInput(const cv::Mat& bgrImage, std::vector<float>& inputData, bool enableOpenMPParallel)
	{
		const int imageHeight = bgrImage.rows;
		const int imageWidth = bgrImage.cols;
		size_t imageArea = 0;
		if (imageHeight <= 0 || imageWidth <= 0 || !TryMultiplySize(static_cast<size_t>(imageHeight), static_cast<size_t>(imageWidth), imageArea))
		{
			inputData.clear();
			return false;
		}

		size_t inputValueCount = 0;
		if (!TryMultiplySize(static_cast<size_t>(3), imageArea, inputValueCount))
		{
			inputData.clear();
			return false;
		}

		inputData.resize(inputValueCount);
		const float meanValues[3] = { 0.485f, 0.456f, 0.406f };
		const float stdValues[3] = { 0.229f, 0.224f, 0.225f };
#ifdef _OPENMP
#pragma omp parallel for if(enableOpenMPParallel && imageHeight >= 64)
#endif
		for (int row = 0; row < imageHeight; row++)
		{
			const cv::Vec3b* rowData = bgrImage.ptr<cv::Vec3b>(row);
			for (int col = 0; col < imageWidth; col++)
			{
				const size_t pixelOffset = static_cast<size_t>(row) * imageWidth + col;
				const cv::Vec3b& pixelValue = rowData[col];
				const float redValue = static_cast<float>(pixelValue[2]) / 255.0f;
				const float greenValue = static_cast<float>(pixelValue[1]) / 255.0f;
				const float blueValue = static_cast<float>(pixelValue[0]) / 255.0f;
				inputData[pixelOffset] = (redValue - meanValues[0]) / stdValues[0];
				inputData[imageArea + pixelOffset] = (greenValue - meanValues[1]) / stdValues[1];
				inputData[imageArea * 2 + pixelOffset] = (blueValue - meanValues[2]) / stdValues[2];
			}
		}

		return true;
	}

	std::vector<float> ConvertBgrMatToDetInput(const cv::Mat& bgrImage)
	{
		std::vector<float> inputData;
		FillBgrMatToDetInput(bgrImage, inputData, false);
		return inputData;
	}

	void FillBgrMatToRecInput(const cv::Mat& bgrImage, int recImageHeight, int recImageWidth, float* inputData, bool enableOpenMPParallel)
	{
		if (bgrImage.empty() || inputData == nullptr)
		{
			return;
		}

		const int safeRecImageHeight = std::max(1, recImageHeight);
		const int safeRecImageWidth = std::max(1, recImageWidth);
		const double widthHeightRatio = bgrImage.rows > 0 ? static_cast<double>(bgrImage.cols) / static_cast<double>(bgrImage.rows) : 1.0;
		const int resizedWidth = std::max(1, std::min(safeRecImageWidth, static_cast<int>(std::ceil(static_cast<double>(safeRecImageHeight) * widthHeightRatio))));

		cv::Mat resizedImage;
		const int interpolationMode = (resizedWidth < bgrImage.cols || safeRecImageHeight < bgrImage.rows) ? cv::INTER_AREA : cv::INTER_LINEAR;
		cv::resize(bgrImage, resizedImage, cv::Size(resizedWidth, safeRecImageHeight), 0.0, 0.0, interpolationMode);

		const size_t imageArea = static_cast<size_t>(safeRecImageHeight) * safeRecImageWidth;
#ifdef _OPENMP
#pragma omp parallel for if(enableOpenMPParallel && safeRecImageHeight >= 32)
#endif
		for (int row = 0; row < safeRecImageHeight; row++)
		{
			const cv::Vec3b* rowData = resizedImage.ptr<cv::Vec3b>(row);
			for (int col = 0; col < resizedWidth; col++)
			{
				const size_t pixelOffset = static_cast<size_t>(row) * safeRecImageWidth + col;
				const cv::Vec3b& pixelValue = rowData[col];
				inputData[pixelOffset] = static_cast<float>(pixelValue[2]) / 127.5f - 1.0f;
				inputData[imageArea + pixelOffset] = static_cast<float>(pixelValue[1]) / 127.5f - 1.0f;
				inputData[imageArea * 2 + pixelOffset] = static_cast<float>(pixelValue[0]) / 127.5f - 1.0f;
			}
		}
	}

	std::vector<float> ConvertBgrMatToRecInput(const cv::Mat& bgrImage, int recImageHeight, int recImageWidth)
	{
		const int safeRecImageHeight = std::max(1, recImageHeight);
		const int safeRecImageWidth = std::max(1, recImageWidth);
		size_t imageArea = 0;
		if (!TryMultiplySize(static_cast<size_t>(safeRecImageHeight), static_cast<size_t>(safeRecImageWidth), imageArea))
		{
			return std::vector<float>();
		}

		size_t inputValueCount = 0;
		if (!TryMultiplySize(static_cast<size_t>(3), imageArea, inputValueCount))
		{
			return std::vector<float>();
		}

		std::vector<float> inputData(inputValueCount, 0.0f);
		FillBgrMatToRecInput(bgrImage, safeRecImageHeight, safeRecImageWidth, inputData.data(), false);
		return inputData;
	}

	double Distance(const cv::Point2f& firstPoint, const cv::Point2f& secondPoint);

	std::vector<cv::Point2f> OrderBoxPoints(const std::vector<cv::Point2f>& inputPoints)
	{
		std::vector<cv::Point2f> points = inputPoints;
		if (points.size() != 4)
		{
			return points;
		}

		std::vector<cv::Point2f> orderedPoints(4);
		auto minSumIter = std::min_element(points.begin(), points.end(), [](const cv::Point2f& leftPoint, const cv::Point2f& rightPoint)
			{
				return leftPoint.x + leftPoint.y < rightPoint.x + rightPoint.y;
			});
		auto maxSumIter = std::max_element(points.begin(), points.end(), [](const cv::Point2f& leftPoint, const cv::Point2f& rightPoint)
			{
				return leftPoint.x + leftPoint.y < rightPoint.x + rightPoint.y;
			});
		auto minDiffIter = std::min_element(points.begin(), points.end(), [](const cv::Point2f& leftPoint, const cv::Point2f& rightPoint)
			{
				return leftPoint.y - leftPoint.x < rightPoint.y - rightPoint.x;
			});
		auto maxDiffIter = std::max_element(points.begin(), points.end(), [](const cv::Point2f& leftPoint, const cv::Point2f& rightPoint)
			{
				return leftPoint.y - leftPoint.x < rightPoint.y - rightPoint.x;
			});

		orderedPoints[0] = *minSumIter;
		orderedPoints[1] = *minDiffIter;
		orderedPoints[2] = *maxSumIter;
		orderedPoints[3] = *maxDiffIter;

		const double diagonalLength1 = Distance(orderedPoints[0], orderedPoints[2]);
		const double diagonalLength2 = Distance(orderedPoints[1], orderedPoints[3]);
		if (diagonalLength1 <= 1e-6 || diagonalLength2 <= 1e-6)
		{
			std::sort(points.begin(), points.end(), [](const cv::Point2f& leftPoint, const cv::Point2f& rightPoint)
				{
					return leftPoint.x < rightPoint.x;
				});

			std::vector<cv::Point2f> leftPoints(points.begin(), points.begin() + 2);
			std::vector<cv::Point2f> rightPoints(points.begin() + 2, points.end());
			std::sort(leftPoints.begin(), leftPoints.end(), [](const cv::Point2f& leftPoint, const cv::Point2f& rightPoint)
				{
					return leftPoint.y < rightPoint.y;
				});
			std::sort(rightPoints.begin(), rightPoints.end(), [](const cv::Point2f& leftPoint, const cv::Point2f& rightPoint)
				{
					return leftPoint.y < rightPoint.y;
				});
			orderedPoints[0] = leftPoints[0];
			orderedPoints[1] = rightPoints[0];
			orderedPoints[2] = rightPoints[1];
			orderedPoints[3] = leftPoints[1];
		}

		return orderedPoints;
	}

	double Distance(const cv::Point2f& firstPoint, const cv::Point2f& secondPoint)
	{
		const double dx = static_cast<double>(firstPoint.x) - static_cast<double>(secondPoint.x);
		const double dy = static_cast<double>(firstPoint.y) - static_cast<double>(secondPoint.y);
		return std::sqrt(dx * dx + dy * dy);
	}

	cv::Mat CropTextImage(const cv::Mat& sourceImage, const std::vector<cv::Point2f>& boxPoints, int textImagePadding)
	{
		if (sourceImage.empty() || boxPoints.size() != 4)
		{
			return cv::Mat();
		}

		const std::vector<cv::Point2f> orderedPoints = OrderBoxPoints(boxPoints);
		const int cropWidth = std::max(1, static_cast<int>(std::round(std::max(Distance(orderedPoints[0], orderedPoints[1]), Distance(orderedPoints[2], orderedPoints[3])))));
		const int cropHeight = std::max(1, static_cast<int>(std::round(std::max(Distance(orderedPoints[0], orderedPoints[3]), Distance(orderedPoints[1], orderedPoints[2])))));

		std::vector<cv::Point2f> dstPoints;
		dstPoints.push_back(cv::Point2f(0.0f, 0.0f));
		dstPoints.push_back(cv::Point2f(static_cast<float>(cropWidth - 1), 0.0f));
		dstPoints.push_back(cv::Point2f(static_cast<float>(cropWidth - 1), static_cast<float>(cropHeight - 1)));
		dstPoints.push_back(cv::Point2f(0.0f, static_cast<float>(cropHeight - 1)));

		const cv::Mat perspectiveMatrix = cv::getPerspectiveTransform(orderedPoints, dstPoints);
		cv::Mat textImage;
		cv::warpPerspective(sourceImage, textImage, perspectiveMatrix, cv::Size(cropWidth, cropHeight), cv::INTER_CUBIC, cv::BORDER_REPLICATE);
		if (textImage.rows >= textImage.cols * 1.5)
		{
			cv::Mat rotatedImage;
			cv::rotate(textImage, rotatedImage, cv::ROTATE_90_CLOCKWISE);
			textImage = rotatedImage;
		}

		if (textImagePadding > 0 && !textImage.empty())
		{
			cv::Mat paddedTextImage;
			cv::copyMakeBorder(textImage, paddedTextImage, textImagePadding, textImagePadding, textImagePadding, textImagePadding, cv::BORDER_REPLICATE);
			if (!paddedTextImage.empty())
			{
				textImage = paddedTextImage;
			}
		}

		return textImage;
	}

	double BoxScoreFast(const cv::Mat& probabilityMap, const std::vector<cv::Point2f>& boxPoints)
	{
		if (probabilityMap.empty() || boxPoints.empty())
		{
			return 0.0;
		}

		float minXValue = boxPoints[0].x;
		float maxXValue = boxPoints[0].x;
		float minYValue = boxPoints[0].y;
		float maxYValue = boxPoints[0].y;
		for (const cv::Point2f& point : boxPoints)
		{
			minXValue = std::min(minXValue, point.x);
			maxXValue = std::max(maxXValue, point.x);
			minYValue = std::min(minYValue, point.y);
			maxYValue = std::max(maxYValue, point.y);
		}

		const int minX = ClampInt(static_cast<int>(std::floor(minXValue)), 0, probabilityMap.cols - 1);
		const int maxX = ClampInt(static_cast<int>(std::ceil(maxXValue)), 0, probabilityMap.cols - 1);
		const int minY = ClampInt(static_cast<int>(std::floor(minYValue)), 0, probabilityMap.rows - 1);
		const int maxY = ClampInt(static_cast<int>(std::ceil(maxYValue)), 0, probabilityMap.rows - 1);
		if (maxX < minX || maxY < minY)
		{
			return 0.0;
		}

		std::vector<cv::Point> localPoints;
		localPoints.reserve(boxPoints.size());
		for (const cv::Point2f& point : boxPoints)
		{
			localPoints.push_back(cv::Point(static_cast<int>(std::round(point.x)) - minX, static_cast<int>(std::round(point.y)) - minY));
		}

		std::vector<std::vector<cv::Point>> localContours(1);
		localContours[0].swap(localPoints);
		const int rectWidth = maxX - minX + 1;
		const int rectHeight = maxY - minY + 1;
		cv::Mat mask = cv::Mat::zeros(rectHeight, rectWidth, CV_8UC1);
		cv::fillPoly(mask, localContours, cv::Scalar(1));
		const cv::Mat croppedProbabilityMap = probabilityMap(cv::Rect(minX, minY, rectWidth, rectHeight));
		const cv::Scalar meanValue = cv::mean(croppedProbabilityMap, mask);
		return meanValue[0];
	}

	double BoxScoreSlow(const cv::Mat& probabilityMap, const std::vector<cv::Point>& contour)
	{
		if (probabilityMap.empty() || contour.empty())
		{
			return 0.0;
		}

		cv::Rect contourRectangle = cv::boundingRect(contour);
		contourRectangle &= cv::Rect(0, 0, probabilityMap.cols, probabilityMap.rows);
		if (contourRectangle.width <= 0 || contourRectangle.height <= 0)
		{
			return 0.0;
		}

		std::vector<cv::Point> localContour;
		localContour.reserve(contour.size());
		for (const cv::Point& point : contour)
		{
			localContour.push_back(cv::Point(point.x - contourRectangle.x, point.y - contourRectangle.y));
		}

		std::vector<std::vector<cv::Point>> localContours(1);
		localContours[0].swap(localContour);
		cv::Mat mask = cv::Mat::zeros(contourRectangle.height, contourRectangle.width, CV_8UC1);
		cv::fillPoly(mask, localContours, cv::Scalar(1));

		const cv::Mat croppedProbabilityMap = probabilityMap(contourRectangle);
		const cv::Scalar meanValue = cv::mean(croppedProbabilityMap, mask);
		return meanValue[0];
	}

	DetectedTextBox MakeBoxFromContour(const cv::Mat& probabilityMap, const std::vector<cv::Point>& contour, double detDbBoxThresh, double detDbUnclipRatio, double detMinBoxSideLen, GB_OCRTextDetectionScoreMode detScoreMode, int originalImageWidth, int originalImageHeight)
	{
		DetectedTextBox textBox;
		if (probabilityMap.empty() || contour.empty() || originalImageWidth <= 0 || originalImageHeight <= 0)
		{
			return textBox;
		}

		const double contourArea = std::fabs(cv::contourArea(contour));
		const double contourPerimeter = cv::arcLength(contour, true);
		if (contourArea <= 1.0 || contourPerimeter <= 1.0)
		{
			return textBox;
		}

		cv::RotatedRect rotatedRect = cv::minAreaRect(contour);
		if (std::min(rotatedRect.size.width, rotatedRect.size.height) < detMinBoxSideLen)
		{
			return textBox;
		}

		cv::Point2f rectanglePoints[4];
		rotatedRect.points(rectanglePoints);
		std::vector<cv::Point2f> scorePoints(rectanglePoints, rectanglePoints + 4);
		scorePoints = OrderBoxPoints(scorePoints);
		textBox.score = detScoreMode == GB_OCRTextDetectionScoreMode::Slow ? BoxScoreSlow(probabilityMap, contour) : BoxScoreFast(probabilityMap, scorePoints);
		if (!std::isfinite(textBox.score) || textBox.score < detDbBoxThresh)
		{
			return textBox;
		}

		const double distance = contourArea * detDbUnclipRatio / contourPerimeter;
		rotatedRect.size.width = static_cast<float>(std::max(1.0, static_cast<double>(rotatedRect.size.width) + distance * 2.0));
		rotatedRect.size.height = static_cast<float>(std::max(1.0, static_cast<double>(rotatedRect.size.height) + distance * 2.0));

		rotatedRect.points(rectanglePoints);
		std::vector<cv::Point2f> points(rectanglePoints, rectanglePoints + 4);
		points = OrderBoxPoints(points);
		if (std::min(rotatedRect.size.width, rotatedRect.size.height) < detMinBoxSideLen + 2.0)
		{
			textBox.points.clear();
			return textBox;
		}

		const double outputToOriginalScaleX = static_cast<double>(originalImageWidth) / static_cast<double>(probabilityMap.cols);
		const double outputToOriginalScaleY = static_cast<double>(originalImageHeight) / static_cast<double>(probabilityMap.rows);
		for (cv::Point2f& point : points)
		{
			const double scaledX = point.x * outputToOriginalScaleX;
			const double scaledY = point.y * outputToOriginalScaleY;
			point.x = static_cast<float>(ClampDouble(scaledX, 0.0, static_cast<double>(originalImageWidth - 1)));
			point.y = static_cast<float>(ClampDouble(scaledY, 0.0, static_cast<double>(originalImageHeight - 1)));
		}

		textBox.points = points;
		return textBox;
	}

	double GetMinBoxSideLen(const std::vector<cv::Point2f>& points)
	{
		if (points.size() != 4)
		{
			return 0.0;
		}

		const double width = std::max(Distance(points[0], points[1]), Distance(points[2], points[3]));
		const double height = std::max(Distance(points[0], points[3]), Distance(points[1], points[2]));
		return std::min(width, height);
	}

	double GetConvexPolygonArea(const std::vector<cv::Point2f>& points)
	{
		if (points.size() < 3)
		{
			return 0.0;
		}

		double area = 0.0;
		for (size_t pointIndex = 0; pointIndex < points.size(); pointIndex++)
		{
			const cv::Point2f& currentPoint = points[pointIndex];
			const cv::Point2f& nextPoint = points[(pointIndex + 1) % points.size()];
			area += static_cast<double>(currentPoint.x) * nextPoint.y - static_cast<double>(nextPoint.x) * currentPoint.y;
		}

		return std::fabs(area) * 0.5;
	}

	bool AreBoundingRectanglesDisjoint(const std::vector<cv::Point2f>& firstPoints, const std::vector<cv::Point2f>& secondPoints)
	{
		if (firstPoints.empty() || secondPoints.empty())
		{
			return true;
		}

		float firstMinX = firstPoints[0].x;
		float firstMaxX = firstPoints[0].x;
		float firstMinY = firstPoints[0].y;
		float firstMaxY = firstPoints[0].y;
		for (const cv::Point2f& point : firstPoints)
		{
			firstMinX = std::min(firstMinX, point.x);
			firstMaxX = std::max(firstMaxX, point.x);
			firstMinY = std::min(firstMinY, point.y);
			firstMaxY = std::max(firstMaxY, point.y);
		}

		float secondMinX = secondPoints[0].x;
		float secondMaxX = secondPoints[0].x;
		float secondMinY = secondPoints[0].y;
		float secondMaxY = secondPoints[0].y;
		for (const cv::Point2f& point : secondPoints)
		{
			secondMinX = std::min(secondMinX, point.x);
			secondMaxX = std::max(secondMaxX, point.x);
			secondMinY = std::min(secondMinY, point.y);
			secondMaxY = std::max(secondMaxY, point.y);
		}

		return firstMaxX < secondMinX || secondMaxX < firstMinX || firstMaxY < secondMinY || secondMaxY < firstMinY;
	}

	double GetTextBoxIoU(const DetectedTextBox& firstBox, const DetectedTextBox& secondBox)
	{
		if (firstBox.points.size() != 4 || secondBox.points.size() != 4)
		{
			return 0.0;
		}

		if (AreBoundingRectanglesDisjoint(firstBox.points, secondBox.points))
		{
			return 0.0;
		}

		std::vector<cv::Point2f> firstPoints = OrderBoxPoints(firstBox.points);
		std::vector<cv::Point2f> secondPoints = OrderBoxPoints(secondBox.points);
		const double firstArea = GetConvexPolygonArea(firstPoints);
		const double secondArea = GetConvexPolygonArea(secondPoints);
		if (firstArea <= 1e-6 || secondArea <= 1e-6)
		{
			return 0.0;
		}

		std::vector<cv::Point2f> intersectionPoints;
		const double intersectionArea = cv::intersectConvexConvex(firstPoints, secondPoints, intersectionPoints, true);
		if (intersectionArea <= 1e-6)
		{
			return 0.0;
		}

		const double unionArea = firstArea + secondArea - intersectionArea;
		return unionArea > 1e-6 ? ClampDouble(intersectionArea / unionArea, 0.0, 1.0) : 0.0;
	}

	void SuppressDuplicatedTextBoxes(std::vector<DetectedTextBox>& textBoxes, double nmsThresh)
	{
		if (textBoxes.size() <= 1 || nmsThresh <= 0.0)
		{
			return;
		}

		std::vector<size_t> sortedIndices(textBoxes.size());
		std::iota(sortedIndices.begin(), sortedIndices.end(), static_cast<size_t>(0));
		std::sort(sortedIndices.begin(), sortedIndices.end(), [&textBoxes](size_t leftIndex, size_t rightIndex)
			{
				return textBoxes[leftIndex].score > textBoxes[rightIndex].score;
			});

		std::vector<unsigned char> suppressed(textBoxes.size(), 0);
		std::vector<DetectedTextBox> keptBoxes;
		keptBoxes.reserve(textBoxes.size());
		for (size_t orderIndex = 0; orderIndex < sortedIndices.size(); orderIndex++)
		{
			const size_t currentIndex = sortedIndices[orderIndex];
			if (suppressed[currentIndex])
			{
				continue;
			}

			keptBoxes.push_back(textBoxes[currentIndex]);
			for (size_t compareIndex = orderIndex + 1; compareIndex < sortedIndices.size(); compareIndex++)
			{
				const size_t otherIndex = sortedIndices[compareIndex];
				if (!suppressed[otherIndex] && GetTextBoxIoU(textBoxes[currentIndex], textBoxes[otherIndex]) > nmsThresh)
				{
					suppressed[otherIndex] = 1;
				}
			}
		}

		textBoxes.swap(keptBoxes);
	}

	void OffsetDetectedTextBoxes(std::vector<DetectedTextBox>& textBoxes, double offsetX, double offsetY, int imageWidth, int imageHeight)
	{
		if (textBoxes.empty() || imageWidth <= 0 || imageHeight <= 0)
		{
			return;
		}

		for (DetectedTextBox& textBox : textBoxes)
		{
			for (cv::Point2f& point : textBox.points)
			{
				point.x = static_cast<float>(ClampDouble(static_cast<double>(point.x) + offsetX, 0.0, static_cast<double>(imageWidth - 1)));
				point.y = static_cast<float>(ClampDouble(static_cast<double>(point.y) + offsetY, 0.0, static_cast<double>(imageHeight - 1)));
			}
		}
	}

	std::vector<std::pair<int, int>> BuildLongImageSliceRanges(int longSideLength, int sliceLength, int overlapLength)
	{
		std::vector<std::pair<int, int>> ranges;
		if (longSideLength <= 0 || sliceLength <= 0)
		{
			return ranges;
		}

		const int safeSliceLength = std::min(longSideLength, std::max(32, sliceLength));
		const int safeOverlapLength = ClampInt(overlapLength, 0, std::max(0, safeSliceLength - 32));
		const int stepLength = std::max(1, safeSliceLength - safeOverlapLength);

		int startPos = 0;
		while (startPos < longSideLength)
		{
			const int endPos = std::min(longSideLength, startPos + safeSliceLength);
			if (endPos <= startPos)
			{
				break;
			}
			if (!ranges.empty() && ranges.back().first == startPos)
			{
				break;
			}

			ranges.push_back(std::make_pair(startPos, endPos));
			if (endPos >= longSideLength)
			{
				break;
			}

			int nextStartPos = startPos + stepLength;
			if (nextStartPos + safeSliceLength > longSideLength)
			{
				nextStartPos = std::max(0, longSideLength - safeSliceLength);
			}
			if (nextStartPos <= startPos)
			{
				nextStartPos = startPos + stepLength;
			}
			startPos = nextStartPos;
		}

		return ranges;
	}

	std::vector<const char*> MakeNamePointers(const std::vector<std::string>& names)
	{
		std::vector<const char*> namePointers;
		namePointers.reserve(names.size());
		for (const std::string& name : names)
		{
			namePointers.push_back(name.c_str());
		}

		return namePointers;
	}

	std::vector<std::string> GetSessionInputNames(Ort::Session& session)
	{
		Ort::AllocatorWithDefaultOptions allocator;
		std::vector<std::string> names;
		const size_t inputCount = session.GetInputCount();
		names.reserve(inputCount);
		for (size_t inputIndex = 0; inputIndex < inputCount; inputIndex++)
		{
			Ort::AllocatedStringPtr name = session.GetInputNameAllocated(inputIndex, allocator);
			names.push_back(name.get() ? name.get() : "");
		}

		return names;
	}

	std::vector<std::string> GetSessionOutputNames(Ort::Session& session)
	{
		Ort::AllocatorWithDefaultOptions allocator;
		std::vector<std::string> names;
		const size_t outputCount = session.GetOutputCount();
		names.reserve(outputCount);
		for (size_t outputIndex = 0; outputIndex < outputCount; outputIndex++)
		{
			Ort::AllocatedStringPtr name = session.GetOutputNameAllocated(outputIndex, allocator);
			names.push_back(name.get() ? name.get() : "");
		}

		return names;
	}


	std::vector<int64_t> GetSessionInputShape(Ort::Session& session, size_t inputIndex)
	{
		if (inputIndex >= session.GetInputCount())
		{
			return std::vector<int64_t>();
		}

		Ort::TypeInfo inputTypeInfo = session.GetInputTypeInfo(inputIndex);
		Ort::ConstTensorTypeAndShapeInfo tensorInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
		return tensorInfo.GetShape();
	}

	ONNXTensorElementDataType GetSessionInputElementType(Ort::Session& session, size_t inputIndex)
	{
		if (inputIndex >= session.GetInputCount())
		{
			return ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
		}

		Ort::TypeInfo inputTypeInfo = session.GetInputTypeInfo(inputIndex);
		Ort::ConstTensorTypeAndShapeInfo tensorInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
		return tensorInfo.GetElementType();
	}

	void ResolveDetectionInputOptions(Ort::Session& detSession, int& fixedDetImageHeight, int& fixedDetImageWidth)
	{
		fixedDetImageHeight = 0;
		fixedDetImageWidth = 0;

		const std::vector<int64_t> inputShape = GetSessionInputShape(detSession, 0);
		if (inputShape.size() != 4)
		{
			return;
		}

		if (inputShape[2] > 0 && CanConvertInt64ToInt(inputShape[2]))
		{
			fixedDetImageHeight = static_cast<int>(inputShape[2]);
		}
		if (inputShape[3] > 0 && CanConvertInt64ToInt(inputShape[3]))
		{
			fixedDetImageWidth = static_cast<int>(inputShape[3]);
		}
	}

	void ResolveRecognitionInputOptions(Ort::Session& recSession, GB_OCROptions& options, int& runtimeRecBatchSize, int& fixedRecBatchSize, bool& isRecImageWidthDynamic)
	{
		runtimeRecBatchSize = std::max(1, options.recBatchSize);
		fixedRecBatchSize = 0;
		isRecImageWidthDynamic = false;

		const std::vector<int64_t> inputShape = GetSessionInputShape(recSession, 0);
		if (inputShape.size() != 4)
		{
			return;
		}

		if (inputShape[2] > 0 && CanConvertInt64ToInt(inputShape[2]))
		{
			options.recImageHeight = static_cast<int>(inputShape[2]);
		}
		if (inputShape[3] > 0 && CanConvertInt64ToInt(inputShape[3]))
		{
			options.recImageWidth = static_cast<int>(inputShape[3]);
			options.recMaxDynamicImageWidth = std::max(options.recImageWidth, options.recMaxDynamicImageWidth);
		}
		else
		{
			isRecImageWidthDynamic = true;
		}

		if (inputShape[0] > 0 && CanConvertInt64ToInt(inputShape[0]))
		{
			fixedRecBatchSize = static_cast<int>(inputShape[0]);
			runtimeRecBatchSize = std::max(1, fixedRecBatchSize);
		}
	}

	void ResolveTextLineOrientationInputOptions(Ort::Session& clsSession, GB_OCROptions& options, int& runtimeClsBatchSize, int& fixedClsBatchSize)
	{
		runtimeClsBatchSize = std::max(1, options.clsBatchSize);
		fixedClsBatchSize = 0;

		const std::vector<int64_t> inputShape = GetSessionInputShape(clsSession, 0);
		if (inputShape.size() != 4)
		{
			return;
		}

		if (inputShape[2] > 0 && CanConvertInt64ToInt(inputShape[2]))
		{
			options.clsImageHeight = static_cast<int>(inputShape[2]);
		}
		if (inputShape[3] > 0 && CanConvertInt64ToInt(inputShape[3]))
		{
			options.clsImageWidth = static_cast<int>(inputShape[3]);
		}
		if (inputShape[0] > 0 && CanConvertInt64ToInt(inputShape[0]))
		{
			fixedClsBatchSize = static_cast<int>(inputShape[0]);
			runtimeClsBatchSize = std::max(1, fixedClsBatchSize);
		}
	}

	float GetStableMaxProbability(const float* values, int valueCount, int maxIndex, float maxValue, float minValue)
	{
		if (values == nullptr || valueCount <= 0 || maxIndex < 0 || maxIndex >= valueCount || !std::isfinite(maxValue) || !std::isfinite(minValue))
		{
			return 0.0f;
		}

		if (minValue >= 0.0f && maxValue <= 1.0f)
		{
			double probabilitySum = 0.0;
			for (int valueIndex = 0; valueIndex < valueCount; valueIndex++)
			{
				if (!std::isfinite(values[valueIndex]) || values[valueIndex] < 0.0f || values[valueIndex] > 1.0f)
				{
					probabilitySum = -1.0;
					break;
				}
				probabilitySum += static_cast<double>(values[valueIndex]);
			}

			const double probabilitySumTolerance = std::max(1e-4, static_cast<double>(valueCount) * static_cast<double>(std::numeric_limits<float>::epsilon()) * 8.0);
			if (probabilitySum >= 0.0 && std::abs(probabilitySum - 1.0) <= probabilitySumTolerance)
			{
				return maxValue;
			}
		}

		double sumExp = 0.0;
		for (int valueIndex = 0; valueIndex < valueCount; valueIndex++)
		{
			if (!std::isfinite(values[valueIndex]))
			{
				return 0.0f;
			}
			sumExp += std::exp(static_cast<double>(values[valueIndex]) - static_cast<double>(maxValue));
		}

		if (!std::isfinite(sumExp) || sumExp <= 0.0)
		{
			return 0.0f;
		}

		return static_cast<float>(1.0 / sumExp);
	}

	bool HasEnoughTensorValues(const std::vector<int64_t>& shape, size_t requiredValueCount)
	{
		if (requiredValueCount == 0)
		{
			return true;
		}

		size_t valueCount = 1;
		for (const int64_t dimension : shape)
		{
			if (dimension <= 0)
			{
				return false;
			}

			const size_t safeDimension = static_cast<size_t>(dimension);
			if (valueCount > (std::numeric_limits<size_t>::max)() / safeDimension)
			{
				return false;
			}

			valueCount *= safeDimension;
		}

		return valueCount >= requiredValueCount;
	}

	bool TryResolveDetectionOutputMapShape(const std::vector<int64_t>& outputShape, size_t outputElementCount, int& outputHeight, int& outputWidth)
	{
		outputHeight = 0;
		outputWidth = 0;
		if (outputShape.empty())
		{
			return false;
		}

		auto assignIfValid = [&outputHeight, &outputWidth, outputElementCount](int64_t heightValue, int64_t widthValue) -> bool
			{
				if (!CanConvertInt64ToInt(heightValue) || !CanConvertInt64ToInt(widthValue))
				{
					return false;
				}

				const int height = static_cast<int>(heightValue);
				const int width = static_cast<int>(widthValue);
				if (height <= 0 || width <= 0)
				{
					return false;
				}

				size_t pixelCount = 0;
				if (!TryMultiplySize(static_cast<size_t>(height), static_cast<size_t>(width), pixelCount) || outputElementCount < pixelCount)
				{
					return false;
				}

				outputHeight = height;
				outputWidth = width;
				return true;
			};

		if (outputShape.size() == 4)
		{
			if (outputShape[1] == 1 && assignIfValid(outputShape[2], outputShape[3]))
			{
				return true;
			}
			if (outputShape[3] == 1 && assignIfValid(outputShape[1], outputShape[2]))
			{
				return true;
			}
		}
		else if (outputShape.size() == 3)
		{
			if (outputShape[0] == 1 && assignIfValid(outputShape[1], outputShape[2]))
			{
				return true;
			}
			if (outputShape[2] == 1 && assignIfValid(outputShape[0], outputShape[1]))
			{
				return true;
			}
		}
		else if (outputShape.size() == 2)
		{
			return assignIfValid(outputShape[0], outputShape[1]);
		}

		return false;
	}

	bool TryResolveRecognitionOutputInfo(const std::vector<int64_t>& outputShape, size_t outputElementCount, size_t expectedBatchSize, size_t modelBatchSize, RecognitionOutputInfo& outputInfo)
	{
		outputInfo = RecognitionOutputInfo();
		if (outputShape.size() == 2)
		{
			if (expectedBatchSize != 1 || !CanConvertInt64ToInt(outputShape[0]) || !CanConvertInt64ToInt(outputShape[1]))
			{
				return false;
			}

			outputInfo.layout = RecognitionOutputLayout::TimeClass;
			outputInfo.outputBatchSize = 1;
			outputInfo.timeSteps = static_cast<int>(outputShape[0]);
			outputInfo.classCount = static_cast<int>(outputShape[1]);
			return outputInfo.timeSteps > 0 && outputInfo.classCount > 1 && outputElementCount >= static_cast<size_t>(outputInfo.timeSteps) * outputInfo.classCount;
		}

		if (outputShape.size() != 3 || !CanConvertInt64ToInt(outputShape[0]) || !CanConvertInt64ToInt(outputShape[1]) || !CanConvertInt64ToInt(outputShape[2]))
		{
			return false;
		}

		const size_t firstDimension = static_cast<size_t>(outputShape[0]);
		const size_t secondDimension = static_cast<size_t>(outputShape[1]);
		const int thirdDimension = static_cast<int>(outputShape[2]);
		if (firstDimension == 0 || secondDimension == 0 || thirdDimension <= 1)
		{
			return false;
		}

		const bool firstDimensionLooksLikeBatch = firstDimension == expectedBatchSize || firstDimension == modelBatchSize || firstDimension == 1;
		const bool secondDimensionLooksLikeBatch = secondDimension == expectedBatchSize || secondDimension == modelBatchSize || secondDimension == 1;
		if (secondDimensionLooksLikeBatch && !firstDimensionLooksLikeBatch)
		{
			outputInfo.layout = RecognitionOutputLayout::TimeBatchClass;
			outputInfo.outputBatchSize = secondDimension;
			outputInfo.timeSteps = static_cast<int>(firstDimension);
		}
		else
		{
			outputInfo.layout = RecognitionOutputLayout::BatchTimeClass;
			outputInfo.outputBatchSize = firstDimension;
			outputInfo.timeSteps = static_cast<int>(secondDimension);
		}

		outputInfo.classCount = thirdDimension;
		size_t sampleOutputValueCount = 0;
		size_t requiredOutputValueCount = 0;
		return outputInfo.outputBatchSize >= expectedBatchSize && outputInfo.timeSteps > 0 && outputInfo.classCount > 1 && TryMultiplySize(static_cast<size_t>(outputInfo.timeSteps), static_cast<size_t>(outputInfo.classCount), sampleOutputValueCount) && TryMultiplySize(sampleOutputValueCount, outputInfo.outputBatchSize, requiredOutputValueCount) && outputElementCount >= requiredOutputValueCount;
	}

	const float* GetRecognitionTimeStepData(const float* outputData, const RecognitionOutputInfo& outputInfo, size_t imageIndex, int timeIndex)
	{
		if (outputData == nullptr || timeIndex < 0 || timeIndex >= outputInfo.timeSteps || imageIndex >= outputInfo.outputBatchSize)
		{
			return nullptr;
		}

		switch (outputInfo.layout)
		{
		case RecognitionOutputLayout::BatchTimeClass:
			return outputData + (imageIndex * static_cast<size_t>(outputInfo.timeSteps) + static_cast<size_t>(timeIndex)) * outputInfo.classCount;
		case RecognitionOutputLayout::TimeBatchClass:
			return outputData + (static_cast<size_t>(timeIndex) * outputInfo.outputBatchSize + imageIndex) * outputInfo.classCount;
		case RecognitionOutputLayout::TimeClass:
			return outputData + static_cast<size_t>(timeIndex) * outputInfo.classCount;
		default:
			return nullptr;
		}
	}

	bool TryResolveBatchClassOutputInfo(const std::vector<int64_t>& outputShape, size_t outputElementCount, size_t expectedBatchSize, size_t modelBatchSize, size_t& outputBatchSize, int& classCount)
	{
		outputBatchSize = 0;
		classCount = 0;
		if (outputShape.size() == 1)
		{
			if (expectedBatchSize != 1 || !CanConvertInt64ToInt(outputShape[0]))
			{
				return false;
			}

			outputBatchSize = 1;
			classCount = static_cast<int>(outputShape[0]);
			return classCount > 1 && outputElementCount >= static_cast<size_t>(classCount);
		}

		if (outputShape.size() != 2 || !CanConvertInt64ToInt(outputShape[0]) || !CanConvertInt64ToInt(outputShape[1]))
		{
			return false;
		}

		const size_t firstDimension = static_cast<size_t>(outputShape[0]);
		const size_t secondDimension = static_cast<size_t>(outputShape[1]);
		const bool firstDimensionLooksLikeBatch = firstDimension == expectedBatchSize || firstDimension == modelBatchSize || firstDimension == 1;
		const bool secondDimensionLooksLikeBatch = secondDimension == expectedBatchSize || secondDimension == modelBatchSize || secondDimension == 1;
		size_t requiredValueCount = 0;
		const bool hasEnoughValues = TryMultiplySize(firstDimension, secondDimension, requiredValueCount) && outputElementCount >= requiredValueCount;

		if (firstDimensionLooksLikeBatch && secondDimension == 2 && hasEnoughValues)
		{
			outputBatchSize = firstDimension;
			classCount = static_cast<int>(secondDimension);
			return outputBatchSize >= expectedBatchSize;
		}

		if (secondDimensionLooksLikeBatch && firstDimension == 2 && hasEnoughValues)
		{
			outputBatchSize = secondDimension;
			classCount = static_cast<int>(firstDimension);
			return outputBatchSize >= expectedBatchSize;
		}

		if (firstDimensionLooksLikeBatch && secondDimension > 1 && hasEnoughValues)
		{
			outputBatchSize = firstDimension;
			classCount = static_cast<int>(secondDimension);
			return outputBatchSize >= expectedBatchSize;
		}

		if (secondDimensionLooksLikeBatch && firstDimension > 1 && hasEnoughValues)
		{
			outputBatchSize = secondDimension;
			classCount = static_cast<int>(firstDimension);
			return outputBatchSize >= expectedBatchSize;
		}

		return false;
	}

	const float* GetBatchClassSampleData(const float* outputData, const std::vector<int64_t>& outputShape, size_t imageIndex, size_t outputBatchSize, int classCount)
	{
		if (outputData == nullptr || imageIndex >= outputBatchSize || classCount <= 1)
		{
			return nullptr;
		}

		if (outputShape.size() == 1)
		{
			return outputData;
		}

		if (outputShape.size() == 2 && static_cast<size_t>(outputShape[0]) == outputBatchSize)
		{
			return outputData + imageIndex * static_cast<size_t>(classCount);
		}

		if (outputShape.size() == 2 && static_cast<size_t>(outputShape[1]) == outputBatchSize)
		{
			return outputData + imageIndex;
		}

		return nullptr;
	}

	float GetBatchClassScore(const float* sampleData, const std::vector<int64_t>& outputShape, size_t outputBatchSize, int classIndex, int classCount)
	{
		if (sampleData == nullptr || classIndex < 0 || classIndex >= classCount)
		{
			return 0.0f;
		}

		if (outputShape.size() == 2 && static_cast<size_t>(outputShape[1]) == outputBatchSize && static_cast<size_t>(outputShape[0]) != outputBatchSize)
		{
			return sampleData[static_cast<size_t>(classIndex) * outputBatchSize];
		}

		return sampleData[classIndex];
	}

	class GB_PPOCRv5MobileOnnxRuntimeBackend : public IGB_OCRBackend
	{
	public:
		explicit GB_PPOCRv5MobileOnnxRuntimeBackend(const GB_OCROptions& inputOptions, GB_OCRBackend inputBackendType)
			: backendType(inputBackendType), options(NormalizeOptions(inputOptions))
		{
		}

		virtual bool Initialize(std::string& errorMessage) override
		{
			errorMessage.clear();

			try
			{
				modelPaths = NormalizeModelPaths(options.ppocrv5MobileModelPaths);
				if (!HasAllRequiredPPOCRv5ModelFiles(modelPaths))
				{
					errorMessage = BuildMissingPPOCRv5ModelFileMessage(modelPaths);
					return false;
				}

				if (backendType == GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCuda)
				{
					if (!PrepareOnnxRuntimeCudaDependencyDlls(errorMessage))
					{
						return false;
					}
				}

				if (!ReadTextLinesUtf8(modelPaths.dictPathUtf8, characterDict))
				{
					errorMessage = GB_STR("读取 PP-OCRv5 字典文件失败。");
					return false;
				}

				const OrtLoggingLevel ortLogSeverityLevel = ToOrtLoggingLevel(options.onnxRuntimeLogSeverityLevel);
				ortEnv.reset(new Ort::Env(ortLogSeverityLevel, "GlobalBase.GB_OCR"));
				Ort::SessionOptions sessionOptions;
				ConfigureOnnxRuntimeSessionOptions(sessionOptions, options, backendType);
				if (backendType == GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCuda)
				{
					if (!AppendCudaExecutionProvider(sessionOptions, options, errorMessage))
					{
						ClearRuntime();
						return false;
					}

					if (!AppendCpuExecutionProvider(sessionOptions, errorMessage))
					{
						ClearRuntime();
						return false;
					}
				}

				const std::basic_string<ORTCHAR_T> detModelPath = ToOrtPath(modelPaths.detModelPathUtf8);
				const std::basic_string<ORTCHAR_T> recModelPath = ToOrtPath(modelPaths.recModelPathUtf8);
				detSession.reset(new Ort::Session(*ortEnv, detModelPath.c_str(), sessionOptions));
				recSession.reset(new Ort::Session(*ortEnv, recModelPath.c_str(), sessionOptions));
				if (options.useTextLineOrientationClassification && HasPPOCRv5TextLineOrientationModelFile(modelPaths))
				{
					const std::basic_string<ORTCHAR_T> clsModelPath = ToOrtPath(modelPaths.clsModelPathUtf8);
					clsSession.reset(new Ort::Session(*ortEnv, clsModelPath.c_str(), sessionOptions));
				}

				detInputNames = GetSessionInputNames(*detSession);
				detOutputNames = GetSessionOutputNames(*detSession);
				recInputNames = GetSessionInputNames(*recSession);
				recOutputNames = GetSessionOutputNames(*recSession);
				if (clsSession)
				{
					clsInputNames = GetSessionInputNames(*clsSession);
					clsOutputNames = GetSessionOutputNames(*clsSession);
				}
				ResolveDetectionInputOptions(*detSession, fixedDetImageHeight, fixedDetImageWidth);
				ResolveRecognitionInputOptions(*recSession, options, runtimeRecBatchSize, fixedRecBatchSize, isRecImageWidthDynamic);
				if (clsSession)
				{
					ResolveTextLineOrientationInputOptions(*clsSession, options, runtimeClsBatchSize, fixedClsBatchSize);
				}

				if (GetSessionInputElementType(*detSession, 0) != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || GetSessionInputElementType(*recSession, 0) != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || (clsSession && GetSessionInputElementType(*clsSession, 0) != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT))
				{
					errorMessage = GB_STR("ONNX 模型输入类型不正确，当前仅支持 float32 输入。");
					ClearRuntime();
					return false;
				}

				if (detInputNames.empty() || detOutputNames.empty() || recInputNames.empty() || recOutputNames.empty() || (clsSession && (clsInputNames.empty() || clsOutputNames.empty())))
				{
					errorMessage = GB_STR("ONNX 模型输入或输出节点为空。");
					ClearRuntime();
					return false;
				}

				detInputNamePointers = MakeNamePointers(detInputNames);
				detOutputNamePointers = MakeNamePointers(detOutputNames);
				recInputNamePointers = MakeNamePointers(recInputNames);
				recOutputNamePointers = MakeNamePointers(recOutputNames);
				if (clsSession)
				{
					clsInputNamePointers = MakeNamePointers(clsInputNames);
					clsOutputNamePointers = MakeNamePointers(clsOutputNames);
				}
				runOptions.reset(new Ort::RunOptions(CreateOnnxRuntimeRunOptions(options)));
			}
			catch (const Ort::Exception& exception)
			{
				errorMessage = GB_STR("ONNX Runtime 初始化失败：") + exception.what();
				ClearRuntime();
				return false;
			}
			catch (const std::exception& exception)
			{
				errorMessage = GB_STR("OCR 初始化失败：") + exception.what();
				ClearRuntime();
				return false;
			}

			return true;
		}

		virtual bool Recognize(const GB_Image& image, std::vector<GB_OCRTextBlock>& textBlocks, std::string& errorMessage) override
		{
			textBlocks.clear();
			errorMessage.clear();

			if (!detSession || !recSession)
			{
				errorMessage = GB_STR("OCR 后端尚未初始化。");
				return false;
			}

			try
			{
				const cv::Mat sourceImage = ConvertGBImageToBgrMat(image, options.blendAlphaWithWhiteBackground);
				if (sourceImage.empty())
				{
					errorMessage = GB_STR("输入图像为空，或当前图像格式不支持 OCR。");
					return false;
				}

				const cv::Mat sourceImageForDetection = options.enableDetectionContrastEnhancement ? ApplyDetectionContrastEnhancement(sourceImage, options.detClaheClipLimit, options.detClaheTileGridSize) : sourceImage;
				const cv::Mat detectionImage = AddWhiteBorderForDetection(sourceImageForDetection, options.detImagePadding);
				std::vector<DetectedTextBox> textBoxes;
				if (!DetectTextBoxes(detectionImage, textBoxes, errorMessage))
				{
					return false;
				}

				if (textBoxes.empty() && !options.enableDetectionContrastEnhancement && options.retryWithContrastEnhancementWhenNoTextDetected)
				{
					const cv::Mat enhancedSourceImageForDetection = ApplyDetectionContrastEnhancement(sourceImage, options.detClaheClipLimit, options.detClaheTileGridSize);
					if (!enhancedSourceImageForDetection.empty())
					{
						const cv::Mat enhancedDetectionImage = AddWhiteBorderForDetection(enhancedSourceImageForDetection, options.detImagePadding);
						std::vector<DetectedTextBox> enhancedTextBoxes;
						if (!DetectTextBoxes(enhancedDetectionImage, enhancedTextBoxes, errorMessage))
						{
							return false;
						}
						if (!enhancedTextBoxes.empty())
						{
							textBoxes.swap(enhancedTextBoxes);
						}
					}
				}

				ShiftDetectedTextBoxesToOriginalImage(textBoxes, options.detImagePadding, sourceImage.cols, sourceImage.rows, options.detMinBoxSideLen);

				std::vector<cv::Mat> candidateTextImages(textBoxes.size());
				std::vector<unsigned char> validTextImageFlags(textBoxes.size(), 0);
#ifdef _OPENMP
#pragma omp parallel for if(options.enableOpenMPParallel && textBoxes.size() >= 4) schedule(dynamic)
#endif
				for (int textBoxIndex = 0; textBoxIndex < static_cast<int>(textBoxes.size()); textBoxIndex++)
				{
					cv::Mat textImage = CropTextImage(sourceImage, textBoxes[static_cast<size_t>(textBoxIndex)].points, options.recImagePadding);
					textImage = ApplyRecognitionSharpen(textImage, options.recSharpenStrength);
					if (!textImage.empty())
					{
						candidateTextImages[static_cast<size_t>(textBoxIndex)] = textImage;
						validTextImageFlags[static_cast<size_t>(textBoxIndex)] = 1;
					}
				}

				std::vector<cv::Mat> textImages;
				std::vector<int> textBoxIndices;
				textImages.reserve(textBoxes.size());
				textBoxIndices.reserve(textBoxes.size());
				for (size_t textBoxIndex = 0; textBoxIndex < textBoxes.size(); textBoxIndex++)
				{
					if (!validTextImageFlags[textBoxIndex])
					{
						continue;
					}

					textImages.push_back(candidateTextImages[textBoxIndex]);
					textBoxIndices.push_back(static_cast<int>(textBoxIndex));
				}

				if (!CorrectTextLineOrientations(textImages, errorMessage))
				{
					return false;
				}

				std::vector<RecognizedText> recognizedTexts;
				if (!RecognizeTextImages(textImages, recognizedTexts, errorMessage))
				{
					return false;
				}

				for (size_t textImageIndex = 0; textImageIndex < recognizedTexts.size(); textImageIndex++)
				{
					const RecognizedText& recognizedText = recognizedTexts[textImageIndex];
					if (recognizedText.text.empty() || recognizedText.confidence < options.recConfidenceThresh)
					{
						continue;
					}

					const DetectedTextBox& textBox = textBoxes[static_cast<size_t>(textBoxIndices[textImageIndex])];
					GB_OCRTextBlock textBlock;
					textBlock.text = recognizedText.text;
					textBlock.detectionConfidence = textBox.score;
					textBlock.recognitionConfidence = recognizedText.confidence;
					textBlock.confidence = textBox.score * recognizedText.confidence;

					for (const cv::Point2f& point : textBox.points)
					{
						textBlock.polygonPoints.push_back(GB_Point2d(point.x, point.y));
					}
					textBlock.boundingRectangle = MakeBoundingRectangle(textBlock.polygonPoints);
					textBlocks.push_back(textBlock);
				}

				if (options.sortTextBlocks)
				{
					SortTextBlocksReadingOrder(textBlocks);
				}
			}
			catch (const Ort::Exception& exception)
			{
				errorMessage = GB_STR("ONNX Runtime 推理失败：") + exception.what();
				return false;
			}
			catch (const std::exception& exception)
			{
				errorMessage = GB_STR("OCR 识别失败：") + exception.what();
				return false;
			}

			return true;
		}

		virtual GB_OCRBackend GetBackendType() const override
		{
			return backendType;
		}

	private:
		bool ShouldUseLongImageDetectionSlice(const cv::Mat& sourceImage, bool verticalDirection) const
		{
			if (!options.enableLongImageDetectionSlice || sourceImage.empty() || sourceImage.rows <= 0 || sourceImage.cols <= 0)
			{
				return false;
			}

			const int longSideLength = verticalDirection ? sourceImage.rows : sourceImage.cols;
			const int shortSideLength = verticalDirection ? sourceImage.cols : sourceImage.rows;
			if (longSideLength <= shortSideLength || shortSideLength <= 0)
			{
				return false;
			}

			const double longShortRatio = static_cast<double>(longSideLength) / static_cast<double>(shortSideLength);
			const double ratioThresh = verticalDirection ? 2.0 : 3.0;
			const int sideLengthThresh = options.detMaxSideLen > 0 ? options.detMaxSideLen : std::max(32, options.detLimitSideLen);
			return longShortRatio > ratioThresh && longSideLength > sideLengthThresh;
		}

		bool DetectTextBoxes(const cv::Mat& sourceImage, std::vector<DetectedTextBox>& textBoxes, std::string& errorMessage)
		{
			textBoxes.clear();
			if (ShouldUseLongImageDetectionSlice(sourceImage, true))
			{
				return DetectTextBoxesBySlices(sourceImage, textBoxes, errorMessage, true);
			}
			if (ShouldUseLongImageDetectionSlice(sourceImage, false))
			{
				return DetectTextBoxesBySlices(sourceImage, textBoxes, errorMessage, false);
			}

			return DetectTextBoxesSingle(sourceImage, textBoxes, errorMessage);
		}

		bool DetectTextBoxesBySlices(const cv::Mat& sourceImage, std::vector<DetectedTextBox>& textBoxes, std::string& errorMessage, bool verticalDirection)
		{
			textBoxes.clear();
			errorMessage.clear();

			if (sourceImage.empty())
			{
				return true;
			}

			const int longSideLength = verticalDirection ? sourceImage.rows : sourceImage.cols;
			const int shortSideLength = verticalDirection ? sourceImage.cols : sourceImage.rows;
			const int safeDetLimitSideLen = std::max(32, options.detLimitSideLen);
			const double preferredLongShortRatio = verticalDirection ? 2.0 : 3.0;
			int preferredSliceLength = std::max(safeDetLimitSideLen, static_cast<int>(std::round(static_cast<double>(shortSideLength) * preferredLongShortRatio)));
			if (options.detMaxSideLen > 0 && safeDetLimitSideLen > 0)
			{
				const double maxSliceLengthByResize = static_cast<double>(options.detMaxSideLen) * static_cast<double>(shortSideLength) / static_cast<double>(safeDetLimitSideLen);
				preferredSliceLength = std::min(preferredSliceLength, std::max(32, static_cast<int>(std::floor(maxSliceLengthByResize))));
			}
			const int sliceLength = ClampInt(preferredSliceLength, 32, longSideLength);
			const std::vector<std::pair<int, int>> ranges = BuildLongImageSliceRanges(longSideLength, sliceLength, options.detSliceOverlap);
			if (ranges.empty() || ranges.size() == 1)
			{
				return DetectTextBoxesSingle(sourceImage, textBoxes, errorMessage);
			}

			for (const std::pair<int, int>& range : ranges)
			{
				const int startPos = range.first;
				const int endPos = range.second;
				const cv::Rect sliceRectangle = verticalDirection ? cv::Rect(0, startPos, sourceImage.cols, endPos - startPos) : cv::Rect(startPos, 0, endPos - startPos, sourceImage.rows);
				const cv::Mat sliceImage = sourceImage(sliceRectangle);

				std::vector<DetectedTextBox> sliceTextBoxes;
				if (!DetectTextBoxesSingle(sliceImage, sliceTextBoxes, errorMessage))
				{
					return false;
				}

				if (!sliceTextBoxes.empty())
				{
					OffsetDetectedTextBoxes(sliceTextBoxes, verticalDirection ? 0.0 : static_cast<double>(startPos), verticalDirection ? static_cast<double>(startPos) : 0.0, sourceImage.cols, sourceImage.rows);
					textBoxes.insert(textBoxes.end(), sliceTextBoxes.begin(), sliceTextBoxes.end());
				}
			}

			SuppressDuplicatedTextBoxes(textBoxes, options.detBoxNmsThresh);
			std::sort(textBoxes.begin(), textBoxes.end(), [](const DetectedTextBox& leftBox, const DetectedTextBox& rightBox)
				{
					const double leftY = std::accumulate(leftBox.points.begin(), leftBox.points.end(), 0.0, [](double value, const cv::Point2f& point) { return value + point.y; }) / std::max<size_t>(1, leftBox.points.size());
					const double rightY = std::accumulate(rightBox.points.begin(), rightBox.points.end(), 0.0, [](double value, const cv::Point2f& point) { return value + point.y; }) / std::max<size_t>(1, rightBox.points.size());
					if (std::fabs(leftY - rightY) > 10.0)
					{
						return leftY < rightY;
					}

					const double leftX = std::accumulate(leftBox.points.begin(), leftBox.points.end(), 0.0, [](double value, const cv::Point2f& point) { return value + point.x; }) / std::max<size_t>(1, leftBox.points.size());
					const double rightX = std::accumulate(rightBox.points.begin(), rightBox.points.end(), 0.0, [](double value, const cv::Point2f& point) { return value + point.x; }) / std::max<size_t>(1, rightBox.points.size());
					return leftX < rightX;
				});

			return true;
		}

		bool DetectTextBoxesSingle(const cv::Mat& sourceImage, std::vector<DetectedTextBox>& textBoxes, std::string& errorMessage)
		{
			textBoxes.clear();

			cv::Mat resizedImage;
			if (fixedDetImageHeight > 0 && fixedDetImageWidth > 0)
			{
				cv::resize(sourceImage, resizedImage, cv::Size(fixedDetImageWidth, fixedDetImageHeight));
			}
			else
			{
				ResizeForDetection(sourceImage, resizedImage, options.detLimitSideLen, options.detLimitByMaxSide, options.detMaxSideLen);
			}
			if (resizedImage.empty())
			{
				errorMessage = GB_STR("文本检测图像预处理失败。");
				return false;
			}

			if (!FillBgrMatToDetInput(resizedImage, detInputDataCache, options.enableOpenMPParallel))
			{
				errorMessage = GB_STR("文本检测输入张量构造失败。");
				return false;
			}

			std::array<int64_t, 4> inputShape = { 1, 3, resizedImage.rows, resizedImage.cols };
			Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
			Ort::Value inputTensor = Ort::Value::CreateTensor<float>(memoryInfo, detInputDataCache.data(), detInputDataCache.size(), inputShape.data(), inputShape.size());

			std::vector<Ort::Value> outputTensors = detSession->Run(GetRunOptions(), detInputNamePointers.data(), &inputTensor, 1, detOutputNamePointers.data(), detOutputNamePointers.size());
			if (outputTensors.empty() || !outputTensors[0].IsTensor())
			{
				errorMessage = GB_STR("文本检测模型输出为空。");
				return false;
			}

			Ort::TensorTypeAndShapeInfo outputInfo = outputTensors[0].GetTensorTypeAndShapeInfo();
			if (outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
			{
				errorMessage = GB_STR("文本检测模型输出类型不正确，当前仅支持 float32 输出。");
				return false;
			}

			const std::vector<int64_t> outputShape = outputInfo.GetShape();
			int outputHeight = 0;
			int outputWidth = 0;
			if (!TryResolveDetectionOutputMapShape(outputShape, outputInfo.GetElementCount(), outputHeight, outputWidth))
			{
				errorMessage = GB_STR("文本检测模型输出尺寸不正确，当前支持 [N,1,H,W]、[N,H,W,1]、[1,H,W]、[H,W,1] 或 [H,W]。");
				return false;
			}

			const float* outputData = outputTensors[0].GetTensorMutableData<float>();
			cv::Mat probabilityMap(outputHeight, outputWidth, CV_32FC1, const_cast<float*>(outputData));
			cv::Mat binaryMap;
			cv::compare(probabilityMap, options.detDbThresh, binaryMap, cv::CMP_GT);
			if (options.detUseDilation)
			{
				const cv::Mat dilationKernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));
				cv::dilate(binaryMap, binaryMap, dilationKernel);
			}

			std::vector<std::vector<cv::Point>> contours;
			cv::findContours(binaryMap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
			if (contours.empty())
			{
				return true;
			}

			std::vector<std::pair<double, int>> contourAreas;
			contourAreas.reserve(contours.size());
			for (size_t contourIndex = 0; contourIndex < contours.size(); contourIndex++)
			{
				contourAreas.push_back(std::make_pair(std::fabs(cv::contourArea(contours[contourIndex])), static_cast<int>(contourIndex)));
			}

			const int maxCandidateCount = options.maxCandidateTextBoxes > 0 ? std::min(options.maxCandidateTextBoxes, static_cast<int>(contourAreas.size())) : static_cast<int>(contourAreas.size());
			if (maxCandidateCount < static_cast<int>(contourAreas.size()))
			{
				std::nth_element(contourAreas.begin(), contourAreas.begin() + maxCandidateCount, contourAreas.end(), [](const std::pair<double, int>& leftArea, const std::pair<double, int>& rightArea)
					{
						return leftArea.first > rightArea.first;
					});
				contourAreas.resize(static_cast<size_t>(maxCandidateCount));
			}
			std::sort(contourAreas.begin(), contourAreas.end(), [](const std::pair<double, int>& leftArea, const std::pair<double, int>& rightArea)
				{
					return leftArea.first > rightArea.first;
				});

			for (const std::pair<double, int>& contourAreaInfo : contourAreas)
			{
				if (contourAreaInfo.first <= 1.0)
				{
					continue;
				}

				const std::vector<cv::Point>& contour = contours[static_cast<size_t>(contourAreaInfo.second)];
				const DetectedTextBox textBox = MakeBoxFromContour(probabilityMap, contour, options.detDbBoxThresh, options.detDbUnclipRatio, options.detMinBoxSideLen, options.detScoreMode, sourceImage.cols, sourceImage.rows);
				if (textBox.points.size() != 4 || textBox.score < options.detDbBoxThresh)
				{
					continue;
				}

				if (GetMinBoxSideLen(textBox.points) < options.detMinBoxSideLen)
				{
					continue;
				}

				textBoxes.push_back(textBox);
			}

			SuppressDuplicatedTextBoxes(textBoxes, options.detBoxNmsThresh);

			std::sort(textBoxes.begin(), textBoxes.end(), [](const DetectedTextBox& leftBox, const DetectedTextBox& rightBox)
				{
					const double leftY = std::accumulate(leftBox.points.begin(), leftBox.points.end(), 0.0, [](double value, const cv::Point2f& point) { return value + point.y; }) / std::max<size_t>(1, leftBox.points.size());
					const double rightY = std::accumulate(rightBox.points.begin(), rightBox.points.end(), 0.0, [](double value, const cv::Point2f& point) { return value + point.y; }) / std::max<size_t>(1, rightBox.points.size());
					if (std::fabs(leftY - rightY) > 10.0)
					{
						return leftY < rightY;
					}

					const double leftX = std::accumulate(leftBox.points.begin(), leftBox.points.end(), 0.0, [](double value, const cv::Point2f& point) { return value + point.x; }) / std::max<size_t>(1, leftBox.points.size());
					const double rightX = std::accumulate(rightBox.points.begin(), rightBox.points.end(), 0.0, [](double value, const cv::Point2f& point) { return value + point.x; }) / std::max<size_t>(1, rightBox.points.size());
					return leftX < rightX;
				});

			return true;
		}

		bool DecodeTextLineOrientation(const float* outputData, const std::vector<int64_t>& outputShape, size_t outputBatchSize, int classCount, size_t imageIndex, TextLineOrientation& orientation)
		{
			orientation = TextLineOrientation();
			const float* sampleData = GetBatchClassSampleData(outputData, outputShape, imageIndex, outputBatchSize, classCount);
			if (sampleData == nullptr)
			{
				return false;
			}

			std::vector<float> classScores(static_cast<size_t>(classCount), 0.0f);
			int maxIndex = 0;
			float maxScore = GetBatchClassScore(sampleData, outputShape, outputBatchSize, 0, classCount);
			float minScore = maxScore;
			classScores[0] = maxScore;
			for (int classIndex = 1; classIndex < classCount; classIndex++)
			{
				const float currentScore = GetBatchClassScore(sampleData, outputShape, outputBatchSize, classIndex, classCount);
				classScores[static_cast<size_t>(classIndex)] = currentScore;
				if (currentScore > maxScore)
				{
					maxScore = currentScore;
					maxIndex = classIndex;
				}
				minScore = std::min(minScore, currentScore);
			}

			orientation.angle = maxIndex == 1 ? 180 : 0;
			orientation.confidence = ClampDouble(GetStableMaxProbability(classScores.data(), classCount, maxIndex, maxScore, minScore), 0.0, 1.0);
			return true;
		}

		bool ClassifyTextLineOrientationBatch(const std::vector<cv::Mat>& textImages, size_t beginIndex, size_t batchSize, std::vector<TextLineOrientation>& orientations, std::string& errorMessage)
		{
			if (!clsSession || batchSize == 0)
			{
				return true;
			}

			const int safeClsImageHeight = std::max(1, options.clsImageHeight);
			const int safeClsImageWidth = std::max(1, options.clsImageWidth);
			size_t imageArea = 0;
			size_t singleInputValueCount = 0;
			const size_t modelBatchSize = fixedClsBatchSize > 0 ? static_cast<size_t>(fixedClsBatchSize) : batchSize;
			size_t inputValueCount = 0;
			if (!TryMultiplySize(static_cast<size_t>(safeClsImageHeight), static_cast<size_t>(safeClsImageWidth), imageArea) || !TryMultiplySize(static_cast<size_t>(3), imageArea, singleInputValueCount) || !TryMultiplySize(singleInputValueCount, modelBatchSize, inputValueCount))
			{
				errorMessage = GB_STR("文字方向分类输入张量尺寸溢出。");
				return false;
			}

			clsInputDataCache.assign(inputValueCount, 0.0f);
#ifdef _OPENMP
#pragma omp parallel for if(options.enableOpenMPParallel && batchSize >= 2) schedule(dynamic)
#endif
			for (int imageIndex = 0; imageIndex < static_cast<int>(batchSize); imageIndex++)
			{
				FillBgrMatToRecInput(textImages[beginIndex + static_cast<size_t>(imageIndex)], safeClsImageHeight, safeClsImageWidth, clsInputDataCache.data() + singleInputValueCount * static_cast<size_t>(imageIndex), batchSize == 1 && options.enableOpenMPParallel);
			}

			std::array<int64_t, 4> inputShape = { static_cast<int64_t>(modelBatchSize), 3, safeClsImageHeight, safeClsImageWidth };
			Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
			Ort::Value inputTensor = Ort::Value::CreateTensor<float>(memoryInfo, clsInputDataCache.data(), clsInputDataCache.size(), inputShape.data(), inputShape.size());

			std::vector<Ort::Value> outputTensors = clsSession->Run(GetRunOptions(), clsInputNamePointers.data(), &inputTensor, 1, clsOutputNamePointers.data(), clsOutputNamePointers.size());
			if (outputTensors.empty() || !outputTensors[0].IsTensor())
			{
				errorMessage = GB_STR("文字方向分类模型输出为空。");
				return false;
			}

			Ort::TensorTypeAndShapeInfo outputInfo = outputTensors[0].GetTensorTypeAndShapeInfo();
			if (outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
			{
				errorMessage = GB_STR("文字方向分类模型输出类型不正确，当前仅支持 float32 输出。");
				return false;
			}

			const std::vector<int64_t> outputShape = outputInfo.GetShape();
			size_t outputBatchSize = 0;
			int classCount = 0;
			if (!TryResolveBatchClassOutputInfo(outputShape, outputInfo.GetElementCount(), batchSize, modelBatchSize, outputBatchSize, classCount))
			{
				errorMessage = GB_STR("文字方向分类模型输出尺寸不正确，当前支持 [batch,class] 或 [class]。");
				return false;
			}

			const float* outputData = outputTensors[0].GetTensorMutableData<float>();
			for (size_t imageIndex = 0; imageIndex < batchSize; imageIndex++)
			{
				TextLineOrientation orientation;
				if (!DecodeTextLineOrientation(outputData, outputShape, outputBatchSize, classCount, imageIndex, orientation))
				{
					errorMessage = GB_STR("文字方向分类模型输出解码失败。");
					return false;
				}
				orientations[beginIndex + imageIndex] = orientation;
			}

			return true;
		}

		bool CorrectTextLineOrientations(std::vector<cv::Mat>& textImages, std::string& errorMessage)
		{
			if (!clsSession || textImages.empty())
			{
				return true;
			}

			std::vector<TextLineOrientation> orientations(textImages.size());
			const size_t safeBatchSize = static_cast<size_t>(std::max(1, runtimeClsBatchSize));
			for (size_t beginIndex = 0; beginIndex < textImages.size(); beginIndex += safeBatchSize)
			{
				const size_t batchSize = std::min(safeBatchSize, textImages.size() - beginIndex);
				if (!ClassifyTextLineOrientationBatch(textImages, beginIndex, batchSize, orientations, errorMessage))
				{
					return false;
				}
			}

#ifdef _OPENMP
#pragma omp parallel for if(options.enableOpenMPParallel && textImages.size() >= 4) schedule(dynamic)
#endif
			for (int imageIndex = 0; imageIndex < static_cast<int>(textImages.size()); imageIndex++)
			{
				const TextLineOrientation& orientation = orientations[static_cast<size_t>(imageIndex)];
				if (orientation.angle == 180 && orientation.confidence >= options.clsConfidenceThresh)
				{
					cv::Mat rotatedImage;
					cv::rotate(textImages[static_cast<size_t>(imageIndex)], rotatedImage, cv::ROTATE_180);
					if (!rotatedImage.empty())
					{
						textImages[static_cast<size_t>(imageIndex)] = rotatedImage;
					}
				}
			}

			return true;
		}

		bool DecodeRecognizedText(const float* outputData, const RecognitionOutputInfo& outputInfo, size_t imageIndex, RecognizedText& recognizedText)
		{
			recognizedText = RecognizedText();
			if (outputData == nullptr || outputInfo.timeSteps <= 0 || outputInfo.classCount <= 1 || imageIndex >= outputInfo.outputBatchSize)
			{
				return false;
			}

			int previousMaxIndex = -1;
			double confidenceSum = 0.0;
			int confidenceCount = 0;
			for (int timeIndex = 0; timeIndex < outputInfo.timeSteps; timeIndex++)
			{
				const float* timeStepData = GetRecognitionTimeStepData(outputData, outputInfo, imageIndex, timeIndex);
				if (timeStepData == nullptr)
				{
					return false;
				}

				int maxIndex = 0;
				float maxScore = timeStepData[0];
				float minScore = timeStepData[0];
				for (int classIndex = 1; classIndex < outputInfo.classCount; classIndex++)
				{
					const float currentScore = timeStepData[classIndex];
					if (currentScore > maxScore)
					{
						maxScore = currentScore;
						maxIndex = classIndex;
					}
					minScore = std::min(minScore, currentScore);
				}

				if (maxIndex > 0 && maxIndex != previousMaxIndex)
				{
					const size_t dictIndex = static_cast<size_t>(maxIndex - 1);
					if (dictIndex < characterDict.size())
					{
						recognizedText.text += characterDict[dictIndex];
						confidenceSum += GetStableMaxProbability(timeStepData, outputInfo.classCount, maxIndex, maxScore, minScore);
						confidenceCount++;
					}
				}

				previousMaxIndex = maxIndex;
			}

			recognizedText.confidence = confidenceCount > 0 ? confidenceSum / static_cast<double>(confidenceCount) : 0.0;
			recognizedText.confidence = ClampDouble(recognizedText.confidence, 0.0, 1.0);
			return true;
		}

		double GetTextImageWidthHeightRatio(const cv::Mat& textImage) const
		{
			if (textImage.empty() || textImage.rows <= 0)
			{
				return 1.0;
			}

			return static_cast<double>(textImage.cols) / static_cast<double>(textImage.rows);
		}

		int GetRecognitionInputWidthForBatch(const std::vector<cv::Mat>& textImages, size_t beginIndex, size_t batchSize) const
		{
			const int safeRecImageWidth = std::max(1, options.recImageWidth);
			if (!isRecImageWidthDynamic || batchSize == 0)
			{
				return safeRecImageWidth;
			}

			int targetWidth = safeRecImageWidth;
			const int maxDynamicImageWidth = std::max(safeRecImageWidth, options.recMaxDynamicImageWidth);
			for (size_t imageIndex = 0; imageIndex < batchSize; imageIndex++)
			{
				const cv::Mat& textImage = textImages[beginIndex + imageIndex];
				const double widthHeightRatio = GetTextImageWidthHeightRatio(textImage);
				const int imageTargetWidth = static_cast<int>(std::ceil(static_cast<double>(std::max(1, options.recImageHeight)) * widthHeightRatio));
				targetWidth = std::max(targetWidth, imageTargetWidth);
			}

			targetWidth = ClampInt(targetWidth, safeRecImageWidth, maxDynamicImageWidth);
			return ClampInt(RoundUpToMultiple(targetWidth, 8), safeRecImageWidth, maxDynamicImageWidth);
		}

		bool RecognizeTextImageBatch(const std::vector<cv::Mat>& textImages, size_t beginIndex, size_t batchSize, std::vector<RecognizedText>& recognizedTexts, std::string& errorMessage)
		{
			if (batchSize == 0)
			{
				return true;
			}

			const int safeRecImageHeight = std::max(1, options.recImageHeight);
			const int safeRecImageWidth = GetRecognitionInputWidthForBatch(textImages, beginIndex, batchSize);
			size_t imageArea = 0;
			size_t singleInputValueCount = 0;
			const size_t modelBatchSize = fixedRecBatchSize > 0 ? static_cast<size_t>(fixedRecBatchSize) : batchSize;
			size_t inputValueCount = 0;
			if (!TryMultiplySize(static_cast<size_t>(safeRecImageHeight), static_cast<size_t>(safeRecImageWidth), imageArea) || !TryMultiplySize(static_cast<size_t>(3), imageArea, singleInputValueCount) || !TryMultiplySize(singleInputValueCount, modelBatchSize, inputValueCount))
			{
				errorMessage = GB_STR("文本识别输入张量尺寸溢出。");
				return false;
			}

			recInputDataCache.assign(inputValueCount, 0.0f);
#ifdef _OPENMP
#pragma omp parallel for if(options.enableOpenMPParallel && batchSize >= 2) schedule(dynamic)
#endif
			for (int imageIndex = 0; imageIndex < static_cast<int>(batchSize); imageIndex++)
			{
				FillBgrMatToRecInput(textImages[beginIndex + static_cast<size_t>(imageIndex)], safeRecImageHeight, safeRecImageWidth, recInputDataCache.data() + singleInputValueCount * static_cast<size_t>(imageIndex), batchSize == 1 && options.enableOpenMPParallel);
			}

			std::array<int64_t, 4> inputShape = { static_cast<int64_t>(modelBatchSize), 3, safeRecImageHeight, safeRecImageWidth };
			Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
			Ort::Value inputTensor = Ort::Value::CreateTensor<float>(memoryInfo, recInputDataCache.data(), recInputDataCache.size(), inputShape.data(), inputShape.size());

			std::vector<Ort::Value> outputTensors = recSession->Run(GetRunOptions(), recInputNamePointers.data(), &inputTensor, 1, recOutputNamePointers.data(), recOutputNamePointers.size());
			if (outputTensors.empty() || !outputTensors[0].IsTensor())
			{
				errorMessage = GB_STR("文本识别模型输出为空。");
				return false;
			}

			Ort::TensorTypeAndShapeInfo outputInfo = outputTensors[0].GetTensorTypeAndShapeInfo();
			if (outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
			{
				errorMessage = GB_STR("文本识别模型输出类型不正确，当前仅支持 float32 输出。");
				return false;
			}

			const std::vector<int64_t> outputShape = outputInfo.GetShape();
			RecognitionOutputInfo recognitionOutputInfo;
			if (!TryResolveRecognitionOutputInfo(outputShape, outputInfo.GetElementCount(), batchSize, modelBatchSize, recognitionOutputInfo))
			{
				errorMessage = GB_STR("文本识别模型输出尺寸不正确，当前支持 [batch,time,class]、[time,batch,class] 或 [time,class]。");
				return false;
			}

			const float* outputData = outputTensors[0].GetTensorMutableData<float>();
			for (size_t imageIndex = 0; imageIndex < batchSize; imageIndex++)
			{
				RecognizedText recognizedText;
				if (!DecodeRecognizedText(outputData, recognitionOutputInfo, imageIndex, recognizedText))
				{
					errorMessage = GB_STR("文本识别模型输出解码失败。");
					return false;
				}
				recognizedTexts[beginIndex + imageIndex] = recognizedText;
			}

			return true;
		}

		bool RecognizeTextImages(const std::vector<cv::Mat>& textImages, std::vector<RecognizedText>& recognizedTexts, std::string& errorMessage)
		{
			recognizedTexts.clear();
			recognizedTexts.resize(textImages.size());
			if (textImages.empty())
			{
				return true;
			}

			std::vector<size_t> sortedIndices(textImages.size());
			std::iota(sortedIndices.begin(), sortedIndices.end(), static_cast<size_t>(0));
			std::sort(sortedIndices.begin(), sortedIndices.end(), [this, &textImages](size_t leftIndex, size_t rightIndex)
				{
					return GetTextImageWidthHeightRatio(textImages[leftIndex]) < GetTextImageWidthHeightRatio(textImages[rightIndex]);
				});

			std::vector<cv::Mat> sortedTextImages;
			sortedTextImages.reserve(textImages.size());
			for (const size_t imageIndex : sortedIndices)
			{
				sortedTextImages.push_back(textImages[imageIndex]);
			}

			std::vector<RecognizedText> sortedRecognizedTexts(sortedTextImages.size());
			const size_t safeBatchSize = static_cast<size_t>(std::max(1, runtimeRecBatchSize));
			for (size_t beginIndex = 0; beginIndex < sortedTextImages.size(); beginIndex += safeBatchSize)
			{
				const size_t batchSize = std::min(safeBatchSize, sortedTextImages.size() - beginIndex);
				if (!RecognizeTextImageBatch(sortedTextImages, beginIndex, batchSize, sortedRecognizedTexts, errorMessage))
				{
					return false;
				}
			}

			for (size_t sortedIndex = 0; sortedIndex < sortedIndices.size(); sortedIndex++)
			{
				recognizedTexts[sortedIndices[sortedIndex]] = sortedRecognizedTexts[sortedIndex];
			}

			return true;
		}

		bool RecognizeTextImage(const cv::Mat& textImage, RecognizedText& recognizedText, std::string& errorMessage)
		{
			recognizedText = RecognizedText();
			if (textImage.empty())
			{
				return true;
			}

			std::vector<cv::Mat> textImages(1, textImage);
			std::vector<RecognizedText> recognizedTexts;
			if (!RecognizeTextImages(textImages, recognizedTexts, errorMessage))
			{
				return false;
			}

			if (!recognizedTexts.empty())
			{
				recognizedText = recognizedTexts[0];
			}

			return true;
		}

		Ort::RunOptions& GetRunOptions()
		{
			if (!runOptions)
			{
				runOptions.reset(new Ort::RunOptions(CreateOnnxRuntimeRunOptions(options)));
			}

			return *runOptions;
		}

		void ClearRuntime()
		{
			detSession.reset();
			recSession.reset();
			clsSession.reset();
			runOptions.reset();
			ortEnv.reset();
			detInputNames.clear();
			detOutputNames.clear();
			recInputNames.clear();
			recOutputNames.clear();
			clsInputNames.clear();
			clsOutputNames.clear();
			detInputNamePointers.clear();
			detOutputNamePointers.clear();
			recInputNamePointers.clear();
			recOutputNamePointers.clear();
			clsInputNamePointers.clear();
			clsOutputNamePointers.clear();
			characterDict.clear();
			detInputDataCache.clear();
			recInputDataCache.clear();
			clsInputDataCache.clear();
			runtimeRecBatchSize = 1;
			runtimeClsBatchSize = 1;
			fixedRecBatchSize = 0;
			fixedClsBatchSize = 0;
			fixedDetImageHeight = 0;
			fixedDetImageWidth = 0;
			isRecImageWidthDynamic = false;
		}

	private:
		GB_OCRBackend backendType = GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCpu;
		GB_OCROptions options;
		GB_PPOCRv5MobileModelPaths modelPaths;
		std::vector<std::string> characterDict;
		std::unique_ptr<Ort::Env> ortEnv;
		std::unique_ptr<Ort::Session> detSession;
		std::unique_ptr<Ort::Session> recSession;
		std::unique_ptr<Ort::Session> clsSession;
		std::unique_ptr<Ort::RunOptions> runOptions;
		int runtimeRecBatchSize = 1;
		int runtimeClsBatchSize = 1;
		int fixedRecBatchSize = 0;
		int fixedClsBatchSize = 0;
		int fixedDetImageHeight = 0;
		int fixedDetImageWidth = 0;
		bool isRecImageWidthDynamic = false;
		std::vector<std::string> detInputNames;
		std::vector<std::string> detOutputNames;
		std::vector<std::string> recInputNames;
		std::vector<std::string> recOutputNames;
		std::vector<std::string> clsInputNames;
		std::vector<std::string> clsOutputNames;
		std::vector<const char*> detInputNamePointers;
		std::vector<const char*> detOutputNamePointers;
		std::vector<const char*> recInputNamePointers;
		std::vector<const char*> recOutputNamePointers;
		std::vector<const char*> clsInputNamePointers;
		std::vector<const char*> clsOutputNamePointers;
		std::vector<float> detInputDataCache;
		std::vector<float> recInputDataCache;
		std::vector<float> clsInputDataCache;
	};

	class GB_BaiduOCRBackend : public IGB_OCRBackend
	{
	public:
		explicit GB_BaiduOCRBackend(const GB_OCROptions& inputOptions)
			: options(NormalizeOptions(inputOptions))
		{
		}

		virtual bool Initialize(std::string& errorMessage) override
		{
			errorMessage.clear();
			lastFailureAllowFallback = false;

			if (GetBaiduOCRApiUrl(options.baiduApiOptions.apiType).empty())
			{
				return SetFailure(errorMessage, GB_STR("百度 OCR API 类型无效。"), false);
			}

			if (options.baiduApiOptions.accessTokenUtf8.empty() && (options.baiduApiOptions.apiKeyUtf8.empty() || options.baiduApiOptions.secretKeyUtf8.empty()))
			{
				return SetFailure(errorMessage, GB_STR("百度 OCR API 未配置 access_token，且 apiKeyUtf8/secretKeyUtf8 不完整。"), false);
			}

			if (options.baiduApiOptions.accessTokenUtf8.empty())
			{
				std::string accessTokenUtf8;
				if (!EnsureAccessToken(accessTokenUtf8, errorMessage))
				{
					return false;
				}
			}

			return true;
		}

		virtual bool Recognize(const GB_Image& image, std::vector<GB_OCRTextBlock>& textBlocks, std::string& errorMessage) override
		{
			textBlocks.clear();
			errorMessage.clear();
			lastFailureAllowFallback = false;

			if (image.IsEmpty() || image.GetWidth() == 0 || image.GetHeight() == 0)
			{
				return SetFailure(errorMessage, GB_STR("输入图像为空，无法调用百度 OCR API。"), false);
			}

			GB_ByteBuffer encodedImageBytes;
			if (!image.EncodeToMemory(encodedImageBytes, options.baiduApiOptions.imageFileExtUtf8, options.baiduApiOptions.imageSaveOptions) || encodedImageBytes.empty())
			{
				return SetFailure(errorMessage, GB_STR("输入图像编码失败，无法调用百度 OCR API。"), false);
			}

			const std::string imageBase64 = GB_Base64Encode(GB_ByteBufferToString(encodedImageBytes), false, false);
			if (imageBase64.empty())
			{
				return SetFailure(errorMessage, GB_STR("输入图像 Base64 编码失败，无法调用百度 OCR API。"), false);
			}

			std::string accessTokenUtf8;
			if (!EnsureAccessToken(accessTokenUtf8, errorMessage))
			{
				return false;
			}

			const std::string apiUrlUtf8 = GetBaiduOCRApiUrl(options.baiduApiOptions.apiType);
			if (apiUrlUtf8.empty())
			{
				return SetFailure(errorMessage, GB_STR("百度 OCR API 类型无效。"), false);
			}

			const std::string requestUrlUtf8 = GB_UrlOperator::SetUrlQueryValue(apiUrlUtf8, "access_token", accessTokenUtf8, GB_UrlOperator::UrlQuerySetMode::ReplaceAll, GB_UrlOperator::UrlEncodingMode::Rfc3986);
			const std::string requestBody = BuildBaiduOCRRequestBody(options.baiduApiOptions, imageBase64);
			const GB_NetworkResponse response = GB_PostUrlData(requestUrlUtf8, requestBody, "application/x-www-form-urlencoded", options.baiduApiOptions.networkRequestOptions);
			if (!response.ok)
			{
				return SetFailure(errorMessage, BuildBaiduNetworkErrorMessage(response, GB_STR("调用百度 OCR API 失败")), true);
			}

			GB_VariantMap responseMap;
			std::string parseErrorMessage;
			if (!GB_JsonParser::ParseToVariantMap(response.body, responseMap, &parseErrorMessage))
			{
				return SetFailure(errorMessage, GB_STR("解析百度 OCR API 响应 JSON 失败：") + parseErrorMessage, true);
			}

			std::string responseErrorMessage;
			if (TryGetBaiduResponseErrorMessage(responseMap, responseErrorMessage))
			{
				return SetFailure(errorMessage, responseErrorMessage, true);
			}

			const GB_VariantList* const wordsResultList = TryGetVariantListField(responseMap, "words_result");
			if (wordsResultList == nullptr)
			{
				return SetFailure(errorMessage, GB_STR("百度 OCR API 响应缺少 words_result 字段。"), true);
			}

			const bool withLocation = IsBaiduOCRApiWithLocation(options.baiduApiOptions.apiType);
			for (const GB_Variant& wordValue : *wordsResultList)
			{
				const GB_VariantMap* const wordMap = wordValue.AnyCast<GB_VariantMap>();
				if (wordMap == nullptr)
				{
					continue;
				}

				std::string textUtf8;
				if (!TryGetVariantStringField(*wordMap, "words", textUtf8) || textUtf8.empty())
				{
					continue;
				}

				GB_OCRTextBlock textBlock;
				textBlock.text = textUtf8;
				ApplyBaiduProbability(*wordMap, textBlock);
				if (withLocation)
				{
					ApplyBaiduLocation(*wordMap, textBlock);
				}
				textBlocks.push_back(textBlock);
			}

			if (options.sortTextBlocks && withLocation)
			{
				SortTextBlocksReadingOrder(textBlocks);
			}

			return true;
		}

		virtual GB_OCRBackend GetBackendType() const override
		{
			return GB_OCRBackend::BaiduApi;
		}

		virtual bool CanFallbackOnFailure() const override
		{
			return lastFailureAllowFallback;
		}

	private:
		bool EnsureAccessToken(std::string& outAccessTokenUtf8, std::string& errorMessage)
		{
			if (!options.baiduApiOptions.accessTokenUtf8.empty())
			{
				outAccessTokenUtf8 = options.baiduApiOptions.accessTokenUtf8;
				return true;
			}

			const long long nowSeconds = GetCurrentUnixTimeSeconds();
			const long long refreshAdvanceSeconds = static_cast<long long>(options.baiduApiOptions.accessTokenRefreshAdvanceSeconds);
			if (!cachedAccessTokenUtf8.empty() && cachedAccessTokenExpireUnixSeconds > nowSeconds + refreshAdvanceSeconds)
			{
				outAccessTokenUtf8 = cachedAccessTokenUtf8;
				return true;
			}

			if (options.baiduApiOptions.apiKeyUtf8.empty() || options.baiduApiOptions.secretKeyUtf8.empty())
			{
				return SetFailure(errorMessage, GB_STR("百度 OCR API 未配置 apiKeyUtf8/secretKeyUtf8，无法获取 access_token。"), false);
			}

			std::string requestBody;
			AppendBaiduFormParameter(requestBody, "grant_type", "client_credentials");
			AppendBaiduFormParameter(requestBody, "client_id", options.baiduApiOptions.apiKeyUtf8);
			AppendBaiduFormParameter(requestBody, "client_secret", options.baiduApiOptions.secretKeyUtf8);

			const GB_NetworkResponse response = GB_PostUrlData("https://aip.baidubce.com/oauth/2.0/token", requestBody, "application/x-www-form-urlencoded", options.baiduApiOptions.networkRequestOptions);
			if (!response.ok)
			{
				return SetFailure(errorMessage, BuildBaiduNetworkErrorMessage(response, GB_STR("获取百度 access_token 失败")), true);
			}

			GB_VariantMap responseMap;
			std::string parseErrorMessage;
			if (!GB_JsonParser::ParseToVariantMap(response.body, responseMap, &parseErrorMessage))
			{
				return SetFailure(errorMessage, GB_STR("解析百度 access_token 响应 JSON 失败：") + parseErrorMessage, true);
			}

			std::string responseErrorMessage;
			std::string responseErrorCode;
			if (TryGetBaiduResponseErrorMessage(responseMap, responseErrorMessage, &responseErrorCode))
			{
				const bool allowFallback = responseErrorCode != "invalid_client" && responseErrorCode != "invalid_grant";
				return SetFailure(errorMessage, responseErrorMessage, allowFallback);
			}

			std::string accessTokenUtf8;
			if (!TryGetVariantStringField(responseMap, "access_token", accessTokenUtf8) || accessTokenUtf8.empty())
			{
				return SetFailure(errorMessage, GB_STR("百度 access_token 响应缺少 access_token 字段。"), true);
			}

			double expiresInSecondsDouble = 0.0;
			if (TryGetVariantDoubleField(responseMap, "expires_in", expiresInSecondsDouble) && expiresInSecondsDouble > 0.0)
			{
				cachedAccessTokenExpireUnixSeconds = nowSeconds + static_cast<long long>(expiresInSecondsDouble);
			}
			else
			{
				cachedAccessTokenExpireUnixSeconds = 0;
			}

			cachedAccessTokenUtf8 = accessTokenUtf8;
			outAccessTokenUtf8 = accessTokenUtf8;
			return true;
		}

		bool SetFailure(std::string& errorMessage, const std::string& inputErrorMessage, bool allowFallback)
		{
			errorMessage = inputErrorMessage;
			lastFailureAllowFallback = allowFallback;
			return false;
		}

		static std::string BuildBaiduNetworkErrorMessage(const GB_NetworkResponse& response, const std::string& operationName)
		{
			std::string message = operationName;
			if (!response.errorMessageUtf8.empty())
			{
				message += GB_STR("：") + response.errorMessageUtf8;
			}
			else if (response.httpStatusCode != 0)
			{
				message += GB_STR("，HTTP 状态码：") + std::to_string(response.httpStatusCode);
			}
			else
			{
				message += GB_STR("。");
			}

			if (response.curlErrorCode != 0)
			{
				message += GB_STR(" curl 错误码：") + std::to_string(response.curlErrorCode);
			}

			return message;
		}

		static void ApplyBaiduProbability(const GB_VariantMap& wordMap, GB_OCRTextBlock& textBlock)
		{
			const GB_VariantMap* const probabilityMap = TryGetVariantMapField(wordMap, "probability");
			if (probabilityMap == nullptr)
			{
				return;
			}

			double averageConfidence = 0.0;
			if (TryGetVariantDoubleField(*probabilityMap, "average", averageConfidence))
			{
				textBlock.recognitionConfidence = averageConfidence;
				textBlock.confidence = averageConfidence;
			}
		}

		static void ApplyBaiduLocation(const GB_VariantMap& wordMap, GB_OCRTextBlock& textBlock)
		{
			std::vector<GB_Point2d> polygonPoints;
			if (TryBuildBaiduPolygonFromVertexesLocation(wordMap, polygonPoints))
			{
				textBlock.polygonPoints = polygonPoints;
				textBlock.boundingRectangle = MakeBoundingRectangle(textBlock.polygonPoints);
				return;
			}

			GB_Rectangle boundingRectangle;
			if (TryBuildBaiduPolygonFromLocation(wordMap, polygonPoints, boundingRectangle))
			{
				textBlock.polygonPoints = polygonPoints;
				textBlock.boundingRectangle = boundingRectangle;
			}
		}

	private:
		GB_OCROptions options;
		std::string cachedAccessTokenUtf8;
		long long cachedAccessTokenExpireUnixSeconds = 0;
		bool lastFailureAllowFallback = false;
	};

	std::unique_ptr<IGB_OCRBackend> CreateBackend(const GB_OCROptions& options, GB_OCRBackend& actualBackend, std::string& errorMessage)
	{
		errorMessage.clear();
		actualBackend = options.backend;

		if (options.backend == GB_OCRBackend::Auto)
		{
			const GB_PPOCRv5MobileModelPaths modelPaths = NormalizeModelPaths(options.ppocrv5MobileModelPaths);
			if (HasAllRequiredPPOCRv5ModelFiles(modelPaths))
			{
				actualBackend = IsOnnxRuntimeCudaExecutionProviderAvailable(options) ? GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCuda : GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCpu;
			}
			else
			{
				errorMessage = BuildMissingPPOCRv5ModelFileMessage(modelPaths) + GB_STR(" Auto 当前只会自动选择 PP-OCRv5 mobile ONNX Runtime CPU/CUDA 后端。");
				return std::unique_ptr<IGB_OCRBackend>();
			}
		}

		switch (actualBackend)
		{
		case GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCpu:
			return std::unique_ptr<IGB_OCRBackend>(new GB_PPOCRv5MobileOnnxRuntimeBackend(options, GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCpu));
		case GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCuda:
			return std::unique_ptr<IGB_OCRBackend>(new GB_PPOCRv5MobileOnnxRuntimeBackend(options, GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCuda));
		case GB_OCRBackend::BaiduApi:
			actualBackend = GB_OCRBackend::BaiduApi;
			return std::unique_ptr<IGB_OCRBackend>(new GB_BaiduOCRBackend(options));
		default:
			errorMessage = GB_STR("未知 OCR 后端。");
			return std::unique_ptr<IGB_OCRBackend>();
		}
	}
}

class GB_OCR::Impl
{
public:
	GB_OCROptions options;
	std::unique_ptr<IGB_OCRBackend> backend;
	GB_OCRBackend actualBackend = GB_OCRBackend::Auto;
	std::string lastErrorMessage;
	mutable std::mutex mutex;

	Impl()
	{
	}

	explicit Impl(const GB_OCROptions& inputOptions)
		: options(NormalizeOptions(inputOptions))
	{
	}

	bool Initialize()
	{
		std::lock_guard<std::mutex> lockGuard(mutex);
		lastErrorMessage.clear();

		if (backend)
		{
			return true;
		}

		return EnsureBackendInitialized();
	}

	bool Recognize(const GB_Image& image, std::vector<GB_OCRTextBlock>& textBlocks, GB_OCRBackend& outputActualBackend, std::string& outputErrorMessage)
	{
		std::lock_guard<std::mutex> lockGuard(mutex);
		lastErrorMessage.clear();
		textBlocks.clear();

		bool success = false;
		if (!backend)
		{
			EnsureBackendInitialized();
		}

		if (backend)
		{
			success = backend->Recognize(image, textBlocks, lastErrorMessage);
		}

		if (!success && backend && ShouldFallbackFromBaiduApiFailure(backend->CanFallbackOnFailure()))
		{
			const std::string baiduErrorMessage = lastErrorMessage;
			success = RecognizeWithPPOCRv5MobileFallback(image, textBlocks, baiduErrorMessage);
		}

		outputActualBackend = actualBackend;
		outputErrorMessage = lastErrorMessage;
		return success;
	}

	bool EnsureBackendInitialized()
	{
		backend = CreateBackend(options, actualBackend, lastErrorMessage);
		if (!backend)
		{
			actualBackend = GB_OCRBackend::Auto;
			return false;
		}

		if (backend->Initialize(lastErrorMessage))
		{
			actualBackend = backend->GetBackendType();
			return true;
		}

		const GB_OCRBackend failedBackend = actualBackend;
		const std::string firstErrorMessage = lastErrorMessage;
		const bool canFallbackOnFailure = backend->CanFallbackOnFailure();
		backend.reset();

		if (options.backend == GB_OCRBackend::Auto && failedBackend == GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCuda)
		{
			GB_OCROptions cpuOptions = options;
			cpuOptions.backend = GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCpu;
			backend = CreateBackend(cpuOptions, actualBackend, lastErrorMessage);
			if (backend && backend->Initialize(lastErrorMessage))
			{
				actualBackend = backend->GetBackendType();
				return true;
			}

			const std::string fallbackErrorMessage = lastErrorMessage;
			backend.reset();
			actualBackend = GB_OCRBackend::Auto;
			lastErrorMessage = firstErrorMessage + GB_STR(" 自动回退 CPU 后端也失败：") + fallbackErrorMessage;
			return false;
		}

		if (ShouldFallbackFromBaiduApiFailure(canFallbackOnFailure))
		{
			return InitializePPOCRv5MobileFallback(firstErrorMessage);
		}

		actualBackend = GB_OCRBackend::Auto;
		return false;
	}

	bool ShouldFallbackFromBaiduApiFailure(bool canFallbackOnFailure) const
	{
		return options.backend == GB_OCRBackend::BaiduApi && actualBackend == GB_OCRBackend::BaiduApi && options.baiduApiOptions.fallbackToPPOCRv5MobileOnFailure && canFallbackOnFailure;
	}

	bool InitializePPOCRv5MobileFallback(const std::string& firstErrorMessage)
	{
		GB_OCROptions fallbackOptions = options;
		fallbackOptions.backend = GB_OCRBackend::Auto;
		fallbackOptions.baiduApiOptions.fallbackToPPOCRv5MobileOnFailure = false;

		GB_OCRBackend fallbackBackendType = GB_OCRBackend::Auto;
		std::string fallbackErrorMessage;
		std::unique_ptr<IGB_OCRBackend> fallbackBackend = CreateBackend(fallbackOptions, fallbackBackendType, fallbackErrorMessage);
		if (!fallbackBackend)
		{
			backend.reset();
			actualBackend = GB_OCRBackend::Auto;
			lastErrorMessage = firstErrorMessage + GB_STR(" 自动回退 PP-OCRv5 mobile 后端也失败：") + fallbackErrorMessage;
			return false;
		}

		if (!fallbackBackend->Initialize(fallbackErrorMessage))
		{
			const GB_OCRBackend failedFallbackBackendType = fallbackBackendType;
			const std::string firstFallbackErrorMessage = fallbackErrorMessage;
			fallbackBackend.reset();
			if (failedFallbackBackendType == GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCuda)
			{
				GB_OCROptions cpuFallbackOptions = fallbackOptions;
				cpuFallbackOptions.backend = GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCpu;
				fallbackBackendType = GB_OCRBackend::Auto;
				fallbackBackend = CreateBackend(cpuFallbackOptions, fallbackBackendType, fallbackErrorMessage);
				if (fallbackBackend && fallbackBackend->Initialize(fallbackErrorMessage))
				{
					backend = std::move(fallbackBackend);
					actualBackend = backend->GetBackendType();
					lastErrorMessage.clear();
					return true;
				}

				fallbackErrorMessage = firstFallbackErrorMessage + GB_STR(" 自动回退 CPU 后端也失败：") + fallbackErrorMessage;
			}
			else
			{
				fallbackErrorMessage = firstFallbackErrorMessage;
			}

			backend.reset();
			actualBackend = GB_OCRBackend::Auto;
			lastErrorMessage = firstErrorMessage + GB_STR(" 自动回退 PP-OCRv5 mobile 后端也失败：") + fallbackErrorMessage;
			return false;
		}

		backend = std::move(fallbackBackend);
		actualBackend = backend->GetBackendType();
		lastErrorMessage.clear();
		return true;
	}

	bool RecognizeWithPPOCRv5MobileFallback(const GB_Image& image, std::vector<GB_OCRTextBlock>& textBlocks, const std::string& baiduErrorMessage)
	{
		if (!InitializePPOCRv5MobileFallback(baiduErrorMessage))
		{
			textBlocks.clear();
			return false;
		}

		if (backend && backend->Recognize(image, textBlocks, lastErrorMessage))
		{
			lastErrorMessage.clear();
			return true;
		}

		const std::string fallbackErrorMessage = lastErrorMessage;
		textBlocks.clear();
		lastErrorMessage = baiduErrorMessage + GB_STR(" 自动回退 PP-OCRv5 mobile 后端也失败：") + fallbackErrorMessage;
		return false;
	}

	void SetLastErrorMessage(const std::string& errorMessage)
	{
		std::lock_guard<std::mutex> lockGuard(mutex);
		lastErrorMessage = errorMessage;
	}

	GB_OCRBackend GetActualBackend() const
	{
		std::lock_guard<std::mutex> lockGuard(mutex);
		return actualBackend;
	}
};

GB_OCR::GB_OCR()
	: ocrImpl(new Impl())
{
}

GB_OCR::GB_OCR(const GB_OCROptions& options)
	: ocrImpl(new Impl(options))
{
}

GB_OCR::~GB_OCR() = default;

GB_OCR::GB_OCR(GB_OCR&& other) noexcept = default;

GB_OCR& GB_OCR::operator=(GB_OCR&& other) noexcept = default;

void GB_OCR::SetOptions(const GB_OCROptions& options)
{
	if (!ocrImpl)
	{
		ocrImpl.reset(new Impl(options));
		return;
	}

	std::lock_guard<std::mutex> lockGuard(ocrImpl->mutex);
	ocrImpl->options = NormalizeOptions(options);
	ocrImpl->backend.reset();
	ocrImpl->actualBackend = GB_OCRBackend::Auto;
	ocrImpl->lastErrorMessage.clear();
}

const GB_OCROptions& GB_OCR::GetOptions() const
{
	static thread_local GB_OCROptions optionsCopy;
	if (!ocrImpl)
	{
		optionsCopy = GB_OCROptions();
		return optionsCopy;
	}

	std::lock_guard<std::mutex> lockGuard(ocrImpl->mutex);
	optionsCopy = ocrImpl->options;
	return optionsCopy;
}

bool GB_OCR::Initialize()
{
	if (!ocrImpl)
	{
		ocrImpl.reset(new Impl());
	}

	return ocrImpl->Initialize();
}

bool GB_OCR::IsInitialized() const
{
	if (!ocrImpl)
	{
		return false;
	}

	std::lock_guard<std::mutex> lockGuard(ocrImpl->mutex);
	return ocrImpl->backend != nullptr;
}

void GB_OCR::Clear()
{
	if (!ocrImpl)
	{
		return;
	}

	std::lock_guard<std::mutex> lockGuard(ocrImpl->mutex);
	ocrImpl->backend.reset();
	ocrImpl->actualBackend = GB_OCRBackend::Auto;
	ocrImpl->lastErrorMessage.clear();
}

GB_OCRResult GB_OCR::Recognize(const GB_Image& image)
{
	GB_OCRResult result;
	if (!ocrImpl)
	{
		ocrImpl.reset(new Impl());
	}

	result.success = ocrImpl->Recognize(image, result.textBlocks, result.backend, result.errorMessage);
	return result;
}

GB_OCRResult GB_OCR::Recognize(const std::string& imageFilePathUtf8)
{
	GB_OCRResult result;
	if (!GB_IsFileExists(imageFilePathUtf8))
	{
		result.success = false;
		result.backend = GetActualBackend();
		result.errorMessage = GB_STR("图像文件不存在。");
		if (!ocrImpl)
		{
			ocrImpl.reset(new Impl());
		}
		ocrImpl->SetLastErrorMessage(result.errorMessage);
		return result;
	}

	GB_ImageLoadOptions loadOptions;
	loadOptions.colorMode = GB_ImageColorMode::BGR;
	loadOptions.preserveBitDepth = false;
	GB_Image image(imageFilePathUtf8, loadOptions);
	if (image.IsEmpty())
	{
		result.success = false;
		result.backend = GetActualBackend();
		result.errorMessage = GB_STR("图像文件读取失败。");
		if (!ocrImpl)
		{
			ocrImpl.reset(new Impl());
		}
		ocrImpl->SetLastErrorMessage(result.errorMessage);
		return result;
	}

	return Recognize(image);
}

bool GB_OCR::Recognize(const GB_Image& image, std::vector<GB_OCRTextBlock>& textBlocks, std::string* errorMessage)
{
	const GB_OCRResult result = Recognize(image);
	textBlocks = result.textBlocks;
	if (errorMessage)
	{
		*errorMessage = result.errorMessage;
	}

	return result.success;
}

bool GB_OCR::Recognize(const std::string& imageFilePathUtf8, std::vector<GB_OCRTextBlock>& textBlocks, std::string* errorMessage)
{
	const GB_OCRResult result = Recognize(imageFilePathUtf8);
	textBlocks = result.textBlocks;
	if (errorMessage)
	{
		*errorMessage = result.errorMessage;
	}

	return result.success;
}

const std::string& GB_OCR::GetLastErrorMessage() const
{
	static thread_local std::string errorMessageCopy;
	if (!ocrImpl)
	{
		errorMessageCopy.clear();
		return errorMessageCopy;
	}

	std::lock_guard<std::mutex> lockGuard(ocrImpl->mutex);
	errorMessageCopy = ocrImpl->lastErrorMessage;
	return errorMessageCopy;
}

GB_OCRBackend GB_OCR::GetActualBackend() const
{
	if (!ocrImpl)
	{
		return GB_OCRBackend::Auto;
	}

	return ocrImpl->GetActualBackend();
}

GB_PPOCRv5MobileModelPaths GB_OCR::GetDefaultPPOCRv5MobileModelPaths()
{
	return BuildDefaultPPOCRv5MobileModelPaths();
}

bool GB_OCR::IsDefaultPPOCRv5MobileModelAvailable()
{
	return HasAllRequiredPPOCRv5ModelFiles(GetDefaultPPOCRv5MobileModelPaths());
}

bool GB_OCR::IsBackendAvailable(GB_OCRBackend backend)
{
	if (backend == GB_OCRBackend::Auto || backend == GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCpu)
	{
		return IsDefaultPPOCRv5MobileModelAvailable();
	}

	if (backend == GB_OCRBackend::PPOCRv5MobileOnnxRuntimeCuda)
	{
		return IsDefaultPPOCRv5MobileModelAvailable() && IsOnnxRuntimeCudaExecutionProviderAvailable(GB_OCROptions());
	}

	if (backend == GB_OCRBackend::BaiduApi)
	{
		return true;
	}

	return false;
}

GB_OCRResult GB_RecognizeTextFromImage(const GB_Image& image, const GB_OCROptions& options)
{
	GB_OCR ocr(options);
	return ocr.Recognize(image);
}

GB_OCRResult GB_RecognizeTextFromImageFile(const std::string& imageFilePathUtf8, const GB_OCROptions& options)
{
	GB_OCR ocr(options);
	return ocr.Recognize(imageFilePathUtf8);
}
