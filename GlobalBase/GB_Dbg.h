#ifndef GLOBALBASE_DBG_H_H
#define GLOBALBASE_DBG_H_H

#include "GlobalBasePort.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _MSC_VER
# pragma warning(push)
# pragma warning(disable: 4251)
#endif

/**
 * @brief 调试栈帧信息。
 *
 * @remarks
 * - 所有字符串均为 UTF-8 编码。
 * - address 为当前栈帧的程序计数器地址，即 Windows x64 下的 RIP 值。
 * - 当符号文件（PDB）不可用、符号路径不正确或模块符号未成功加载时，函数名、源码路径、行号等字段可能为空。
 */
struct GB_DbgStackFrame
{
    uint64_t address = 0;                           // 程序计数器地址（Windows x64: RIP）
    std::string moduleNameUtf8 = "";                // 模块名称，例如 GlobalBase.dll
    std::string modulePathUtf8 = "";                // 模块路径，可能为空
    std::string functionNameUtf8 = "";              // 函数名称，可能为空
    std::string sourceFilePathUtf8 = "";            // 源码文件路径，可能为空
    uint32_t sourceLine = 0;                         // 源码行号，0 表示不可用
    uint64_t symbolDisplacement = 0;                 // 地址相对符号起始位置的偏移
    uint32_t lineDisplacement = 0;                   // 地址相对源码行起始位置的偏移
    bool hasModule = false;                          // 是否解析到模块信息
    bool hasFunction = false;                        // 是否解析到函数符号
    bool hasSourceLocation = false;                  // 是否解析到源码文件和行号
};

/**
 * @brief 调用栈信息。
 *
 * @remarks
 * - frames 按调用栈从近到远排列：frames[0] 通常是最靠近采集点的函数。
 * - 该对象只保存一次采集结果，不持有系统句柄，也不要求调用者手动释放。
 */
struct GB_DbgStackTrace
{
    std::vector<GB_DbgStackFrame> frames;
};

/**
 * @brief 符号引擎初始化选项。
 *
 * @remarks
 * Windows 下内部基于 DbgHelp 符号引擎。通常只需要使用默认值。
 */
struct GB_DbgSymbolOptions
{
    std::string symbolSearchPathUtf8 = "";           // 符号搜索路径；为空时自动使用“程序目录 + 当前目录 + _NT_SYMBOL_PATH + _NT_ALTERNATE_SYMBOL_PATH”
    bool loadLineInfo = true;                        // 是否加载源码行号信息
    bool undecorateSymbolNames = true;               // 是否尽量输出未修饰函数名
    bool deferredLoadSymbols = true;                 // 是否延迟加载符号
    bool failCriticalErrors = true;                  // 是否避免弹出系统级严重错误对话框
    bool noSymbolPrompts = true;                     // 是否禁用符号服务器交互式提示，适合无人值守程序和崩溃处理路径
    bool loadModules = true;                         // 初始化时是否枚举并加载当前进程已加载模块
};

/**
 * @brief MiniDump 写出级别。
 *
 * @remarks
 * - Normal：适合常规崩溃定位，文件较小。
 * - WithDataSegments：额外包含数据段，文件略大。
 * - WithFullMemory：包含完整进程内存，文件可能非常大，一般只在疑难问题复现时使用。
 */
enum class GB_DbgMiniDumpLevel
{
    Normal,
    WithDataSegments,
    WithFullMemory
};

/**
 * @brief RAII 方式初始化/清理当前进程的调试符号引擎。
 *
 * @remarks
 * - 构造时调用 GB_DbgInitializeSymbols；析构时调用 GB_DbgCleanupSymbols。
 * - 适合在 main、WinMain 或测试入口中创建一个局部对象。
 * - DbgHelp 更适合作为进程级设施长期初始化；不建议在业务热路径中频繁构造/析构本类。
 */
class GLOBALBASE_PORT GB_DbgSymbolScope
{
public:
    explicit GB_DbgSymbolScope(const GB_DbgSymbolOptions& options = GB_DbgSymbolOptions());
    ~GB_DbgSymbolScope();

    GB_DbgSymbolScope(const GB_DbgSymbolScope&) = delete;
    GB_DbgSymbolScope& operator = (const GB_DbgSymbolScope&) = delete;

