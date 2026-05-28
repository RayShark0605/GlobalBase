#ifndef GLOBALBASE_PRIVILEGE_SCOPE_H_H
#define GLOBALBASE_PRIVILEGE_SCOPE_H_H

#include "GB_SystemResult.h"

#include <cstdint>
#include <string>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief Windows 访问令牌目标。
 *
 * 说明：
 * - CurrentProcess 表示调整当前进程主令牌中的权限，适合关机、重启、调试其它进程等大多数系统自动化能力；
 * - CurrentThread 表示调整当前线程模拟令牌中的权限，适合线程正在 impersonation 的场景；
 * - CurrentThreadThenProcess 表示优先使用当前线程令牌；若当前线程没有令牌，则回退到当前进程令牌；
 * - 非 Windows 平台下仅保留枚举和接口，实际调整操作会返回 UnsupportedPlatform。
 */
enum class GB_PrivilegeTokenTarget : uint16_t
{
    /** @brief 当前进程主令牌。 */
    CurrentProcess = 0,

    /** @brief 当前线程模拟令牌。 */
    CurrentThread = 1,

    /** @brief 优先当前线程模拟令牌；当前线程无令牌时回退到当前进程主令牌。 */
    CurrentThreadThenProcess = 2
};

/**
 * @brief 权限调整动作。
 *
 * 说明：
 * - Enable 表示启用令牌中已经存在的指定权限；
 * - Disable 表示禁用令牌中已经存在的指定权限；
 * - 本模块不会移除权限，因为 Windows 的 SE_PRIVILEGE_REMOVED 行为不可逆，不适合作为 RAII 作用域能力。
 */
enum class GB_PrivilegeAction : uint16_t
{
    /** @brief 启用权限。 */
    Enable = 0,

    /** @brief 禁用权限。 */
    Disable = 1
};

/**
 * @brief 常用 Windows 权限名称枚举。
 *
 * 说明：
 * - 本枚举只覆盖系统自动化、诊断、备份恢复、服务/驱动管理等常见场景；
 * - 未列出的权限仍可直接通过字符串形式传入，例如 "SeShutdownPrivilege"；
 * - 枚举值只用于获取权限名称字符串，不表示当前用户或当前令牌一定拥有对应权限。
 */
enum class GB_WindowsPrivilege : uint16_t
{
    /** @brief SeCreateTokenPrivilege。 */
    CreateToken = 0,

    /** @brief SeAssignPrimaryTokenPrivilege。 */
    AssignPrimaryToken = 1,

    /** @brief SeLockMemoryPrivilege。 */
    LockMemory = 2,

    /** @brief SeIncreaseQuotaPrivilege。 */
    IncreaseQuota = 3,

    /** @brief SeMachineAccountPrivilege。 */
    MachineAccount = 4,

    /** @brief SeTcbPrivilege。 */
    Tcb = 5,

    /** @brief SeSecurityPrivilege。 */
    Security = 6,

    /** @brief SeTakeOwnershipPrivilege。 */
    TakeOwnership = 7,

    /** @brief SeLoadDriverPrivilege。 */
    LoadDriver = 8,

    /** @brief SeSystemProfilePrivilege。 */
    SystemProfile = 9,

    /** @brief SeSystemtimePrivilege。 */
    Systemtime = 10,

    /** @brief SeProfileSingleProcessPrivilege。 */
    ProfileSingleProcess = 11,

    /** @brief SeIncreaseBasePriorityPrivilege。 */
    IncreaseBasePriority = 12,

    /** @brief SeCreatePagefilePrivilege。 */
    CreatePagefile = 13,

    /** @brief SeCreatePermanentPrivilege。 */
    CreatePermanent = 14,

    /** @brief SeBackupPrivilege。 */
    Backup = 15,

    /** @brief SeRestorePrivilege。 */
    Restore = 16,

    /** @brief SeShutdownPrivilege。 */
    Shutdown = 17,

    /** @brief SeDebugPrivilege。 */
    Debug = 18,

    /** @brief SeAuditPrivilege。 */
    Audit = 19,

