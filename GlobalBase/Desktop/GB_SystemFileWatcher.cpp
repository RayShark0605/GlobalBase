#include "GB_SystemFileWatcher.h"

#include "../GB_FileSystem.h"
#include "../GB_Utf8String.h"
#include "GB_WinHandleScope.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace
{
    static const char* const GB_SystemFileOperationAddTarget = "GB_SystemFileWatcher::AddTarget";
    static const char* const GB_SystemFileOperationRemoveTarget = "GB_SystemFileWatcher::RemoveTarget";
    static const char* const GB_SystemFileOperationClearTargets = "GB_SystemFileWatcher::ClearTargets";
    static const char* const GB_SystemFileOperationStart = "GB_SystemFileWatcher::Start";
    static const char* const GB_SystemFileOperationStop = "GB_SystemFileWatcher::Stop";
    static const char* const GB_SystemFileOperationPause = "GB_SystemFileWatcher::Pause";
    static const char* const GB_SystemFileOperationResume = "GB_SystemFileWatcher::Resume";
    static const char* const GB_SystemFileOperationRescan = "GB_SystemFileWatcher::RequestRescan";
    static const char* const GB_SystemFileOperationWaitStable = "GB_SystemFileWatcher::WaitForFileStable";

    static std::atomic<uint64_t>& GetWatcherIdSeed()
    {
        static std::atomic<uint64_t> watcherIdSeed(1);
        return watcherIdSeed;
    }

    static uint64_t AllocateWatcherId()
    {
        uint64_t watcherId = GetWatcherIdSeed().fetch_add(1, std::memory_order_relaxed);
        if (watcherId == 0)
        {
            watcherId = GetWatcherIdSeed().fetch_add(1, std::memory_order_relaxed);
        }
        return watcherId == 0 ? 1 : watcherId;
    }

    static uint64_t TakeNextId(uint64_t& nextId)
    {
        if (nextId == 0)
        {
            nextId = 1;
        }
        const uint64_t currentId = nextId;
        nextId = nextId == (std::numeric_limits<uint64_t>::max)() ? 1 : nextId + 1;
        return currentId;
    }

    static bool HasEmbeddedNull(const std::string& text)
    {
        return text.find('\0') != std::string::npos;
    }

    static bool IsValidTargetType(const GB_SystemFileWatchTargetType targetType)
    {
        return targetType == GB_SystemFileWatchTargetType::File || targetType == GB_SystemFileWatchTargetType::Directory;
    }

    static bool IsValidDeliveryMode(const GB_SystemFileEventDeliveryMode deliveryMode)
    {
        return deliveryMode == GB_SystemFileEventDeliveryMode::Raw || deliveryMode == GB_SystemFileEventDeliveryMode::Normalized || deliveryMode == GB_SystemFileEventDeliveryMode::Both;
    }

    static bool IsValidRecoveryMode(const GB_SystemFileRecoveryMode recoveryMode)
    {
        return recoveryMode == GB_SystemFileRecoveryMode::ReportOnly || recoveryMode == GB_SystemFileRecoveryMode::SnapshotDiff;
    }

    static bool WantsRawEvents(const GB_SystemFileEventDeliveryMode deliveryMode)
    {
        return deliveryMode == GB_SystemFileEventDeliveryMode::Raw || deliveryMode == GB_SystemFileEventDeliveryMode::Both;
    }

    static bool WantsNormalizedEvents(const GB_SystemFileEventDeliveryMode deliveryMode)
    {
        return deliveryMode == GB_SystemFileEventDeliveryMode::Normalized || deliveryMode == GB_SystemFileEventDeliveryMode::Both;
    }

    static GB_SystemResult MakeAllocationFailedResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, operationName, u8"分配文件监听器内部资源失败。");
    }

#if !defined(_WIN32)
    static GB_SystemResult MakeUnsupportedPlatformResult(const std::string& operationName)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::UnsupportedPlatform, operationName, u8"当前平台不支持 Windows 文件系统监听器。");
    }
#endif

