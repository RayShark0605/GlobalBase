#ifndef GLOBALBASE_COM_SCOPE_H_H
#define GLOBALBASE_COM_SCOPE_H_H

#include "GB_SystemResult.h"

#include <cstdint>
#include <string>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief COM 线程单元模型。
 *
 * 说明：
 * - COM 初始化是线程级行为，每个需要使用 COM 的线程都应独立初始化；
 * - UI 线程、Shell 相关线程、需要窗口消息循环的线程通常使用 SingleThreaded；
 * - 后台工作线程、线程池任务、无窗口消息循环的系统能力通常使用 MultiThreaded；
 * - 同一线程已经使用某一种单元模型初始化后，不能在未完全反初始化前切换为另一种单元模型。
 */
enum class GB_ComApartmentModel : uint16_t
{
    /** @brief 多线程单元，对应 COINIT_MULTITHREADED。 */
    MultiThreaded = 0,

    /** @brief 单线程单元，对应 COINIT_APARTMENTTHREADED。 */
    SingleThreaded = 1
};

/**
 * @brief COM 初始化选项。
 *
 * 说明：
 * - apartmentModel 指定 CoInitializeEx 的线程单元模型；
 * - disableOle1Dde 对应 COINIT_DISABLE_OLE1DDE，通常可用于现代程序避免旧式 OLE 1.0 DDE 行为；
 * - speedOverMemory 对应 COINIT_SPEED_OVER_MEMORY，表示优先速度而非内存占用；
 * - 本结构不依赖 Windows 头文件，便于在公共头文件中稳定暴露。
 */
struct GB_ComInitializeOptions
{
    /** @brief COM 线程单元模型。 */
    GB_ComApartmentModel apartmentModel = GB_ComApartmentModel::MultiThreaded;

    /** @brief 是否启用 COINIT_DISABLE_OLE1DDE。 */
    bool disableOle1Dde = false;

    /** @brief 是否启用 COINIT_SPEED_OVER_MEMORY。 */
    bool speedOverMemory = false;
};

/**
 * @brief COM 安全初始化选项。
 *
 * 说明：
 * - 该结构用于封装 CoInitializeSecurity 的常用客户端初始化参数；
 * - authenticationServiceCount 默认 -1，表示由 COM 选择认证服务；当前封装未暴露 asAuthSvc 数组，因此只允许 -1 或 0；
 * - authenticationLevel 默认 0，对应 RPC_C_AUTHN_LEVEL_DEFAULT；
 * - impersonationLevel 默认 3，对应 RPC_C_IMP_LEVEL_IMPERSONATE；不允许使用 RPC_C_IMP_LEVEL_DEFAULT；
 * - capabilities 默认 0，对应 EOAC_NONE；
 * - CoInitializeSecurity 是进程级设置，没有与之对应的反初始化函数，因此本模块只提供一次性初始化函数，不做 RAII 反初始化。
 */
struct GB_ComSecurityOptions
{
    /** @brief 认证服务数量，默认 -1 表示让 COM 自动注册认证服务；当前封装不暴露 asAuthSvc 数组，因此只允许 -1 或 0。 */
    int32_t authenticationServiceCount = -1;

    /** @brief 认证级别，默认 RPC_C_AUTHN_LEVEL_DEFAULT。 */
    uint32_t authenticationLevel = 0;

    /** @brief 模拟级别，默认 RPC_C_IMP_LEVEL_IMPERSONATE；CoInitializeSecurity 不应使用 RPC_C_IMP_LEVEL_DEFAULT。 */
    uint32_t impersonationLevel = 3;

    /** @brief 其他能力标志，默认 EOAC_NONE。 */
    uint32_t capabilities = 0;

    /** @brief CoInitializeSecurity 已经由进程内其它代码调用时，是否把 RPC_E_TOO_LATE 视为成功。 */
    bool treatAlreadyInitializedAsSucceeded = true;
};

