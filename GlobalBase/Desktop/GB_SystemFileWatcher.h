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

/** @brief 文件监听器生命周期状态。 */
enum class GB_SystemFileWatcherState : uint16_t
{
    Stopped = 0,
    Starting = 1,
    Watching = 2,
    Paused = 3,
    Recovering = 4,
    Stopping = 5,
    Failed = 6
};

/** @brief 单个监听目标状态。 */
enum class GB_SystemFileWatchTargetState : uint16_t
{
    Stopped = 0,
    Watching = 1,
    Paused = 2,
    Recovering = 3,
    Unavailable = 4,
    Failed = 5
};

/** @brief 监听目标类型。调用方必须显式指定，模块不会根据不存在的路径进行猜测。 */
enum class GB_SystemFileWatchTargetType : uint16_t
{
    File = 0,
    Directory = 1
};

/** @brief 事件投递模式。 */
enum class GB_SystemFileEventDeliveryMode : uint16_t
{
    Raw = 0,
    Normalized = 1,
    Both = 2
};

/** @brief 通知丢失后的恢复策略。 */
enum class GB_SystemFileRecoveryMode : uint16_t
{
    ReportOnly = 0,
    SnapshotDiff = 1
};

/** @brief 文件系统对象类型。 */
enum class GB_SystemFileObjectType : uint16_t
{
    Unknown = 0,
    File = 1,
    Directory = 2
};

/** @brief 文件系统事件类型。 */
enum class GB_SystemFileEventType : uint16_t
{
    Unknown = 0,
    Added = 1,
    Removed = 2,
    Modified = 3,
    RenameOldName = 4,
    RenameNewName = 5,
    Renamed = 6,
    Overflow = 7,
    RescanRequired = 8,
    RecoveryStarted = 9,
    RecoveryCompleted = 10,
    TargetUnavailable = 11,
    TargetRecovered = 12,
    TargetFailed = 13,
    WatcherStarted = 14,
    WatcherStopped = 15
};

/** @brief ReadDirectoryChangesW 通知掩码。 */
enum class GB_SystemFileNotifyFilter : uint32_t
{
    None = 0,
    FileName = 1u << 0,
    DirectoryName = 1u << 1,
    Attributes = 1u << 2,
    Size = 1u << 3,
    LastWrite = 1u << 4,
    LastAccess = 1u << 5,
    Creation = 1u << 6,
    Security = 1u << 7,
    Default = FileName | DirectoryName | Attributes | Size | LastWrite | Creation,
    All = FileName | DirectoryName | Attributes | Size | LastWrite | LastAccess | Creation | Security
};