    bool IsInitialized() const;

private:
    bool m_initialized = false;
};

/**
 * @brief 初始化当前进程的调试符号引擎。
 *
 * @param options 符号初始化选项。
 * @return true 初始化成功或已经初始化；false 初始化失败。
 *
 * @remarks
 * - Windows x64 下内部基于 DbgHelp 的 SymInitialize/SymSetOptions 等接口。
 * - DbgHelp 是进程级调试基础设施，不建议在业务热路径中反复初始化/清理。
 * - 本模块内部对 DbgHelp 调用做了互斥保护，可在多线程中安全调用本模块接口。
 */
GLOBALBASE_PORT bool GB_DbgInitializeSymbols(const GB_DbgSymbolOptions& options = GB_DbgSymbolOptions());

/**
 * @brief 清理当前进程的调试符号引擎。
 *
 * @remarks
 * 若符号引擎未初始化，本函数无操作。建议在进程退出阶段调用，而不是在业务线程中频繁调用。
 */
GLOBALBASE_PORT void GB_DbgCleanupSymbols();

/**
 * @brief 刷新当前进程已加载模块列表。
 *
 * @return true 刷新成功；false 符号引擎无法初始化或刷新失败。
 *
 * @remarks
 * 如果进程运行期间动态加载了新的 DLL，并希望解析这些 DLL 的符号，可调用本函数。
 */
GLOBALBASE_PORT bool GB_DbgRefreshSymbols();

/**
 * @brief 判断当前进程的调试符号引擎是否已经初始化。
 */
GLOBALBASE_PORT bool GB_DbgIsSymbolEngineInitialized();

/**
 * @brief 判断当前进程是否正在被用户态调试器调试。
 */
GLOBALBASE_PORT bool GB_DbgIsDebuggerPresent();

/**
 * @brief 触发断点异常。
 *
 * @remarks
 * 若当前没有调试器接管，断点异常通常会按未处理异常处理，调用前应明确这是期望行为。
 */
GLOBALBASE_PORT void GB_DbgBreak();

/**
 * @brief 仅在当前进程正在被调试时触发断点异常。
 */
GLOBALBASE_PORT void GB_DbgBreakIfDebuggerPresent();

/**
 * @brief 捕获当前线程的调用栈。
 *
 * @param skipFrameCount 额外跳过的栈帧数量。默认 0 表示返回调用者视角的栈。
 * @param maxFrameCount 最多捕获的栈帧数量。建议 16~128，过大意义有限。
 * @param resolveSymbols 是否解析模块、函数名和源码行号。
 * @return GB_DbgStackTrace 调用栈信息。失败时 frames 为空；若符号解析失败，仍会尽量返回地址帧。
 *
 * @remarks
 * - Windows x64 下当前线程采集优先使用 CaptureStackBackTrace，开销比完整 StackWalk64 更低。
 * - 当 resolveSymbols 为 true 时，本函数会自动确保符号引擎已初始化，以便解析模块、函数名和源码行号。
 * - 当 resolveSymbols 为 false 时，本函数只填充地址字段，可避免 DbgHelp 初始化和符号解析开销。
 */
GLOBALBASE_PORT GB_DbgStackTrace GB_DbgCaptureStackTrace(size_t skipFrameCount = 0, size_t maxFrameCount = 64, bool resolveSymbols = true);

/**
 * @brief 捕获当前线程的调用栈地址。
 *
 * @param skipFrameCount 额外跳过的栈帧数量。默认 0 表示返回调用者视角的栈。
 * @param maxFrameCount 最多捕获的栈帧数量。
 * @return std::vector<uint64_t> 调用栈地址。失败时返回空数组。
 *
 * @remarks
 * - 本函数只采集地址，不初始化 DbgHelp 符号引擎，也不解析模块/函数/源码行号，适合高频日志、采样或性能敏感路径。
 * - 若后续需要解析，可调用 GB_DbgResolveStackTraceAddresses。
 */
GLOBALBASE_PORT std::vector<uint64_t> GB_DbgCaptureStackTraceAddresses(size_t skipFrameCount = 0, size_t maxFrameCount = 64);


