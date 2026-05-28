#ifndef GLOBALBASE_SYSTEM_RESULT_H_H
#define GLOBALBASE_SYSTEM_RESULT_H_H

#include "GB_SystemError.h"

#include <cstdint>
#include <string>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief 系统模块统一返回结果。
 *
 * 说明：
 * - 该类型用于系统自动化相关模块的统一返回值，避免仅使用 bool 导致失败原因丢失；
 * - errorCode 表达 GlobalBase 归一化后的错误类别；
 * - errorSource / nativeErrorCode / hresult / nativeMessage 保存 Win32、HRESULT、NTSTATUS、errno 等原生错误上下文；
 * - operationName 用于保存当前操作名，例如 "ShutdownSystem"、"SetSystemVolume"、"OpenUrlWithDefaultBrowser"；
 * - message 用于保存业务层或模块层的补充说明，所有 std::string 均约定为 UTF-8 编码；
 * - HRESULT 的非负值表示成功，因此 FromHResult(S_FALSE) 等调用会生成成功结果，但仍保留原始 HRESULT；
 * - 本类是轻量值类型，不持有句柄，不抛出自定义异常，不依赖 Windows 头文件。
 */
class GLOBALBASE_PORT GB_SystemResult final
{
public:
    /** @brief 归一化后的错误码。 */
    GB_SystemErrorCode errorCode = GB_SystemErrorCode::Succeeded;

    /** @brief 原生错误来源。 */
    GB_NativeErrorSource errorSource = GB_NativeErrorSource::None;

    /** @brief 原生错误码。Win32 为 DWORD，HRESULT 为 uint32_t 形式，NTSTATUS 为 uint32_t 形式，errno 为非负 int。 */
    uint64_t nativeErrorCode = 0;

    /** @brief HRESULT 表达。无可用 HRESULT 时为 0；调用 ToHResult() 可获取建议 HRESULT。 */
    int32_t hresult = 0;

    /** @brief 操作名，UTF-8 编码。 */
    std::string operationName = "";

    /** @brief 高层错误说明或业务说明，UTF-8 编码。 */
    std::string message = "";

    /** @brief 原生错误文本，UTF-8 编码。仅失败结果通常会填充该字段。 */
    std::string nativeMessage = "";

public:
    /**
     * @brief 构造成功结果。
     */
    GB_SystemResult();

    /**
     * @brief 构造指定统一错误码的结果。
     *
     * 说明：
     * - 当 @p errorCode 为 Succeeded 时构造成功结果；
     * - 当 @p errorCode 非 Succeeded 时构造失败结果；
     * - 若 @p errorCode 不是当前已定义的有效值，会归一化为 UnknownError；
     * - 该构造函数不附加原生错误信息。
     */
    explicit GB_SystemResult(GB_SystemErrorCode errorCode, const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 创建成功结果。
     */
    static GB_SystemResult Succeeded(const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 创建失败结果。
     *
     * 说明：如果传入 Succeeded，会自动归一化为 OperationFailed，避免 Failed(...) 产生成功结果。
     */
    static GB_SystemResult Failed(GB_SystemErrorCode errorCode, const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 根据 GlobalBase 统一错误码创建结果。
     */
    static GB_SystemResult FromGlobalBaseError(GB_SystemErrorCode errorCode, const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 根据当前线程最后一个 Win32 错误码创建结果。
     *
     * 说明：调用方应当在 Win32 API 失败后立即调用本函数，避免后续 API 改写当前线程的 LastError。
     */
    static GB_SystemResult FromLastWin32Error(const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 根据 Win32 错误码创建结果。
     */
    static GB_SystemResult FromWin32Error(uint32_t win32ErrorCode, const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 根据 HRESULT 创建结果。
     */
    static GB_SystemResult FromHResult(int32_t hresult, const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 根据 COM HRESULT 创建结果。
     */
    static GB_SystemResult FromComHResult(int32_t hresult, const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 根据 Windows Runtime HRESULT 创建结果。
     */
    static GB_SystemResult FromWinRtHResult(int32_t hresult, const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 根据 NTSTATUS 创建结果。
     */
    static GB_SystemResult FromNtStatus(uint32_t ntStatus, const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 根据 C 运行时 errno 创建结果。
     */
    static GB_SystemResult FromCRuntimeError(int errorNumber, const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 根据原生错误来源和错误码创建结果。
     */
    static GB_SystemResult FromNativeError(GB_NativeErrorSource errorSource, uint64_t nativeErrorCode, const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 根据原生错误信息创建结果。
     */
    static GB_SystemResult FromNativeErrorInfo(const GB_NativeErrorInfo& nativeErrorInfo, const std::string& operationName = std::string(), const std::string& message = std::string());

    /**
     * @brief 当前结果是否表示成功。
     */
    bool IsSucceeded() const;

    /**
     * @brief 当前结果是否表示失败。
     */
    bool IsFailed() const;

    /**
     * @brief 当前结果是否附带原生错误码或原生状态码。
     */
    bool HasNativeCode() const;

    /**
     * @brief 当前结果是否附带原生失败信息。
     */
    bool HasNativeError() const;

    /**
     * @brief 当前结果是否附带操作名。
     */
    bool HasOperationName() const;

    /**
     * @brief 当前结果是否附带高层消息。
     */
    bool HasMessage() const;

    /**
     * @brief 当前结果是否附带原生错误文本。
     */
    bool HasNativeMessage() const;

    /**
     * @brief 显式 bool 转换。true 表示成功，false 表示失败。
     */
    explicit operator bool() const;

    /**
     * @brief 获取当前结果对应的原生错误信息。
     */
    GB_NativeErrorInfo GetNativeErrorInfo() const;

    /**
     * @brief 获取统一错误码英文名称。
     */
    std::string GetErrorCodeName() const;

    /**
     * @brief 获取统一错误码中文描述。
     */
    std::string GetErrorCodeDescription() const;

    /**
     * @brief 获取原生错误来源英文名称。
     */
    std::string GetNativeErrorSourceName() const;

    /**
     * @brief 获取原生错误来源中文描述。
     */
    std::string GetNativeErrorSourceDescription() const;

    /**
     * @brief 获取适合直接显示给用户或日志系统的消息。
     */
    std::string GetDisplayMessage() const;

    /**
     * @brief 获取完整诊断文本。
     */
    std::string ToString() const;

    /**
     * @brief 获取建议 HRESULT。
     *
     * 说明：
     * - 如果 hresult 已保存非零失败值，直接返回该值；
     * - 如果当前结果是成功结果，返回已保存的非负 HRESULT 或 S_OK；
     * - 如果当前结果是失败结果，但 hresult 为 0 或被外部错误地设置为非负值，则根据统一错误码返回失败 HRESULT。
     */
    int32_t ToHResult() const;

    /**
     * @brief 清空为成功结果。
     */
    void Reset();

    /**
     * @brief 覆盖操作名，返回自身以便链式调用。
     */
    GB_SystemResult& WithOperationName(const std::string& operationName);

    /**
     * @brief 覆盖高层消息，返回自身以便链式调用。
     */
    GB_SystemResult& WithMessage(const std::string& message);

    /**
     * @brief 覆盖原生错误文本，返回自身以便链式调用。
     */
    GB_SystemResult& WithNativeMessage(const std::string& nativeMessage);

    /**
     * @brief 将统一错误码转换为建议 HRESULT。
     */
    static int32_t ErrorCodeToHResult(GB_SystemErrorCode errorCode);
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_RESULT_H_H
