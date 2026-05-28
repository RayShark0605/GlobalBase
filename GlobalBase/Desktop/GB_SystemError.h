#ifndef GLOBALBASE_SYSTEM_ERROR_H_H
#define GLOBALBASE_SYSTEM_ERROR_H_H

#include "../GlobalBasePort.h"

#include <cstdint>
#include <string>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief GlobalBase 系统模块通用错误码。
 *
 * 说明：
 * - 该枚举用于表达跨平台、跨 Windows 子系统的统一错误语义；
 * - Win32 GetLastError、HRESULT、NTSTATUS、errno 等原生错误码不应直接暴露为业务层错误码，
 *   而应作为原生错误信息附加保存；
 * - GB_SystemResult 会在该错误码基础上进一步封装“操作名、原生错误码、详细消息”等上下文。
 */
enum class GB_SystemErrorCode : uint16_t
{
    /** @brief 操作成功。 */
    Succeeded = 0,

    /** @brief 当前平台、当前系统版本或当前环境不支持该能力。 */
    UnsupportedPlatform = 1,

    /** @brief 参数非法。 */
    InvalidArgument = 2,

    /** @brief 当前对象或系统状态不满足操作前置条件。 */
    InvalidState = 3,

    /** @brief 对象或子系统尚未初始化。 */
    NotInitialized = 4,

    /** @brief 对象或子系统已经初始化。 */
    AlreadyInitialized = 5,

    /** @brief 权限不足或访问被拒绝。 */
    PermissionDenied = 6,

    /** @brief 目标对象不存在。 */
    NotFound = 7,

    /** @brief 目标对象已经存在。 */
    AlreadyExists = 8,

    /** @brief 操作超时。 */
    Timeout = 9,

    /** @brief 操作被取消。 */
    Cancelled = 10,

    /** @brief 操作失败，但无法归入更具体的错误类别。 */
    OperationFailed = 11,

    /** @brief Win32 或其他平台原生 API 调用失败。 */
    NativeApiFailed = 12,

    /** @brief COM API 调用失败。 */
    ComApiFailed = 13,

    /** @brief Windows Runtime API 调用失败。 */
    WinRtApiFailed = 14,

    /** @brief 资源分配失败，例如内存、句柄、系统对象等资源不足。 */
    ResourceAllocationFailed = 15,

    /** @brief 字符编码转换失败。 */
    EncodingConversionFailed = 16,

    /** @brief 解析失败，例如 JSON、XML、INI、命令行、系统返回文本解析失败。 */
    ParseFailed = 17,

    /** @brief 内部逻辑错误。 */
    InternalError = 18,

    /** @brief 未知错误。 */
    UnknownError = 19
};

/**
 * @brief 原生错误来源。
 *
 * 说明：
 * - 该枚举用于说明 nativeErrorCode / hresult 等原生错误值来自哪个错误体系；
 * - GB_SystemErrorCode 负责表达“归一化后的错误类别”，GB_NativeErrorSource 负责表达“原始错误码体系”。
 */
enum class GB_NativeErrorSource : uint16_t
{
    /** @brief 无原生错误。 */
    None = 0,

    /** @brief GlobalBase 自身定义的错误。 */
    GlobalBase = 1,

    /** @brief Win32 GetLastError / 系统错误码。 */
    Win32 = 2,

    /** @brief HRESULT 错误码。 */
    HResult = 3,

    /** @brief NTSTATUS 错误码。 */
    NtStatus = 4,

    /** @brief COM 错误。通常仍以 HRESULT 表达，但语义上来自 COM 调用。 */
    Com = 5,

    /** @brief Windows Runtime 错误。通常仍以 HRESULT 表达，但语义上来自 WinRT 调用。 */
    WinRt = 6,

    /** @brief C 运行时错误，例如 errno。 */
    CRuntime = 7
};

/**
 * @brief 原生错误信息。
 *
 * 说明：
 * - message 必须使用 UTF-8 编码；
 * - 当 errorSource 为 None 且 errorCode 为 0 时，表示没有原生错误。
 */
struct GB_NativeErrorInfo
{
    /** @brief 原生错误来源。 */
    GB_NativeErrorSource errorSource = GB_NativeErrorSource::None;

    /** @brief 原生错误码。 */
    uint64_t errorCode = 0;

