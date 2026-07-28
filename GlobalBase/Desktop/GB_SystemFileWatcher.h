#ifndef GLOBALBASE_SYSTEM_FILE_WATCHER_H_H
#define GLOBALBASE_SYSTEM_FILE_WATCHER_H_H

#include "../GlobalBasePort.h"
#include "GB_EventDispatcher.h"
#include "GB_SystemResult.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief 文件监听器整体生命周期状态。
 *
 * @details
 * 该状态描述 GB_SystemFileWatcher 实例级别的运行状态，不表示某一个具体监听目标的状态。
 */
enum class GB_SystemFileWatcherState : uint16_t
{
    /** @brief 监听器未启动，内部线程和底层目录监听句柄均未运行。 */
    Stopped = 0,

    /** @brief 监听器正在启动，正在创建分发线程、恢复线程、IOCP 线程或底层目录监听句柄。 */
    Starting = 1,

    /** @brief 监听器正在正常监听文件系统变化。 */
    Watching = 2,

    /** @brief 监听器已暂停，普通文件变化事件不会继续投递，但底层变化会被记录为需要恢复扫描。 */
    Paused = 3,

    /** @brief 监听器正在执行快照恢复或重建状态，恢复完成后通常会回到 Watching。 */
    Recovering = 4,

    /** @brief 监听器正在停止，内部线程和底层目录监听句柄正在释放。 */
    Stopping = 5,

    /** @brief 监听器发生不可自动恢复的错误，调用方应读取结果信息并重新创建或停止后再启动。 */
    Failed = 6
};

/**
 * @brief 单个文件或目录监听目标的状态。
 *
 * @details
 * 一个监听器可以包含多个监听目标。某个目标不可用不一定表示整个监听器不可用。
 */
enum class GB_SystemFileWatchTargetState : uint16_t
{
    /** @brief 目标未参与监听，通常表示监听器整体尚未启动或已经停止。 */
    Stopped = 0,

    /** @brief 目标正在正常监听。 */
    Watching = 1,

    /** @brief 目标随监听器进入暂停状态。 */
    Paused = 2,

    /** @brief 目标正在执行快照恢复或状态重建。 */
    Recovering = 3,

    /** @brief 目标当前不可用，例如路径不存在、父目录不存在或底层 session 出错。 */
    Unavailable = 4,

    /** @brief 目标发生不可自动恢复的错误。 */
    Failed = 5
};

/**
 * @brief 监听目标类型。
 *
 * @details
 * 调用方必须显式指定目标是文件还是目录。模块不会对不存在的路径做类型猜测。
 */
enum class GB_SystemFileWatchTargetType : uint16_t
{
    /** @brief 监听单个文件。底层会监听其父目录，并只筛选该文件名相关事件。 */
    File = 0,

    /** @brief 监听目录。可通过 GB_SystemFileWatchTargetOptions::recursive 控制是否递归监听子目录。 */
    Directory = 1
};

/**
 * @brief 文件系统事件投递模式。
 *
 * @details
 * 原始事件尽量保留 ReadDirectoryChangesW 的原生动作；归一化事件会合并重命名、去抖修改事件并补充路径信息。
 */
enum class GB_SystemFileEventDeliveryMode : uint16_t
{
    /** @brief 只投递原始事件，例如 RenameOldName / RenameNewName 分开投递。 */
    Raw = 0,

    /** @brief 只投递归一化事件，例如重命名会合并为 Renamed。 */
    Normalized = 1,

    /** @brief 同时投递原始事件和归一化事件。 */
    Both = 2
};

/**
 * @brief 文件通知丢失或主动重扫后的恢复策略。
 */
enum class GB_SystemFileRecoveryMode : uint16_t
{
    /** @brief 只报告 Overflow / RescanRequired，由调用方自行重新枚举目标。 */
    ReportOnly = 0,

    /** @brief 维护目录快照，通知丢失后通过快照差异生成补偿事件。 */
    SnapshotDiff = 1
};

/**
 * @brief 文件系统对象类型。
 */
enum class GB_SystemFileObjectType : uint16_t
{
    /** @brief 类型未知，通常表示对象已被删除或属性查询失败。 */
    Unknown = 0,

    /** @brief 普通文件、符号链接文件或其他非目录文件系统对象。 */
    File = 1,

    /** @brief 目录对象。 */
    Directory = 2
};

/**
 * @brief 文件系统事件类型。
 */
