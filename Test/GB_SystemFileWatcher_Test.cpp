#include "GB_RunTests.h"
#include "Desktop/GB_SystemFileWatcher.h"
#include "GB_FileSystem.h"
#include "GB_Utf8String.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace
{
    int totalSystemFileWatcherCaseCount = 0;

    [[noreturn]] void FailSystemFileWatcherTest(const std::string& caseName, const std::string& detail)
    {
        std::cerr << "[FAILED] " << caseName << "\n" << detail << std::endl;
        std::exit(1);
    }

    void RequireTrueSystemFileWatcher(const bool condition, const std::string& caseName, const std::string& detail)
    {
        totalSystemFileWatcherCaseCount++;
        if (!condition)
        {
            FailSystemFileWatcherTest(caseName, detail);
        }
    }

    void RequireFileWatcherResultSucceeded(const GB_SystemResult& result, const std::string& caseName)
    {
        totalSystemFileWatcherCaseCount++;
        if (result.IsFailed())
        {
            FailSystemFileWatcherTest(caseName, result.ToString());
        }
    }

#if defined(_WIN32)
    std::wstring ToExtendedPath(const std::string& pathUtf8)
    {
        std::wstring path = GB_Utf8ToWString(pathUtf8);
        for (size_t index = 0; index < path.size(); index++)
        {
            if (path[index] == L'/')
            {
                path[index] = L'\\';
            }
        }
        if (path.compare(0, 2, L"\\\\") == 0)
        {
            return std::wstring(L"\\\\?\\UNC\\") + path.substr(2);
        }
        return std::wstring(L"\\\\?\\") + path;
    }

    bool WriteFileBytes(const std::string& pathUtf8, const std::string& bytes, const bool append)
    {
        const std::wstring path = ToExtendedPath(pathUtf8);
        HANDLE fileHandle = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        if (append)
        {
            (void)::SetFilePointer(fileHandle, 0, nullptr, FILE_END);
        }
        DWORD writtenBytes = 0;
        const BOOL writeResult = bytes.empty() ? TRUE : ::WriteFile(fileHandle, bytes.data(), static_cast<DWORD>(bytes.size()), &writtenBytes, nullptr);
        (void)::FlushFileBuffers(fileHandle);
        (void)::CloseHandle(fileHandle);
        return writeResult != FALSE && writtenBytes == bytes.size();
    }

    bool RenamePath(const std::string& oldPathUtf8, const std::string& newPathUtf8)
    {
        return ::MoveFileExW(ToExtendedPath(oldPathUtf8).c_str(), ToExtendedPath(newPathUtf8).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }

    std::string MakeTestRoot()
    {
        return GB_JoinPath(GB_GetTempDirectory(), std::string(u8"GB_SystemFileWatcher_测试_") + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64()));
    }

    class EventCollector final
    {
    public:
        void Add(const GB_SystemFileEvent& event)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                events.push_back(event);
            }
            condition.notify_all();
        }

        bool WaitFor(const std::function<bool(const GB_SystemFileEvent&)>& predicate, const uint32_t timeoutMilliseconds, GB_SystemFileEvent* matchedEvent = nullptr)
        {
            std::unique_lock<std::mutex> lock(mutex);
            const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
            for (;;)
            {
                for (size_t index = 0; index < events.size(); index++)
                {
                    if (predicate(events[index]))
                    {
                        if (matchedEvent != nullptr)
                        {
                            *matchedEvent = events[index];
                        }
                        return true;
                    }
                }
                if (condition.wait_until(lock, deadline) == std::cv_status::timeout)
                {
                    return false;
                }
            }
        }

        size_t Count(const std::function<bool(const GB_SystemFileEvent&)>& predicate) const
        {
            size_t count = 0;
            std::lock_guard<std::mutex> lock(mutex);
            for (size_t index = 0; index < events.size(); index++)
            {
                if (predicate(events[index]))
                {
                    count++;
                }
            }
            return count;
        }

    private:
        mutable std::mutex mutex;
        std::condition_variable condition;
        std::vector<GB_SystemFileEvent> events;
    };

    bool IsTargetEvent(const GB_SystemFileEvent& event, const GB_SystemFileWatchTargetId& targetId, const GB_SystemFileEventType eventType, const std::string& relativePath = std::string())
    {
        return event.targetId == targetId && event.eventType == eventType && (relativePath.empty() || event.relativePath == relativePath);
    }

    void RunValidationTests(const std::string& rootPath)
    {
        GB_SystemFileWatcher watcher;
        RequireTrueSystemFileWatcher(watcher.Start().errorCode == GB_SystemErrorCode::InvalidState, "Start without targets", "Starting without targets must fail.");

        GB_SystemFileWatchTargetOptions invalidOptions;
        invalidOptions.bufferSizeBytes = 1024;
        GB_SystemFileWatchTargetId invalidTargetId;
        RequireTrueSystemFileWatcher(watcher.AddTarget(rootPath, invalidOptions, invalidTargetId).errorCode == GB_SystemErrorCode::InvalidArgument, "Reject small buffer", "A buffer below 4 KB must be rejected.");

        GB_SystemFileWatchTargetOptions directoryOptions;
        GB_SystemFileWatchTargetId targetId;
        RequireFileWatcherResultSucceeded(watcher.AddTarget(rootPath, directoryOptions, targetId), "Add validation directory target");
        RequireTrueSystemFileWatcher(targetId.IsValid(), "Target id valid", "Expected a valid watcher-scoped target id.");

        GB_SystemFileWatcher otherWatcher;
        RequireTrueSystemFileWatcher(otherWatcher.RemoveTarget(targetId).errorCode == GB_SystemErrorCode::InvalidArgument, "Cross watcher target id rejected", "A target id from another watcher must be rejected.");
        RequireTrueSystemFileWatcher(watcher.GetTargets().size() == 1, "GetTargets count", "Expected exactly one configured target.");
        RequireFileWatcherResultSucceeded(watcher.RemoveTarget(targetId), "Remove validation target");
        RequireTrueSystemFileWatcher(watcher.GetTargets().empty(), "Remove target result", "Expected the target list to be empty.");
    }

    void RunDirectoryAndFileEventTests(const std::string& rootPath)
    {
        GB_SystemFileWatcherOptions watcherOptions;
        watcherOptions.maxBatchSize = 16;
        watcherOptions.batchWindowMilliseconds = 5;
        watcherOptions.modifiedDebounceMilliseconds = 60;
        GB_SystemFileWatcher watcher(watcherOptions);
        EventCollector collector;
        std::atomic<uint64_t> publicEventCount(0);

        watcher.SetEventCallback([&collector](const GB_SystemFileEvent& event)
            {
                collector.Add(event);
            });
        GB_EventSubscriptionToken publicToken;
        RequireFileWatcherResultSucceeded(watcher.GetEventDispatcher().SubscribeAll([&publicEventCount](const GB_Event&)
            {
                publicEventCount.fetch_add(1, std::memory_order_relaxed);
            }, publicToken), "Subscribe public file events");

        GB_SystemFileWatchTargetOptions directoryOptions;
        directoryOptions.targetType = GB_SystemFileWatchTargetType::Directory;
        directoryOptions.recursive = true;
        directoryOptions.deliveryMode = GB_SystemFileEventDeliveryMode::Both;
        directoryOptions.recoveryMode = GB_SystemFileRecoveryMode::SnapshotDiff;
        directoryOptions.includeGlobs.push_back("**/*.txt");
        directoryOptions.extensions.push_back(".txt");
        GB_SystemFileWatchTargetId directoryTargetId;
        RequireFileWatcherResultSucceeded(watcher.AddTarget(rootPath, directoryOptions, directoryTargetId), "Add directory target");

        GB_SystemFileWatchTargetOptions fileOptions;
        fileOptions.targetType = GB_SystemFileWatchTargetType::File;
        fileOptions.deliveryMode = GB_SystemFileEventDeliveryMode::Normalized;
        GB_SystemFileWatchTargetId fileTargetId;
        const std::string configPath = GB_JoinPath(rootPath, "config.ini");
        RequireFileWatcherResultSucceeded(watcher.AddTarget(configPath, fileOptions, fileTargetId), "Add file target");
        RequireFileWatcherResultSucceeded(watcher.Start(), "Start directory and file watcher");

        RequireTrueSystemFileWatcher(collector.WaitFor([directoryTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, directoryTargetId, GB_SystemFileEventType::RecoveryCompleted);
            }, 5000), "Initial snapshot completed", "Expected initial snapshot completion.");

        const GB_SystemFileWatcherStatistics initialStatistics = watcher.GetStatistics();
        RequireTrueSystemFileWatcher(initialStatistics.currentDirectorySessionCount == 2, "Shared directory sessions", "The directory root and its parent guard should be the only two sessions.");

        const std::string textPath = GB_JoinPath(rootPath, "a.txt");
        const std::string ignoredPath = GB_JoinPath(rootPath, "ignored.bin");
        RequireTrueSystemFileWatcher(WriteFileBytes(textPath, "one", false), "Create text file", "Failed to create the text file.");
        RequireTrueSystemFileWatcher(WriteFileBytes(ignoredPath, "ignored", false), "Create ignored file", "Failed to create the ignored file.");
        RequireTrueSystemFileWatcher(WriteFileBytes(configPath, "config", false), "Create watched file", "Failed to create the watched file.");

        RequireTrueSystemFileWatcher(collector.WaitFor([directoryTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, directoryTargetId, GB_SystemFileEventType::Added, "a.txt") && event.isNormalized;
            }, 5000), "Directory added event", "Expected a normalized Added event for a.txt.");
        RequireTrueSystemFileWatcher(collector.WaitFor([fileTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, fileTargetId, GB_SystemFileEventType::Added, "config.ini");
            }, 5000), "File target added event", "Expected an Added event for config.ini.");
        RequireTrueSystemFileWatcher(collector.Count([directoryTargetId](const GB_SystemFileEvent& event)
            {
                return event.targetId == directoryTargetId && event.relativePath == "ignored.bin";
            }) == 0, "Extension filter", "The .bin event must be filtered out.");

        RequireTrueSystemFileWatcher(WriteFileBytes(textPath, "two", true), "Modify text file once", "Failed to append to a.txt.");
        RequireTrueSystemFileWatcher(WriteFileBytes(textPath, "three", true), "Modify text file twice", "Failed to append to a.txt.");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        RequireTrueSystemFileWatcher(collector.Count([directoryTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, directoryTargetId, GB_SystemFileEventType::Modified, "a.txt") && event.isNormalized;
            }) >= 1, "Modified debounce emits", "Expected at least one normalized Modified event.");

        const std::string renamedPath = GB_JoinPath(rootPath, "b.txt");
        RequireTrueSystemFileWatcher(RenamePath(textPath, renamedPath), "Rename text file", "Failed to rename a.txt to b.txt.");
        const bool renamedReceived = collector.WaitFor([directoryTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, directoryTargetId, GB_SystemFileEventType::Renamed, "b.txt") && event.oldRelativePath == "a.txt";
            }, 5000);
        const bool conservativeRenameReceived = collector.WaitFor([directoryTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, directoryTargetId, GB_SystemFileEventType::Added, "b.txt");
            }, 500);
        RequireTrueSystemFileWatcher(renamedReceived || conservativeRenameReceived, "Rename normalization", "Expected Renamed or conservative Added fallback for b.txt.");
        RequireTrueSystemFileWatcher(collector.WaitFor([directoryTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, directoryTargetId, GB_SystemFileEventType::RenameOldName, "a.txt") && event.isRaw;
            }, 5000), "Raw rename old event", "Expected raw RenameOldName.");
        RequireTrueSystemFileWatcher(collector.WaitFor([directoryTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, directoryTargetId, GB_SystemFileEventType::RenameNewName, "b.txt") && event.isRaw;
            }, 5000), "Raw rename new event", "Expected raw RenameNewName.");

        const std::string subdirectoryPath = GB_JoinPath(rootPath, u8"子目录");
        RequireTrueSystemFileWatcher(GB_CreateDirectory(subdirectoryPath), "Create recursive subdirectory", "Failed to create the recursive test directory.");
        const std::string recursiveFilePath = GB_JoinPath(subdirectoryPath, "child.txt");
        RequireTrueSystemFileWatcher(WriteFileBytes(recursiveFilePath, "child", false), "Create recursive file", "Failed to create child.txt.");
        RequireTrueSystemFileWatcher(collector.WaitFor([directoryTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, directoryTargetId, GB_SystemFileEventType::Added, u8"子目录/child.txt") && event.isRecursiveChild;
            }, 5000), "Recursive child event", "Expected a recursive Added event.");

        GB_SystemFileWatchTargetOptions dynamicOptions;
        dynamicOptions.targetType = GB_SystemFileWatchTargetType::File;
        GB_SystemFileWatchTargetId dynamicTargetId;
        const std::string dynamicPath = GB_JoinPath(rootPath, "dynamic.dat");
        RequireFileWatcherResultSucceeded(watcher.AddTarget(dynamicPath, dynamicOptions, dynamicTargetId), "Dynamically add file target");
        RequireTrueSystemFileWatcher(watcher.GetStatistics().currentDirectorySessionCount == 2, "Dynamic target shares session", "The dynamic file target must share the existing root session.");
        RequireTrueSystemFileWatcher(WriteFileBytes(dynamicPath, "dynamic", false), "Create dynamic file", "Failed to create dynamic.dat.");
        RequireTrueSystemFileWatcher(collector.WaitFor([dynamicTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, dynamicTargetId, GB_SystemFileEventType::Added, "dynamic.dat");
            }, 5000), "Dynamic target event", "Expected an event for the dynamically added target.");
        RequireFileWatcherResultSucceeded(watcher.RemoveTarget(dynamicTargetId), "Dynamically remove file target");

        RequireFileWatcherResultSucceeded(watcher.Pause(), "Pause file watcher");
        const std::string pausedPath = GB_JoinPath(rootPath, "paused.txt");
        RequireTrueSystemFileWatcher(WriteFileBytes(pausedPath, "paused", false), "Create file while paused", "Failed to create paused.txt.");
        RequireTrueSystemFileWatcher(WriteFileBytes(configPath, "paused", true), "Modify file target while paused", "Failed to modify config.ini.");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        RequireFileWatcherResultSucceeded(watcher.Resume(), "Resume file watcher");
        RequireTrueSystemFileWatcher(collector.WaitFor([directoryTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, directoryTargetId, GB_SystemFileEventType::Added, "paused.txt") && event.isFromSnapshotRecovery;
            }, 5000), "Pause snapshot recovery", "Expected snapshot recovery to report paused.txt.");
        RequireTrueSystemFileWatcher(collector.WaitFor([fileTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, fileTargetId, GB_SystemFileEventType::RescanRequired);
            }, 5000), "Pause report-only rescan", "Expected RescanRequired for the report-only file target.");

        RequireFileWatcherResultSucceeded(watcher.RequestRescan(directoryTargetId), "Manual snapshot rescan");
        RequireTrueSystemFileWatcher(collector.WaitFor([directoryTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, directoryTargetId, GB_SystemFileEventType::RecoveryCompleted);
            }, 5000), "Manual rescan completed", "Expected manual recovery completion.");

        RequireTrueSystemFileWatcher(publicEventCount.load(std::memory_order_acquire) > 0, "Public dispatcher events", "Expected events through the public dispatcher.");
        RequireFileWatcherResultSucceeded(watcher.Stop(), "Stop directory and file watcher");
        RequireTrueSystemFileWatcher(watcher.GetState() == GB_SystemFileWatcherState::Stopped, "Stopped state", "Expected Stopped after Stop.");
    }

    void RunAutoReconnectTest(const std::string& rootPath)
    {
        const std::string reconnectPath = GB_JoinPath(rootPath, "reconnect");
        RequireTrueSystemFileWatcher(GB_CreateDirectory(reconnectPath), "Create reconnect directory", "Failed to create reconnect directory.");

        GB_SystemFileWatcher watcher;
        EventCollector collector;
        watcher.SetEventCallback([&collector](const GB_SystemFileEvent& event)
            {
                collector.Add(event);
            });

        GB_SystemFileWatchTargetOptions options;
        options.targetType = GB_SystemFileWatchTargetType::Directory;
        options.autoReconnect = true;
        options.recoveryMode = GB_SystemFileRecoveryMode::SnapshotDiff;
        GB_SystemFileWatchTargetId targetId;
        RequireFileWatcherResultSucceeded(watcher.AddTarget(reconnectPath, options, targetId), "Add reconnect target");
        RequireFileWatcherResultSucceeded(watcher.Start(), "Start reconnect watcher");
        RequireTrueSystemFileWatcher(collector.WaitFor([targetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, targetId, GB_SystemFileEventType::RecoveryCompleted);
            }, 5000), "Reconnect initial snapshot", "Expected initial snapshot completion.");

        RequireTrueSystemFileWatcher(GB_DeleteDirectory(reconnectPath), "Delete watched root", "Failed to delete the watched root.");
        RequireTrueSystemFileWatcher(collector.WaitFor([targetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, targetId, GB_SystemFileEventType::TargetUnavailable);
            }, 5000), "Target unavailable event", "Expected TargetUnavailable after deleting the watched root.");
        RequireTrueSystemFileWatcher(GB_CreateDirectory(reconnectPath), "Recreate watched root", "Failed to recreate the watched root.");
        RequireTrueSystemFileWatcher(collector.WaitFor([targetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, targetId, GB_SystemFileEventType::TargetRecovered);
            }, 10000), "Target recovered event", "Expected TargetRecovered after recreating the fixed path.");

        const std::string recoveredFilePath = GB_JoinPath(reconnectPath, "recovered.txt");
        RequireTrueSystemFileWatcher(WriteFileBytes(recoveredFilePath, "recovered", false), "Create file after reconnect", "Failed to create recovered.txt.");
        RequireTrueSystemFileWatcher(collector.WaitFor([targetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, targetId, GB_SystemFileEventType::Added, "recovered.txt");
            }, 5000), "Event after reconnect", "Expected events after reconnect.");
        RequireFileWatcherResultSucceeded(watcher.Stop(), "Stop reconnect watcher");
    }

    void RunStopAndStableTests(const std::string& rootPath)
    {
        const std::string callbackPath = GB_JoinPath(rootPath, "callback");
        RequireTrueSystemFileWatcher(GB_CreateDirectory(callbackPath), "Create callback directory", "Failed to create callback directory.");

        GB_SystemFileWatcher watcher;
        std::atomic<bool> callbackStopped(false);
        watcher.SetEventCallback([&watcher, &callbackStopped](const GB_SystemFileEvent& event)
            {
                if (event.eventType == GB_SystemFileEventType::Added)
                {
                    callbackStopped.store(watcher.Stop().IsSucceeded(), std::memory_order_release);
                }
            });
        GB_SystemFileWatchTargetOptions targetOptions;
        GB_SystemFileWatchTargetId targetId;
        RequireFileWatcherResultSucceeded(watcher.AddTarget(callbackPath, targetOptions, targetId), "Add callback-stop target");
        RequireFileWatcherResultSucceeded(watcher.Start(), "Start callback-stop watcher");
        RequireTrueSystemFileWatcher(WriteFileBytes(GB_JoinPath(callbackPath, "stop.txt"), "stop", false), "Create callback-stop file", "Failed to create stop.txt.");
        const std::chrono::steady_clock::time_point stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!callbackStopped.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < stopDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        RequireTrueSystemFileWatcher(callbackStopped.load(std::memory_order_acquire), "Stop inside callback", "Expected Stop to succeed inside the typed callback.");

        GB_SystemFileWatcher concurrentWatcher;
        GB_SystemFileWatchTargetId concurrentTargetId;
        RequireFileWatcherResultSucceeded(concurrentWatcher.AddTarget(callbackPath, targetOptions, concurrentTargetId), "Add concurrent-stop target");
        RequireFileWatcherResultSucceeded(concurrentWatcher.Start(), "Start concurrent-stop watcher");
        GB_SystemResult stopResult1;
        GB_SystemResult stopResult2;
        std::thread stopThread1([&concurrentWatcher, &stopResult1]() { stopResult1 = concurrentWatcher.Stop(); });
        std::thread stopThread2([&concurrentWatcher, &stopResult2]() { stopResult2 = concurrentWatcher.Stop(); });
        stopThread1.join();
        stopThread2.join();
        RequireTrueSystemFileWatcher(stopResult1.IsSucceeded() && stopResult2.IsSucceeded(), "Concurrent Stop", "Both concurrent Stop calls must succeed.");

        const std::string stablePath = GB_JoinPath(rootPath, "stable.bin");
        std::thread writer([stablePath]()
            {
                (void)WriteFileBytes(stablePath, "part1", false);
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                (void)WriteFileBytes(stablePath, "part2", true);
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                (void)WriteFileBytes(stablePath, "part3", true);
            });
        GB_SystemFileStableWaitOptions stableOptions;
        stableOptions.timeoutMilliseconds = 5000;
        stableOptions.pollIntervalMilliseconds = 25;
        stableOptions.stableWindowMilliseconds = 200;
        stableOptions.requireReadable = true;
        const GB_SystemResult stableResult = GB_SystemFileWatcher::WaitForFileStable(stablePath, stableOptions);
        writer.join();
        RequireFileWatcherResultSucceeded(stableResult, "WaitForFileStable");

        std::atomic<bool> cancellationFlag(true);
        stableOptions.cancellationFlag = &cancellationFlag;
        RequireTrueSystemFileWatcher(GB_SystemFileWatcher::WaitForFileStable(stablePath, stableOptions).errorCode == GB_SystemErrorCode::Cancelled, "WaitForFileStable cancellation", "Expected cancellation to be reported.");
    }

    void RunLongPathAndStressTests(const std::string& rootPath)
    {
        std::string longDirectoryPath = GB_JoinPath(rootPath, "long_path");
        for (int index = 0; index < 20; index++)
        {
            longDirectoryPath = GB_JoinPath(longDirectoryPath, std::string("segment_") + std::to_string(index) + "_abcdef");
        }
        RequireTrueSystemFileWatcher(longDirectoryPath.size() > 260, "Long path length", "Expected the generated path to exceed MAX_PATH.");
        RequireTrueSystemFileWatcher(GB_CreateDirectory(longDirectoryPath), "Create long directory path", "Failed to create the long directory path.");

        GB_SystemFileWatcher longPathWatcher;
        EventCollector longPathCollector;
        longPathWatcher.SetEventCallback([&longPathCollector](const GB_SystemFileEvent& event)
            {
                longPathCollector.Add(event);
            });
        GB_SystemFileWatchTargetOptions longPathOptions;
        GB_SystemFileWatchTargetId longPathTargetId;
        RequireFileWatcherResultSucceeded(longPathWatcher.AddTarget(longDirectoryPath, longPathOptions, longPathTargetId), "Add long path target");
        RequireFileWatcherResultSucceeded(longPathWatcher.Start(), "Start long path watcher");
        const std::string longFilePath = GB_JoinPath(longDirectoryPath, u8"长路径文件.txt");
        RequireTrueSystemFileWatcher(WriteFileBytes(longFilePath, "long", false), "Create long path file", "Failed to create a file below the long path.");
        RequireTrueSystemFileWatcher(longPathCollector.WaitFor([longPathTargetId](const GB_SystemFileEvent& event)
            {
                return IsTargetEvent(event, longPathTargetId, GB_SystemFileEventType::Added, u8"长路径文件.txt");
            }, 5000), "Long path event", "Expected an Added event below the long path.");
        RequireFileWatcherResultSucceeded(longPathWatcher.Stop(), "Stop long path watcher");

        const std::string stressDirectoryPath = GB_JoinPath(rootPath, "stress");
        RequireTrueSystemFileWatcher(GB_CreateDirectory(stressDirectoryPath), "Create stress directory", "Failed to create stress directory.");
        for (int iteration = 0; iteration < 10; iteration++)
        {
            GB_SystemFileWatcher watcher;
            GB_SystemFileWatchTargetOptions targetOptions;
            targetOptions.recursive = true;
            GB_SystemFileWatchTargetId targetId;
            RequireFileWatcherResultSucceeded(watcher.AddTarget(stressDirectoryPath, targetOptions, targetId), "Stress add target");
            RequireFileWatcherResultSucceeded(watcher.Start(), "Stress start watcher");
            const std::string firstPath = GB_JoinPath(stressDirectoryPath, std::string("stress_") + std::to_string(iteration) + ".tmp");
            const std::string secondPath = GB_JoinPath(stressDirectoryPath, std::string("stress_") + std::to_string(iteration) + ".dat");
            RequireTrueSystemFileWatcher(WriteFileBytes(firstPath, "a", false), "Stress create file", "Failed to create a stress file.");
            RequireTrueSystemFileWatcher(WriteFileBytes(firstPath, "b", true), "Stress modify file", "Failed to modify a stress file.");
            RequireTrueSystemFileWatcher(RenamePath(firstPath, secondPath), "Stress rename file", "Failed to rename a stress file.");
            RequireTrueSystemFileWatcher(GB_DeleteFile(secondPath), "Stress delete file", "Failed to delete a stress file.");
            RequireFileWatcherResultSucceeded(watcher.Stop(), "Stress stop watcher");
        }
    }
#endif
}

int RunGB_SystemFileWatcherTests()
{
#if defined(_WIN32)
    const std::string rootPath = MakeTestRoot();
    if (!GB_CreateDirectory(rootPath))
    {
        FailSystemFileWatcherTest("Create test root", "Failed to create the file watcher test root.");
    }

    RunValidationTests(rootPath);
    RunDirectoryAndFileEventTests(rootPath);
    RunAutoReconnectTest(rootPath);
    RunStopAndStableTests(rootPath);
    RunLongPathAndStressTests(rootPath);
    RequireTrueSystemFileWatcher(GB_DeletePath(rootPath), "Delete test root", "Failed to remove the file watcher test root.");
#else
    GB_SystemFileWatcher watcher;
    RequireTrueSystemFileWatcher(watcher.Start().errorCode == GB_SystemErrorCode::UnsupportedPlatform, "Unsupported platform watcher", "Expected UnsupportedPlatform.");
#endif

    std::cout << "GB_SystemFileWatcher tests passed. Cases: " << totalSystemFileWatcherCaseCount << std::endl;
    return 0;
}