    /** @brief 原生错误文本，UTF-8 编码。 */
    std::string message = "";
};

/**
 * @brief 系统错误工具类。
 *
 * 说明：
 * - 该类只提供错误码名称、错误码描述、Win32/HRESULT/NTSTATUS/errno 转换、系统错误文本格式化等纯工具能力；
 * - 该类不保存状态，不负责表达某一次调用的完整结果；
 * - 某一次调用的完整结果应由 GB_SystemResult 表达；
 * - 本类所有 std::string 入参和返回值均约定为 UTF-8 编码；
 * - UTF-8 返回值本身是正确编码；若直接输出到 Windows 控制台，控制台代码页或输出方式仍可能影响显示效果；
 * - Windows 下所有系统错误文本均通过宽字符 API 获取，再转换为 UTF-8，避免受 ANSI 代码页影响；
 * - 格式化 Windows 错误文本时会尽量保留调用线程原有的 LastError，避免错误诊断代码污染后续诊断。
 */
class GLOBALBASE_PORT GB_SystemError final
{
public:
    GB_SystemError() = delete;
    ~GB_SystemError() = delete;

    /**
     * @brief 判断统一错误码是否表示成功。
     */
    static bool IsSucceeded(GB_SystemErrorCode errorCode);

    /**
     * @brief 判断统一错误码是否表示失败。
     */
    static bool IsFailed(GB_SystemErrorCode errorCode);

    /**
     * @brief 判断统一错误码数值是否为当前已定义的有效值。
     */
    static bool IsValidErrorCodeValue(uint64_t errorCodeValue);

    /**
     * @brief 判断原生错误来源数值是否为当前已定义的有效值。
     */
    static bool IsValidNativeErrorSourceValue(uint64_t errorSourceValue);

    /**
     * @brief 获取统一错误码的英文名称。
     *
     * @return UTF-8 编码文本。
     */
    static std::string GetErrorCodeName(GB_SystemErrorCode errorCode);

    /**
     * @brief 获取统一错误码的中文描述。
     *
     * @return UTF-8 编码文本。
     */
    static std::string GetErrorCodeDescription(GB_SystemErrorCode errorCode);

    /**
     * @brief 获取原生错误来源英文名称。
     *
     * @return UTF-8 编码文本。
     */
    static std::string GetNativeErrorSourceName(GB_NativeErrorSource errorSource);

    /**
     * @brief 获取原生错误来源中文描述。
     *
     * @return UTF-8 编码文本。
     */
    static std::string GetNativeErrorSourceDescription(GB_NativeErrorSource errorSource);

    /**
     * @brief 获取当前线程最后一个 Win32 错误码。
     *
     * 说明：
     * - Windows 下返回 GetLastError()；
     * - 非 Windows 平台下返回 0。
     */
    static uint32_t GetLastWin32ErrorCode();

    /**
     * @brief 获取当前线程最后一个 Win32 错误码对应的系统错误文本。
     *
     * @return UTF-8 编码错误文本；若无法获取，则返回包含错误码的降级文本。
     */
    static std::string GetLastWin32ErrorMessage();

    /**
     * @brief 获取指定 Win32 错误码对应的系统错误文本。
     *
     * @param win32ErrorCode Win32 系统错误码。
     * @return UTF-8 编码错误文本；若无法获取，则返回包含错误码的降级文本。
     */
    static std::string GetWin32ErrorMessage(uint32_t win32ErrorCode);

    /**
     * @brief 获取指定 HRESULT 对应的系统错误文本。
     *
     * @param hresult HRESULT 数值。
     * @return UTF-8 编码错误文本；若无法获取，则返回包含 HRESULT 的降级文本。
     */
    static std::string GetHResultMessage(int32_t hresult);

    /**
     * @brief 获取指定 NTSTATUS 对应的系统错误文本。
     *
     * @param ntStatus NTSTATUS 数值。
     * @return UTF-8 编码错误文本；若无法获取，则返回包含 NTSTATUS 的降级文本。
     */
    static std::string GetNtStatusMessage(uint32_t ntStatus);

    /**
     * @brief 获取指定 C 运行时 errno 对应的错误文本。
     *
     * @return UTF-8 编码错误文本；若无法获取，则返回包含 errno 的降级文本。
     */
    static std::string GetCRuntimeErrorMessage(int errorNumber);