enum class GB_SystemFileEventType : uint16_t
{
    /** @brief 未知事件类型。 */
    Unknown = 0,

    /** @brief 文件或目录被新增。 */
    Added = 1,

    /** @brief 文件或目录被删除。 */
    Removed = 2,

    /** @brief 文件或目录被修改，可能包括内容、大小、属性或时间戳变化。 */
    Modified = 3,

    /** @brief 原始重命名旧名称事件。 */
    RenameOldName = 4,

    /** @brief 原始重命名新名称事件。 */
    RenameNewName = 5,

    /** @brief 归一化后的重命名事件，oldRelativePath / oldAbsolutePath 保存旧路径。 */
    Renamed = 6,

    /** @brief 底层通知缓冲区或内部队列溢出，表示部分事件可能丢失。 */
    Overflow = 7,

    /** @brief 调用方需要重新枚举目标，通常由 ReportOnly 恢复策略或恢复失败触发。 */
    RescanRequired = 8,

    /** @brief 快照恢复开始。 */
    RecoveryStarted = 9,

    /** @brief 快照恢复完成。 */
    RecoveryCompleted = 10,

    /** @brief 监听目标不可用。 */
    TargetUnavailable = 11,

    /** @brief 监听目标重新可用。 */
    TargetRecovered = 12,

    /** @brief 监听目标发生失败。 */
    TargetFailed = 13,

    /** @brief 监听器已经启动。 */
    WatcherStarted = 14,

    /** @brief 监听器已经停止。 */
    WatcherStopped = 15
};

/**
 * @brief ReadDirectoryChangesW 通知掩码。
 *
 * @details
 * 该枚举是对 Win32 FILE_NOTIFY_CHANGE_* 标志的跨接口封装，可以使用 operator| 组合多个值。
 */
enum class GB_SystemFileNotifyFilter : uint32_t
{
    /** @brief 空掩码。作为监听选项传入时无效。 */
    None = 0,

    /** @brief 监听文件名变化。 */
    FileName = 1u << 0,

    /** @brief 监听目录名变化。 */
    DirectoryName = 1u << 1,

    /** @brief 监听文件或目录属性变化。 */
    Attributes = 1u << 2,

    /** @brief 监听文件大小变化。 */
    Size = 1u << 3,

    /** @brief 监听最后写入时间变化。 */
    LastWrite = 1u << 4,

    /** @brief 监听最后访问时间变化。 */
    LastAccess = 1u << 5,

    /** @brief 监听创建时间变化。 */
    Creation = 1u << 6,

    /** @brief 监听安全描述符变化。 */
    Security = 1u << 7,

    /** @brief 默认掩码：文件名、目录名、属性、大小、最后写入时间和创建时间。 */
    Default = FileName | DirectoryName | Attributes | Size | LastWrite | Creation,

    /** @brief 当前模块支持的全部通知掩码。 */
    All = FileName | DirectoryName | Attributes | Size | LastWrite | LastAccess | Creation | Security
};