#if defined(_WIN32)
    static std::wstring ReplaceSeparators(std::wstring path)
    {
        for (size_t index = 0; index < path.size(); index++)
        {
            if (path[index] == L'/')
            {
                path[index] = L'\\';
            }
        }
        return path;
    }

    static bool IsDriveRoot(const std::wstring& path)
    {
        return path.size() == 3 && path[1] == L':' && path[2] == L'\\';
    }

    static bool IsUncShareRoot(const std::wstring& path)
    {
        if (path.size() < 5 || path[0] != L'\\' || path[1] != L'\\')
        {
            return false;
        }
        const size_t serverEnd = path.find(L'\\', 2);
        if (serverEnd == std::wstring::npos)
        {
            return false;
        }
        const size_t shareEnd = path.find(L'\\', serverEnd + 1);
        return shareEnd == std::wstring::npos || shareEnd == path.size() - 1;
    }

    static void TrimTrailingSeparators(std::wstring& path)
    {
        while (path.size() > 1 && path[path.size() - 1] == L'\\' && !IsDriveRoot(path) && !IsUncShareRoot(path))
        {
            path.erase(path.size() - 1);
        }
    }

    static std::wstring MakeExtendedPath(const std::wstring& path)
    {
        if (path.compare(0, 4, L"\\\\?\\") == 0)
        {
            return path;
        }
        if (path.compare(0, 2, L"\\\\") == 0)
        {
            return std::wstring(L"\\\\?\\UNC\\") + path.substr(2);
        }
        return std::wstring(L"\\\\?\\") + path;
    }

    static std::wstring RemoveExtendedPrefix(const std::wstring& path)
    {
        if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        {
            return std::wstring(L"\\\\") + path.substr(8);
        }
        if (path.compare(0, 4, L"\\\\?\\") == 0)
        {
            return path.substr(4);
        }
        return path;
    }

    static bool NormalizeAbsolutePath(const std::string& pathUtf8, std::wstring& normalizedPath, std::string& normalizedPathUtf8, GB_SystemResult& result)
    {
        normalizedPath.clear();
        normalizedPathUtf8.clear();
        if (pathUtf8.empty() || HasEmbeddedNull(pathUtf8) || !GB_IsUtf8(pathUtf8))
        {
            result = GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationAddTarget, u8"监听路径必须是非空、无嵌入 NUL 的合法 UTF-8 字符串。");
            return false;
        }

        try
        {
            const std::wstring inputPath = ReplaceSeparators(GB_Utf8ToWString(pathUtf8));
            const DWORD requiredLength = ::GetFullPathNameW(inputPath.c_str(), 0, nullptr, nullptr);
            if (requiredLength == 0)
            {
                result = GB_SystemResult::FromLastWin32Error(GB_SystemFileOperationAddTarget, u8"将监听路径转换为绝对路径失败。");
                return false;
            }
            std::vector<wchar_t> buffer(static_cast<size_t>(requiredLength) + 1, L'\0');
            const DWORD writtenLength = ::GetFullPathNameW(inputPath.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
            if (writtenLength == 0 || writtenLength >= buffer.size())
            {
                result = GB_SystemResult::FromLastWin32Error(GB_SystemFileOperationAddTarget, u8"将监听路径转换为绝对路径失败。");
                return false;
            }
            normalizedPath.assign(buffer.data(), writtenLength);
            normalizedPath = ReplaceSeparators(RemoveExtendedPrefix(normalizedPath));
            TrimTrailingSeparators(normalizedPath);
            normalizedPathUtf8 = GB_WStringToUtf8(normalizedPath);
            std::replace(normalizedPathUtf8.begin(), normalizedPathUtf8.end(), '\\', '/');
        }
        catch (const std::bad_alloc&)
        {
            result = MakeAllocationFailedResult(GB_SystemFileOperationAddTarget);
            return false;
        }
        catch (...)
        {
            result = GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_SystemFileOperationAddTarget, u8"监听路径的 UTF-8 与 UTF-16 转换失败。");
            return false;
        }

        result = GB_SystemResult::Succeeded(GB_SystemFileOperationAddTarget);
        return true;
    }

    static std::wstring GetParentPath(const std::wstring& path)
    {
        if (IsDriveRoot(path) || IsUncShareRoot(path))
        {
            return std::wstring();
        }
        const size_t separatorIndex = path.find_last_of(L'\\');
        if (separatorIndex == std::wstring::npos)
        {
            return std::wstring();
        }
        if (separatorIndex == 2 && path.size() >= 3 && path[1] == L':')
        {
            return path.substr(0, 3);
        }
        std::wstring parentPath = path.substr(0, separatorIndex);
        TrimTrailingSeparators(parentPath);
        return parentPath;
    }

    static std::wstring GetPathFileName(const std::wstring& path)
    {
        const size_t separatorIndex = path.find_last_of(L'\\');
        return separatorIndex == std::wstring::npos ? path : path.substr(separatorIndex + 1);
    }

    static std::wstring JoinWidePath(const std::wstring& leftPath, const std::wstring& rightPath)
    {
        if (leftPath.empty())
        {
            return rightPath;
        }
        if (rightPath.empty())
        {
            return leftPath;
        }
        return leftPath[leftPath.size() - 1] == L'\\' ? leftPath + rightPath : leftPath + L"\\" + rightPath;
    }

    static std::wstring NormalizeRelativeWidePath(std::wstring path)
    {
        path = ReplaceSeparators(std::move(path));
        const size_t firstNameIndex = path.find_first_not_of(L'\\');
        if (firstNameIndex == std::wstring::npos)
        {
            return std::wstring();
        }
        if (firstNameIndex > 0)
        {
            path.erase(0, firstNameIndex);
        }
        return path;
    }

    static std::string WidePathToUtf8ForwardSlash(const std::wstring& path)
    {
        std::string pathUtf8 = GB_WStringToUtf8(path);
        std::replace(pathUtf8.begin(), pathUtf8.end(), '\\', '/');
        return pathUtf8;
    }

    static bool IsValidUtf16(const std::wstring& text)
    {
        if (text.empty())
        {
            return true;
        }
        return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr) > 0;
    }

    static std::wstring FoldCaseOrdinal(const std::wstring& text)
    {
        if (text.empty())
        {
            return text;
        }
        const int requiredLength = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr, 0);
        if (requiredLength <= 0)
        {
            std::wstring fallback = text;
            for (size_t index = 0; index < fallback.size(); index++)
            {
                if (fallback[index] >= L'A' && fallback[index] <= L'Z')
                {
                    fallback[index] = static_cast<wchar_t>(fallback[index] - L'A' + L'a');
                }
            }
            return fallback;
        }
        std::wstring folded(static_cast<size_t>(requiredLength), L'\0');
        const int writtenLength = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), static_cast<int>(text.size()), &folded[0], requiredLength, nullptr, nullptr, 0);
        if (writtenLength <= 0)
        {
            return text;
        }
        return folded;
    }

    static bool EqualWidePath(const std::wstring& leftPath, const std::wstring& rightPath, const bool caseSensitive)
    {
        if (caseSensitive)
        {
            return leftPath == rightPath;
        }
        return ::CompareStringOrdinal(leftPath.data(), static_cast<int>(leftPath.size()), rightPath.data(), static_cast<int>(rightPath.size()), TRUE) == CSTR_EQUAL;
    }

    static bool IsDirectoryPath(const std::wstring& path)
    {
        const DWORD attributes = ::GetFileAttributesW(MakeExtendedPath(path).c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    static bool IsRegularFilePath(const std::wstring& path)
    {
        const DWORD attributes = ::GetFileAttributesW(MakeExtendedPath(path).c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    static bool PathExists(const std::wstring& path)
    {
        return ::GetFileAttributesW(MakeExtendedPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    static uint64_t FileTimeToUInt64(const FILETIME& fileTime)
    {
        ULARGE_INTEGER value;
        value.LowPart = fileTime.dwLowDateTime;
        value.HighPart = fileTime.dwHighDateTime;
        return value.QuadPart;
    }

    static DWORD ToNativeNotifyFilter(const GB_SystemFileNotifyFilter notifyFilter)
    {
        const uint32_t filterValue = static_cast<uint32_t>(notifyFilter);
        DWORD nativeFilter = 0;
        if ((filterValue & static_cast<uint32_t>(GB_SystemFileNotifyFilter::FileName)) != 0)
        {
            nativeFilter |= FILE_NOTIFY_CHANGE_FILE_NAME;
        }
        if ((filterValue & static_cast<uint32_t>(GB_SystemFileNotifyFilter::DirectoryName)) != 0)
        {
            nativeFilter |= FILE_NOTIFY_CHANGE_DIR_NAME;
        }
        if ((filterValue & static_cast<uint32_t>(GB_SystemFileNotifyFilter::Attributes)) != 0)
        {
            nativeFilter |= FILE_NOTIFY_CHANGE_ATTRIBUTES;
        }
        if ((filterValue & static_cast<uint32_t>(GB_SystemFileNotifyFilter::Size)) != 0)
        {
            nativeFilter |= FILE_NOTIFY_CHANGE_SIZE;
        }
        if ((filterValue & static_cast<uint32_t>(GB_SystemFileNotifyFilter::LastWrite)) != 0)
        {
            nativeFilter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
        }
        if ((filterValue & static_cast<uint32_t>(GB_SystemFileNotifyFilter::LastAccess)) != 0)
        {
            nativeFilter |= FILE_NOTIFY_CHANGE_LAST_ACCESS;
        }
        if ((filterValue & static_cast<uint32_t>(GB_SystemFileNotifyFilter::Creation)) != 0)
        {
            nativeFilter |= FILE_NOTIFY_CHANGE_CREATION;
        }
        if ((filterValue & static_cast<uint32_t>(GB_SystemFileNotifyFilter::Security)) != 0)
        {
            nativeFilter |= FILE_NOTIFY_CHANGE_SECURITY;
        }
        return nativeFilter;
    }

    static bool GlobMatchWide(const std::wstring& inputPattern, const std::wstring& inputPath, const bool caseSensitive)
    {
        const std::wstring& pattern = inputPattern;
        const std::wstring path = caseSensitive ? inputPath : FoldCaseOrdinal(inputPath);
        if (pattern.size() > 32768 || path.size() > 32768)
        {
            return false;
        }
        const size_t maximumTransitionCount = 4u * 1024u * 1024u;
        if (!pattern.empty() && path.size() > maximumTransitionCount / pattern.size())
        {
            return false;
        }

        std::vector<unsigned char> previous(path.size() + 1, 0);
        std::vector<unsigned char> current(path.size() + 1, 0);
        previous[0] = 1;

        size_t patternIndex = 0;
        while (patternIndex < pattern.size())
        {
            std::fill(current.begin(), current.end(), static_cast<unsigned char>(0));
            const wchar_t patternChar = pattern[patternIndex];
            if (patternChar == L'*')
            {
                const bool doubleStar = patternIndex + 1 < pattern.size() && pattern[patternIndex + 1] == L'*';
                const bool doubleStarDirectory = doubleStar && patternIndex + 2 < pattern.size() && pattern[patternIndex + 2] == L'\\';
                if (doubleStarDirectory)
                {
                    bool canReachDirectoryBoundary = false;
                    for (size_t pathIndex = 0; pathIndex <= path.size(); pathIndex++)
                    {
                        canReachDirectoryBoundary = canReachDirectoryBoundary || previous[pathIndex] != 0;
                        if (previous[pathIndex] != 0)
                        {
                            current[pathIndex] = 1;
                        }
                        if (pathIndex < path.size() && path[pathIndex] == L'\\' && canReachDirectoryBoundary)
                        {
                            current[pathIndex + 1] = 1;
                        }
                    }
                    patternIndex += 3;
                    previous.swap(current);
                    continue;
                }
                current[0] = previous[0];
                for (size_t pathIndex = 0; pathIndex < path.size(); pathIndex++)
                {
                    const bool canConsume = doubleStar || path[pathIndex] != L'\\';
                    current[pathIndex + 1] = static_cast<unsigned char>(previous[pathIndex + 1] || (canConsume && current[pathIndex]));
                }
                patternIndex += doubleStar ? 2 : 1;
            }
            else
            {
                for (size_t pathIndex = 0; pathIndex < path.size(); pathIndex++)
                {
                    const bool matches = patternChar == L'?' ? path[pathIndex] != L'\\' : patternChar == path[pathIndex];
                    current[pathIndex + 1] = static_cast<unsigned char>(previous[pathIndex] && matches);
                }
                patternIndex++;
            }
            previous.swap(current);
        }
        return previous[path.size()] != 0;
    }

    static std::wstring GetExtensionWide(const std::wstring& relativePath)
    {
        const size_t separatorIndex = relativePath.find_last_of(L'\\');
        const size_t nameStart = separatorIndex == std::wstring::npos ? 0 : separatorIndex + 1;
        const size_t dotIndex = relativePath.find_last_of(L'.');
        if (dotIndex == std::wstring::npos || dotIndex <= nameStart || dotIndex + 1 >= relativePath.size())
        {
            return std::wstring();
        }
        return relativePath.substr(dotIndex);
    }

    static bool IsShellShortcutExtension(const std::wstring& extension)
    {
        const std::wstring foldedExtension = FoldCaseOrdinal(extension);
        return foldedExtension == L".lnk" || foldedExtension == L".url";
    }

    static bool IsReadableFile(const std::wstring& path)
    {
        HANDLE fileHandle = ::CreateFileW(MakeExtendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        (void)::CloseHandle(fileHandle);
        return true;
    }
#endif
}

bool GB_SystemFileWatchTargetId::IsValid() const
{
    return watcherId != 0 && targetId != 0;
}

void GB_SystemFileWatchTargetId::Reset()
{
    watcherId = 0;
    targetId = 0;
}

GB_SystemFileWatchTargetId::operator bool() const
{
    return IsValid();
}

bool GB_SystemFileWatchTargetId::operator==(const GB_SystemFileWatchTargetId& other) const
{
    return watcherId == other.watcherId && targetId == other.targetId;
}

bool GB_SystemFileWatchTargetId::operator!=(const GB_SystemFileWatchTargetId& other) const
{
    return !(*this == other);
}

#if defined(_WIN32)
class GB_SystemFileWatcher::Impl final
{
public:
    explicit Impl(const GB_SystemFileWatcherOptions& inputOptions)
        : watcherId(AllocateWatcherId()),
        options(inputOptions),
        typedDispatcher(GB_EventDispatcher::MakeQueuedOptions(inputOptions.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropOldest, "GB_SystemFileWatcher.Typed")),
        publicDispatcher(GB_EventDispatcher::MakeQueuedOptions(inputOptions.maxDispatchQueueSize, GB_EventQueueOverflowPolicy::DropOldest, "GB_SystemFileWatcher.Public"))
    {
        callbackSetupResult = typedDispatcher.SubscribeAll([this](const GB_Event& event)
            {
                DispatchTypedCallbacks(event);
            }, typedSubscriptionToken);
    }

    ~Impl() noexcept
    {
        (void)Stop();
    }

    GB_SystemResult AddTarget(const std::string& path, const GB_SystemFileWatchTargetOptions& targetOptions, GB_SystemFileWatchTargetId& targetId)
    {
        targetId.Reset();
        const GB_SystemResult optionsResult = ValidateTargetOptions(targetOptions);
        if (optionsResult.IsFailed())
        {
            return optionsResult;
        }

        std::wstring normalizedPath;
        std::string normalizedPathUtf8;
        GB_SystemResult normalizeResult;
        if (!NormalizeAbsolutePath(path, normalizedPath, normalizedPathUtf8, normalizeResult))
        {
            return normalizeResult.WithOperationName(GB_SystemFileOperationAddTarget);
        }

        const std::wstring parentPath = GetParentPath(normalizedPath);
        const std::wstring fileName = GetPathFileName(normalizedPath);
        if (targetOptions.targetType == GB_SystemFileWatchTargetType::Directory)
        {
            if (PathExists(normalizedPath) && !IsDirectoryPath(normalizedPath))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationAddTarget, u8"目录监听目标已存在，但不是目录。");
            }
            if (!PathExists(normalizedPath) && !targetOptions.autoReconnect)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_SystemFileOperationAddTarget, u8"目录监听目标不存在；仅启用 autoReconnect 时允许先添加不存在的目录。");
            }
        }
        else
        {
            if (PathExists(normalizedPath) && !IsRegularFilePath(normalizedPath))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationAddTarget, u8"文件监听目标已存在，但不是常规文件。");
            }
            if (parentPath.empty() || (!IsDirectoryPath(parentPath) && !targetOptions.autoReconnect))
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_SystemFileOperationAddTarget, u8"文件监听目标的父目录不存在；仅启用 autoReconnect 时允许父目录暂时不可用。");
            }
        }

        std::unique_lock<std::mutex> operationLock(operationMutex);
        std::shared_ptr<TargetRecord> targetRecord(new TargetRecord());
        try
        {
            for (size_t index = 0; index < targetOptions.includeGlobs.size(); index++)
            {
                std::wstring pattern = ReplaceSeparators(GB_Utf8ToWString(targetOptions.includeGlobs[index]));
                targetRecord->includeGlobsWide.push_back(targetOptions.caseSensitive ? pattern : FoldCaseOrdinal(pattern));
            }
            for (size_t index = 0; index < targetOptions.excludeGlobs.size(); index++)
            {
                std::wstring pattern = ReplaceSeparators(GB_Utf8ToWString(targetOptions.excludeGlobs[index]));
                targetRecord->excludeGlobsWide.push_back(targetOptions.caseSensitive ? pattern : FoldCaseOrdinal(pattern));
            }
            for (size_t index = 0; index < targetOptions.extensions.size(); index++)
            {
                std::wstring extension = GB_Utf8ToWString(targetOptions.extensions[index]);
                if (!extension.empty() && extension[0] != L'.')
                {
                    extension.insert(extension.begin(), L'.');
                }
                targetRecord->extensionsWide.push_back(targetOptions.caseSensitive ? extension : FoldCaseOrdinal(extension));
            }
        }
        catch (const std::bad_alloc&)
        {
            return MakeAllocationFailedResult(GB_SystemFileOperationAddTarget);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_SystemFileOperationAddTarget, u8"预编译监听过滤规则时编码转换失败。");
        }
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::const_iterator iter = targets.begin(); iter != targets.end(); ++iter)
            {
                if (iter->second->info.options.targetType == targetOptions.targetType && EqualWidePath(iter->second->normalizedPath, normalizedPath, false))
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::AlreadyExists, GB_SystemFileOperationAddTarget, u8"相同类型和路径的监听目标已经存在。");
                }
            }

            targetRecord->info.targetId.watcherId = watcherId;
            targetRecord->info.targetId.targetId = TakeNextId(nextTargetId);
            targetRecord->info.path = normalizedPathUtf8;
            targetRecord->info.options = targetOptions;
            targetRecord->info.state = IsTargetAvailable(*targetRecord, normalizedPath, parentPath) ? GB_SystemFileWatchTargetState::Stopped : GB_SystemFileWatchTargetState::Unavailable;
            targetRecord->info.lastResult = GB_SystemResult::Succeeded(GB_SystemFileOperationAddTarget);
            targetRecord->normalizedPath = normalizedPath;
            targetRecord->parentPath = parentPath;
            targetRecord->fileName = fileName;
            targetRecord->reconnectDelayMilliseconds = options.reconnectInitialDelayMilliseconds;
            targets[targetRecord->info.targetId.targetId] = targetRecord;
            targetId = targetRecord->info.targetId;
        }

        const GB_SystemFileWatcherState currentState = GetState();
        if (currentState == GB_SystemFileWatcherState::Watching || currentState == GB_SystemFileWatcherState::Paused || currentState == GB_SystemFileWatcherState::Recovering)
        {
            GB_SystemResult rebuildResult = RebuildNativeEngine();
            if (rebuildResult.IsFailed())
            {
                {
                    std::lock_guard<std::mutex> stateLock(stateMutex);
                    targets.erase(targetId.targetId);
                }
                targetId.Reset();
                (void)RebuildNativeEngine();
                return rebuildResult.WithOperationName(GB_SystemFileOperationAddTarget);
            }
            const bool targetAvailable = IsTargetAvailable(*targetRecord, targetRecord->normalizedPath, targetRecord->parentPath);
            const GB_SystemFileWatchTargetState targetState = !targetAvailable ? GB_SystemFileWatchTargetState::Unavailable : currentState == GB_SystemFileWatcherState::Paused ? GB_SystemFileWatchTargetState::Paused : GB_SystemFileWatchTargetState::Watching;
            SetTargetState(targetRecord->info.targetId, targetState, GB_SystemResult::Succeeded(GB_SystemFileOperationAddTarget));
            if (currentState != GB_SystemFileWatcherState::Paused)
            {
                ScheduleAllSnapshotRecoveries();
            }
        }

        return GB_SystemResult::Succeeded(GB_SystemFileOperationAddTarget);
    }

    GB_SystemResult RemoveTarget(const GB_SystemFileWatchTargetId& targetId)
    {
        if (!OwnsTargetId(targetId))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationRemoveTarget, u8"监听目标 ID 无效或属于另一个监听器实例。");
        }

        std::unique_lock<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (targets.erase(targetId.targetId) == 0)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_SystemFileOperationRemoveTarget, u8"未找到指定监听目标。");
            }
        }
        RemoveRecoveryJob(targetId.targetId);

        if (IsCoreRunning())
        {
            EnqueueCleanupTargetOutputBuffers(targetId.targetId);
            GB_SystemResult rebuildResult = RebuildNativeEngine();
            if (rebuildResult.IsFailed())
            {
                SetWatcherFailed(rebuildResult);
                return rebuildResult.WithOperationName(GB_SystemFileOperationRemoveTarget);
            }
            ScheduleAllSnapshotRecoveries();
        }
        return GB_SystemResult::Succeeded(GB_SystemFileOperationRemoveTarget);
    }

    GB_SystemResult ClearTargets()
    {
        std::unique_lock<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            targets.clear();
        }
        {
            std::lock_guard<std::mutex> recoveryLock(recoveryMutex);
            recoveryJobs.clear();
            queuedRecoveryIds.clear();
        }
        if (IsCoreRunning())
        {
            EnqueueCleanupAllOutputBuffers();
            GB_SystemResult rebuildResult = RebuildNativeEngine();
            if (rebuildResult.IsFailed())
            {
                SetWatcherFailed(rebuildResult);
                return rebuildResult.WithOperationName(GB_SystemFileOperationClearTargets);
            }
        }
        return GB_SystemResult::Succeeded(GB_SystemFileOperationClearTargets);
    }

    std::vector<GB_SystemFileWatchTargetInfo> GetTargets() const
    {
        std::vector<GB_SystemFileWatchTargetInfo> targetInfos;
        std::lock_guard<std::mutex> stateLock(stateMutex);
        targetInfos.reserve(targets.size());
        for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::const_iterator iter = targets.begin(); iter != targets.end(); ++iter)
        {
            targetInfos.push_back(iter->second->info);
        }
        return targetInfos;
    }

    GB_SystemResult Start()
    {
        std::unique_lock<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (state == GB_SystemFileWatcherState::Watching || state == GB_SystemFileWatcherState::Paused || state == GB_SystemFileWatcherState::Recovering)
            {
                return GB_SystemResult::Succeeded(GB_SystemFileOperationStart, u8"文件监听器已经启动。");
            }
            if (state == GB_SystemFileWatcherState::Starting || state == GB_SystemFileWatcherState::Stopping)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, GB_SystemFileOperationStart, u8"文件监听器正在执行另一个生命周期操作。");
            }
            if (targets.empty())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, GB_SystemFileOperationStart, u8"启动文件监听器前至少需要添加一个监听目标。");
            }
            state = GB_SystemFileWatcherState::Starting;
            coreStopping = false;
            paused = false;
        }

        const GB_SystemResult optionsResult = ValidateWatcherOptions();
        if (optionsResult.IsFailed())
        {
            SetStoppedAfterStartFailure(optionsResult);
            return optionsResult;
        }
        if (callbackSetupResult.IsFailed())
        {
            SetStoppedAfterStartFailure(callbackSetupResult);
            return callbackSetupResult.WithOperationName(GB_SystemFileOperationStart);
        }

        ResetRuntimeState();
        GB_SystemResult result = typedDispatcher.Start();
        if (result.IsFailed())
        {
            SetStoppedAfterStartFailure(result);
            return result.WithOperationName(GB_SystemFileOperationStart);
        }
        result = publicDispatcher.Start();
        if (result.IsFailed())
        {
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            SetStoppedAfterStartFailure(result);
            return result.WithOperationName(GB_SystemFileOperationStart);
        }

        try
        {
            processingThread = std::thread(&Impl::ProcessingThreadMain, this);
            recoveryThread = std::thread(&Impl::RecoveryThreadMain, this);
        }
        catch (const std::system_error& exception)
        {
            StopWorkerThreads(false);
            (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            result = GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_SystemFileOperationStart, std::string(u8"创建文件监听器工作线程失败：") + exception.what());
            SetStoppedAfterStartFailure(result);
            return result;
        }
        catch (const std::bad_alloc&)
        {
            StopWorkerThreads(false);
            (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            result = MakeAllocationFailedResult(GB_SystemFileOperationStart);
            SetStoppedAfterStartFailure(result);
            return result;
        }

        result = RebuildNativeEngine();
        if (result.IsFailed())
        {
            StopWorkerThreads(false);
            (void)publicDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            (void)typedDispatcher.Stop(GB_EventDispatcherStopMode::Discard);
            SetStoppedAfterStartFailure(result);
            return result.WithOperationName(GB_SystemFileOperationStart);
        }

        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            state = GB_SystemFileWatcherState::Watching;
            for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::iterator iter = targets.begin(); iter != targets.end(); ++iter)
            {
                iter->second->info.state = IsTargetAvailable(*iter->second, iter->second->normalizedPath, iter->second->parentPath) ? GB_SystemFileWatchTargetState::Watching : GB_SystemFileWatchTargetState::Unavailable;
                iter->second->info.lastResult = GB_SystemResult::Succeeded(GB_SystemFileOperationStart);
            }
        }

        EnqueueSyntheticEvent(MakeWatcherEvent(GB_SystemFileEventType::WatcherStarted));
        const std::vector<TargetView> startedTargetViews = GetTargetViews();
        for (size_t index = 0; index < startedTargetViews.size(); index++)
        {
            const std::shared_ptr<TargetRecord> targetRecord = FindTarget(startedTargetViews[index].targetId);
            if (targetRecord && targetRecord->info.state == GB_SystemFileWatchTargetState::Unavailable)
            {
                EnqueueSyntheticEvent(MakeTargetEvent(startedTargetViews[index], GB_SystemFileEventType::TargetUnavailable, GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_SystemFileOperationStart, u8"监听目标在启动时不可用，已进入自动重连等待。")));
            }
        }
        const std::vector<GB_SystemFileWatchTargetId> snapshotTargets = GetSnapshotTargetIds();
        for (size_t index = 0; index < snapshotTargets.size(); index++)
        {
            ScheduleRecovery(snapshotTargets[index]);
        }
        return GB_SystemResult::Succeeded(GB_SystemFileOperationStart);
    }

    GB_SystemResult Stop()
    {
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (state == GB_SystemFileWatcherState::Stopped)
            {
                return GB_SystemResult::Succeeded(GB_SystemFileOperationStop, u8"文件监听器已经停止。");
            }
            if (state == GB_SystemFileWatcherState::Stopping)
            {
                return GB_SystemResult::Succeeded(GB_SystemFileOperationStop, u8"文件监听器已经在停止过程中。");
            }
            state = GB_SystemFileWatcherState::Stopping;
            coreStopping = true;
            paused = false;
        }

        std::unique_lock<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> rebuildLock(nativeRebuildMutex);
            StopNativeEngine();
        }
        EnqueueSyntheticEvent(MakeWatcherEvent(GB_SystemFileEventType::WatcherStopped));
        StopWorkerThreads(true);

        GB_SystemResult publicStopResult = publicDispatcher.Stop(GB_EventDispatcherStopMode::Drain);
        GB_SystemResult typedStopResult = typedDispatcher.Stop(GB_EventDispatcherStopMode::Drain);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            state = GB_SystemFileWatcherState::Stopped;
            coreStopping = false;
            for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::iterator iter = targets.begin(); iter != targets.end(); ++iter)
            {
                iter->second->info.state = GB_SystemFileWatchTargetState::Stopped;
            }
        }
        if (publicStopResult.IsFailed())
        {
            return publicStopResult.WithOperationName(GB_SystemFileOperationStop);
        }
        if (typedStopResult.IsFailed())
        {
            return typedStopResult.WithOperationName(GB_SystemFileOperationStop);
        }
        return GB_SystemResult::Succeeded(GB_SystemFileOperationStop);
    }

    GB_SystemResult Pause()
    {
        std::unique_lock<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (state == GB_SystemFileWatcherState::Paused)
            {
                return GB_SystemResult::Succeeded(GB_SystemFileOperationPause, u8"文件监听器已经暂停。");
            }
            if (state != GB_SystemFileWatcherState::Watching && state != GB_SystemFileWatcherState::Recovering)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, GB_SystemFileOperationPause, u8"只有正在监听或恢复中的文件监听器可以暂停。");
            }
        }

        uint64_t barrierId = 0;
        {
            std::lock_guard<std::mutex> queueLock(nativeQueueMutex);
            barrierId = TakeNextId(nextBarrierId);
            NativePacket barrierPacket;
            barrierPacket.kind = NativePacketKind::PauseBarrier;
            barrierPacket.barrierId = barrierId;
            nativeQueue.push_back(std::move(barrierPacket));
        }
        nativeQueueCondition.notify_all();

        std::unique_lock<std::mutex> barrierLock(barrierMutex);
        barrierCondition.wait(barrierLock, [this, barrierId]()
            {
                return completedBarrierId >= barrierId || IsStopping();
            });
        return IsStopping() ? GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, GB_SystemFileOperationPause, u8"暂停操作因监听器停止而取消。") : GB_SystemResult::Succeeded(GB_SystemFileOperationPause);
    }

    GB_SystemResult Resume()
    {
        std::unique_lock<std::mutex> operationLock(operationMutex);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (state == GB_SystemFileWatcherState::Watching || state == GB_SystemFileWatcherState::Recovering)
            {
                return GB_SystemResult::Succeeded(GB_SystemFileOperationResume, u8"文件监听器未处于暂停状态。");
            }
            if (state != GB_SystemFileWatcherState::Paused)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, GB_SystemFileOperationResume, u8"只有已暂停的文件监听器可以恢复。");
            }
            paused = false;
            state = GB_SystemFileWatcherState::Recovering;
            for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::iterator iter = targets.begin(); iter != targets.end(); ++iter)
            {
                if (iter->second->info.state != GB_SystemFileWatchTargetState::Unavailable)
                {
                    iter->second->info.state = GB_SystemFileWatchTargetState::Recovering;
                }
            }
        }

        const std::vector<GB_SystemFileWatchTargetId> targetIds = GetAllTargetIds();
        bool scheduledRecovery = false;
        for (size_t index = 0; index < targetIds.size(); index++)
        {
            const std::shared_ptr<TargetRecord> targetRecord = FindTarget(targetIds[index]);
            if (!targetRecord)
            {
                continue;
            }
            if (targetRecord->info.options.recoveryMode == GB_SystemFileRecoveryMode::SnapshotDiff)
            {
                ScheduleRecovery(targetIds[index]);
                scheduledRecovery = true;
            }
            else
            {
                EnqueueSyntheticEvent(MakeTargetEvent(*targetRecord, GB_SystemFileEventType::RescanRequired, GB_SystemResult::Succeeded(GB_SystemFileOperationResume)));
                if (targetRecord->info.state != GB_SystemFileWatchTargetState::Unavailable)
                {
                    SetTargetState(targetIds[index], GB_SystemFileWatchTargetState::Watching, GB_SystemResult::Succeeded(GB_SystemFileOperationResume));
                }
            }
        }
        if (!scheduledRecovery)
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (state == GB_SystemFileWatcherState::Recovering)
            {
                state = GB_SystemFileWatcherState::Watching;
            }
        }
        return GB_SystemResult::Succeeded(GB_SystemFileOperationResume);
    }

    GB_SystemFileWatcherState GetState() const
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        return state;
    }

    bool IsRunning() const
    {
        const GB_SystemFileWatcherState currentState = GetState();
        return currentState == GB_SystemFileWatcherState::Watching || currentState == GB_SystemFileWatcherState::Paused || currentState == GB_SystemFileWatcherState::Recovering;
    }

    GB_SystemResult RequestRescan(const GB_SystemFileWatchTargetId& targetId)
    {
        if (!OwnsTargetId(targetId))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationRescan, u8"监听目标 ID 无效或属于另一个监听器实例。");
        }
        const std::shared_ptr<TargetRecord> targetRecord = FindTarget(targetId);
        if (!targetRecord)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_SystemFileOperationRescan, u8"未找到指定监听目标。");
        }
        if (!IsRunning())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, GB_SystemFileOperationRescan, u8"文件监听器未运行。");
        }
        if (targetRecord->info.options.recoveryMode == GB_SystemFileRecoveryMode::SnapshotDiff)
        {
            ScheduleRecovery(targetId);
        }
        else
        {
            EnqueueSyntheticEvent(MakeTargetEvent(*targetRecord, GB_SystemFileEventType::RescanRequired, GB_SystemResult::Succeeded(GB_SystemFileOperationRescan)));
        }
        return GB_SystemResult::Succeeded(GB_SystemFileOperationRescan);
    }

    GB_SystemResult RequestRescanAll()
    {
        if (!IsRunning())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, GB_SystemFileOperationRescan, u8"文件监听器未运行。");
        }
        const std::vector<GB_SystemFileWatchTargetId> targetIds = GetAllTargetIds();
        for (size_t index = 0; index < targetIds.size(); index++)
        {
            const GB_SystemResult result = RequestRescan(targetIds[index]);
            if (result.IsFailed())
            {
                return result;
            }
        }
        return GB_SystemResult::Succeeded(GB_SystemFileOperationRescan);
    }

    void SetEventCallback(const GB_SystemFileWatcher::EventCallback& inputCallback)
    {
        std::lock_guard<std::mutex> callbackLock(callbackMutex);
        eventCallback = inputCallback;
    }

    void SetBatchEventCallback(const GB_SystemFileWatcher::BatchEventCallback& inputCallback)
    {
        std::lock_guard<std::mutex> callbackLock(callbackMutex);
        batchEventCallback = inputCallback;
    }

    GB_EventDispatcher& GetEventDispatcher()
    {
        return publicDispatcher;
    }

    GB_SystemFileWatcherStatistics GetStatistics() const
    {
        GB_SystemFileWatcherStatistics statistics;
        statistics.receivedNativeEventCount = receivedNativeEventCount.load(std::memory_order_acquire);
        statistics.parsedNativeEventCount = parsedNativeEventCount.load(std::memory_order_acquire);
        statistics.deliveredEventCount = deliveredEventCount.load(std::memory_order_acquire);
        statistics.deliveredBatchCount = deliveredBatchCount.load(std::memory_order_acquire);
        statistics.droppedNativeEventCount = droppedNativeEventCount.load(std::memory_order_acquire);
        statistics.droppedDispatchEventCount = typedDispatcher.GetDroppedEventCount() + publicDispatcher.GetDroppedEventCount();
        statistics.overflowCount = overflowCount.load(std::memory_order_acquire);
        statistics.recoveryCount = recoveryCount.load(std::memory_order_acquire);
        statistics.reconnectCount = reconnectCount.load(std::memory_order_acquire);
        statistics.callbackExceptionCount = callbackExceptionCount.load(std::memory_order_acquire) + typedDispatcher.GetCallbackExceptionCount() + publicDispatcher.GetCallbackExceptionCount();
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            statistics.currentTargetCount = targets.size();
        }
        {
            std::lock_guard<std::mutex> engineLock(engineMutex);
            statistics.currentDirectorySessionCount = sessions.size();
        }
        {
            std::lock_guard<std::mutex> queueLock(nativeQueueMutex);
            statistics.pendingNativeEventCount = pendingNativeRecordCount;
        }
        return statistics;
    }