/**
 * @brief COM 初始化 RAII 作用域。
 *
 * 说明：
 * - 本类只负责当前线程的 CoInitializeEx / CoUninitialize 配平，不负责创建任何业务 COM 对象；
 * - CoInitializeEx 返回 S_OK 或 S_FALSE 时均表示初始化成功，本类都会在析构或 Uninitialize() 中调用 CoUninitialize；
 * - CoInitializeEx 返回 RPC_E_CHANGED_MODE 等失败 HRESULT 时，本类不会调用 CoUninitialize；
 * - COM 初始化和反初始化必须发生在同一线程，本类会记录初始化线程 ID，并拒绝在非初始化线程调用 CoUninitialize；
 * - 本类不可复制、不可移动，避免 COM 初始化配平责任被转移到其他线程；
 * - 所有 std::string 均约定为 UTF-8 编码。
 */
class GLOBALBASE_PORT GB_ComScope final
{
public:
    /**
     * @brief 构造空作用域，不执行 COM 初始化。
     */
    GB_ComScope();

    /**
     * @brief 构造并按指定单元模型初始化 COM。
     */
    explicit GB_ComScope(GB_ComApartmentModel apartmentModel, const std::string& operationName = std::string());

    /**
     * @brief 构造并按指定选项初始化 COM。
     */
    explicit GB_ComScope(const GB_ComInitializeOptions& options, const std::string& operationName = std::string());

    /**
     * @brief 析构并在初始化线程自动调用 CoUninitialize。
     */
    ~GB_ComScope() noexcept;

    GB_ComScope(const GB_ComScope&) = delete;
    GB_ComScope& operator=(const GB_ComScope&) = delete;
    GB_ComScope(GB_ComScope&&) = delete;
    GB_ComScope& operator=(GB_ComScope&&) = delete;

    /** @brief 创建 STA 初始化选项。 */
    static GB_ComInitializeOptions MakeStaOptions(bool disableOle1Dde = true, bool speedOverMemory = false);

    /** @brief 创建 MTA 初始化选项。 */
    static GB_ComInitializeOptions MakeMtaOptions(bool disableOle1Dde = false, bool speedOverMemory = false);

    /** @brief 创建默认 COM 安全初始化选项。 */
    static GB_ComSecurityOptions MakeDefaultSecurityOptions(bool treatAlreadyInitializedAsSucceeded = true);

    /**
     * @brief 初始化当前线程 COM。
     *
     * 说明：
     * - 若当前对象已经持有一次成功初始化，返回 AlreadyInitialized；
     * - 若当前线程已用相同单元模型初始化，CoInitializeEx 可能返回 S_FALSE，本函数仍视为成功，并在之后配平 CoUninitialize；
     * - 若当前线程已用不同单元模型初始化，CoInitializeEx 通常返回 RPC_E_CHANGED_MODE，本函数返回失败结果。
     */
    GB_SystemResult Initialize(const GB_ComInitializeOptions& options, const std::string& operationName = std::string());

    /** @brief 按指定单元模型初始化当前线程 COM。 */
    GB_SystemResult Initialize(GB_ComApartmentModel apartmentModel, const std::string& operationName = std::string());

    /**
     * @brief 显式反初始化当前作用域持有的 COM 初始化。
     *
     * 说明：
     * - 只有成功执行过 Initialize() 的对象才会调用 CoUninitialize；
     * - 如果当前线程不是初始化线程，返回 InvalidState，并且不会调用 CoUninitialize。
     */
    GB_SystemResult Uninitialize();

    /**
     * @brief 重置当前作用域，先反初始化已有 COM 初始化，再按新选项重新初始化。
     */
    GB_SystemResult Reset(const GB_ComInitializeOptions& options, const std::string& operationName = std::string());

    /** @brief 重置当前作用域，先反初始化已有 COM 初始化，再按新单元模型重新初始化。 */
    GB_SystemResult Reset(GB_ComApartmentModel apartmentModel, const std::string& operationName = std::string());