/** @brief 组合两个文件通知掩码。 */
inline GB_SystemFileNotifyFilter operator|(const GB_SystemFileNotifyFilter left, const GB_SystemFileNotifyFilter right)
{
    return static_cast<GB_SystemFileNotifyFilter>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

/** @brief 计算两个文件通知掩码的交集。 */
inline GB_SystemFileNotifyFilter operator&(const GB_SystemFileNotifyFilter left, const GB_SystemFileNotifyFilter right)
{
    return static_cast<GB_SystemFileNotifyFilter>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
}

/** @brief 将右侧文件通知掩码合并到左侧文件通知掩码。 */
inline GB_SystemFileNotifyFilter& operator|=(GB_SystemFileNotifyFilter& left, const GB_SystemFileNotifyFilter right)
{
    left = left | right;
    return left;
}

/**
 * @brief 快照恢复实际检测到的字段差异。
 */
enum class GB_SystemFileDetectedChange : uint32_t
{
    /** @brief 没有检测到差异。 */
    None = 0,

    /** @brief 文件系统对象类型发生变化。 */
    ObjectType = 1u << 0,

    /** @brief 文件大小发生变化。 */
    Size = 1u << 1,

    /** @brief 文件或目录属性发生变化。 */
    Attributes = 1u << 2,

    /** @brief 创建时间发生变化。 */
    CreationTime = 1u << 3,

    /** @brief 最后写入时间发生变化。 */
    LastWriteTime = 1u << 4
};

/** @brief 组合两个快照差异掩码。 */
inline GB_SystemFileDetectedChange operator|(const GB_SystemFileDetectedChange left, const GB_SystemFileDetectedChange right)
{
    return static_cast<GB_SystemFileDetectedChange>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

/** @brief 计算两个快照差异掩码的交集。 */
inline GB_SystemFileDetectedChange operator&(const GB_SystemFileDetectedChange left, const GB_SystemFileDetectedChange right)
{
    return static_cast<GB_SystemFileDetectedChange>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
}

/** @brief 将右侧快照差异掩码合并到左侧快照差异掩码。 */
inline GB_SystemFileDetectedChange& operator|=(GB_SystemFileDetectedChange& left, const GB_SystemFileDetectedChange right)
{
    left = left | right;
    return left;
}

/**
 * @brief 监听目标标识。
 *
 * @details
 * watcherId 用于区分不同 GB_SystemFileWatcher 实例，targetId 用于区分同一个监听器内的不同目标。
 * 这样可以避免把一个监听器创建的 targetId 误用于另一个监听器。
 */
struct GLOBALBASE_PORT GB_SystemFileWatchTargetId
{
    /** @brief 创建该目标的监听器实例 ID。0 表示无效。 */
    uint64_t watcherId = 0;

    /** @brief 监听器内部的目标 ID。0 表示无效。 */
    uint64_t targetId = 0;

    /** @brief 判断当前 ID 是否同时具备有效 watcherId 和 targetId。 */
    bool IsValid() const;

    /** @brief 将 watcherId 和 targetId 清零，使当前 ID 变为无效。 */
    void Reset();

    /** @brief 显式 bool 转换。true 表示当前 ID 有效。 */
    explicit operator bool() const;

    /** @brief 判断两个目标 ID 是否完全相同。 */
    bool operator==(const GB_SystemFileWatchTargetId& other) const;

    /** @brief 判断两个目标 ID 是否不同。 */
    bool operator!=(const GB_SystemFileWatchTargetId& other) const;
};

/**
 * @brief 单个监听目标选项。
 *
 * @details
 * 路径过滤统一作用于使用正斜杠分隔的 UTF-8 相对路径。glob 支持 *、? 和跨目录的 **。
 */
struct GB_SystemFileWatchTargetOptions
{
    /** @brief 监听目标类型。调用方必须显式指定文件或目录。 */
    GB_SystemFileWatchTargetType targetType = GB_SystemFileWatchTargetType::Directory;

    /** @brief 文件系统通知掩码，用于控制底层监听哪些类型的变化。 */
    GB_SystemFileNotifyFilter notifyFilter = GB_SystemFileNotifyFilter::Default;

    /** @brief 事件投递模式，用于控制投递原始事件、归一化事件或两者都投递。 */
    GB_SystemFileEventDeliveryMode deliveryMode = GB_SystemFileEventDeliveryMode::Normalized;

    /** @brief 通知丢失后的恢复策略。 */
    GB_SystemFileRecoveryMode recoveryMode = GB_SystemFileRecoveryMode::ReportOnly;

    /** @brief 单个底层目录监听 session 的缓冲区大小，单位为字节，Windows 网络目录建议不超过 64 KB。 */
    size_t bufferSizeBytes = 64u * 1024u;

    /** @brief 快照恢复时允许保存的最大条目数，用于避免超大目录造成内存失控。 */
    size_t maxSnapshotEntryCount = 1000000;

    /** @brief 当 targetType 为 Directory 时，是否递归监听子目录。 */
    bool recursive = false;

    /** @brief 路径过滤、扩展名过滤和目标文件名匹配是否区分大小写。 */
    bool caseSensitive = false;

    /** @brief 目标不可用后是否按指数退避方式尝试自动重连。 */
    bool autoReconnect = false;

    /** @brief 扩展名过滤启用时，是否额外保留 .lnk 和 .url 快捷方式文件事件。 */
    bool includeShortcutFileEvents = true;

    /** @brief 包含 glob 列表。非空时，只有匹配任一包含规则的相对路径才会投递。 */
    std::vector<std::string> includeGlobs;

    /** @brief 排除 glob 列表。匹配任一排除规则的相对路径不会投递。 */
    std::vector<std::string> excludeGlobs;

    /** @brief 扩展名过滤列表。支持传入 "txt" 或 ".txt"；非空时目录事件默认不会投递。 */
    std::vector<std::string> extensions;
};

/**
 * @brief 文件监听器全局选项。
 */
struct GB_SystemFileWatcherOptions
{
    /** @brief 内部原始事件队列最多允许暂存的原生记录数，超出后会丢弃旧事件并报告 Overflow。 */
    size_t maxPendingNativeEvents = 8192;

    /** @brief GB_EventDispatcher 分发队列最大长度。 */
    size_t maxDispatchQueueSize = 4096;

    /** @brief 单次批量回调最多包含的事件数。 */
    size_t maxBatchSize = 64;

    /** @brief 批处理窗口，单位为毫秒。为 0 时事件会尽快成批投递。 */
    uint32_t batchWindowMilliseconds = 10;

    /** @brief Modified 归一化事件去抖窗口，单位为毫秒。 */
    uint32_t modifiedDebounceMilliseconds = 50;

    /** @brief 快照恢复扫描最多重复执行的次数，用于规避扫描期间目录持续变化。 */
    size_t maxRecoveryScanPasses = 3;

    /** @brief 自动重连的初始退避时间，单位为毫秒。 */
    uint32_t reconnectInitialDelayMilliseconds = 250;

    /** @brief 自动重连的最大退避时间，单位为毫秒。 */
    uint32_t reconnectMaximumDelayMilliseconds = 5000;

    /** @brief 是否合并同一目标、同一路径上的连续 Modified 归一化事件。 */
    bool coalesceModifiedEvents = true;
};

/**
 * @brief 当前监听目标信息快照。
 */
struct GB_SystemFileWatchTargetInfo
{
    /** @brief 监听目标 ID。 */
    GB_SystemFileWatchTargetId targetId;

    /** @brief 规范化后的目标路径，UTF-8 编码，路径分隔符统一为正斜杠。 */
    std::string path = "";

    /** @brief 添加目标时使用的目标选项快照。 */
    GB_SystemFileWatchTargetOptions options;

    /** @brief 当前目标状态。 */
    GB_SystemFileWatchTargetState state = GB_SystemFileWatchTargetState::Stopped;

    /** @brief 最近一次影响该目标状态的操作结果。 */
    GB_SystemResult lastResult;
};

/**
 * @brief 文件系统事件。
 *
 * @details
 * 所有 std::string 字段均使用 UTF-8 编码。路径字段统一使用正斜杠作为分隔符。
 */
struct GB_SystemFileEvent
{
    /** @brief 事件类型。 */
    GB_SystemFileEventType eventType = GB_SystemFileEventType::Unknown;

    /** @brief 事件所属监听目标 ID。监听器级别事件可能为空 ID。 */
    GB_SystemFileWatchTargetId targetId;

    /** @brief 事件名，通常形如 "SystemFile.Added"。 */
    std::string eventName = "";

    /** @brief 事件来源名，例如 "ReadDirectoryChangesW"、"SnapshotDiff" 或 "GB_SystemFileWatcher"。 */
    std::string sourceName = "ReadDirectoryChangesW";

    /** @brief 监听目标根路径，UTF-8 编码，路径分隔符统一为正斜杠。 */
    std::string targetPath = "";

    /** @brief 相对于监听目标根路径的相对路径，UTF-8 编码，路径分隔符统一为正斜杠。 */
    std::string relativePath = "";

    /** @brief 事件对象的绝对路径，UTF-8 编码，路径分隔符统一为正斜杠。 */
    std::string absolutePath = "";

    /** @brief 重命名前的相对路径，仅 Renamed 等事件通常会填充。 */
    std::string oldRelativePath = "";

    /** @brief 重命名前的绝对路径，仅 Renamed 等事件通常会填充。 */
    std::string oldAbsolutePath = "";

    /** @brief 事件对象类型。删除事件可能只能从快照或路径推断，无法推断时为 Unknown。 */
    GB_SystemFileObjectType objectType = GB_SystemFileObjectType::Unknown;

    /** @brief 快照恢复检测到的字段差异。普通 ReadDirectoryChangesW 事件通常为 None。 */
    GB_SystemFileDetectedChange detectedChanges = GB_SystemFileDetectedChange::None;

    /** @brief 与事件相关的操作结果。失败事件会在这里保存诊断信息。 */
    GB_SystemResult result;

    /** @brief 事件生成时间戳，Unix epoch 毫秒。 */
    uint64_t timestampMilliseconds = 0;

    /** @brief Win32 FILE_ACTION_* 原生动作值。非原生事件通常为 0。 */
    uint32_t nativeAction = 0;

    /** @brief 合并到当前事件中的原始事件数量。 */
    uint32_t rawEventCount = 1;

    /** @brief 当前事件是否为原始事件。 */
    bool isRaw = false;

    /** @brief 当前事件是否为归一化事件。 */
    bool isNormalized = true;

    /** @brief 当前事件对象是否位于递归监听目录的子级路径中。 */
    bool isRecursiveChild = false;

    /** @brief 当前事件是否由快照恢复生成，而不是直接来自 ReadDirectoryChangesW。 */
    bool isFromSnapshotRecovery = false;

    /** @brief 当前事件是否由多个 Modified 事件合并得到。 */
    bool isCoalesced = false;
};

/**
 * @brief 一次批量回调中的事件集合。
 */
struct GB_SystemFileEventBatch
{
    /** @brief 本批次包含的文件系统事件。 */
    std::vector<GB_SystemFileEvent> events;

    /** @brief 本批次最早事件的时间戳，Unix epoch 毫秒。 */
    uint64_t firstTimestampMilliseconds = 0;

    /** @brief 本批次最晚事件的时间戳，Unix epoch 毫秒。 */
    uint64_t lastTimestampMilliseconds = 0;
};

/**
 * @brief 文件监听器统计信息快照。
 */
struct GB_SystemFileWatcherStatistics
{
    /** @brief IOCP 完成层收到的底层通知次数。 */
    uint64_t receivedNativeEventCount = 0;

    /** @brief 成功解析出的原生 FILE_NOTIFY_INFORMATION 记录数。 */
    uint64_t parsedNativeEventCount = 0;

    /** @brief 成功进入 typedDispatcher 批量事件中的业务事件总数。 */
    uint64_t deliveredEventCount = 0;

    /** @brief 成功进入 typedDispatcher 的批次数。 */
    uint64_t deliveredBatchCount = 0;

    /** @brief 因内部原始事件队列溢出而丢弃的原生记录数。 */
    uint64_t droppedNativeEventCount = 0;

    /** @brief 因事件分发队列溢出而丢弃的事件数。 */
    uint64_t droppedDispatchEventCount = 0;

    /** @brief 监听缓冲区、内部队列或分发队列溢出次数。 */
    uint64_t overflowCount = 0;

    /** @brief 已完成的快照恢复次数。 */
    uint64_t recoveryCount = 0;

    /** @brief 成功重连不可用目标的次数。 */
    uint64_t reconnectCount = 0;

    /** @brief 用户回调和事件分发回调捕获到的异常次数。 */
    uint64_t callbackExceptionCount = 0;

    /** @brief 当前监听目标数量。 */
    size_t currentTargetCount = 0;

    /** @brief 当前底层目录监听 session 数量。 */
    size_t currentDirectorySessionCount = 0;

    /** @brief 当前内部原始事件队列中尚未处理的原生记录数量。 */
    size_t pendingNativeEventCount = 0;
};

/**
 * @brief WaitForFileStable 的等待选项。
 */
struct GB_SystemFileStableWaitOptions
{
    /** @brief 最大等待时间，单位为毫秒。小于 0 表示无限等待。 */
    int64_t timeoutMilliseconds = 30000;

    /** @brief 轮询间隔，单位为毫秒。 */
    uint32_t pollIntervalMilliseconds = 100;

    /** @brief 文件大小和最后写入时间保持不变所需的稳定窗口，单位为毫秒。 */
    uint32_t stableWindowMilliseconds = 500;

    /** @brief 是否要求文件可读。启用后，文件被其他进程独占占用时会继续等待。 */
    bool requireReadable = false;

    /** @brief 可选取消标志。非空且值变为 true 时，等待会返回 Cancelled。 */
    const std::atomic<bool>* cancellationFlag = nullptr;
};

/**
 * @brief Windows 文件系统变化监听器。
 *
 * @details
 * Windows 下内部使用 ReadDirectoryChangesW、OVERLAPPED 和 IOCP；相同规范化目录会共享一个底层 session。
 * 用户回调通过 GB_EventDispatcher 异步执行，不占用 IOCP 线程。非 Windows 平台保留同名接口，但会返回 UnsupportedPlatform。
 */
class GLOBALBASE_PORT GB_SystemFileWatcher final
{
public:
    /** @brief 单事件回调类型。 */
    using EventCallback = std::function<void(const GB_SystemFileEvent& event)>;

    /** @brief 批量事件回调类型。 */
    using BatchEventCallback = std::function<void(const GB_SystemFileEventBatch& batch)>;

    /** @brief 使用默认全局选项构造文件监听器。 */
    GB_SystemFileWatcher();

    /** @brief 使用指定全局选项构造文件监听器。 */
    explicit GB_SystemFileWatcher(const GB_SystemFileWatcherOptions& options);

    /** @brief 析构文件监听器。析构时会自动停止监听并释放内部线程和系统句柄。 */
    ~GB_SystemFileWatcher() noexcept;

    /** @brief 禁止拷贝构造。 */
    GB_SystemFileWatcher(const GB_SystemFileWatcher&) = delete;

    /** @brief 禁止拷贝赋值。 */
    GB_SystemFileWatcher& operator=(const GB_SystemFileWatcher&) = delete;

    /**
     * @brief 添加一个文件或目录监听目标。
     *
     * @param path UTF-8 路径。可以是相对路径或绝对路径，内部会规范化为绝对路径。
     * @param options 该目标的监听选项。
     * @param targetId 输出监听目标 ID，失败时会被置为无效。
     * @return 操作结果。
     */
    GB_SystemResult AddTarget(const std::string& path, const GB_SystemFileWatchTargetOptions& options, GB_SystemFileWatchTargetId& targetId);

    /** @brief 移除指定监听目标。监听器运行中调用时会重建底层监听 session。 */
    GB_SystemResult RemoveTarget(const GB_SystemFileWatchTargetId& targetId);

    /** @brief 移除全部监听目标。监听器运行中调用时会停止全部底层监听 session。 */
    GB_SystemResult ClearTargets();

    /** @brief 获取当前全部监听目标的信息快照。 */
    std::vector<GB_SystemFileWatchTargetInfo> GetTargets() const;

    /** @brief 启动文件监听器。启动前至少需要添加一个监听目标。 */
    GB_SystemResult Start();

    /** @brief 停止文件监听器，释放内部线程、IOCP、目录句柄和待处理事件。 */
    GB_SystemResult Stop();

    /** @brief 暂停普通文件变化事件投递，并在恢复时要求重新扫描或快照恢复。 */
    GB_SystemResult Pause();

    /** @brief 从暂停状态恢复监听。恢复时会根据目标恢复策略生成 RescanRequired 或快照差异事件。 */
    GB_SystemResult Resume();

    /** @brief 获取监听器整体状态。 */
    GB_SystemFileWatcherState GetState() const;

    /** @brief 判断监听器是否处于 Watching、Paused 或 Recovering 状态。 */
    bool IsRunning() const;

    /** @brief 请求对指定目标执行一次重扫或快照恢复。 */
    GB_SystemResult RequestRescan(const GB_SystemFileWatchTargetId& targetId);

    /** @brief 请求对全部目标执行一次重扫或快照恢复。 */
    GB_SystemResult RequestRescanAll();

    /** @brief 设置单事件回调。传入空 std::function 可清除回调。 */
    void SetEventCallback(const EventCallback& callback);

    /** @brief 设置批量事件回调。传入空 std::function 可清除回调。 */
    void SetBatchEventCallback(const BatchEventCallback& callback);

    /** @brief 获取公共事件分发器，用于按事件名订阅 GB_Event 形式的事件。 */
    GB_EventDispatcher& GetEventDispatcher();

    /** @brief 获取监听器统计信息快照。 */
    GB_SystemFileWatcherStatistics GetStatistics() const;

    /**
     * @brief 等待指定文件达到稳定状态。
     *
     * @details
     * 稳定状态指文件存在，并且大小与最后写入时间在 stableWindowMilliseconds 内保持不变；
     * 如果 requireReadable 为 true，还要求文件能以共享读写删除方式打开；
     * 目标暂时不存在时会继续等待，目标是目录或属性查询因权限、路径格式等非“不存在”原因失败时会立即返回错误。
     */
    static GB_SystemResult WaitForFileStable(const std::string& path, const GB_SystemFileStableWaitOptions& options = GB_SystemFileStableWaitOptions());

    /** @brief 获取文件系统事件类型的英文名称。 */
    static std::string GetEventTypeName(GB_SystemFileEventType eventType);

    /** @brief 获取监听器整体状态的英文名称。 */
    static std::string GetWatcherStateName(GB_SystemFileWatcherState state);

    /** @brief 获取监听目标状态的英文名称。 */
    static std::string GetTargetStateName(GB_SystemFileWatchTargetState state);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_FILE_WATCHER_H_H