inline GB_SystemFileNotifyFilter operator|(const GB_SystemFileNotifyFilter left, const GB_SystemFileNotifyFilter right)
{
    return static_cast<GB_SystemFileNotifyFilter>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

inline GB_SystemFileNotifyFilter operator&(const GB_SystemFileNotifyFilter left, const GB_SystemFileNotifyFilter right)
{
    return static_cast<GB_SystemFileNotifyFilter>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
}

inline GB_SystemFileNotifyFilter& operator|=(GB_SystemFileNotifyFilter& left, const GB_SystemFileNotifyFilter right)
{
    left = left | right;
    return left;
}

/** @brief 快照恢复实际检测到的字段差异。 */
enum class GB_SystemFileDetectedChange : uint32_t
{
    None = 0,
    ObjectType = 1u << 0,
    Size = 1u << 1,
    Attributes = 1u << 2,
    CreationTime = 1u << 3,
    LastWriteTime = 1u << 4
};

inline GB_SystemFileDetectedChange operator|(const GB_SystemFileDetectedChange left, const GB_SystemFileDetectedChange right)
{
    return static_cast<GB_SystemFileDetectedChange>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

inline GB_SystemFileDetectedChange& operator|=(GB_SystemFileDetectedChange& left, const GB_SystemFileDetectedChange right)
{
    left = left | right;
    return left;
}

/**
 * @brief 监听目标标识。
 *
 * watcherId 防止把一个监听器创建的 targetId 误用于另一个监听器。
 */
struct GLOBALBASE_PORT GB_SystemFileWatchTargetId
{
    uint64_t watcherId = 0;
    uint64_t targetId = 0;

    bool IsValid() const;
    void Reset();
    explicit operator bool() const;
    bool operator==(const GB_SystemFileWatchTargetId& other) const;
    bool operator!=(const GB_SystemFileWatchTargetId& other) const;
};

/**
 * @brief 单个监听目标选项。
 *
 * 路径过滤统一作用于使用正斜杠分隔的 UTF-8 相对路径。glob 支持 *、? 和跨目录的 **。
 */
struct GB_SystemFileWatchTargetOptions
{
    GB_SystemFileWatchTargetType targetType = GB_SystemFileWatchTargetType::Directory;
    GB_SystemFileNotifyFilter notifyFilter = GB_SystemFileNotifyFilter::Default;
    GB_SystemFileEventDeliveryMode deliveryMode = GB_SystemFileEventDeliveryMode::Normalized;
    GB_SystemFileRecoveryMode recoveryMode = GB_SystemFileRecoveryMode::ReportOnly;
    size_t bufferSizeBytes = 32u * 1024u;
    size_t maxSnapshotEntryCount = 1000000;
    bool recursive = false;
    bool caseSensitive = false;
    bool autoReconnect = false;
    std::vector<std::string> includeGlobs;
    std::vector<std::string> excludeGlobs;
    std::vector<std::string> extensions;
};

/** @brief 监听器全局选项。 */
struct GB_SystemFileWatcherOptions
{
    size_t maxPendingNativeEvents = 8192;
    size_t maxDispatchQueueSize = 4096;
    size_t maxBatchSize = 64;
    uint32_t batchWindowMilliseconds = 10;
    uint32_t modifiedDebounceMilliseconds = 50;
    size_t maxRecoveryScanPasses = 3;
    uint32_t reconnectInitialDelayMilliseconds = 250;
    uint32_t reconnectMaximumDelayMilliseconds = 5000;
    bool coalesceModifiedEvents = true;
};

/** @brief 当前监听目标信息快照。 */
struct GB_SystemFileWatchTargetInfo
{
    GB_SystemFileWatchTargetId targetId;
    std::string path = "";
    GB_SystemFileWatchTargetOptions options;
    GB_SystemFileWatchTargetState state = GB_SystemFileWatchTargetState::Stopped;
    GB_SystemResult lastResult;
};

/** @brief 文件系统事件。所有 std::string 均为 UTF-8 编码。 */
struct GB_SystemFileEvent
{
    GB_SystemFileEventType eventType = GB_SystemFileEventType::Unknown;
    GB_SystemFileWatchTargetId targetId;
    std::string eventName = "";
    std::string sourceName = "ReadDirectoryChangesW";
    std::string targetPath = "";
    std::string relativePath = "";
    std::string absolutePath = "";
    std::string oldRelativePath = "";
    std::string oldAbsolutePath = "";
    GB_SystemFileObjectType objectType = GB_SystemFileObjectType::Unknown;
    GB_SystemFileDetectedChange detectedChanges = GB_SystemFileDetectedChange::None;
    GB_SystemResult result;
    uint64_t timestampMilliseconds = 0;
    uint32_t nativeAction = 0;
    uint32_t rawEventCount = 1;
    bool isRaw = false;
    bool isNormalized = true;
    bool isRecursiveChild = false;
    bool isFromSnapshotRecovery = false;
    bool isCoalesced = false;
};

/** @brief 一次批量回调中的事件集合。 */
struct GB_SystemFileEventBatch
{
    std::vector<GB_SystemFileEvent> events;
    uint64_t firstTimestampMilliseconds = 0;
    uint64_t lastTimestampMilliseconds = 0;
};

/** @brief 监听器统计信息快照。 */
struct GB_SystemFileWatcherStatistics
{
    uint64_t receivedNativeEventCount = 0;
    uint64_t parsedNativeEventCount = 0;
    uint64_t deliveredEventCount = 0;
    uint64_t deliveredBatchCount = 0;
    uint64_t droppedNativeEventCount = 0;
    uint64_t droppedDispatchEventCount = 0;
    uint64_t overflowCount = 0;
    uint64_t recoveryCount = 0;
    uint64_t reconnectCount = 0;
    uint64_t callbackExceptionCount = 0;
    size_t currentTargetCount = 0;
    size_t currentDirectorySessionCount = 0;
    size_t pendingNativeEventCount = 0;
};

/** @brief WaitForFileStable 选项。 */
struct GB_SystemFileStableWaitOptions
{
    int64_t timeoutMilliseconds = 30000;
    uint32_t pollIntervalMilliseconds = 100;
    uint32_t stableWindowMilliseconds = 500;
    bool requireReadable = false;
    const std::atomic<bool>* cancellationFlag = nullptr;
};

/**
 * @brief Windows 文件系统变化监听器。
 *
 * 内部使用 ReadDirectoryChangesW、OVERLAPPED 和 IOCP。相同规范化目录共享一个底层句柄，
 * 回调通过 GB_EventDispatcher 异步执行，不会占用 IOCP 线程。
 */
class GLOBALBASE_PORT GB_SystemFileWatcher final
{
public:
    using EventCallback = std::function<void(const GB_SystemFileEvent& event)>;
    using BatchEventCallback = std::function<void(const GB_SystemFileEventBatch& batch)>;

    GB_SystemFileWatcher();
    explicit GB_SystemFileWatcher(const GB_SystemFileWatcherOptions& options);
    ~GB_SystemFileWatcher() noexcept;

    GB_SystemFileWatcher(const GB_SystemFileWatcher&) = delete;
    GB_SystemFileWatcher& operator=(const GB_SystemFileWatcher&) = delete;

    GB_SystemResult AddTarget(const std::string& path, const GB_SystemFileWatchTargetOptions& options, GB_SystemFileWatchTargetId& targetId);
    GB_SystemResult RemoveTarget(const GB_SystemFileWatchTargetId& targetId);
    GB_SystemResult ClearTargets();
    std::vector<GB_SystemFileWatchTargetInfo> GetTargets() const;

    GB_SystemResult Start();
    GB_SystemResult Stop();
    GB_SystemResult Pause();
    GB_SystemResult Resume();
    GB_SystemFileWatcherState GetState() const;
    bool IsRunning() const;

    GB_SystemResult RequestRescan(const GB_SystemFileWatchTargetId& targetId);
    GB_SystemResult RequestRescanAll();

    void SetEventCallback(const EventCallback& callback);
    void SetBatchEventCallback(const BatchEventCallback& callback);
    GB_EventDispatcher& GetEventDispatcher();
    GB_SystemFileWatcherStatistics GetStatistics() const;

    static GB_SystemResult WaitForFileStable(const std::string& path, const GB_SystemFileStableWaitOptions& options = GB_SystemFileStableWaitOptions());
    static std::string GetEventTypeName(GB_SystemFileEventType eventType);
    static std::string GetWatcherStateName(GB_SystemFileWatcherState state);
    static std::string GetTargetStateName(GB_SystemFileWatchTargetState state);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_FILE_WATCHER_H_H