private:
    struct SnapshotEntry
    {
        std::string relativePath;
        GB_SystemFileObjectType objectType = GB_SystemFileObjectType::Unknown;
        uint64_t size = 0;
        uint64_t creationTime = 0;
        uint64_t lastWriteTime = 0;
        uint32_t attributes = 0;
    };

    struct TargetRecord
    {
        GB_SystemFileWatchTargetInfo info;
        std::wstring normalizedPath;
        std::wstring parentPath;
        std::wstring fileName;
        std::vector<std::wstring> includeGlobsWide;
        std::vector<std::wstring> excludeGlobsWide;
        std::vector<std::wstring> extensionsWide;
        std::map<std::wstring, SnapshotEntry> snapshot;
        bool snapshotInitialized = false;
        bool dirtyDuringRecovery = false;
        uint32_t reconnectDelayMilliseconds = 0;
        std::chrono::steady_clock::time_point nextReconnectTime;
    };

    struct TargetView
    {
        GB_SystemFileWatchTargetId targetId;
        std::string path;
        GB_SystemFileWatchTargetOptions options;
        std::wstring normalizedPath;
        std::wstring parentPath;
        std::wstring fileName;
        std::vector<std::wstring> includeGlobsWide;
        std::vector<std::wstring> excludeGlobsWide;
        std::vector<std::wstring> extensionsWide;
    };

    struct NativeRecord
    {
        DWORD action = 0;
        std::wstring relativePath;
    };

    enum class NativePacketKind
    {
        Changes,
        Overflow,
        SessionError,
        Synthetic,
        PauseBarrier,
        CleanupTargets,
        CleanupAllTargets
    };

    struct NativePacket
    {
        NativePacketKind kind = NativePacketKind::Changes;
        std::wstring sessionPath;
        std::vector<NativeRecord> records;
        std::vector<GB_SystemFileEvent> syntheticEvents;
        std::vector<uint64_t> cleanupTargetIds;
        DWORD errorCode = ERROR_SUCCESS;
        uint64_t barrierId = 0;
    };

    struct DirectorySession
    {
        std::wstring path;
        std::wstring key;
        DWORD notifyFilter = 0;
        bool recursive = false;
        size_t bufferSizeBytes = 0;
        std::vector<DWORD> buffer;
        OVERLAPPED overlapped;
        GB_WinHandleScope directoryHandle;
        bool readPending = false;

        DirectorySession()
            : overlapped()
        {
        }
    };

    struct SessionConfiguration
    {
        std::wstring path;
        DWORD notifyFilter = 0;
        bool recursive = false;
        size_t bufferSizeBytes = 4096;
    };

    struct PendingModifiedEvent
    {
        GB_SystemFileEvent event;
        std::chrono::steady_clock::time_point deadline;
    };

    bool OwnsTargetId(const GB_SystemFileWatchTargetId& targetId) const
    {
        return targetId.IsValid() && targetId.watcherId == watcherId;
    }

    bool IsStopping() const
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        return coreStopping || state == GB_SystemFileWatcherState::Stopping;
    }

    bool IsCoreRunning() const
    {
        const GB_SystemFileWatcherState currentState = GetState();
        return currentState == GB_SystemFileWatcherState::Watching || currentState == GB_SystemFileWatcherState::Paused || currentState == GB_SystemFileWatcherState::Recovering || currentState == GB_SystemFileWatcherState::Starting;
    }

    bool IsTargetAvailable(const TargetRecord& targetRecord, const std::wstring& normalizedPath, const std::wstring& parentPath) const
    {
        return targetRecord.info.options.targetType == GB_SystemFileWatchTargetType::Directory ? IsDirectoryPath(normalizedPath) : IsDirectoryPath(parentPath);
    }

    GB_SystemResult ValidateWatcherOptions() const
    {
        if (options.maxPendingNativeEvents == 0 || options.maxDispatchQueueSize == 0 || options.maxBatchSize == 0 || options.maxRecoveryScanPasses == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationStart, u8"队列容量、批量大小和恢复扫描次数必须大于 0。");
        }
        if (options.reconnectInitialDelayMilliseconds == 0 || options.reconnectMaximumDelayMilliseconds < options.reconnectInitialDelayMilliseconds)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationStart, u8"自动重连退避参数无效。");
        }
        return GB_SystemResult::Succeeded(GB_SystemFileOperationStart);
    }

    GB_SystemResult ValidateTargetOptions(const GB_SystemFileWatchTargetOptions& targetOptions) const
    {
        if (!IsValidTargetType(targetOptions.targetType) || !IsValidDeliveryMode(targetOptions.deliveryMode) || !IsValidRecoveryMode(targetOptions.recoveryMode))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationAddTarget, u8"监听目标的枚举选项包含无效值。");
        }
        const uint32_t filterValue = static_cast<uint32_t>(targetOptions.notifyFilter);
        const uint32_t allFilterValue = static_cast<uint32_t>(GB_SystemFileNotifyFilter::All);
        if (filterValue == 0 || (filterValue & ~allFilterValue) != 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationAddTarget, u8"通知掩码不能为空且不能包含未定义位。");
        }
        if (targetOptions.bufferSizeBytes < 4096 || targetOptions.bufferSizeBytes > 65536 || targetOptions.bufferSizeBytes % sizeof(DWORD) != 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationAddTarget, u8"监听缓冲区必须在 4 KB 至 64 KB 之间并按 DWORD 对齐。");
        }
        if (targetOptions.maxSnapshotEntryCount == 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationAddTarget, u8"快照条目上限必须大于 0。");
        }
        try
        {
            for (size_t index = 0; index < targetOptions.includeGlobs.size(); index++)
            {
                if (!GB_IsUtf8(targetOptions.includeGlobs[index]) || HasEmbeddedNull(targetOptions.includeGlobs[index]))
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationAddTarget, u8"包含 glob 必须是无嵌入 NUL 的合法 UTF-8 字符串。");
                }
                (void)GB_Utf8ToWString(targetOptions.includeGlobs[index]);
            }
            for (size_t index = 0; index < targetOptions.excludeGlobs.size(); index++)
            {
                if (!GB_IsUtf8(targetOptions.excludeGlobs[index]) || HasEmbeddedNull(targetOptions.excludeGlobs[index]))
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationAddTarget, u8"排除 glob 必须是无嵌入 NUL 的合法 UTF-8 字符串。");
                }
                (void)GB_Utf8ToWString(targetOptions.excludeGlobs[index]);
            }
            for (size_t index = 0; index < targetOptions.extensions.size(); index++)
            {
                if (targetOptions.extensions[index].empty() || !GB_IsUtf8(targetOptions.extensions[index]) || HasEmbeddedNull(targetOptions.extensions[index]))
                {
                    return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationAddTarget, u8"扩展名过滤项必须是非空、无嵌入 NUL 的合法 UTF-8 字符串。");
                }
                (void)GB_Utf8ToWString(targetOptions.extensions[index]);
            }
        }
        catch (const std::bad_alloc&)
        {
            return MakeAllocationFailedResult(GB_SystemFileOperationAddTarget);
        }
        catch (...)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, GB_SystemFileOperationAddTarget, u8"转换监听过滤规则时编码转换失败。");
        }
        return GB_SystemResult::Succeeded(GB_SystemFileOperationAddTarget);
    }

    void ResetRuntimeState()
    {
        {
            std::lock_guard<std::mutex> queueLock(nativeQueueMutex);
            nativeQueue.clear();
            pendingNativeRecordCount = 0;
            processingStopRequested = false;
            queueOverflowPending = false;
        }
        {
            std::lock_guard<std::mutex> recoveryLock(recoveryMutex);
            recoveryJobs.clear();
            queuedRecoveryIds.clear();
            recoveryStopRequested = false;
        }
        {
            std::lock_guard<std::mutex> barrierLock(barrierMutex);
            completedBarrierId = 0;
        }
        receivedNativeEventCount.store(0, std::memory_order_release);
        parsedNativeEventCount.store(0, std::memory_order_release);
        deliveredEventCount.store(0, std::memory_order_release);
        deliveredBatchCount.store(0, std::memory_order_release);
        droppedNativeEventCount.store(0, std::memory_order_release);
        overflowCount.store(0, std::memory_order_release);
        recoveryCount.store(0, std::memory_order_release);
        reconnectCount.store(0, std::memory_order_release);
        callbackExceptionCount.store(0, std::memory_order_release);
        dispatchOverflowPending.store(false, std::memory_order_release);
    }

    void SetStoppedAfterStartFailure(const GB_SystemResult& result)
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        state = GB_SystemFileWatcherState::Stopped;
        coreStopping = false;
        for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::iterator iter = targets.begin(); iter != targets.end(); ++iter)
        {
            iter->second->info.state = GB_SystemFileWatchTargetState::Stopped;
            iter->second->info.lastResult = result;
        }
    }

    void SetWatcherFailed(const GB_SystemResult& result)
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        state = GB_SystemFileWatcherState::Failed;
        for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::iterator iter = targets.begin(); iter != targets.end(); ++iter)
        {
            iter->second->info.lastResult = result;
            if (iter->second->info.state != GB_SystemFileWatchTargetState::Unavailable)
            {
                iter->second->info.state = GB_SystemFileWatchTargetState::Failed;
            }
        }
    }

    std::shared_ptr<TargetRecord> FindTarget(const GB_SystemFileWatchTargetId& targetId) const
    {
        if (!OwnsTargetId(targetId))
        {
            return std::shared_ptr<TargetRecord>();
        }
        std::lock_guard<std::mutex> stateLock(stateMutex);
        const std::map<uint64_t, std::shared_ptr<TargetRecord>>::const_iterator iter = targets.find(targetId.targetId);
        return iter == targets.end() ? std::shared_ptr<TargetRecord>() : iter->second;
    }

    TargetView MakeTargetView(const TargetRecord& targetRecord) const
    {
        TargetView targetView;
        targetView.targetId = targetRecord.info.targetId;
        targetView.path = targetRecord.info.path;
        targetView.options = targetRecord.info.options;
        targetView.normalizedPath = targetRecord.normalizedPath;
        targetView.parentPath = targetRecord.parentPath;
        targetView.fileName = targetRecord.fileName;
        targetView.includeGlobsWide = targetRecord.includeGlobsWide;
        targetView.excludeGlobsWide = targetRecord.excludeGlobsWide;
        targetView.extensionsWide = targetRecord.extensionsWide;
        return targetView;
    }

    std::vector<TargetView> GetTargetViews() const
    {
        std::vector<TargetView> targetViews;
        std::lock_guard<std::mutex> stateLock(stateMutex);
        targetViews.reserve(targets.size());
        for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::const_iterator iter = targets.begin(); iter != targets.end(); ++iter)
        {
            targetViews.push_back(MakeTargetView(*iter->second));
        }
        return targetViews;
    }

    std::vector<TargetView> GetTargetViewsForSession(const std::wstring& sessionPath) const
    {
        std::vector<TargetView> targetViews;
        std::lock_guard<std::mutex> stateLock(stateMutex);
        targetViews.reserve(targets.size());
        for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::const_iterator iter = targets.begin(); iter != targets.end(); iter++)
        {
            const TargetRecord& targetRecord = *iter->second;
            if (EqualWidePath(targetRecord.normalizedPath, sessionPath, false) || EqualWidePath(targetRecord.parentPath, sessionPath, false))
            {
                targetViews.push_back(MakeTargetView(targetRecord));
            }
        }
        return targetViews;
    }

    std::vector<GB_SystemFileWatchTargetId> GetAllTargetIds() const
    {
        std::vector<GB_SystemFileWatchTargetId> targetIds;
        std::lock_guard<std::mutex> stateLock(stateMutex);
        targetIds.reserve(targets.size());
        for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::const_iterator iter = targets.begin(); iter != targets.end(); ++iter)
        {
            targetIds.push_back(iter->second->info.targetId);
        }
        return targetIds;
    }

    std::vector<GB_SystemFileWatchTargetId> GetSnapshotTargetIds() const
    {
        std::vector<GB_SystemFileWatchTargetId> targetIds;
        std::lock_guard<std::mutex> stateLock(stateMutex);
        for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::const_iterator iter = targets.begin(); iter != targets.end(); ++iter)
        {
            if (iter->second->info.options.recoveryMode == GB_SystemFileRecoveryMode::SnapshotDiff)
            {
                targetIds.push_back(iter->second->info.targetId);
            }
        }
        return targetIds;
    }

    void SetTargetState(const GB_SystemFileWatchTargetId& targetId, const GB_SystemFileWatchTargetState targetState, const GB_SystemResult& result)
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        const std::map<uint64_t, std::shared_ptr<TargetRecord>>::iterator iter = targets.find(targetId.targetId);
        if (iter != targets.end())
        {
            iter->second->info.state = targetState;
            iter->second->info.lastResult = result;
        }
    }

    std::map<std::wstring, SessionConfiguration> BuildSessionConfigurations(GB_SystemResult& result) const
    {
        std::map<std::wstring, SessionConfiguration> configurations;
        const std::vector<TargetView> targetViews = GetTargetViews();
        for (size_t index = 0; index < targetViews.size(); index++)
        {
            const TargetView& targetView = targetViews[index];
            std::vector<std::pair<std::wstring, bool>> sessionPaths;
            if (targetView.options.targetType == GB_SystemFileWatchTargetType::Directory)
            {
                if (IsDirectoryPath(targetView.normalizedPath))
                {
                    sessionPaths.push_back(std::make_pair(targetView.normalizedPath, targetView.options.recursive));
                }
                else if (!targetView.options.autoReconnect)
                {
                    result = GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_SystemFileOperationStart, u8"目录监听目标在启动时不存在。");
                    return std::map<std::wstring, SessionConfiguration>();
                }
                if (!targetView.parentPath.empty() && IsDirectoryPath(targetView.parentPath))
                {
                    sessionPaths.push_back(std::make_pair(targetView.parentPath, false));
                }
            }
            else
            {
                if (IsDirectoryPath(targetView.parentPath))
                {
                    sessionPaths.push_back(std::make_pair(targetView.parentPath, false));
                }
                else if (!targetView.options.autoReconnect)
                {
                    result = GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, GB_SystemFileOperationStart, u8"文件监听目标的父目录在启动时不存在。");
                    return std::map<std::wstring, SessionConfiguration>();
                }
            }

            for (size_t pathIndex = 0; pathIndex < sessionPaths.size(); pathIndex++)
            {
                const std::wstring sessionKey = FoldCaseOrdinal(sessionPaths[pathIndex].first);
                SessionConfiguration& configuration = configurations[sessionKey];
                configuration.path = sessionPaths[pathIndex].first;
                configuration.notifyFilter |= ToNativeNotifyFilter(targetView.options.notifyFilter);
                configuration.notifyFilter |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
                if (targetView.options.includeShortcutFileEvents)
                {
                    configuration.notifyFilter |= FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
                }
                configuration.recursive = configuration.recursive || sessionPaths[pathIndex].second;
                configuration.bufferSizeBytes = (std::max)(configuration.bufferSizeBytes, targetView.options.bufferSizeBytes);
            }
        }
        result = GB_SystemResult::Succeeded(GB_SystemFileOperationStart);
        return configurations;
    }

    GB_SystemResult RebuildNativeEngine()
    {
        std::lock_guard<std::mutex> rebuildLock(nativeRebuildMutex);
        if (IsStopping())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, GB_SystemFileOperationStart, u8"监听器正在停止，取消底层目录 session 重建。");
        }
        StopNativeEngine();

        GB_SystemResult configurationResult;
        const std::map<std::wstring, SessionConfiguration> configurations = BuildSessionConfigurations(configurationResult);
        if (configurationResult.IsFailed())
        {
            return configurationResult;
        }
        if (configurations.empty())
        {
            return GB_SystemResult::Succeeded(GB_SystemFileOperationStart, u8"当前目标均不可用，等待自动重连。");
        }
        return StartNativeEngine(configurations);
    }

    GB_SystemResult StartNativeEngine(const std::map<std::wstring, SessionConfiguration>& configurations)
    {
        std::unique_lock<std::mutex> engineLock(engineMutex);
        HANDLE completionPort = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (completionPort == nullptr)
        {
            return GB_SystemResult::FromLastWin32Error(GB_SystemFileOperationStart, u8"创建文件监听 IOCP 失败。");
        }
        iocpHandle = GB_WinHandleScope::FromKernelHandle(completionPort, "GB_SystemFileWatcher.IOCP");
        engineStopping = false;
        pendingReadCount = 0;
        sessions.clear();

        for (std::map<std::wstring, SessionConfiguration>::const_iterator iter = configurations.begin(); iter != configurations.end(); ++iter)
        {
            const SessionConfiguration& configuration = iter->second;
            HANDLE directoryHandle = ::CreateFileW(MakeExtendedPath(configuration.path).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (directoryHandle == INVALID_HANDLE_VALUE)
            {
                const DWORD errorCode = ::GetLastError();
                engineLock.unlock();
                StopNativeEngine();
                return GB_SystemResult::FromWin32Error(errorCode, GB_SystemFileOperationStart, u8"打开目录监听句柄失败。");
            }

            GB_WinHandleScope directoryHandleScope = GB_WinHandleScope::FromKernelHandle(directoryHandle, "GB_SystemFileWatcher.Directory");
            std::shared_ptr<DirectorySession> session;
            try
            {
                session.reset(new DirectorySession());
                session->path = configuration.path;
                session->key = iter->first;
                session->notifyFilter = configuration.notifyFilter;
                session->recursive = configuration.recursive;
                session->bufferSizeBytes = configuration.bufferSizeBytes;
                session->buffer.resize((configuration.bufferSizeBytes + sizeof(DWORD) - 1) / sizeof(DWORD));
                session->directoryHandle = std::move(directoryHandleScope);
            }
            catch (const std::bad_alloc&)
            {
                engineLock.unlock();
                StopNativeEngine();
                return MakeAllocationFailedResult(GB_SystemFileOperationStart);
            }
            catch (...)
            {
                engineLock.unlock();
                StopNativeEngine();
                return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_SystemFileOperationStart, u8"创建目录监听 session 失败。");
            }

            if (::CreateIoCompletionPort(session->directoryHandle.GetHandleAs<HANDLE>(), completionPort, reinterpret_cast<ULONG_PTR>(session.get()), 0) == nullptr)
            {
                const DWORD errorCode = ::GetLastError();
                engineLock.unlock();
                StopNativeEngine();
                return GB_SystemResult::FromWin32Error(errorCode, GB_SystemFileOperationStart, u8"将目录句柄关联到 IOCP 失败。");
            }
            sessions.push_back(session);
        }

        try
        {
            iocpThread = std::thread(&Impl::IocpThreadMain, this);
        }
        catch (const std::system_error& exception)
        {
            engineLock.unlock();
            StopNativeEngine();
            return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceAllocationFailed, GB_SystemFileOperationStart, std::string(u8"创建 IOCP 线程失败：") + exception.what());
        }

        for (size_t index = 0; index < sessions.size(); index++)
        {
            const GB_SystemResult readResult = SubmitReadLocked(sessions[index]);
            if (readResult.IsFailed())
            {
                engineLock.unlock();
                StopNativeEngine();
                return readResult;
            }
        }
        return GB_SystemResult::Succeeded(GB_SystemFileOperationStart);
    }

    GB_SystemResult SubmitReadLocked(const std::shared_ptr<DirectorySession>& session)
    {
        if (engineStopping || !session || !session->directoryHandle.IsValid())
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, GB_SystemFileOperationStart, u8"目录读取已经停止。");
        }
        std::memset(&session->overlapped, 0, sizeof(session->overlapped));
        const BOOL readResult = ::ReadDirectoryChangesW(session->directoryHandle.GetHandleAs<HANDLE>(), session->buffer.data(), static_cast<DWORD>(session->bufferSizeBytes), session->recursive ? TRUE : FALSE, session->notifyFilter, nullptr, &session->overlapped, nullptr);
        if (readResult == FALSE)
        {
            return GB_SystemResult::FromLastWin32Error(GB_SystemFileOperationStart, u8"提交 ReadDirectoryChangesW 异步读取失败。");
        }
        session->readPending = true;
        pendingReadCount++;
        return GB_SystemResult::Succeeded(GB_SystemFileOperationStart);
    }

    void StopNativeEngine()
    {
        std::thread localIocpThread;
        HANDLE completionPort = nullptr;
        {
            std::unique_lock<std::mutex> engineLock(engineMutex);
            engineStopping = true;
            completionPort = iocpHandle.GetHandleAs<HANDLE>();
            for (size_t index = 0; index < sessions.size(); index++)
            {
                if (sessions[index]->readPending && sessions[index]->directoryHandle.IsValid())
                {
                    if (::CancelIoEx(sessions[index]->directoryHandle.GetHandleAs<HANDLE>(), &sessions[index]->overlapped) == FALSE)
                    {
                        const DWORD errorCode = ::GetLastError();
                        if (errorCode != ERROR_NOT_FOUND)
                        {
                            (void)errorCode;
                        }
                    }
                }
            }
            for (size_t index = 0; index < sessions.size(); index++)
            {
                if (sessions[index]->directoryHandle.IsValid())
                {
                    (void)sessions[index]->directoryHandle.Close();
                }
            }
            if (iocpThread.joinable())
            {
                engineCondition.wait(engineLock, [this]()
                    {
                        return pendingReadCount == 0;
                    });
                localIocpThread = std::move(iocpThread);
            }
        }

        if (completionPort != nullptr)
        {
            (void)::PostQueuedCompletionStatus(completionPort, 0, 0, nullptr);
        }
        if (localIocpThread.joinable())
        {
            localIocpThread.join();
        }

        std::lock_guard<std::mutex> engineLock(engineMutex);
        sessions.clear();
        (void)iocpHandle.Close();
        pendingReadCount = 0;
        engineStopping = false;
    }

    void IocpThreadMain()
    {
        for (;;)
        {
            DWORD transferredBytes = 0;
            ULONG_PTR completionKey = 0;
            OVERLAPPED* overlapped = nullptr;
            const BOOL completionResult = ::GetQueuedCompletionStatus(iocpHandle.GetHandleAs<HANDLE>(), &transferredBytes, &completionKey, &overlapped, INFINITE);
            const DWORD errorCode = completionResult == FALSE ? ::GetLastError() : ERROR_SUCCESS;
            if (overlapped == nullptr && completionKey == 0)
            {
                break;
            }

            DirectorySession* rawSession = reinterpret_cast<DirectorySession*>(completionKey);
            std::shared_ptr<DirectorySession> session = FindSession(rawSession);
            if (!session)
            {
                continue;
            }

            bool stoppingNow = false;
            {
                std::lock_guard<std::mutex> engineLock(engineMutex);
                if (session->readPending)
                {
                    session->readPending = false;
                    if (pendingReadCount > 0)
                    {
                        pendingReadCount--;
                    }
                }
                stoppingNow = engineStopping;
            }
            engineCondition.notify_all();
            if (stoppingNow || errorCode == ERROR_OPERATION_ABORTED)
            {
                continue;
            }

            receivedNativeEventCount.fetch_add(1, std::memory_order_relaxed);
            if (completionResult == FALSE)
            {
                if (errorCode == ERROR_NOTIFY_ENUM_DIR)
                {
                    EnqueueOverflow(session->path, errorCode);
                    ResubmitSession(session);
                }
                else
                {
                    EnqueueSessionError(session->path, errorCode);
                }
                continue;
            }
            if (transferredBytes == 0)
            {
                EnqueueOverflow(session->path, ERROR_NOTIFY_ENUM_DIR);
                ResubmitSession(session);
                continue;
            }

            NativePacket packet;
            packet.kind = NativePacketKind::Changes;
            packet.sessionPath = session->path;
            bool parseSucceeded = false;
            try
            {
                parseSucceeded = ParseNativeBuffer(*session, transferredBytes, packet.records);
            }
            catch (...)
            {
                parseSucceeded = false;
            }
            if (!parseSucceeded)
            {
                EnqueueOverflow(session->path, ERROR_INVALID_DATA);
            }
            else
            {
                parsedNativeEventCount.fetch_add(static_cast<uint64_t>(packet.records.size()), std::memory_order_relaxed);
                EnqueueNativePacket(std::move(packet));
            }
            ResubmitSession(session);
        }
    }

    std::shared_ptr<DirectorySession> FindSession(DirectorySession* rawSession)
    {
        std::lock_guard<std::mutex> engineLock(engineMutex);
        for (size_t index = 0; index < sessions.size(); index++)
        {
            if (sessions[index].get() == rawSession)
            {
                return sessions[index];
            }
        }
        return std::shared_ptr<DirectorySession>();
    }

    void ResubmitSession(const std::shared_ptr<DirectorySession>& session)
    {
        GB_SystemResult readResult;
        {
            std::lock_guard<std::mutex> engineLock(engineMutex);
            if (engineStopping)
            {
                return;
            }
            readResult = SubmitReadLocked(session);
        }
        if (readResult.IsFailed())
        {
            const DWORD errorCode = readResult.nativeErrorCode == 0 ? ERROR_INVALID_FUNCTION : static_cast<DWORD>(readResult.nativeErrorCode);
            EnqueueSessionError(session->path, errorCode);
        }
    }

    bool ParseNativeBuffer(const DirectorySession& session, const DWORD transferredBytes, std::vector<NativeRecord>& records) const
    {
        records.clear();
        if (transferredBytes > session.bufferSizeBytes || transferredBytes < offsetof(FILE_NOTIFY_INFORMATION, FileName))
        {
            return false;
        }
        records.reserve((std::max)(static_cast<DWORD>(1), transferredBytes / 32u));

        const unsigned char* bufferBytes = reinterpret_cast<const unsigned char*>(session.buffer.data());
        size_t offset = 0;
        for (;;)
        {
            const size_t headerSize = offsetof(FILE_NOTIFY_INFORMATION, FileName);
            if (offset > transferredBytes || transferredBytes - offset < headerSize)
            {
                return false;
            }
            const FILE_NOTIFY_INFORMATION* information = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(bufferBytes + offset);
            if (information->FileNameLength == 0 || information->FileNameLength % sizeof(wchar_t) != 0)
            {
                return false;
            }
            const size_t recordBytes = headerSize + static_cast<size_t>(information->FileNameLength);
            if (recordBytes > transferredBytes - offset)
            {
                return false;
            }

            NativeRecord record;
            record.action = information->Action;
            record.relativePath.assign(information->FileName, information->FileNameLength / sizeof(wchar_t));
            record.relativePath = NormalizeRelativeWidePath(std::move(record.relativePath));
            if (record.relativePath.empty() || !IsValidUtf16(record.relativePath))
            {
                return false;
            }
            records.push_back(std::move(record));

            if (information->NextEntryOffset == 0)
            {
                const size_t trailingBytes = transferredBytes - offset - recordBytes;
                return trailingBytes < sizeof(DWORD);
            }
            if (information->NextEntryOffset % sizeof(DWORD) != 0 || information->NextEntryOffset < recordBytes || information->NextEntryOffset > transferredBytes - offset)
            {
                return false;
            }
            offset += information->NextEntryOffset;
        }
    }

    void EnqueueOverflow(const std::wstring& sessionPath, const DWORD errorCode)
    {
        NativePacket packet;
        packet.kind = NativePacketKind::Overflow;
        packet.sessionPath = sessionPath;
        packet.errorCode = errorCode;
        EnqueueNativePacket(std::move(packet));
    }

    void EnqueueSessionError(const std::wstring& sessionPath, const DWORD errorCode)
    {
        NativePacket packet;
        packet.kind = NativePacketKind::SessionError;
        packet.sessionPath = sessionPath;
        packet.errorCode = errorCode;
        EnqueueNativePacket(std::move(packet));
    }

    void EnqueueSyntheticEvent(const GB_SystemFileEvent& event)
    {
        NativePacket packet;
        packet.kind = NativePacketKind::Synthetic;
        packet.syntheticEvents.push_back(event);
        EnqueueNativePacket(std::move(packet));
    }

    void EnqueueSyntheticEvents(std::vector<GB_SystemFileEvent> events)
    {
        if (events.empty())
        {
            return;
        }
        NativePacket packet;
        packet.kind = NativePacketKind::Synthetic;
        packet.syntheticEvents = std::move(events);
        EnqueueNativePacket(std::move(packet));
    }

    void EnqueueCleanupTargetOutputBuffers(const uint64_t targetId)
    {
        NativePacket packet;
        packet.kind = NativePacketKind::CleanupTargets;
        packet.cleanupTargetIds.push_back(targetId);
        EnqueueNativePacket(std::move(packet));
    }

    void EnqueueCleanupAllOutputBuffers()
    {
        NativePacket packet;
        packet.kind = NativePacketKind::CleanupAllTargets;
        EnqueueNativePacket(std::move(packet));
    }

    void EnqueueNativePacket(NativePacket packet)
    {
        {
            std::lock_guard<std::mutex> queueLock(nativeQueueMutex);
            if (processingStopRequested)
            {
                return;
            }
            if (packet.kind == NativePacketKind::Changes)
            {
                const size_t incomingRecordCount = packet.records.size();
                if (incomingRecordCount > options.maxPendingNativeEvents)
                {
                    droppedNativeEventCount.fetch_add(static_cast<uint64_t>(incomingRecordCount), std::memory_order_relaxed);
                    queueOverflowPending = true;
                    nativeQueueCondition.notify_all();
                    return;
                }
                while (pendingNativeRecordCount + incomingRecordCount > options.maxPendingNativeEvents)
                {
                    bool removedPacket = false;
                    for (std::deque<NativePacket>::iterator iter = nativeQueue.begin(); iter != nativeQueue.end(); ++iter)
                    {
                        if (iter->kind == NativePacketKind::Changes)
                        {
                            const size_t removedRecordCount = iter->records.size();
                            pendingNativeRecordCount -= (std::min)(pendingNativeRecordCount, removedRecordCount);
                            droppedNativeEventCount.fetch_add(static_cast<uint64_t>(removedRecordCount), std::memory_order_relaxed);
                            nativeQueue.erase(iter);
                            removedPacket = true;
                            break;
                        }
                    }
                    if (!removedPacket)
                    {
                        droppedNativeEventCount.fetch_add(static_cast<uint64_t>(incomingRecordCount), std::memory_order_relaxed);
                        queueOverflowPending = true;
                        nativeQueueCondition.notify_all();
                        return;
                    }
                    queueOverflowPending = true;
                }
                pendingNativeRecordCount += incomingRecordCount;
            }
            nativeQueue.push_back(std::move(packet));
        }
        nativeQueueCondition.notify_all();
    }

    void ProcessingThreadMain()
    {
        for (;;)
        {
            NativePacket packet;
            bool hasPacket = false;
            bool reportQueueOverflow = false;
            {
                std::unique_lock<std::mutex> queueLock(nativeQueueMutex);
                nativeQueueCondition.wait_for(queueLock, std::chrono::milliseconds(10), [this]()
                    {
                        return processingStopRequested || queueOverflowPending || !nativeQueue.empty();
                    });
                reportQueueOverflow = queueOverflowPending;
                queueOverflowPending = false;
                if (!nativeQueue.empty())
                {
                    packet = std::move(nativeQueue.front());
                    nativeQueue.pop_front();
                    if (packet.kind == NativePacketKind::Changes)
                    {
                        pendingNativeRecordCount -= (std::min)(pendingNativeRecordCount, packet.records.size());
                    }
                    hasPacket = true;
                }
                else if (processingStopRequested)
                {
                    FlushAllPendingModifiedEvents();
                    FlushEventBatch();
                    break;
                }
            }

            if (reportQueueOverflow)
            {
                ProcessGlobalOverflow(GB_SystemResult::Failed(GB_SystemErrorCode::ResourceBusy, "GB_SystemFileWatcher.NativeQueue", u8"文件监听器内部原始事件队列溢出，部分事件已丢失。"));
            }
            if (dispatchOverflowPending.exchange(false, std::memory_order_acq_rel))
            {
                ProcessGlobalOverflow(GB_SystemResult::Failed(GB_SystemErrorCode::ResourceBusy, "GB_SystemFileWatcher.DispatchQueue", u8"文件监听器事件分发队列溢出，部分回调事件已丢失。"));
            }
            if (hasPacket)
            {
                ProcessNativePacket(packet);
            }
            FlushDueModifiedEvents();
            FlushBatchIfDue();
        }
    }

    void ProcessNativePacket(const NativePacket& packet)
    {
        if (packet.kind == NativePacketKind::PauseBarrier)
        {
            FlushAllPendingModifiedEvents();
            FlushEventBatch();
            {
                std::lock_guard<std::mutex> stateLock(stateMutex);
                paused = true;
                state = GB_SystemFileWatcherState::Paused;
                for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::iterator iter = targets.begin(); iter != targets.end(); ++iter)
                {
                    if (iter->second->info.state == GB_SystemFileWatchTargetState::Watching || iter->second->info.state == GB_SystemFileWatchTargetState::Recovering)
                    {
                        iter->second->info.state = GB_SystemFileWatchTargetState::Paused;
                    }
                }
            }
            {
                std::lock_guard<std::mutex> barrierLock(barrierMutex);
                completedBarrierId = (std::max)(completedBarrierId, packet.barrierId);
            }
            barrierCondition.notify_all();
            return;
        }
        if (packet.kind == NativePacketKind::CleanupTargets)
        {
            RemoveBufferedEventsForTargets(packet.cleanupTargetIds);
            return;
        }
        if (packet.kind == NativePacketKind::CleanupAllTargets)
        {
            ClearBufferedEvents();
            return;
        }
        if (packet.kind == NativePacketKind::Synthetic)
        {
            for (size_t index = 0; index < packet.syntheticEvents.size(); index++)
            {
                if (IsPaused() && IsOrdinaryFileChangeEvent(packet.syntheticEvents[index].eventType))
                {
                    const std::shared_ptr<TargetRecord> targetRecord = FindTarget(packet.syntheticEvents[index].targetId);
                    if (targetRecord)
                    {
                        std::lock_guard<std::mutex> stateLock(stateMutex);
                        targetRecord->dirtyDuringRecovery = true;
                    }
                    continue;
                }
                QueueOutputEvent(packet.syntheticEvents[index]);
            }
            return;
        }
        if (IsPaused())
        {
            MarkMatchingTargetsDirty(packet.sessionPath);
            return;
        }
        if (packet.kind == NativePacketKind::Overflow)
        {
            const GB_SystemResult result = GB_SystemResult::FromWin32Error(packet.errorCode, "ReadDirectoryChangesW", u8"底层目录通知缓冲区溢出或通知记录已丢失。");
            ProcessSessionOverflow(packet.sessionPath, result);
            return;
        }
        if (packet.kind == NativePacketKind::SessionError)
        {
            ProcessSessionError(packet.sessionPath, packet.errorCode);
            return;
        }

        size_t recordIndex = 0;
        while (recordIndex < packet.records.size())
        {
            const NativeRecord& record = packet.records[recordIndex];
            if (record.action == FILE_ACTION_RENAMED_OLD_NAME && recordIndex + 1 < packet.records.size() && packet.records[recordIndex + 1].action == FILE_ACTION_RENAMED_NEW_NAME)
            {
                ProcessRawRecord(packet.sessionPath, record);
                ProcessRawRecord(packet.sessionPath, packet.records[recordIndex + 1]);
                ProcessNormalizedRename(packet.sessionPath, record.relativePath, packet.records[recordIndex + 1].relativePath);
                recordIndex += 2;
                continue;
            }

            ProcessRawRecord(packet.sessionPath, record);
            if (record.action == FILE_ACTION_RENAMED_OLD_NAME)
            {
                ProcessNormalizedSingle(packet.sessionPath, FILE_ACTION_REMOVED, record.relativePath);
            }
            else if (record.action == FILE_ACTION_RENAMED_NEW_NAME)
            {
                ProcessNormalizedSingle(packet.sessionPath, FILE_ACTION_ADDED, record.relativePath);
            }
            else
            {
                ProcessNormalizedSingle(packet.sessionPath, record.action, record.relativePath);
            }
            recordIndex++;
        }
    }

    bool IsPaused() const
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        return paused;
    }

    bool IsOrdinaryFileChangeEvent(const GB_SystemFileEventType eventType) const
    {
        return eventType == GB_SystemFileEventType::Added || eventType == GB_SystemFileEventType::Removed || eventType == GB_SystemFileEventType::Modified || eventType == GB_SystemFileEventType::RenameOldName || eventType == GB_SystemFileEventType::RenameNewName || eventType == GB_SystemFileEventType::Renamed;
    }

    void MarkMatchingTargetsDirty(const std::wstring& sessionPath)
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::iterator iter = targets.begin(); iter != targets.end(); ++iter)
        {
            if (EqualWidePath(iter->second->normalizedPath, sessionPath, false) || EqualWidePath(iter->second->parentPath, sessionPath, false))
            {
                iter->second->dirtyDuringRecovery = true;
            }
        }
    }

    void ProcessGlobalOverflow(const GB_SystemResult& result)
    {
        overflowCount.fetch_add(1, std::memory_order_relaxed);
        const std::vector<TargetView> targetViews = GetTargetViews();
        for (size_t index = 0; index < targetViews.size(); index++)
        {
            ProcessOverflowForTarget(targetViews[index], result);
        }
    }

    void ProcessSessionOverflow(const std::wstring& sessionPath, const GB_SystemResult& result)
    {
        overflowCount.fetch_add(1, std::memory_order_relaxed);
        const std::vector<TargetView> targetViews = GetTargetViewsForSession(sessionPath);
        for (size_t index = 0; index < targetViews.size(); index++)
        {
            ProcessOverflowForTarget(targetViews[index], result);
        }
    }

    void ProcessOverflowForTarget(const TargetView& targetView, const GB_SystemResult& result)
    {
        {
            const std::shared_ptr<TargetRecord> targetRecord = FindTarget(targetView.targetId);
            if (targetRecord)
            {
                std::lock_guard<std::mutex> stateLock(stateMutex);
                if (targetRecord->info.state == GB_SystemFileWatchTargetState::Recovering)
                {
                    targetRecord->dirtyDuringRecovery = true;
                }
            }
        }
        GB_SystemFileEvent overflowEvent = MakeTargetEvent(targetView, GB_SystemFileEventType::Overflow, result);
        QueueOutputEvent(overflowEvent);
        if (targetView.options.recoveryMode == GB_SystemFileRecoveryMode::SnapshotDiff)
        {
            ScheduleRecovery(targetView.targetId);
        }
        else
        {
            QueueOutputEvent(MakeTargetEvent(targetView, GB_SystemFileEventType::RescanRequired, result));
        }
    }

    void ProcessSessionError(const std::wstring& sessionPath, const DWORD errorCode)
    {
        const GB_SystemResult result = GB_SystemResult::FromWin32Error(errorCode, "ReadDirectoryChangesW", u8"目录监听 session 发生错误，目标可能暂时不可用。");
        const std::vector<TargetView> targetViews = GetTargetViewsForSession(sessionPath);
        for (size_t index = 0; index < targetViews.size(); index++)
        {
            MarkTargetUnavailable(targetViews[index], result);
        }
    }

    void ProcessRawRecord(const std::wstring& sessionPath, const NativeRecord& record)
    {
        GB_SystemFileEventType eventType = GB_SystemFileEventType::Unknown;
        switch (record.action)
        {
        case FILE_ACTION_ADDED:
            eventType = GB_SystemFileEventType::Added;
            break;
        case FILE_ACTION_REMOVED:
            eventType = GB_SystemFileEventType::Removed;
            break;
        case FILE_ACTION_MODIFIED:
            eventType = GB_SystemFileEventType::Modified;
            break;
        case FILE_ACTION_RENAMED_OLD_NAME:
            eventType = GB_SystemFileEventType::RenameOldName;
            break;
        case FILE_ACTION_RENAMED_NEW_NAME:
            eventType = GB_SystemFileEventType::RenameNewName;
            break;
        default:
            return;
        }
        FanoutSingleEvent(sessionPath, record.relativePath, eventType, record.action, true);
    }

    void ProcessNormalizedSingle(const std::wstring& sessionPath, const DWORD action, const std::wstring& relativePath)
    {
        GB_SystemFileEventType eventType = GB_SystemFileEventType::Unknown;
        if (action == FILE_ACTION_ADDED)
        {
            eventType = GB_SystemFileEventType::Added;
        }
        else if (action == FILE_ACTION_REMOVED)
        {
            eventType = GB_SystemFileEventType::Removed;
        }
        else if (action == FILE_ACTION_MODIFIED)
        {
            eventType = GB_SystemFileEventType::Modified;
        }
        if (eventType != GB_SystemFileEventType::Unknown)
        {
            FanoutSingleEvent(sessionPath, relativePath, eventType, action, false);
        }
    }

    void ProcessNormalizedRename(const std::wstring& sessionPath, const std::wstring& oldRelativePath, const std::wstring& newRelativePath)
    {
        const std::vector<TargetView> targetViews = GetTargetViewsForSession(sessionPath);
        for (size_t index = 0; index < targetViews.size(); index++)
        {
            std::wstring oldTargetRelativePath;
            std::wstring newTargetRelativePath;
            bool oldGuard = false;
            bool newGuard = false;
            const bool oldMatchesScope = MatchTargetScope(targetViews[index], sessionPath, oldRelativePath, oldTargetRelativePath, oldGuard);
            const bool newMatchesScope = MatchTargetScope(targetViews[index], sessionPath, newRelativePath, newTargetRelativePath, newGuard);
            if (oldGuard)
            {
                MarkTargetUnavailable(targetViews[index], GB_SystemResult::Succeeded("ReadDirectoryChangesW", u8"监听目录已被重命名，固定路径目标进入不可用状态。"));
                continue;
            }
            if (newGuard && EqualWidePath(newRelativePath, targetViews[index].fileName, targetViews[index].options.caseSensitive))
            {
                MarkTargetRecovered(targetViews[index]);
                continue;
            }
            if (!WantsNormalizedEvents(targetViews[index].options.deliveryMode))
            {
                UpdateSnapshotForRename(targetViews[index], oldTargetRelativePath, newTargetRelativePath, oldMatchesScope, newMatchesScope);
                continue;
            }

            const GB_SystemFileObjectType oldObjectType = oldMatchesScope ? GetSnapshotObjectType(targetViews[index], oldTargetRelativePath) : GB_SystemFileObjectType::Unknown;
            const bool oldPasses = oldMatchesScope && MatchesFilters(targetViews[index], oldTargetRelativePath, oldObjectType);
            const GB_SystemFileObjectType newObjectType = newMatchesScope ? QueryObjectType(GetTargetAbsolutePath(targetViews[index], newTargetRelativePath), targetViews[index].options.targetType) : GB_SystemFileObjectType::Unknown;
            const bool newPasses = newMatchesScope && MatchesFilters(targetViews[index], newTargetRelativePath, newObjectType);
            UpdateSnapshotForRename(targetViews[index], oldTargetRelativePath, newTargetRelativePath, oldMatchesScope, newMatchesScope);

            if (oldPasses && newPasses)
            {
                GB_SystemFileEvent event = MakePathEvent(targetViews[index], GB_SystemFileEventType::Renamed, newTargetRelativePath, newObjectType, FILE_ACTION_RENAMED_NEW_NAME, false);
                event.oldRelativePath = WidePathToUtf8ForwardSlash(oldTargetRelativePath);
                event.oldAbsolutePath = WidePathToUtf8ForwardSlash(GetTargetAbsolutePath(targetViews[index], oldTargetRelativePath));
                event.rawEventCount = 2;
                QueueOutputEvent(event);
            }
            else if (oldPasses)
            {
                QueueOutputEvent(MakePathEvent(targetViews[index], GB_SystemFileEventType::Removed, oldTargetRelativePath, oldObjectType, FILE_ACTION_RENAMED_OLD_NAME, false));
            }
            else if (newPasses)
            {
                QueueOutputEvent(MakePathEvent(targetViews[index], GB_SystemFileEventType::Added, newTargetRelativePath, newObjectType, FILE_ACTION_RENAMED_NEW_NAME, false));
            }
        }
    }

    void FanoutSingleEvent(const std::wstring& sessionPath, const std::wstring& sessionRelativePath, const GB_SystemFileEventType eventType, const DWORD nativeAction, const bool raw)
    {
        const std::vector<TargetView> targetViews = GetTargetViewsForSession(sessionPath);
        for (size_t index = 0; index < targetViews.size(); index++)
        {
            std::wstring targetRelativePath;
            bool isGuard = false;
            if (!MatchTargetScope(targetViews[index], sessionPath, sessionRelativePath, targetRelativePath, isGuard))
            {
                continue;
            }
            if (isGuard)
            {
                if (eventType == GB_SystemFileEventType::Removed || eventType == GB_SystemFileEventType::RenameOldName)
                {
                    MarkTargetUnavailable(targetViews[index], GB_SystemResult::Succeeded("ReadDirectoryChangesW", u8"监听目录的固定路径已不可用。"));
                }
                else if (eventType == GB_SystemFileEventType::Added || eventType == GB_SystemFileEventType::RenameNewName)
                {
                    MarkTargetRecovered(targetViews[index]);
                }
                continue;
            }
            if (raw && !WantsRawEvents(targetViews[index].options.deliveryMode))
            {
                continue;
            }
            if (!raw && !WantsNormalizedEvents(targetViews[index].options.deliveryMode))
            {
                UpdateSnapshotForSingle(targetViews[index], eventType, targetRelativePath);
                continue;
            }

            GB_SystemFileObjectType objectType = eventType == GB_SystemFileEventType::Removed || eventType == GB_SystemFileEventType::RenameOldName ? GetSnapshotObjectType(targetViews[index], targetRelativePath) : QueryObjectType(GetTargetAbsolutePath(targetViews[index], targetRelativePath), targetViews[index].options.targetType);
            if (!raw)
            {
                UpdateSnapshotForSingle(targetViews[index], eventType, targetRelativePath);
            }
            if (!MatchesFilters(targetViews[index], targetRelativePath, objectType))
            {
                continue;
            }
            QueueOutputEvent(MakePathEvent(targetViews[index], eventType, targetRelativePath, objectType, nativeAction, raw));
        }
    }

    bool MatchTargetScope(const TargetView& targetView, const std::wstring& sessionPath, const std::wstring& sessionRelativePath, std::wstring& targetRelativePath, bool& isGuard) const
    {
        targetRelativePath.clear();
        isGuard = false;
        if (targetView.options.targetType == GB_SystemFileWatchTargetType::File)
        {
            if (!EqualWidePath(targetView.parentPath, sessionPath, false) || !EqualWidePath(targetView.fileName, sessionRelativePath, targetView.options.caseSensitive))
            {
                return false;
            }
            targetRelativePath = targetView.fileName;
            return true;
        }
        if (EqualWidePath(targetView.normalizedPath, sessionPath, false))
        {
            if (!targetView.options.recursive && sessionRelativePath.find(L'\\') != std::wstring::npos)
            {
                return false;
            }
            targetRelativePath = sessionRelativePath;
            return true;
        }
        if (EqualWidePath(targetView.parentPath, sessionPath, false) && EqualWidePath(targetView.fileName, sessionRelativePath, targetView.options.caseSensitive))
        {
            isGuard = true;
            return true;
        }
        return false;
    }

    std::wstring GetTargetAbsolutePath(const TargetView& targetView, const std::wstring& targetRelativePath) const
    {
        return targetView.options.targetType == GB_SystemFileWatchTargetType::File ? targetView.normalizedPath : JoinWidePath(targetView.normalizedPath, targetRelativePath);
    }

    GB_SystemFileObjectType QueryObjectType(const std::wstring& absolutePath, const GB_SystemFileWatchTargetType fallbackTargetType) const
    {
        const DWORD attributes = ::GetFileAttributesW(MakeExtendedPath(absolutePath).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? GB_SystemFileObjectType::Directory : GB_SystemFileObjectType::File;
        }
        return fallbackTargetType == GB_SystemFileWatchTargetType::File ? GB_SystemFileObjectType::File : GB_SystemFileObjectType::Unknown;
    }

    bool MatchesFilters(const TargetView& targetView, const std::wstring& relativePath, const GB_SystemFileObjectType objectType) const
    {
        try
        {
            if (!targetView.options.includeGlobs.empty())
            {
                bool included = false;
                for (size_t index = 0; index < targetView.includeGlobsWide.size(); index++)
                {
                    if (GlobMatchWide(targetView.includeGlobsWide[index], relativePath, targetView.options.caseSensitive))
                    {
                        included = true;
                        break;
                    }
                }
                if (!included)
                {
                    return false;
                }
            }
            for (size_t index = 0; index < targetView.excludeGlobsWide.size(); index++)
            {
                if (GlobMatchWide(targetView.excludeGlobsWide[index], relativePath, targetView.options.caseSensitive))
                {
                    return false;
                }
            }
            if (!targetView.options.extensions.empty())
            {
                if (objectType == GB_SystemFileObjectType::Directory)
                {
                    return false;
                }
                const std::wstring extension = GetExtensionWide(relativePath);
                bool extensionMatched = targetView.options.includeShortcutFileEvents && IsShellShortcutExtension(extension);
                const std::wstring comparableExtension = targetView.options.caseSensitive ? extension : FoldCaseOrdinal(extension);
                for (size_t index = 0; !extensionMatched && index < targetView.extensionsWide.size(); index++)
                {
                    if (comparableExtension == targetView.extensionsWide[index])
                    {
                        extensionMatched = true;
                        break;
                    }
                }
                if (!extensionMatched)
                {
                    return false;
                }
            }
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    GB_SystemFileEvent MakePathEvent(const TargetView& targetView, const GB_SystemFileEventType eventType, const std::wstring& relativePath, const GB_SystemFileObjectType objectType, const DWORD nativeAction, const bool raw) const
    {
        GB_SystemFileEvent event;
        event.eventType = eventType;
        event.targetId = targetView.targetId;
        event.eventName = std::string("SystemFile.") + GB_SystemFileWatcher::GetEventTypeName(eventType);
        event.sourceName = "ReadDirectoryChangesW";
        event.targetPath = targetView.path;
        event.relativePath = WidePathToUtf8ForwardSlash(relativePath);
        event.absolutePath = WidePathToUtf8ForwardSlash(GetTargetAbsolutePath(targetView, relativePath));
        event.objectType = objectType;
        event.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
        event.nativeAction = nativeAction;
        event.isRaw = raw;
        event.isNormalized = !raw;
        event.isRecursiveChild = relativePath.find(L'\\') != std::wstring::npos;
        event.result = GB_SystemResult::Succeeded("ReadDirectoryChangesW");
        return event;
    }

    GB_SystemFileEvent MakeTargetEvent(const TargetView& targetView, const GB_SystemFileEventType eventType, const GB_SystemResult& result) const
    {
        GB_SystemFileEvent event;
        event.eventType = eventType;
        event.targetId = targetView.targetId;
        event.eventName = std::string("SystemFile.") + GB_SystemFileWatcher::GetEventTypeName(eventType);
        event.sourceName = eventType == GB_SystemFileEventType::RecoveryStarted || eventType == GB_SystemFileEventType::RecoveryCompleted ? "SnapshotDiff" : "GB_SystemFileWatcher";
        event.targetPath = targetView.path;
        event.absolutePath = targetView.path;
        event.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
        event.result = result;
        event.isNormalized = true;
        return event;
    }

    GB_SystemFileEvent MakeTargetEvent(const TargetRecord& targetRecord, const GB_SystemFileEventType eventType, const GB_SystemResult& result) const
    {
        TargetView targetView;
        targetView.targetId = targetRecord.info.targetId;
        targetView.path = targetRecord.info.path;
        targetView.options = targetRecord.info.options;
        targetView.normalizedPath = targetRecord.normalizedPath;
        targetView.parentPath = targetRecord.parentPath;
        targetView.fileName = targetRecord.fileName;
        targetView.includeGlobsWide = targetRecord.includeGlobsWide;
        targetView.excludeGlobsWide = targetRecord.excludeGlobsWide;
        targetView.extensionsWide = targetRecord.extensionsWide;
        return MakeTargetEvent(targetView, eventType, result);
    }

    GB_SystemFileEvent MakeWatcherEvent(const GB_SystemFileEventType eventType) const
    {
        GB_SystemFileEvent event;
        event.eventType = eventType;
        event.eventName = std::string("SystemFile.") + GB_SystemFileWatcher::GetEventTypeName(eventType);
        event.sourceName = "GB_SystemFileWatcher";
        event.timestampMilliseconds = GB_EventDispatcher::GetCurrentTimestampMilliseconds();
        event.result = GB_SystemResult::Succeeded(event.eventName);
        return event;
    }

    void MarkTargetUnavailable(const TargetView& targetView, const GB_SystemResult& result)
    {
        bool stateChanged = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            const std::map<uint64_t, std::shared_ptr<TargetRecord>>::iterator iter = targets.find(targetView.targetId.targetId);
            if (iter == targets.end())
            {
                return;
            }
            stateChanged = iter->second->info.state != GB_SystemFileWatchTargetState::Unavailable;
            iter->second->info.state = GB_SystemFileWatchTargetState::Unavailable;
            iter->second->info.lastResult = result;
            iter->second->nextReconnectTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(iter->second->reconnectDelayMilliseconds);
        }
        if (stateChanged)
        {
            QueueOutputEvent(MakeTargetEvent(targetView, GB_SystemFileEventType::TargetUnavailable, result));
        }
        if (targetView.options.recoveryMode == GB_SystemFileRecoveryMode::ReportOnly)
        {
            QueueOutputEvent(MakeTargetEvent(targetView, GB_SystemFileEventType::RescanRequired, result));
        }
    }

    void MarkTargetRecovered(const TargetView& targetView)
    {
        const std::shared_ptr<TargetRecord> targetRecord = FindTarget(targetView.targetId);
        if (!targetRecord || !IsTargetAvailable(*targetRecord, targetRecord->normalizedPath, targetRecord->parentPath))
        {
            return;
        }

        bool stateChanged = false;
        const GB_SystemResult reconnectResult = GB_SystemResult::Succeeded("GB_SystemFileWatcher.Reconnect");
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            const bool currentlyPaused = paused || state == GB_SystemFileWatcherState::Paused;
            const GB_SystemFileWatchTargetState newState = currentlyPaused ? GB_SystemFileWatchTargetState::Paused : GB_SystemFileWatchTargetState::Watching;
            stateChanged = targetRecord->info.state != newState;
            targetRecord->info.state = newState;
            targetRecord->info.lastResult = reconnectResult;
            targetRecord->reconnectDelayMilliseconds = options.reconnectInitialDelayMilliseconds;
        }
        if (!stateChanged)
        {
            return;
        }

        reconnectCount.fetch_add(1, std::memory_order_relaxed);
        QueueOutputEvent(MakeTargetEvent(targetView, GB_SystemFileEventType::TargetRecovered, reconnectResult));
        RequestNativeRebuild();
    }

    void EnqueueTargetFailedEvents(const GB_SystemResult& result)
    {
        const std::vector<TargetView> targetViews = GetTargetViews();
        std::vector<GB_SystemFileEvent> events;
        try
        {
            events.reserve(targetViews.size());
            for (size_t index = 0; index < targetViews.size(); index++)
            {
                events.push_back(MakeTargetEvent(targetViews[index], GB_SystemFileEventType::TargetFailed, result));
            }
        }
        catch (...)
        {
            return;
        }
        EnqueueSyntheticEvents(std::move(events));
    }

    void RequestNativeRebuild()
    {
        {
            std::lock_guard<std::mutex> recoveryLock(recoveryMutex);
            nativeRebuildRequested = true;
        }
        recoveryCondition.notify_all();
    }

    static bool ContainsTargetId(const std::vector<uint64_t>& targetIds, const uint64_t targetId)
    {
        for (size_t index = 0; index < targetIds.size(); index++)
        {
            if (targetIds[index] == targetId)
            {
                return true;
            }
        }
        return false;
    }

    void RemoveBufferedEventsForTargets(const std::vector<uint64_t>& targetIds)
    {
        if (targetIds.empty())
        {
            return;
        }
        for (std::map<std::string, PendingModifiedEvent>::iterator iter = pendingModifiedEvents.begin(); iter != pendingModifiedEvents.end();)
        {
            if (ContainsTargetId(targetIds, iter->second.event.targetId.targetId))
            {
                iter = pendingModifiedEvents.erase(iter);
            }
            else
            {
                iter++;
            }
        }
        pendingBatch.events.erase(std::remove_if(pendingBatch.events.begin(), pendingBatch.events.end(), [&targetIds](const GB_SystemFileEvent& event)
            {
                return ContainsTargetId(targetIds, event.targetId.targetId);
            }), pendingBatch.events.end());
        RecalculatePendingBatchTimestamps();
    }

    void ClearBufferedEvents()
    {
        pendingModifiedEvents.clear();
        pendingBatch.events.erase(std::remove_if(pendingBatch.events.begin(), pendingBatch.events.end(), [](const GB_SystemFileEvent& event)
            {
                return event.targetId.IsValid();
            }), pendingBatch.events.end());
        RecalculatePendingBatchTimestamps();
    }

    void RecalculatePendingBatchTimestamps()
    {
        if (pendingBatch.events.empty())
        {
            pendingBatch.firstTimestampMilliseconds = 0;
            pendingBatch.lastTimestampMilliseconds = 0;
            return;
        }
        pendingBatch.firstTimestampMilliseconds = pendingBatch.events.front().timestampMilliseconds;
        pendingBatch.lastTimestampMilliseconds = pendingBatch.events.front().timestampMilliseconds;
        for (size_t index = 1; index < pendingBatch.events.size(); index++)
        {
            pendingBatch.firstTimestampMilliseconds = (std::min)(pendingBatch.firstTimestampMilliseconds, pendingBatch.events[index].timestampMilliseconds);
            pendingBatch.lastTimestampMilliseconds = (std::max)(pendingBatch.lastTimestampMilliseconds, pendingBatch.events[index].timestampMilliseconds);
        }
    }

    void QueueOutputEvent(GB_SystemFileEvent event)
    {
        if (event.eventType != GB_SystemFileEventType::Modified)
        {
            FlushPendingModifiedEvent(event.targetId, event.relativePath);
            if (!event.oldRelativePath.empty())
            {
                FlushPendingModifiedEvent(event.targetId, event.oldRelativePath);
            }
        }
        if (event.eventType == GB_SystemFileEventType::Modified && event.isNormalized && options.coalesceModifiedEvents && options.modifiedDebounceMilliseconds > 0)
        {
            const std::string key = MakeModifiedEventKey(event.targetId, event.relativePath);
            std::map<std::string, PendingModifiedEvent>::iterator iter = pendingModifiedEvents.find(key);
            if (iter == pendingModifiedEvents.end())
            {
                PendingModifiedEvent pendingEvent;
                pendingEvent.event = std::move(event);
                pendingEvent.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.modifiedDebounceMilliseconds);
                pendingModifiedEvents[key] = std::move(pendingEvent);
            }
            else
            {
                iter->second.event.rawEventCount += event.rawEventCount;
                iter->second.event.timestampMilliseconds = event.timestampMilliseconds;
                iter->second.event.isCoalesced = true;
                iter->second.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.modifiedDebounceMilliseconds);
            }
            return;
        }
        AppendToBatch(std::move(event));
    }

    std::string MakeModifiedEventKey(const GB_SystemFileWatchTargetId& targetId, const std::string& relativePath) const
    {
        return std::to_string(targetId.watcherId) + ":" + std::to_string(targetId.targetId) + "|" + relativePath;
    }

    void FlushPendingModifiedEvent(const GB_SystemFileWatchTargetId& targetId, const std::string& relativePath)
    {
        const std::string key = MakeModifiedEventKey(targetId, relativePath);
        const std::map<std::string, PendingModifiedEvent>::iterator iter = pendingModifiedEvents.find(key);
        if (iter == pendingModifiedEvents.end())
        {
            return;
        }
        AppendToBatch(std::move(iter->second.event));
        pendingModifiedEvents.erase(iter);
    }

    void FlushDueModifiedEvents()
    {
        const std::chrono::steady_clock::time_point nowTime = std::chrono::steady_clock::now();
        for (std::map<std::string, PendingModifiedEvent>::iterator iter = pendingModifiedEvents.begin(); iter != pendingModifiedEvents.end();)
        {
            if (iter->second.deadline <= nowTime)
            {
                AppendToBatch(std::move(iter->second.event));
                iter = pendingModifiedEvents.erase(iter);
            }
            else
            {
                iter++;
            }
        }
    }

    void FlushAllPendingModifiedEvents()
    {
        for (std::map<std::string, PendingModifiedEvent>::iterator iter = pendingModifiedEvents.begin(); iter != pendingModifiedEvents.end(); ++iter)
        {
            AppendToBatch(std::move(iter->second.event));
        }
        pendingModifiedEvents.clear();
    }

    void AppendToBatch(GB_SystemFileEvent event)
    {
        if (pendingBatch.events.empty())
        {
            pendingBatch.firstTimestampMilliseconds = event.timestampMilliseconds;
            batchDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.batchWindowMilliseconds);
        }
        pendingBatch.lastTimestampMilliseconds = event.timestampMilliseconds;
        pendingBatch.events.push_back(std::move(event));
        if (pendingBatch.events.size() >= options.maxBatchSize || options.batchWindowMilliseconds == 0)
        {
            FlushEventBatch();
        }
    }

    void FlushBatchIfDue()
    {
        if (!pendingBatch.events.empty() && std::chrono::steady_clock::now() >= batchDeadline)
        {
            FlushEventBatch();
        }
    }

    void FlushEventBatch()
    {
        if (pendingBatch.events.empty())
        {
            return;
        }
        GB_SystemFileEventBatch batch;
        batch.events.swap(pendingBatch.events);
        batch.firstTimestampMilliseconds = pendingBatch.firstTimestampMilliseconds;
        batch.lastTimestampMilliseconds = pendingBatch.lastTimestampMilliseconds;
        pendingBatch.firstTimestampMilliseconds = 0;
        pendingBatch.lastTimestampMilliseconds = 0;

        const uint64_t droppedDispatchCountBefore = typedDispatcher.GetDroppedEventCount() + publicDispatcher.GetDroppedEventCount();
        GB_Event typedEvent("SystemFile.Batch", GB_Variant(batch), "GB_SystemFileWatcher");
        const GB_SystemResult typedResult = typedDispatcher.Post(typedEvent);
        if (typedResult.IsSucceeded())
        {
            deliveredBatchCount.fetch_add(1, std::memory_order_relaxed);
            deliveredEventCount.fetch_add(static_cast<uint64_t>(batch.events.size()), std::memory_order_relaxed);
        }
        for (size_t index = 0; index < batch.events.size(); index++)
        {
            GB_Event publicEvent(batch.events[index].eventName, GB_Variant(batch.events[index]), batch.events[index].sourceName);
            publicEvent.SetAttribute("targetId", GB_Variant(static_cast<unsigned long long>(batch.events[index].targetId.targetId)));
            publicEvent.SetAttribute("eventType", GB_Variant(static_cast<unsigned int>(batch.events[index].eventType)));
            publicEvent.SetAttribute("eventTypeName", GB_Variant(GB_SystemFileWatcher::GetEventTypeName(batch.events[index].eventType)));
            publicEvent.SetAttribute("relativePath", GB_Variant(batch.events[index].relativePath));
            publicEvent.SetAttribute("isRaw", GB_Variant(static_cast<bool>(batch.events[index].isRaw)));
            (void)publicDispatcher.Post(publicEvent);
        }
        const uint64_t droppedDispatchCountAfter = typedDispatcher.GetDroppedEventCount() + publicDispatcher.GetDroppedEventCount();
        if (droppedDispatchCountAfter > droppedDispatchCountBefore)
        {
            dispatchOverflowPending.store(true, std::memory_order_release);
            nativeQueueCondition.notify_all();
        }
    }

    void DispatchTypedCallbacks(const GB_Event& event)
    {
        const GB_SystemFileEventBatch* batch = event.payload.AnyCast<GB_SystemFileEventBatch>();
        if (batch == nullptr)
        {
            return;
        }
        GB_SystemFileWatcher::EventCallback localEventCallback;
        GB_SystemFileWatcher::BatchEventCallback localBatchCallback;
        {
            std::lock_guard<std::mutex> callbackLock(callbackMutex);
            localEventCallback = eventCallback;
            localBatchCallback = batchEventCallback;
        }
        if (localEventCallback)
        {
            for (size_t index = 0; index < batch->events.size(); index++)
            {
                try
                {
                    localEventCallback(batch->events[index]);
                }
                catch (...)
                {
                    callbackExceptionCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (localBatchCallback)
        {
            try
            {
                localBatchCallback(*batch);
            }
            catch (...)
            {
                callbackExceptionCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void StopWorkerThreads(const bool drainProcessingQueue)
    {
        {
            std::lock_guard<std::mutex> recoveryLock(recoveryMutex);
            recoveryStopRequested = true;
            recoveryJobs.clear();
            queuedRecoveryIds.clear();
        }
        recoveryCondition.notify_all();
        if (recoveryThread.joinable())
        {
            recoveryThread.join();
        }
        {
            std::lock_guard<std::mutex> queueLock(nativeQueueMutex);
            if (!drainProcessingQueue)
            {
                nativeQueue.clear();
                pendingNativeRecordCount = 0;
                queueOverflowPending = false;
            }
            processingStopRequested = true;
        }
        nativeQueueCondition.notify_all();
        if (processingThread.joinable())
        {
            processingThread.join();
        }
        barrierCondition.notify_all();
    }

    void ScheduleRecovery(const GB_SystemFileWatchTargetId& targetId)
    {
        if (!OwnsTargetId(targetId))
        {
            return;
        }
        {
            std::lock_guard<std::mutex> recoveryLock(recoveryMutex);
            if (recoveryStopRequested || queuedRecoveryIds.find(targetId.targetId) != queuedRecoveryIds.end())
            {
                return;
            }
            recoveryJobs.push_back(targetId);
            queuedRecoveryIds.insert(targetId.targetId);
        }
        recoveryCondition.notify_all();
    }

    void ScheduleAllSnapshotRecoveries()
    {
        const std::vector<GB_SystemFileWatchTargetId> targetIds = GetSnapshotTargetIds();
        for (size_t index = 0; index < targetIds.size(); index++)
        {
            ScheduleRecovery(targetIds[index]);
        }
    }

    void RemoveRecoveryJob(const uint64_t targetId)
    {
        std::lock_guard<std::mutex> recoveryLock(recoveryMutex);
        queuedRecoveryIds.erase(targetId);
        for (std::deque<GB_SystemFileWatchTargetId>::iterator iter = recoveryJobs.begin(); iter != recoveryJobs.end();)
        {
            if (iter->targetId == targetId)
            {
                iter = recoveryJobs.erase(iter);
            }
            else
            {
                iter++;
            }
        }
    }

    void RecoveryThreadMain()
    {
        for (;;)
        {
            GB_SystemFileWatchTargetId targetId;
            bool hasJob = false;
            bool rebuildRequested = false;
            {
                std::unique_lock<std::mutex> recoveryLock(recoveryMutex);
                recoveryCondition.wait_for(recoveryLock, std::chrono::milliseconds(100), [this]()
                    {
                        return recoveryStopRequested || nativeRebuildRequested || !recoveryJobs.empty();
                    });
                if (recoveryStopRequested)
                {
                    break;
                }
                rebuildRequested = nativeRebuildRequested;
                nativeRebuildRequested = false;
                if (!recoveryJobs.empty())
                {
                    targetId = recoveryJobs.front();
                    recoveryJobs.pop_front();
                    queuedRecoveryIds.erase(targetId.targetId);
                    hasJob = true;
                }
            }
            if (rebuildRequested)
            {
                const GB_SystemResult rebuildResult = RebuildNativeEngine();
                if (rebuildResult.IsSucceeded())
                {
                    ScheduleAllSnapshotRecoveries();
                }
                else
                {
                    SetWatcherFailed(rebuildResult);
                    EnqueueTargetFailedEvents(rebuildResult);
                }
            }
            CheckReconnectTargets();
            if (hasJob)
            {
                RecoverTarget(targetId);
            }
        }
    }

    void CheckReconnectTargets()
    {
        const std::chrono::steady_clock::time_point nowTime = std::chrono::steady_clock::now();
        std::vector<TargetView> dueTargets;
        std::vector<TargetView> recoveredTargets;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::iterator iter = targets.begin(); iter != targets.end(); ++iter)
            {
                TargetRecord& targetRecord = *iter->second;
                if (!targetRecord.info.options.autoReconnect || targetRecord.info.state != GB_SystemFileWatchTargetState::Unavailable || targetRecord.nextReconnectTime > nowTime)
                {
                    continue;
                }
                TargetView targetView;
                targetView.targetId = targetRecord.info.targetId;
                targetView.path = targetRecord.info.path;
                targetView.options = targetRecord.info.options;
                targetView.normalizedPath = targetRecord.normalizedPath;
                targetView.parentPath = targetRecord.parentPath;
                targetView.fileName = targetRecord.fileName;
                targetView.includeGlobsWide = targetRecord.includeGlobsWide;
                targetView.excludeGlobsWide = targetRecord.excludeGlobsWide;
                targetView.extensionsWide = targetRecord.extensionsWide;
                dueTargets.push_back(std::move(targetView));
            }
        }
        for (size_t index = 0; index < dueTargets.size(); index++)
        {
            const bool available = dueTargets[index].options.targetType == GB_SystemFileWatchTargetType::Directory ? IsDirectoryPath(dueTargets[index].normalizedPath) : IsDirectoryPath(dueTargets[index].parentPath);
            std::lock_guard<std::mutex> stateLock(stateMutex);
            const std::map<uint64_t, std::shared_ptr<TargetRecord>>::iterator iter = targets.find(dueTargets[index].targetId.targetId);
            if (iter == targets.end() || iter->second->info.state != GB_SystemFileWatchTargetState::Unavailable)
            {
                continue;
            }
            TargetRecord& targetRecord = *iter->second;
            if (available)
            {
                const bool currentlyPaused = paused || state == GB_SystemFileWatcherState::Paused;
                targetRecord.info.state = currentlyPaused ? GB_SystemFileWatchTargetState::Paused : GB_SystemFileWatchTargetState::Watching;
                targetRecord.info.lastResult = GB_SystemResult::Succeeded("GB_SystemFileWatcher.Reconnect");
                targetRecord.reconnectDelayMilliseconds = options.reconnectInitialDelayMilliseconds;
                recoveredTargets.push_back(dueTargets[index]);
            }
            else
            {
                targetRecord.reconnectDelayMilliseconds = (std::min)(options.reconnectMaximumDelayMilliseconds, targetRecord.reconnectDelayMilliseconds > options.reconnectMaximumDelayMilliseconds / 2 ? options.reconnectMaximumDelayMilliseconds : targetRecord.reconnectDelayMilliseconds * 2);
                targetRecord.nextReconnectTime = nowTime + std::chrono::milliseconds(targetRecord.reconnectDelayMilliseconds);
            }
        }
        if (!recoveredTargets.empty())
        {
            const GB_SystemResult rebuildResult = RebuildNativeEngine();
            if (rebuildResult.IsFailed())
            {
                SetWatcherFailed(rebuildResult);
                EnqueueTargetFailedEvents(rebuildResult);
                return;
            }
            for (size_t index = 0; index < recoveredTargets.size(); index++)
            {
                reconnectCount.fetch_add(1, std::memory_order_relaxed);
                EnqueueSyntheticEvent(MakeTargetEvent(recoveredTargets[index], GB_SystemFileEventType::TargetRecovered, rebuildResult));
            }
            ScheduleAllSnapshotRecoveries();
        }
    }

    void RecoverTarget(const GB_SystemFileWatchTargetId& targetId)
    {
        const std::shared_ptr<TargetRecord> targetRecord = FindTarget(targetId);
        if (!targetRecord || targetRecord->info.options.recoveryMode != GB_SystemFileRecoveryMode::SnapshotDiff)
        {
            FinishRecoveryState();
            return;
        }

        TargetView targetView;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            targetView.targetId = targetRecord->info.targetId;
            targetView.path = targetRecord->info.path;
            targetView.options = targetRecord->info.options;
            targetView.normalizedPath = targetRecord->normalizedPath;
            targetView.parentPath = targetRecord->parentPath;
            targetView.fileName = targetRecord->fileName;
            targetView.includeGlobsWide = targetRecord->includeGlobsWide;
            targetView.excludeGlobsWide = targetRecord->excludeGlobsWide;
            targetView.extensionsWide = targetRecord->extensionsWide;
            targetRecord->info.state = GB_SystemFileWatchTargetState::Recovering;
            targetRecord->dirtyDuringRecovery = false;
            if (state == GB_SystemFileWatcherState::Watching)
            {
                state = GB_SystemFileWatcherState::Recovering;
            }
        }
        EnqueueSyntheticEvent(MakeTargetEvent(targetView, GB_SystemFileEventType::RecoveryStarted, GB_SystemResult::Succeeded("GB_SystemFileWatcher.Recovery")));

        std::map<std::wstring, SnapshotEntry> oldSnapshot;
        bool snapshotInitialized = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            oldSnapshot = targetRecord->snapshot;
            snapshotInitialized = targetRecord->snapshotInitialized;
        }

        std::map<std::wstring, SnapshotEntry> newSnapshot;
        GB_SystemResult scanResult;
        bool stableScan = false;
        for (size_t passIndex = 0; passIndex < options.maxRecoveryScanPasses; passIndex++)
        {
            {
                std::lock_guard<std::mutex> stateLock(stateMutex);
                targetRecord->dirtyDuringRecovery = false;
            }
            newSnapshot.clear();
            try
            {
                scanResult = BuildSnapshot(targetView, newSnapshot);
            }
            catch (const std::bad_alloc&)
            {
                scanResult = MakeAllocationFailedResult("GB_SystemFileWatcher.BuildSnapshot");
            }
            catch (...)
            {
                scanResult = GB_SystemResult::Failed(GB_SystemErrorCode::EncodingConversionFailed, "GB_SystemFileWatcher.BuildSnapshot", u8"恢复扫描遇到无法转换为 UTF-8 的文件名。");
            }
            if (scanResult.IsFailed())
            {
                break;
            }
            {
                std::lock_guard<std::mutex> stateLock(stateMutex);
                stableScan = !targetRecord->dirtyDuringRecovery;
            }
            if (stableScan)
            {
                break;
            }
        }

        if (scanResult.IsFailed())
        {
            SetTargetState(targetId, GB_SystemFileWatchTargetState::Unavailable, scanResult);
            EnqueueSyntheticEvent(MakeTargetEvent(targetView, GB_SystemFileEventType::TargetFailed, scanResult));
            if (targetView.options.autoReconnect)
            {
                std::lock_guard<std::mutex> stateLock(stateMutex);
                targetRecord->nextReconnectTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(targetRecord->reconnectDelayMilliseconds);
            }
            FinishRecoveryState();
            return;
        }

        std::vector<GB_SystemFileEvent> recoveryEvents;
        if (snapshotInitialized)
        {
            if (!BuildSnapshotDiffEvents(targetView, oldSnapshot, newSnapshot, recoveryEvents))
            {
                recoveryEvents.clear();
                recoveryEvents.push_back(MakeTargetEvent(targetView, GB_SystemFileEventType::Overflow, GB_SystemResult::Failed(GB_SystemErrorCode::ResourceBusy, "GB_SystemFileWatcher.Recovery", u8"快照差异事件数量超过内部队列容量，已停止生成补偿事件。")));
                recoveryEvents.push_back(MakeTargetEvent(targetView, GB_SystemFileEventType::RescanRequired, GB_SystemResult::Failed(GB_SystemErrorCode::ResourceBusy, "GB_SystemFileWatcher.Recovery", u8"快照差异过大，调用方需要重新枚举目标。")));
            }
        }
        if (!stableScan)
        {
            recoveryEvents.push_back(MakeTargetEvent(targetView, GB_SystemFileEventType::RescanRequired, GB_SystemResult::Failed(GB_SystemErrorCode::ResourceBusy, "GB_SystemFileWatcher.Recovery", u8"恢复扫描期间目录持续变化，达到最大重复扫描次数。")));
        }
        bool completedWhilePaused = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            completedWhilePaused = paused;
            if (completedWhilePaused)
            {
                targetRecord->dirtyDuringRecovery = true;
                targetRecord->info.state = GB_SystemFileWatchTargetState::Paused;
            }
            else
            {
                targetRecord->snapshot = std::move(newSnapshot);
                targetRecord->snapshotInitialized = true;
                targetRecord->dirtyDuringRecovery = false;
                targetRecord->info.state = GB_SystemFileWatchTargetState::Watching;
                targetRecord->info.lastResult = GB_SystemResult::Succeeded("GB_SystemFileWatcher.Recovery");
            }
        }
        recoveryCount.fetch_add(1, std::memory_order_relaxed);
        if (completedWhilePaused)
        {
            recoveryEvents.clear();
        }
        recoveryEvents.push_back(MakeTargetEvent(targetView, GB_SystemFileEventType::RecoveryCompleted, GB_SystemResult::Succeeded("GB_SystemFileWatcher.Recovery")));
        EnqueueSyntheticEvents(std::move(recoveryEvents));
        FinishRecoveryState();
    }

    void FinishRecoveryState()
    {
        bool hasRecoveringTarget = false;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            for (std::map<uint64_t, std::shared_ptr<TargetRecord>>::const_iterator iter = targets.begin(); iter != targets.end(); ++iter)
            {
                if (iter->second->info.state == GB_SystemFileWatchTargetState::Recovering)
                {
                    hasRecoveringTarget = true;
                    break;
                }
            }
            if (!hasRecoveringTarget && state == GB_SystemFileWatcherState::Recovering && !paused)
            {
                state = GB_SystemFileWatcherState::Watching;
            }
        }
    }

    GB_SystemResult BuildSnapshot(const TargetView& targetView, std::map<std::wstring, SnapshotEntry>& snapshot) const
    {
        snapshot.clear();
        if (targetView.options.targetType == GB_SystemFileWatchTargetType::File)
        {
            if (!PathExists(targetView.normalizedPath))
            {
                return IsDirectoryPath(targetView.parentPath) ? GB_SystemResult::Succeeded("GB_SystemFileWatcher.BuildSnapshot") : GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, "GB_SystemFileWatcher.BuildSnapshot", u8"文件目标的父目录不可用。");
            }
            SnapshotEntry entry;
            const GB_SystemResult queryResult = QuerySnapshotEntry(targetView.normalizedPath, targetView.fileName, entry);
            if (queryResult.IsFailed())
            {
                return queryResult;
            }
            snapshot[MakeSnapshotKey(targetView, targetView.fileName)] = entry;
            return GB_SystemResult::Succeeded("GB_SystemFileWatcher.BuildSnapshot");
        }
        if (!IsDirectoryPath(targetView.normalizedPath))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::NotFound, "GB_SystemFileWatcher.BuildSnapshot", u8"目录监听目标当前不可用。");
        }

        std::vector<std::wstring> pendingDirectories;
        pendingDirectories.push_back(std::wstring());
        while (!pendingDirectories.empty())
        {
            if (IsRecoveryStopping())
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, "GB_SystemFileWatcher.BuildSnapshot", u8"恢复扫描已取消。");
            }
            const std::wstring relativeDirectory = pendingDirectories.back();
            pendingDirectories.pop_back();
            const std::wstring absoluteDirectory = relativeDirectory.empty() ? targetView.normalizedPath : JoinWidePath(targetView.normalizedPath, relativeDirectory);
            const std::wstring searchPath = JoinWidePath(MakeExtendedPath(absoluteDirectory), L"*");
            WIN32_FIND_DATAW findData;
            HANDLE findHandle = ::FindFirstFileExW(searchPath.c_str(), FindExInfoBasic, &findData, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (findHandle == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_INVALID_PARAMETER)
            {
                findHandle = ::FindFirstFileExW(searchPath.c_str(), FindExInfoBasic, &findData, FindExSearchNameMatch, nullptr, 0);
            }
            if (findHandle == INVALID_HANDLE_VALUE)
            {
                const DWORD errorCode = ::GetLastError();
                if (errorCode == ERROR_FILE_NOT_FOUND)
                {
                    continue;
                }
                return GB_SystemResult::FromWin32Error(errorCode, "GB_SystemFileWatcher.BuildSnapshot", u8"枚举目录快照失败。");
            }
            GB_WinHandleScope findHandleScope = GB_WinHandleScope::FromFindHandle(findHandle, "GB_SystemFileWatcher.SnapshotFind");
            for (;;)
            {
                const std::wstring name(findData.cFileName);
                if (name != L"." && name != L"..")
                {
                    const std::wstring relativePath = relativeDirectory.empty() ? name : JoinWidePath(relativeDirectory, name);
                    SnapshotEntry entry;
                    entry.relativePath = WidePathToUtf8ForwardSlash(relativePath);
                    entry.objectType = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? GB_SystemFileObjectType::Directory : GB_SystemFileObjectType::File;
                    entry.size = (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) | findData.nFileSizeLow;
                    entry.creationTime = FileTimeToUInt64(findData.ftCreationTime);
                    entry.lastWriteTime = FileTimeToUInt64(findData.ftLastWriteTime);
                    entry.attributes = findData.dwFileAttributes;
                    snapshot[MakeSnapshotKey(targetView, relativePath)] = entry;
                    if (snapshot.size() > targetView.options.maxSnapshotEntryCount)
                    {
                        return GB_SystemResult::Failed(GB_SystemErrorCode::ResourceBusy, "GB_SystemFileWatcher.BuildSnapshot", u8"目录快照条目数超过配置上限。");
                    }
                    if (targetView.options.recursive && entry.objectType == GB_SystemFileObjectType::Directory && (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
                    {
                        pendingDirectories.push_back(relativePath);
                    }
                }
                if (::FindNextFileW(findHandle, &findData) == FALSE)
                {
                    const DWORD errorCode = ::GetLastError();
                    if (errorCode != ERROR_NO_MORE_FILES)
                    {
                        return GB_SystemResult::FromWin32Error(errorCode, "GB_SystemFileWatcher.BuildSnapshot", u8"继续枚举目录快照失败。");
                    }
                    break;
                }
            }
        }
        return GB_SystemResult::Succeeded("GB_SystemFileWatcher.BuildSnapshot");
    }

    bool IsRecoveryStopping() const
    {
        std::lock_guard<std::mutex> recoveryLock(recoveryMutex);
        return recoveryStopRequested;
    }

    GB_SystemResult QuerySnapshotEntry(const std::wstring& absolutePath, const std::wstring& relativePath, SnapshotEntry& entry) const
    {
        WIN32_FILE_ATTRIBUTE_DATA attributeData;
        if (::GetFileAttributesExW(MakeExtendedPath(absolutePath).c_str(), GetFileExInfoStandard, &attributeData) == FALSE)
        {
            return GB_SystemResult::FromLastWin32Error("GB_SystemFileWatcher.QuerySnapshotEntry", u8"读取文件系统对象属性失败。");
        }
        entry.relativePath = WidePathToUtf8ForwardSlash(relativePath);
        entry.objectType = (attributeData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? GB_SystemFileObjectType::Directory : GB_SystemFileObjectType::File;
        entry.size = (static_cast<uint64_t>(attributeData.nFileSizeHigh) << 32) | attributeData.nFileSizeLow;
        entry.creationTime = FileTimeToUInt64(attributeData.ftCreationTime);
        entry.lastWriteTime = FileTimeToUInt64(attributeData.ftLastWriteTime);
        entry.attributes = attributeData.dwFileAttributes;
        return GB_SystemResult::Succeeded("GB_SystemFileWatcher.QuerySnapshotEntry");
    }

    std::wstring MakeSnapshotKey(const TargetView& targetView, const std::wstring& relativePath) const
    {
        return targetView.options.caseSensitive ? relativePath : FoldCaseOrdinal(relativePath);
    }

    bool BuildSnapshotDiffEvents(const TargetView& targetView, const std::map<std::wstring, SnapshotEntry>& oldSnapshot, const std::map<std::wstring, SnapshotEntry>& newSnapshot, std::vector<GB_SystemFileEvent>& events) const
    {
        for (std::map<std::wstring, SnapshotEntry>::const_iterator iter = oldSnapshot.begin(); iter != oldSnapshot.end(); ++iter)
        {
            const std::map<std::wstring, SnapshotEntry>::const_iterator newIter = newSnapshot.find(iter->first);
            if (newIter == newSnapshot.end())
            {
                const std::wstring relativePath = GB_Utf8ToWString(iter->second.relativePath);
                if (MatchesFilters(targetView, ReplaceSeparators(relativePath), iter->second.objectType))
                {
                    GB_SystemFileEvent event = MakePathEvent(targetView, GB_SystemFileEventType::Removed, ReplaceSeparators(relativePath), iter->second.objectType, 0, false);
                    event.sourceName = "SnapshotDiff";
                    event.isFromSnapshotRecovery = true;
                    events.push_back(std::move(event));
                    if (events.size() > options.maxPendingNativeEvents)
                    {
                        return false;
                    }
                }
                continue;
            }
            GB_SystemFileDetectedChange detectedChanges = GetDetectedChanges(iter->second, newIter->second);
            if (detectedChanges != GB_SystemFileDetectedChange::None)
            {
                const std::wstring relativePath = ReplaceSeparators(GB_Utf8ToWString(newIter->second.relativePath));
                if (MatchesFilters(targetView, relativePath, newIter->second.objectType))
                {
                    GB_SystemFileEvent event = MakePathEvent(targetView, GB_SystemFileEventType::Modified, relativePath, newIter->second.objectType, 0, false);
                    event.sourceName = "SnapshotDiff";
                    event.isFromSnapshotRecovery = true;
                    event.detectedChanges = detectedChanges;
                    events.push_back(std::move(event));
                    if (events.size() > options.maxPendingNativeEvents)
                    {
                        return false;
                    }
                }
            }
        }
        for (std::map<std::wstring, SnapshotEntry>::const_iterator iter = newSnapshot.begin(); iter != newSnapshot.end(); ++iter)
        {
            if (oldSnapshot.find(iter->first) != oldSnapshot.end())
            {
                continue;
            }
            const std::wstring relativePath = ReplaceSeparators(GB_Utf8ToWString(iter->second.relativePath));
            if (MatchesFilters(targetView, relativePath, iter->second.objectType))
            {
                GB_SystemFileEvent event = MakePathEvent(targetView, GB_SystemFileEventType::Added, relativePath, iter->second.objectType, 0, false);
                event.sourceName = "SnapshotDiff";
                event.isFromSnapshotRecovery = true;
                events.push_back(std::move(event));
                if (events.size() > options.maxPendingNativeEvents)
                {
                    return false;
                }
            }
        }
        return true;
    }

    GB_SystemFileDetectedChange GetDetectedChanges(const SnapshotEntry& oldEntry, const SnapshotEntry& newEntry) const
    {
        GB_SystemFileDetectedChange changes = GB_SystemFileDetectedChange::None;
        if (oldEntry.objectType != newEntry.objectType)
        {
            changes |= GB_SystemFileDetectedChange::ObjectType;
        }
        if (oldEntry.size != newEntry.size)
        {
            changes |= GB_SystemFileDetectedChange::Size;
        }
        if (oldEntry.attributes != newEntry.attributes)
        {
            changes |= GB_SystemFileDetectedChange::Attributes;
        }
        if (oldEntry.creationTime != newEntry.creationTime)
        {
            changes |= GB_SystemFileDetectedChange::CreationTime;
        }
        if (oldEntry.lastWriteTime != newEntry.lastWriteTime)
        {
            changes |= GB_SystemFileDetectedChange::LastWriteTime;
        }
        return changes;
    }

    GB_SystemFileObjectType GetSnapshotObjectType(const TargetView& targetView, const std::wstring& relativePath) const
    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        const std::map<uint64_t, std::shared_ptr<TargetRecord>>::const_iterator targetIter = targets.find(targetView.targetId.targetId);
        if (targetIter == targets.end())
        {
            return GB_SystemFileObjectType::Unknown;
        }
        const std::map<std::wstring, SnapshotEntry>::const_iterator entryIter = targetIter->second->snapshot.find(MakeSnapshotKey(targetView, relativePath));
        if (entryIter != targetIter->second->snapshot.end())
        {
            return entryIter->second.objectType;
        }
        return targetView.options.targetType == GB_SystemFileWatchTargetType::File ? GB_SystemFileObjectType::File : GB_SystemFileObjectType::Unknown;
    }

    void UpdateSnapshotForSingle(const TargetView& targetView, const GB_SystemFileEventType eventType, const std::wstring& relativePath)
    {
        const std::shared_ptr<TargetRecord> targetRecord = FindTarget(targetView.targetId);
        if (!targetRecord || targetView.options.recoveryMode != GB_SystemFileRecoveryMode::SnapshotDiff)
        {
            return;
        }
        const std::wstring snapshotKey = MakeSnapshotKey(targetView, relativePath);
        if (eventType == GB_SystemFileEventType::Removed)
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (targetRecord->info.state == GB_SystemFileWatchTargetState::Recovering)
            {
                targetRecord->dirtyDuringRecovery = true;
            }
            else
            {
                const std::wstring subtreePrefix = snapshotKey + L"\\";
                for (std::map<std::wstring, SnapshotEntry>::iterator iter = targetRecord->snapshot.begin(); iter != targetRecord->snapshot.end();)
                {
                    if (iter->first == snapshotKey || iter->first.compare(0, subtreePrefix.size(), subtreePrefix) == 0)
                    {
                        iter = targetRecord->snapshot.erase(iter);
                    }
                    else
                    {
                        iter++;
                    }
                }
            }
            return;
        }
        SnapshotEntry entry;
        const GB_SystemResult queryResult = QuerySnapshotEntry(GetTargetAbsolutePath(targetView, relativePath), relativePath, entry);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (targetRecord->info.state == GB_SystemFileWatchTargetState::Recovering)
            {
                targetRecord->dirtyDuringRecovery = true;
            }
            else if (queryResult.IsSucceeded())
            {
                targetRecord->snapshot[snapshotKey] = entry;
            }
        }
        if (eventType == GB_SystemFileEventType::Added && queryResult.IsSucceeded() && entry.objectType == GB_SystemFileObjectType::Directory && targetView.options.recursive)
        {
            ScheduleRecovery(targetView.targetId);
        }
    }

    void UpdateSnapshotForRename(const TargetView& targetView, const std::wstring& oldRelativePath, const std::wstring& newRelativePath, const bool oldMatchesScope, const bool newMatchesScope)
    {
        const std::shared_ptr<TargetRecord> targetRecord = FindTarget(targetView.targetId);
        if (!targetRecord || targetView.options.recoveryMode != GB_SystemFileRecoveryMode::SnapshotDiff)
        {
            return;
        }
        SnapshotEntry newEntry;
        GB_SystemResult queryResult = GB_SystemResult::Failed(GB_SystemErrorCode::NotFound);
        if (newMatchesScope)
        {
            queryResult = QuerySnapshotEntry(GetTargetAbsolutePath(targetView, newRelativePath), newRelativePath, newEntry);
        }
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            if (targetRecord->info.state == GB_SystemFileWatchTargetState::Recovering)
            {
                targetRecord->dirtyDuringRecovery = true;
                return;
            }
            if (oldMatchesScope)
            {
                const std::wstring oldSnapshotKey = MakeSnapshotKey(targetView, oldRelativePath);
                const std::wstring oldSubtreePrefix = oldSnapshotKey + L"\\";
                std::vector<std::pair<std::wstring, SnapshotEntry>> movedEntries;
                for (std::map<std::wstring, SnapshotEntry>::iterator iter = targetRecord->snapshot.begin(); iter != targetRecord->snapshot.end();)
                {
                    if (iter->first == oldSnapshotKey || iter->first.compare(0, oldSubtreePrefix.size(), oldSubtreePrefix) == 0)
                    {
                        if (newMatchesScope)
                        {
                            SnapshotEntry movedEntry = iter->second;
                            const std::wstring oldEntryRelativePath = ReplaceSeparators(GB_Utf8ToWString(movedEntry.relativePath));
                            const std::wstring suffix = oldEntryRelativePath.size() >= oldRelativePath.size() ? oldEntryRelativePath.substr(oldRelativePath.size()) : std::wstring();
                            const std::wstring movedRelativePath = newRelativePath + suffix;
                            movedEntry.relativePath = WidePathToUtf8ForwardSlash(movedRelativePath);
                            movedEntries.push_back(std::make_pair(MakeSnapshotKey(targetView, movedRelativePath), std::move(movedEntry)));
                        }
                        iter = targetRecord->snapshot.erase(iter);
                    }
                    else
                    {
                        iter++;
                    }
                }
                for (size_t index = 0; index < movedEntries.size(); index++)
                {
                    targetRecord->snapshot[movedEntries[index].first] = std::move(movedEntries[index].second);
                }
            }
            if (newMatchesScope && queryResult.IsSucceeded())
            {
                targetRecord->snapshot[MakeSnapshotKey(targetView, newRelativePath)] = newEntry;
            }
        }
        if (!oldMatchesScope && newMatchesScope && queryResult.IsSucceeded() && newEntry.objectType == GB_SystemFileObjectType::Directory && targetView.options.recursive)
        {
            ScheduleRecovery(targetView.targetId);
        }
    }