    /**
     * @brief 将 Win32 错误码转换为 HRESULT。
     */
    static int32_t Win32ErrorCodeToHResult(uint32_t win32ErrorCode);

    /**
     * @brief 尝试从 HRESULT 中提取 Win32 错误码。
     *
     * @param hresult HRESULT 数值。
     * @param converted [out] true=成功提取；false=该 HRESULT 不是由 Win32 错误码转换而来。
     * @return 提取到的 Win32 错误码；失败时返回 0。
     */
    static uint32_t HResultToWin32ErrorCode(int32_t hresult, bool& converted);

    /**
     * @brief 判断 HRESULT 是否表示成功。
     */
    static bool IsHResultSucceeded(int32_t hresult);

    /**
     * @brief 判断 HRESULT 是否表示失败。
     */
    static bool IsHResultFailed(int32_t hresult);

    /**
     * @brief 按 8 位十六进制格式化 32 位数值。
     *
     * @param value 32 位数值。
     * @param withPrefix 是否带 0x 前缀。
     * @return UTF-8 编码文本。
     */
    static std::string FormatHex32(uint32_t value, bool withPrefix = true);

    /**
     * @brief 按 16 位十六进制格式化 64 位数值。
     *
     * @param value 64 位数值。
     * @param withPrefix 是否带 0x 前缀。
     * @return UTF-8 编码文本。
     */
    static std::string FormatHex64(uint64_t value, bool withPrefix = true);

    /**
     * @brief 按指定原生错误来源格式化错误码。
     *
     * @return UTF-8 编码文本。
     */
    static std::string FormatNativeErrorCode(GB_NativeErrorSource errorSource, uint64_t errorCode);

    /**
     * @brief 按指定原生错误来源格式化错误文本。
     *
     * @return UTF-8 编码文本。
     */
    static std::string FormatNativeErrorMessage(GB_NativeErrorSource errorSource, uint64_t errorCode);

    /**
     * @brief 创建原生错误信息。
     *
     * @return message 字段为 UTF-8 编码文本。
     */
    static GB_NativeErrorInfo MakeNativeErrorInfo(GB_NativeErrorSource errorSource, uint64_t errorCode);

    /**
     * @brief 创建当前线程最后一个 Win32 错误信息。
     *
     * 说明：调用方应在 Win32 API 失败后立即调用本函数，避免后续 API 改写 LastError。
     */
    static GB_NativeErrorInfo MakeLastWin32ErrorInfo();

    /**
     * @brief 基于常见 Win32 错误码推断统一错误码。
     *
     * 说明：
     * - 该函数只做保守推断；
     * - 不能可靠分类的错误统一归为 NativeApiFailed。
     */
    static GB_SystemErrorCode GuessErrorCodeFromWin32ErrorCode(uint32_t win32ErrorCode);

    /**
     * @brief 基于常见 HRESULT 推断统一错误码。
     *
     * 说明：
     * - FACILITY_WIN32 的 HRESULT 会优先转回 Win32 错误码再推断；
     * - 不能可靠分类的错误统一归为 NativeApiFailed；
     * - COM / WinRT 场景应优先通过 GB_SystemResult::FromComHResult / FromWinRtHResult 创建结果。
     */
    static GB_SystemErrorCode GuessErrorCodeFromHResult(int32_t hresult);

    /**
     * @brief 基于常见 NTSTATUS 推断统一错误码。
     *
     * 说明：
     * - 该函数只做保守推断；
     * - 不能可靠分类的失败状态统一归为 NativeApiFailed。
     */
    static GB_SystemErrorCode GuessErrorCodeFromNtStatus(uint32_t ntStatus);

    /**
     * @brief 基于常见 errno 推断统一错误码。
     */
    static GB_SystemErrorCode GuessErrorCodeFromCRuntimeErrorCode(int errorNumber);

    /**
     * @brief 基于原生错误来源和错误码推断统一错误码。
     *
     * 说明：
     * - 该函数只做保守推断；
     * - 不能可靠分类的错误会按来源分别归为 NativeApiFailed、ComApiFailed、WinRtApiFailed 或 OperationFailed。
     */
    static GB_SystemErrorCode GuessErrorCodeFromNativeError(GB_NativeErrorSource errorSource, uint64_t errorCode);
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_ERROR_H_H