    /** @brief SeSystemEnvironmentPrivilege。 */
    SystemEnvironment = 20,

    /** @brief SeChangeNotifyPrivilege。 */
    ChangeNotify = 21,

    /** @brief SeRemoteShutdownPrivilege。 */
    RemoteShutdown = 22,

    /** @brief SeUndockPrivilege。 */
    Undock = 23,

    /** @brief SeManageVolumePrivilege。 */
    ManageVolume = 24,

    /** @brief SeImpersonatePrivilege。 */
    Impersonate = 25,

    /** @brief SeCreateGlobalPrivilege。 */
    CreateGlobal = 26,

    /** @brief SeIncreaseWorkingSetPrivilege。 */
    IncreaseWorkingSet = 27,

    /** @brief SeTimeZonePrivilege。 */
    TimeZone = 28,

    /** @brief SeCreateSymbolicLinkPrivilege。 */
    CreateSymbolicLinkPrivilege = 29,

    /** @brief SeDelegateSessionUserImpersonatePrivilege。 */
    DelegateSessionUserImpersonate = 30
};

/**
 * @brief 权限信息。
 *
 * 说明：
 * - privilegeName 为 UTF-8 编码的 Windows 权限名称；
 * - luidLowPart / luidHighPart 保存 LookupPrivilegeValue 解析出的 LUID；
 * - attributes 保存令牌中该权限的原始属性位，便于日志诊断；
 * - exists 为 false 时，表示当前令牌中没有该权限，通常无法通过 AdjustTokenPrivileges 启用；
 * - enabled 表示当前权限是否处于启用状态。
 */
struct GB_PrivilegeInfo
{
    /** @brief 权限名称，UTF-8 编码。 */
    std::string privilegeName = "";

    /** @brief LUID 低 32 位。 */
    uint32_t luidLowPart = 0;

    /** @brief LUID 高 32 位。 */
    int32_t luidHighPart = 0;

    /** @brief 原始权限属性位。 */
    uint32_t attributes = 0;

    /** @brief 当前令牌是否包含该权限。 */
    bool exists = false;

    /** @brief 当前权限是否启用。 */
    bool enabled = false;

    /** @brief 当前权限是否默认启用。 */
    bool enabledByDefault = false;

    /** @brief 当前权限是否被标记为已移除。 */
    bool removed = false;

    /** @brief 当前权限是否在一次访问检查中被使用过。 */
    bool usedForAccess = false;
};

/**
 * @brief 权限作用域选项。
 *
 * 说明：
 * - privilegeName 使用 Windows 权限名称，例如 "SeShutdownPrivilege"、"SeDebugPrivilege"；
 * - action 指定启用或禁用；
 * - tokenTarget 指定要调整的访问令牌；
 * - openThreadAsSelf 仅用于 OpenThreadToken；
 * - restoreOnDestruct 为 true 时，GB_PrivilegeScope 会在 Restore() 或析构时尽量恢复调整前状态。
 */
struct GB_PrivilegeScopeOptions
{
    /** @brief 权限名称，UTF-8 编码。 */
    std::string privilegeName = "";

    /** @brief 权限调整动作。 */
    GB_PrivilegeAction action = GB_PrivilegeAction::Enable;

    /** @brief 访问令牌目标。 */
    GB_PrivilegeTokenTarget tokenTarget = GB_PrivilegeTokenTarget::CurrentProcess;

    /** @brief 调用 OpenThreadToken 时是否使用 OpenAsSelf。 */
    bool openThreadAsSelf = true;

    /** @brief 析构或 Restore() 时是否恢复调整前状态。 */
    bool restoreOnDestruct = true;
};