private:
    const uint64_t watcherId;
    GB_SystemFileWatcherOptions options;

    mutable std::mutex operationMutex;
    mutable std::mutex stateMutex;
    std::map<uint64_t, std::shared_ptr<TargetRecord>> targets;
    uint64_t nextTargetId = 1;
    GB_SystemFileWatcherState state = GB_SystemFileWatcherState::Stopped;
    bool coreStopping = false;
    bool paused = false;

    GB_EventDispatcher typedDispatcher;
    GB_EventDispatcher publicDispatcher;
    GB_EventSubscriptionToken typedSubscriptionToken;
    GB_SystemResult callbackSetupResult;
    mutable std::mutex callbackMutex;
    GB_SystemFileWatcher::EventCallback eventCallback;
    GB_SystemFileWatcher::BatchEventCallback batchEventCallback;

    mutable std::mutex engineMutex;
    std::mutex nativeRebuildMutex;
    std::condition_variable engineCondition;
    GB_WinHandleScope iocpHandle;
    std::vector<std::shared_ptr<DirectorySession>> sessions;
    std::thread iocpThread;
    size_t pendingReadCount = 0;
    bool engineStopping = false;

    mutable std::mutex nativeQueueMutex;
    std::condition_variable nativeQueueCondition;
    std::deque<NativePacket> nativeQueue;
    std::thread processingThread;
    bool processingStopRequested = false;
    bool queueOverflowPending = false;
    size_t pendingNativeRecordCount = 0;

    mutable std::mutex recoveryMutex;
    std::condition_variable recoveryCondition;
    std::deque<GB_SystemFileWatchTargetId> recoveryJobs;
    std::set<uint64_t> queuedRecoveryIds;
    std::thread recoveryThread;
    bool recoveryStopRequested = false;
    bool nativeRebuildRequested = false;

    std::map<std::string, PendingModifiedEvent> pendingModifiedEvents;
    GB_SystemFileEventBatch pendingBatch;
    std::chrono::steady_clock::time_point batchDeadline;

    std::mutex barrierMutex;
    std::condition_variable barrierCondition;
    uint64_t nextBarrierId = 1;
    uint64_t completedBarrierId = 0;

    std::atomic<uint64_t> receivedNativeEventCount{ 0 };
    std::atomic<uint64_t> parsedNativeEventCount{ 0 };
    std::atomic<uint64_t> deliveredEventCount{ 0 };
    std::atomic<uint64_t> deliveredBatchCount{ 0 };
    std::atomic<uint64_t> droppedNativeEventCount{ 0 };
    std::atomic<uint64_t> overflowCount{ 0 };
    std::atomic<uint64_t> recoveryCount{ 0 };
    std::atomic<uint64_t> reconnectCount{ 0 };
    std::atomic<uint64_t> callbackExceptionCount{ 0 };
    std::atomic<bool> dispatchOverflowPending{ false };
};
#else
class GB_SystemFileWatcher::Impl final
{
public:
    explicit Impl(const GB_SystemFileWatcherOptions& inputOptions)
        : options(inputOptions), eventDispatcher(GB_EventDispatcher::MakeDirectOptions("GB_SystemFileWatcher.Unsupported"))
    {
    }