/**
 * @brief 基于 Windows EXCEPTION_POINTERS 中的 ContextRecord 捕获异常现场调用栈。
 *
 * @param contextRecord Windows CONTEXT* 指针；为避免在头文件中暴露 Windows.h，此处使用 const void*。
 * @param maxFrameCount 最多捕获的栈帧数量。
 * @param resolveSymbols 是否解析模块、函数名和源码行号。
 * @return GB_DbgStackTrace 调用栈信息。参数非法或失败时 frames 为空。
 *
 * @remarks
 * - 该接口适合在 SetUnhandledExceptionFilter、AddVectoredExceptionHandler 或 __try/__except 中使用。
 * - 与 GB_DbgCaptureStackTrace 不同，本函数从异常发生点的寄存器上下文开始回溯，更适合崩溃现场分析。
 * - Windows x64 下地址回溯优先使用系统 Unwind 元数据；只有 resolveSymbols 为 true 时才会初始化并使用 DbgHelp 符号引擎。
 * - 当前实现面向 Windows x64 当前进程、当前线程异常现场。
 */
GLOBALBASE_PORT GB_DbgStackTrace GB_DbgCaptureStackTraceFromContext(const void* contextRecord, size_t maxFrameCount = 64, bool resolveSymbols = true);

/**
 * @brief 基于 Windows CONTEXT* 捕获异常现场调用栈地址。
 *
 * @param contextRecord Windows CONTEXT* 指针；为避免在头文件中暴露 Windows.h，此处使用 const void*。
 * @param maxFrameCount 最多捕获的栈帧数量。
 * @return std::vector<uint64_t> 调用栈地址。参数非法或失败时返回空数组。
 *
 * @remarks
 * Windows x64 下本函数只采集地址，不初始化 DbgHelp 符号引擎，也不解析符号；适合崩溃处理中的低开销兜底记录。
 */
GLOBALBASE_PORT std::vector<uint64_t> GB_DbgCaptureStackTraceAddressesFromContext(const void* contextRecord, size_t maxFrameCount = 64);


/**
 * @brief 基于 Windows EXCEPTION_POINTERS* 捕获异常现场调用栈。
 *
 * @param exceptionPointers Windows EXCEPTION_POINTERS* 指针；为避免在头文件中暴露 Windows.h，此处使用 const void*。
 * @param maxFrameCount 最多捕获的栈帧数量。
 * @param resolveSymbols 是否解析模块、函数名和源码行号。
 * @return GB_DbgStackTrace 调用栈信息。参数非法或失败时 frames 为空。
 */
GLOBALBASE_PORT GB_DbgStackTrace GB_DbgCaptureStackTraceFromExceptionPointers(const void* exceptionPointers, size_t maxFrameCount = 64, bool resolveSymbols = true);

/**
 * @brief 基于 Windows EXCEPTION_POINTERS* 捕获异常现场调用栈地址。
 *
 * @param exceptionPointers Windows EXCEPTION_POINTERS* 指针；为避免在头文件中暴露 Windows.h，此处使用 const void*。
 * @param maxFrameCount 最多捕获的栈帧数量。
 * @return std::vector<uint64_t> 调用栈地址。参数非法或失败时返回空数组。
 */
GLOBALBASE_PORT std::vector<uint64_t> GB_DbgCaptureStackTraceAddressesFromExceptionPointers(const void* exceptionPointers, size_t maxFrameCount = 64);


/**
 * @brief 解析单个程序地址的符号信息。
 *
 * @param address 程序地址。
 * @param frame 输出栈帧信息。
 * @return true 至少解析到模块、函数名或源码位置之一；false 完全无法解析。
 */
GLOBALBASE_PORT bool GB_DbgResolveAddress(uint64_t address, GB_DbgStackFrame& frame);

/**
 * @brief 批量解析调用栈地址。
 *
 * @param addresses 调用栈地址数组。
 * @return GB_DbgStackTrace 解析后的调用栈信息。若符号解析失败，仍会尽量返回仅包含地址或模块信息的帧。
 *
 * @remarks
 * 该接口适合与 GB_DbgCaptureStackTraceAddresses 配合使用：热路径先只记录地址，非热路径再统一解析。
 */
GLOBALBASE_PORT GB_DbgStackTrace GB_DbgResolveStackTraceAddresses(const std::vector<uint64_t>& addresses);


/**
 * @brief 将调用栈格式化为便于日志记录的 UTF-8 文本。
 *
 * @param stackTrace 调用栈。
 * @param withFrameIndex 是否输出栈帧序号。
 * @return std::string 格式化后的调用栈文本。
 */