/**
 * @brief Windows 权限 RAII 作用域。
 *
 * 说明：
 * - 本类用于在一个明确的 C++ 作用域内临时启用或禁用 Windows 访问令牌中的指定权限；
 * - 构造或 Adjust() 成功后，会保存被修改权限的旧状态，并在 Restore() 或析构时恢复；
 * - 若目标权限已经处于期望状态，Windows 可能不会返回旧状态，此时本类会认为没有发生实际修改，析构时只关闭令牌句柄；
 * - AdjustTokenPrivileges 不能给令牌新增权限；如果当前令牌不包含指定权限，会返回失败结果；
 * - 本类不会移除权限，不使用 SE_PRIVILEGE_REMOVED，避免不可逆副作用；
 * - 进程令牌权限调整会影响当前进程，调用方应尽量缩小作用域，并避免多个线程并发调整同一权限；
 * - 本类不可复制，只能移动；移动后由新对象负责恢复权限状态；
 * - std::string 均约定为 UTF-8 编码；
 * - 该类不保证线程安全，同一个实例不应被多个线程并发读写。
 */
class GLOBALBASE_PORT GB_PrivilegeScope final
{
public:
    /** @brief 原生令牌句柄存储类型。 */
    using NativeTokenHandle = void*;

public:
    /**
     * @brief 构造空权限作用域，不执行任何权限调整。
     */
    GB_PrivilegeScope();

    /**
     * @brief 构造并调整当前进程令牌中的指定权限。
     */
    explicit GB_PrivilegeScope(const std::string& privilegeName, GB_PrivilegeAction action = GB_PrivilegeAction::Enable, const std::string& operationName = std::string());

    /**
     * @brief 构造并调整当前进程令牌中的指定常用权限。
     */
    explicit GB_PrivilegeScope(GB_WindowsPrivilege privilege, GB_PrivilegeAction action = GB_PrivilegeAction::Enable, const std::string& operationName = std::string());

    /**
     * @brief 构造并按指定选项调整权限。
     */
    explicit GB_PrivilegeScope(const GB_PrivilegeScopeOptions& options, const std::string& operationName = std::string());

    /**
     * @brief 析构并在需要时自动恢复权限旧状态。
     */
    ~GB_PrivilegeScope() noexcept;

    GB_PrivilegeScope(const GB_PrivilegeScope&) = delete;
    GB_PrivilegeScope& operator=(const GB_PrivilegeScope&) = delete;

    /** @brief 移动构造，转移权限恢复责任。 */
    GB_PrivilegeScope(GB_PrivilegeScope&& other);

    /** @brief 移动赋值，先恢复当前权限作用域，再接管 @p other 的恢复责任。 */
    GB_PrivilegeScope& operator=(GB_PrivilegeScope&& other);

    /** @brief 创建默认启用权限选项。 */
    static GB_PrivilegeScopeOptions MakeEnableOptions(const std::string& privilegeName, GB_PrivilegeTokenTarget tokenTarget = GB_PrivilegeTokenTarget::CurrentProcess, bool restoreOnDestruct = true);

    /** @brief 创建默认禁用权限选项。 */
    static GB_PrivilegeScopeOptions MakeDisableOptions(const std::string& privilegeName, GB_PrivilegeTokenTarget tokenTarget = GB_PrivilegeTokenTarget::CurrentProcess, bool restoreOnDestruct = true);

    /** @brief 创建默认启用常用权限选项。 */
    static GB_PrivilegeScopeOptions MakeEnableOptions(GB_WindowsPrivilege privilege, GB_PrivilegeTokenTarget tokenTarget = GB_PrivilegeTokenTarget::CurrentProcess, bool restoreOnDestruct = true);

    /** @brief 创建默认禁用常用权限选项。 */
    static GB_PrivilegeScopeOptions MakeDisableOptions(GB_WindowsPrivilege privilege, GB_PrivilegeTokenTarget tokenTarget = GB_PrivilegeTokenTarget::CurrentProcess, bool restoreOnDestruct = true);

    /** @brief 创建并启用当前进程令牌中的指定权限。 */
    static GB_PrivilegeScope EnableProcessPrivilege(const std::string& privilegeName, const std::string& operationName = std::string());

    /** @brief 创建并启用当前进程令牌中的指定常用权限。 */
    static GB_PrivilegeScope EnableProcessPrivilege(GB_WindowsPrivilege privilege, const std::string& operationName = std::string());