    GB_SystemResult AddTarget(const std::string&, const GB_SystemFileWatchTargetOptions&, GB_SystemFileWatchTargetId& targetId)
    {
        targetId.Reset();
        return MakeUnsupportedPlatformResult(GB_SystemFileOperationAddTarget);
    }

    GB_SystemResult RemoveTarget(const GB_SystemFileWatchTargetId&)
    {
        return MakeUnsupportedPlatformResult(GB_SystemFileOperationRemoveTarget);
    }

    GB_SystemResult ClearTargets()
    {
        return MakeUnsupportedPlatformResult(GB_SystemFileOperationClearTargets);
    }

    std::vector<GB_SystemFileWatchTargetInfo> GetTargets() const
    {
        return std::vector<GB_SystemFileWatchTargetInfo>();
    }

    GB_SystemResult Start()
    {
        return MakeUnsupportedPlatformResult(GB_SystemFileOperationStart);
    }

    GB_SystemResult Stop()
    {
        return GB_SystemResult::Succeeded(GB_SystemFileOperationStop);
    }

    GB_SystemResult Pause()
    {
        return MakeUnsupportedPlatformResult(GB_SystemFileOperationPause);
    }

    GB_SystemResult Resume()
    {
        return MakeUnsupportedPlatformResult(GB_SystemFileOperationResume);
    }