    /**
     * @brief 释放配平责任，不调用 CoUninitialize。
     *
     * 说明：
     * - 仅当外部代码明确接管 CoUninitialize 配平责任时才应调用；
     * - 调用后本对象回到空作用域状态。
     */
    GB_SystemResult Detach();

    /** @brief 判断当前对象是否已经成功初始化并持有配平责任。 */
    bool IsInitialized() const;

    /** @brief 判断当前对象是否为空作用域。 */
    bool IsEmpty() const;

    /** @brief 判断当前对象是否持有需要 CoUninitialize 配平的初始化。 */
    bool HasOwnership() const;

    /** @brief 判断当前作用域的初始化是否发生在当前线程。 */
    bool IsCurrentThreadOwner() const;

    /** @brief 判断 CoInitializeEx 是否返回过 S_FALSE。 */
    bool IsAlreadyInitializedOnThread() const;

    /** @brief 显式 bool 转换。true 表示当前对象已经成功初始化 COM。 */
    explicit operator bool() const;

    /** @brief 获取初始化选项。 */
    GB_ComInitializeOptions GetInitializeOptions() const;

    /** @brief 获取初始化使用的 COM 单元模型。 */
    GB_ComApartmentModel GetApartmentModel() const;

    /** @brief 获取初始化线程 ID。非 Windows 平台或尚未初始化时返回 0。 */
    uint64_t GetOwnerThreadId() const;

    /** @brief 获取 CoInitializeEx 返回的 HRESULT。 */
    int32_t GetInitializeHResult() const;

    /** @brief 获取上一次 Initialize() 的结果。 */
    GB_SystemResult GetInitializeResult() const;

    /** @brief 获取上一次 Uninitialize() / Detach() 的结果。 */
    GB_SystemResult GetLastUninitializeResult() const;

    /** @brief 判断指定 COM 单元模型数值是否有效。 */
    static bool IsValidApartmentModelValue(uint64_t apartmentModelValue);

    /** @brief 判断初始化选项是否合法。 */
    static bool IsValidInitializeOptions(const GB_ComInitializeOptions& options);

    /** @brief 判断 COM 安全初始化选项是否合法。 */
    static bool IsValidSecurityOptions(const GB_ComSecurityOptions& options);

    /** @brief 获取 COM 单元模型英文名称。 */
    static std::string GetApartmentModelName(GB_ComApartmentModel apartmentModel);

    /** @brief 获取 COM 单元模型中文描述。 */
    static std::string GetApartmentModelDescription(GB_ComApartmentModel apartmentModel);

    /** @brief 将初始化选项转换为 CoInitializeEx 标志位。 */
    static uint32_t BuildCoInitializeExFlags(const GB_ComInitializeOptions& options);

    /**
     * @brief 使用默认客户端参数初始化进程级 COM 安全。
     *
     * 说明：
     * - 该函数封装 CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr)；
     * - CoInitializeSecurity 是进程级初始化，通常应尽早调用；
     * - 如果 COM 已经自动或显式初始化安全，CoInitializeSecurity 会返回 RPC_E_TOO_LATE；
     * - 当 options.treatAlreadyInitializedAsSucceeded 为 true 时，RPC_E_TOO_LATE 会被视为成功结果。
     */
    static GB_SystemResult InitializeSecurity(const GB_ComSecurityOptions& options = GB_ComSecurityOptions(), const std::string& operationName = std::string());

private:
    void CloseSilently() noexcept;
    void ClearInitializationState() noexcept;

private:
    bool initialized = false;
    bool alreadyInitializedOnThread = false;
    GB_ComInitializeOptions initializeOptions;
    uint64_t ownerThreadId = 0;
    int32_t initializeHResult = 0;
    GB_SystemResult initializeResult;
    GB_SystemResult lastUninitializeResult;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_COM_SCOPE_H_H