    /** @brief 创建并启用当前线程令牌中的指定权限。 */
    static GB_PrivilegeScope EnableThreadPrivilege(const std::string& privilegeName, const std::string& operationName = std::string(), bool openThreadAsSelf = true);

    /** @brief 创建并启用当前线程令牌中的指定常用权限。 */
    static GB_PrivilegeScope EnableThreadPrivilege(GB_WindowsPrivilege privilege, const std::string& operationName = std::string(), bool openThreadAsSelf = true);

    /**
     * @brief 按指定选项调整权限。
     *
     * 说明：
     * - 当前对象已经持有一次成功权限调整时，本函数返回 AlreadyInitialized；
     * - 如需重新调整，请使用 Reset()；
     * - 成功后本对象会持有令牌句柄，直到 Restore()、Detach() 或析构。
     */
    GB_SystemResult Adjust(const GB_PrivilegeScopeOptions& options, const std::string& operationName = std::string());

    /** @brief 启用当前进程令牌中的指定权限。 */
    GB_SystemResult Enable(const std::string& privilegeName, const std::string& operationName = std::string());

    /** @brief 禁用当前进程令牌中的指定权限。 */
    GB_SystemResult Disable(const std::string& privilegeName, const std::string& operationName = std::string());

    /** @brief 启用当前进程令牌中的指定常用权限。 */
    GB_SystemResult Enable(GB_WindowsPrivilege privilege, const std::string& operationName = std::string());

    /** @brief 禁用当前进程令牌中的指定常用权限。 */
    GB_SystemResult Disable(GB_WindowsPrivilege privilege, const std::string& operationName = std::string());

    /** @brief 先恢复当前权限状态，再按新选项重新调整。 */
    GB_SystemResult Reset(const GB_PrivilegeScopeOptions& options, const std::string& operationName = std::string());

    /**
     * @brief 显式恢复权限旧状态。
     *
     * 说明：
     * - 若当前对象没有持有权限调整责任，返回成功；
     * - 若权限调整时没有发生实际状态变化，返回成功并只释放令牌句柄；
     * - 若 restoreOnDestruct 为 false，返回成功并只释放令牌句柄，不恢复权限状态。
     */
    GB_SystemResult Restore();

    /**
     * @brief 释放权限恢复责任，不恢复权限旧状态。
     *
     * 说明：调用后会关闭内部令牌句柄，并使当前对象回到空作用域状态。
     */
    GB_SystemResult Detach();

    /** @brief 判断当前对象是否已经成功调整权限并持有令牌句柄。 */
    bool IsAdjusted() const;

    /** @brief 判断当前对象是否为空作用域。 */
    bool IsEmpty() const;

    /** @brief 判断当前对象是否持有权限恢复责任。 */
    bool HasOwnership() const;

    /** @brief 判断权限调整是否实际修改了令牌权限状态。 */
    bool WasStateChanged() const;

    /** @brief 判断析构时是否会尝试恢复权限旧状态。 */
    bool IsRestoreOnDestruct() const;

    /** @brief 显式 bool 转换。true 表示当前对象成功执行过权限调整。 */
    explicit operator bool() const;

    /** @brief 获取原生令牌句柄。 */
    NativeTokenHandle GetTokenHandle() const;

    /** @brief 获取原生令牌句柄整数值，便于日志诊断。 */
    uint64_t GetTokenHandleValue() const;

    /** @brief 获取当前作用域选项。 */
    GB_PrivilegeScopeOptions GetOptions() const;

    /** @brief 获取权限名称。 */
    std::string GetPrivilegeName() const;

    /** @brief 获取权限调整动作。 */
    GB_PrivilegeAction GetAction() const;

    /** @brief 获取请求的令牌目标。 */
    GB_PrivilegeTokenTarget GetRequestedTokenTarget() const;

    /** @brief 获取实际打开的令牌目标。 */
    GB_PrivilegeTokenTarget GetOpenedTokenTarget() const;

    /** @brief 获取请求写入的权限属性位。 */
    uint32_t GetRequestedAttributes() const;

    /** @brief 获取调整前权限信息。 */
    GB_PrivilegeInfo GetPreviousPrivilegeInfo() const;