    GB_SystemFileWatcherState GetState() const
    {
        return GB_SystemFileWatcherState::Stopped;
    }

    bool IsRunning() const
    {
        return false;
    }

    GB_SystemResult RequestRescan(const GB_SystemFileWatchTargetId&)
    {
        return MakeUnsupportedPlatformResult(GB_SystemFileOperationRescan);
    }

    GB_SystemResult RequestRescanAll()
    {
        return MakeUnsupportedPlatformResult(GB_SystemFileOperationRescan);
    }

    void SetEventCallback(const GB_SystemFileWatcher::EventCallback&)
    {
    }

    void SetBatchEventCallback(const GB_SystemFileWatcher::BatchEventCallback&)
    {
    }

    GB_EventDispatcher& GetEventDispatcher()
    {
        return eventDispatcher;
    }

    GB_SystemFileWatcherStatistics GetStatistics() const
    {
        return GB_SystemFileWatcherStatistics();
    }

private:
    GB_SystemFileWatcherOptions options;
    GB_EventDispatcher eventDispatcher;
};
#endif

GB_SystemFileWatcher::GB_SystemFileWatcher()
    : impl(new Impl(GB_SystemFileWatcherOptions()))
{
}

GB_SystemFileWatcher::GB_SystemFileWatcher(const GB_SystemFileWatcherOptions& options)
    : impl(new Impl(options))
{
}