GLOBALBASE_PORT std::string GB_DbgFormatStackTrace(const GB_DbgStackTrace& stackTrace, bool withFrameIndex = true);

/**
 * @brief 将 Windows 系统错误码格式化为 UTF-8 文本。
 *
 * @param errorCode Windows 错误码，例如 GetLastError() 的返回值。
 * @return std::string 系统错误说明；若系统无法提供说明，则返回包含错误码的兜底文本。
 */
GLOBALBASE_PORT std::string GB_DbgFormatSystemErrorMessage(uint32_t errorCode);

/**
 * @brief 获取当前线程最近一次 Windows 错误码的 UTF-8 文本说明。
 */
GLOBALBASE_PORT std::string GB_DbgGetLastSystemErrorMessage();

/**
 * @brief 将 UTF-8 文本输出到调试器窗口。
 *
 * @param textUtf8 待输出文本。
 *
 * @remarks
 * Windows 下输出到 OutputDebugStringW；非调试状态下通常不会产生可见效果。
 */
GLOBALBASE_PORT void GB_DbgOutputDebugStringUtf8(const std::string& textUtf8);

/**
 * @brief 捕获当前调用栈并输出到调试器窗口。
 *
 * @param skipFrameCount 额外跳过的栈帧数量。
 * @param maxFrameCount 最多捕获的栈帧数量。
 */
GLOBALBASE_PORT void GB_DbgOutputCurrentStackTrace(size_t skipFrameCount = 0, size_t maxFrameCount = 64);

/**
 * @brief 获取 Windows SEH 异常码名称。
 *
 * @param exceptionCode 异常码，例如 0xC0000005。
 * @return std::string 异常名称；未知异常返回 "UNKNOWN_EXCEPTION"。
 */
GLOBALBASE_PORT std::string GB_DbgGetExceptionCodeName(uint32_t exceptionCode);

/**
 * @brief 获取 Windows SEH 异常码的中文说明。
 *
 * @param exceptionCode 异常码，例如 0xC0000005。
 * @return std::string UTF-8 中文说明；未知异常返回简要未知说明。
 */
GLOBALBASE_PORT std::string GB_DbgGetExceptionCodeDescription(uint32_t exceptionCode);

/**
 * @brief 写出当前进程的 MiniDump 文件。
 *
 * @param dumpFilePathUtf8 dump 文件路径（UTF-8）。Windows 下内部转换为 UTF-16。
 * @param exceptionPointers 可选的 Windows EXCEPTION_POINTERS* 指针；没有异常上下文时传 nullptr。
 * @param level MiniDump 写出级别。
 * @return true 写出成功；false 写出失败。
 *
 * @remarks
 * - 该接口面向“当前进程”写 dump。
 * - 如果 exceptionPointers 不为空，默认使用当前线程 ID 作为异常线程 ID。
 * - 如果在崩溃现场调用，建议尽量在专用异常处理线程或外部进程中调用，避免崩溃线程栈空间不足或进程锁状态不稳定造成二次问题。
 */
GLOBALBASE_PORT bool GB_DbgWriteMiniDump(const std::string& dumpFilePathUtf8, const void* exceptionPointers = nullptr, GB_DbgMiniDumpLevel level = GB_DbgMiniDumpLevel::Normal);

/**
 * @brief 写出当前进程的 MiniDump 文件，并显式指定异常线程 ID。
 *
 * @param dumpFilePathUtf8 dump 文件路径（UTF-8）。Windows 下内部转换为 UTF-16。
 * @param exceptionPointers 可选的 Windows EXCEPTION_POINTERS* 指针；没有异常上下文时传 nullptr。
 * @param exceptionThreadId 异常线程 ID；传 0 时自动使用当前线程 ID。
 * @param level MiniDump 写出级别。
 * @return true 写出成功；false 写出失败。
 */
GLOBALBASE_PORT bool GB_DbgWriteMiniDumpEx(const std::string& dumpFilePathUtf8, const void* exceptionPointers, uint32_t exceptionThreadId, GB_DbgMiniDumpLevel level = GB_DbgMiniDumpLevel::Normal);

#ifdef _MSC_VER
# pragma warning(pop)
#endif

#endif