    /** @brief 获取调整后权限信息。 */
    GB_PrivilegeInfo GetAdjustedPrivilegeInfo() const;

    /** @brief 获取上一次 Adjust() 的结果。 */
    GB_SystemResult GetAdjustResult() const;

    /** @brief 获取上一次 Restore() / Detach() 的结果。 */
    GB_SystemResult GetLastRestoreResult() const;

    /** @brief 交换两个权限作用域对象。 */
    void Swap(GB_PrivilegeScope& other);

    /** @brief 判断指定令牌目标数值是否有效。 */
    static bool IsValidTokenTargetValue(uint64_t tokenTargetValue);

    /** @brief 判断指定权限调整动作数值是否有效。 */
    static bool IsValidActionValue(uint64_t actionValue);

    /** @brief 判断指定常用权限枚举数值是否有效。 */
    static bool IsValidWindowsPrivilegeValue(uint64_t privilegeValue);

    /** @brief 判断权限名称是否合法。 */
    static bool IsValidPrivilegeName(const std::string& privilegeName);

    /** @brief 判断权限作用域选项是否合法。 */
    static bool IsValidOptions(const GB_PrivilegeScopeOptions& options);

    /** @brief 获取令牌目标英文名称。 */
    static std::string GetTokenTargetName(GB_PrivilegeTokenTarget tokenTarget);

    /** @brief 获取令牌目标中文描述。 */
    static std::string GetTokenTargetDescription(GB_PrivilegeTokenTarget tokenTarget);

    /** @brief 获取权限调整动作英文名称。 */
    static std::string GetActionName(GB_PrivilegeAction action);

    /** @brief 获取权限调整动作中文描述。 */
    static std::string GetActionDescription(GB_PrivilegeAction action);

    /** @brief 获取常用 Windows 权限名称。 */
    static std::string GetWindowsPrivilegeName(GB_WindowsPrivilege privilege);

    /** @brief 获取常用 Windows 权限中文描述。 */
    static std::string GetWindowsPrivilegeDescription(GB_WindowsPrivilege privilege);

    /** @brief 根据动作构造写入 AdjustTokenPrivileges 的权限属性位。 */
    static uint32_t BuildPrivilegeAttributes(GB_PrivilegeAction action);

    /**
     * @brief 查询当前令牌中的指定权限信息。
     *
     * 说明：
     * - 该函数只查询权限，不调整权限；
     * - 查询成功但 exists=false 时，表示令牌中没有该权限；
     * - 非 Windows 平台下返回 UnsupportedPlatform。
     */
    static GB_SystemResult QueryPrivilegeInfo(const std::string& privilegeName, GB_PrivilegeInfo& privilegeInfo, GB_PrivilegeTokenTarget tokenTarget = GB_PrivilegeTokenTarget::CurrentProcess, bool openThreadAsSelf = true);

    /** @brief 查询当前令牌中的指定常用权限信息。 */
    static GB_SystemResult QueryPrivilegeInfo(GB_WindowsPrivilege privilege, GB_PrivilegeInfo& privilegeInfo, GB_PrivilegeTokenTarget tokenTarget = GB_PrivilegeTokenTarget::CurrentProcess, bool openThreadAsSelf = true);

private:
    void CloseSilently() noexcept;
    void ClearPrivilegeState() noexcept;
    void MoveFrom(GB_PrivilegeScope& other);

private:
    NativeTokenHandle tokenHandle = nullptr;
    bool adjusted = false;
    bool stateChanged = false;
    bool restoreOnDestruct = true;
    GB_PrivilegeScopeOptions options;
    GB_PrivilegeTokenTarget openedTokenTarget = GB_PrivilegeTokenTarget::CurrentProcess;
    uint32_t requestedAttributes = 0;
    GB_PrivilegeInfo previousPrivilegeInfo;
    GB_PrivilegeInfo adjustedPrivilegeInfo;
    GB_SystemResult adjustResult;
    GB_SystemResult lastRestoreResult;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_PRIVILEGE_SCOPE_H_H