GB_SystemFileWatcher::~GB_SystemFileWatcher() noexcept
{
}

GB_SystemResult GB_SystemFileWatcher::AddTarget(const std::string& path, const GB_SystemFileWatchTargetOptions& options, GB_SystemFileWatchTargetId& targetId)
{
    return impl->AddTarget(path, options, targetId);
}

GB_SystemResult GB_SystemFileWatcher::RemoveTarget(const GB_SystemFileWatchTargetId& targetId)
{
    return impl->RemoveTarget(targetId);
}

GB_SystemResult GB_SystemFileWatcher::ClearTargets()
{
    return impl->ClearTargets();
}

std::vector<GB_SystemFileWatchTargetInfo> GB_SystemFileWatcher::GetTargets() const
{
    return impl->GetTargets();
}

GB_SystemResult GB_SystemFileWatcher::Start()
{
    return impl->Start();
}

GB_SystemResult GB_SystemFileWatcher::Stop()
{
    return impl->Stop();
}

GB_SystemResult GB_SystemFileWatcher::Pause()
{
    return impl->Pause();
}

GB_SystemResult GB_SystemFileWatcher::Resume()
{
    return impl->Resume();
}

GB_SystemFileWatcherState GB_SystemFileWatcher::GetState() const
{
    return impl->GetState();
}

bool GB_SystemFileWatcher::IsRunning() const
{
    return impl->IsRunning();
}

GB_SystemResult GB_SystemFileWatcher::RequestRescan(const GB_SystemFileWatchTargetId& targetId)
{
    return impl->RequestRescan(targetId);
}

GB_SystemResult GB_SystemFileWatcher::RequestRescanAll()
{
    return impl->RequestRescanAll();
}

void GB_SystemFileWatcher::SetEventCallback(const EventCallback& callback)
{
    impl->SetEventCallback(callback);
}

void GB_SystemFileWatcher::SetBatchEventCallback(const BatchEventCallback& callback)
{
    impl->SetBatchEventCallback(callback);
}

GB_EventDispatcher& GB_SystemFileWatcher::GetEventDispatcher()
{
    return impl->GetEventDispatcher();
}

GB_SystemFileWatcherStatistics GB_SystemFileWatcher::GetStatistics() const
{
    return impl->GetStatistics();
}

GB_SystemResult GB_SystemFileWatcher::WaitForFileStable(const std::string& path, const GB_SystemFileStableWaitOptions& options)
{
#if defined(_WIN32)
    if (options.timeoutMilliseconds < -1 || options.pollIntervalMilliseconds == 0)
    {
        return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationWaitStable, u8"超时时间必须大于等于 -1，轮询间隔必须大于 0。");
    }
    std::wstring normalizedPath;
    std::string normalizedPathUtf8;
    GB_SystemResult normalizeResult;
    if (!NormalizeAbsolutePath(path, normalizedPath, normalizedPathUtf8, normalizeResult))
    {
        return normalizeResult.WithOperationName(GB_SystemFileOperationWaitStable);
    }

    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point stableStartTime;
    uint64_t previousSize = 0;
    uint64_t previousLastWriteTime = 0;
    bool hasPreviousState = false;

    for (;;)
    {
        if (options.cancellationFlag != nullptr && options.cancellationFlag->load(std::memory_order_acquire))
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::Cancelled, GB_SystemFileOperationWaitStable, u8"等待文件稳定操作已取消。");
        }

        WIN32_FILE_ATTRIBUTE_DATA attributeData = {};
        ::SetLastError(ERROR_SUCCESS);
        const BOOL attributeResult = ::GetFileAttributesExW(MakeExtendedPath(normalizedPath).c_str(), GetFileExInfoStandard, &attributeData);
        if (attributeResult == FALSE)
        {
            const DWORD errorCode = ::GetLastError();
            if (errorCode != ERROR_FILE_NOT_FOUND && errorCode != ERROR_PATH_NOT_FOUND)
            {
                return GB_SystemResult::FromWin32Error(errorCode, GB_SystemFileOperationWaitStable, u8"读取待稳定文件属性失败。");
            }
            hasPreviousState = false;
        }
        else if ((attributeData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return GB_SystemResult::Failed(GB_SystemErrorCode::InvalidArgument, GB_SystemFileOperationWaitStable, u8"等待稳定的目标必须是文件，不能是目录。");
        }
        else
        {
            const uint64_t currentSize = (static_cast<uint64_t>(attributeData.nFileSizeHigh) << 32) | attributeData.nFileSizeLow;
            const uint64_t currentLastWriteTime = FileTimeToUInt64(attributeData.ftLastWriteTime);
            if (!hasPreviousState || currentSize != previousSize || currentLastWriteTime != previousLastWriteTime)
            {
                previousSize = currentSize;
                previousLastWriteTime = currentLastWriteTime;
                stableStartTime = std::chrono::steady_clock::now();
                hasPreviousState = true;
            }
            const uint64_t stableMilliseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stableStartTime).count());
            if (stableMilliseconds >= options.stableWindowMilliseconds && (!options.requireReadable || IsReadableFile(normalizedPath)))
            {
                return GB_SystemResult::Succeeded(GB_SystemFileOperationWaitStable);
            }
        }

        if (options.timeoutMilliseconds >= 0)
        {
            const int64_t elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
            if (elapsedMilliseconds >= options.timeoutMilliseconds)
            {
                return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, GB_SystemFileOperationWaitStable, u8"等待文件稳定超时。");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.pollIntervalMilliseconds));
    }
#else
    (void)path;
    (void)options;
    return MakeUnsupportedPlatformResult(GB_SystemFileOperationWaitStable);
#endif
}

std::string GB_SystemFileWatcher::GetEventTypeName(const GB_SystemFileEventType eventType)
{
    switch (eventType)
    {
    case GB_SystemFileEventType::Added:
        return "Added";
    case GB_SystemFileEventType::Removed:
        return "Removed";
    case GB_SystemFileEventType::Modified:
        return "Modified";
    case GB_SystemFileEventType::RenameOldName:
        return "RenameOldName";
    case GB_SystemFileEventType::RenameNewName:
        return "RenameNewName";
    case GB_SystemFileEventType::Renamed:
        return "Renamed";
    case GB_SystemFileEventType::Overflow:
        return "Overflow";
    case GB_SystemFileEventType::RescanRequired:
        return "RescanRequired";
    case GB_SystemFileEventType::RecoveryStarted:
        return "RecoveryStarted";
    case GB_SystemFileEventType::RecoveryCompleted:
        return "RecoveryCompleted";
    case GB_SystemFileEventType::TargetUnavailable:
        return "TargetUnavailable";
    case GB_SystemFileEventType::TargetRecovered:
        return "TargetRecovered";
    case GB_SystemFileEventType::TargetFailed:
        return "TargetFailed";
    case GB_SystemFileEventType::WatcherStarted:
        return "WatcherStarted";
    case GB_SystemFileEventType::WatcherStopped:
        return "WatcherStopped";
    case GB_SystemFileEventType::Unknown:
    default:
        return "Unknown";
    }
}

std::string GB_SystemFileWatcher::GetWatcherStateName(const GB_SystemFileWatcherState state)
{
    switch (state)
    {
    case GB_SystemFileWatcherState::Starting:
        return "Starting";
    case GB_SystemFileWatcherState::Watching:
        return "Watching";
    case GB_SystemFileWatcherState::Paused:
        return "Paused";
    case GB_SystemFileWatcherState::Recovering:
        return "Recovering";
    case GB_SystemFileWatcherState::Stopping:
        return "Stopping";
    case GB_SystemFileWatcherState::Failed:
        return "Failed";
    case GB_SystemFileWatcherState::Stopped:
    default:
        return "Stopped";
    }
}

std::string GB_SystemFileWatcher::GetTargetStateName(const GB_SystemFileWatchTargetState state)
{
    switch (state)
    {
    case GB_SystemFileWatchTargetState::Watching:
        return "Watching";
    case GB_SystemFileWatchTargetState::Paused:
        return "Paused";
    case GB_SystemFileWatchTargetState::Recovering:
        return "Recovering";
    case GB_SystemFileWatchTargetState::Unavailable:
        return "Unavailable";
    case GB_SystemFileWatchTargetState::Failed:
        return "Failed";
    case GB_SystemFileWatchTargetState::Stopped:
    default:
        return "Stopped";
    }
}
