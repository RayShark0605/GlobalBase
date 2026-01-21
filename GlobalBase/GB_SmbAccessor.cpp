#include "GB_SmbAccessor.h"
#include "GB_ThreadPool.h"
#ifdef _WIN32
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace
{
    class FindHandleGuard
    {
    public:
        explicit FindHandleGuard(HANDLE handle) : handle_(handle)
        {
        }

        ~FindHandleGuard()
        {
            if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr)
            {
                ::FindClose(handle_);
            }
        }

        HANDLE Get() const
        {
            return handle_;
        }

        FindHandleGuard(const FindHandleGuard&) = delete;
        FindHandleGuard& operator=(const FindHandleGuard&) = delete;

    private:
        HANDLE handle_;
    };

    struct NetApiBufferDeleter
    {
        void operator()(void* ptr) const
        {
            if (ptr != nullptr)
            {
                ::NetApiBufferFree(ptr);
            }
        }
    };

    static bool StartsWith(const std::wstring& text, const std::wstring& prefix)
    {
        if (text.size() < prefix.size())
        {
            return false;
        }
        return std::equal(prefix.begin(), prefix.end(), text.begin());
    }

    static bool IsUncLikePath(const std::wstring& path)
    {
        return StartsWith(path, L"\\\\") || StartsWith(path, L"\\\\?\\UNC\\");
    }

    // 规范化路径中的 "." 与 ".." 段。
    //
    // 设计目标：
    // 1) 递归创建目录时，代码会按 '\\' 分段逐级 CreateDirectoryW。
    //    如果路径里含有 ".."，按段创建会错误地尝试创建名为 ".." 的目录。
    // 2) 对于 "\\?\\" 扩展长度路径，Win32 会关闭大部分自动解析/展开逻辑，
    //    因而 ".." / "." 不会被折叠（这会让递归创建/拷贝等逻辑变脆）。
    //
    // 该函数做“词法级”规范化（lexical normalization）：只基于字符串段进行折叠，
    // 不触碰文件系统，也不解析符号链接/Junction。
    static std::wstring CanonicalizeDotsInPath(const std::wstring& path, bool keepLeadingParentSegmentsForRelative)
    {
        std::wstring normalized = path;
        for (size_t i = 0; i < normalized.size(); i++)
        {
            if (normalized[i] == L'/')
            {
                normalized[i] = L'\\';
            }
        }

        // 提取前缀，并确定“根保护段数”（不允许 .. 弹出到根之前）。
        // UNC: \\server\share\...
        // Ext-UNC: \\?\UNC\server\share\...
        // Drive: C:\...
        // Ext-Drive: \\?\C:\...

        enum class Kind
        {
            Unc,
            ExtUnc,
            Drive,
            ExtDrive,
            Relative,
            Other
        };

        Kind kind = Kind::Other;
        std::wstring prefix;
        size_t scanPos = 0;

        if (StartsWith(normalized, L"\\\\?\\UNC\\"))
        {
            kind = Kind::ExtUnc;
            prefix = L"\\\\?\\UNC\\";
            scanPos = prefix.size();
        }
        else if (StartsWith(normalized, L"\\\\"))
        {
            kind = Kind::Unc;
            prefix = L"\\\\";
            scanPos = prefix.size();
        }
        else if (StartsWith(normalized, L"\\\\?\\"))
        {
            // 只处理最常见的扩展盘符路径：\\?\C:\...
            const size_t prefixLen = 4;
            if (normalized.size() >= prefixLen + 3 && normalized[prefixLen + 1] == L':' && normalized[prefixLen + 2] == L'\\')
            {
                kind = Kind::ExtDrive;
                prefix = normalized.substr(0, prefixLen + 3); // "\\?\C:\\"
                scanPos = prefix.size();
            }
        }
        else if (normalized.size() >= 3 && normalized[1] == L':' && normalized[2] == L'\\')
        {
            kind = Kind::Drive;
            prefix = normalized.substr(0, 3); // "C:\\"
            scanPos = prefix.size();
        }
        else
        {
            // 相对路径 / 其它形式（保持尽量可用）
            kind = Kind::Relative;
            prefix.clear();
            scanPos = 0;
        }

        std::vector<std::wstring> segments;
        segments.reserve(16);

        // UNC 需要把 server/share 作为根保护段。
        size_t protectedCount = 0;
        if (kind == Kind::Unc || kind == Kind::ExtUnc)
        {
            // server
            const size_t serverEnd = normalized.find(L'\\', scanPos);
            if (serverEnd == std::wstring::npos)
            {
                return normalized;
            }
            const std::wstring server = normalized.substr(scanPos, serverEnd - scanPos);
            // share
            scanPos = serverEnd + 1;
            const size_t shareEnd = normalized.find(L'\\', scanPos);
            if (shareEnd == std::wstring::npos)
            {
                // 只有到 share 根
                return prefix + server + L"\\" + normalized.substr(scanPos);
            }
            const std::wstring share = normalized.substr(scanPos, shareEnd - scanPos);

            segments.push_back(server);
            segments.push_back(share);
            protectedCount = segments.size();
            scanPos = shareEnd + 1;
        }

        // 盘符路径：prefix 已包含 "C:\\" 或 "\\?\C:\\"，无需额外根段。

        while (scanPos < normalized.size())
        {
            const size_t nextSep = normalized.find(L'\\', scanPos);
            const std::wstring segment = (nextSep == std::wstring::npos)
                ? normalized.substr(scanPos)
                : normalized.substr(scanPos, nextSep - scanPos);

            if (!segment.empty() && segment != L".")
            {
                if (segment == L"..")
                {
                    if (segments.size() > protectedCount)
                    {
                        segments.pop_back();
                    }
                    else
                    {
                        // 不允许越过根。
                        // 对相对路径是否保留多余的 ".."，由调用者决定。
                        if (kind == Kind::Relative && keepLeadingParentSegmentsForRelative)
                        {
                            segments.push_back(segment);
                        }
                    }
                }
                else
                {
                    segments.push_back(segment);
                }
            }

            if (nextSep == std::wstring::npos)
            {
                break;
            }
            scanPos = nextSep + 1;
        }

        if (kind == Kind::Unc || kind == Kind::ExtUnc)
        {
            if (segments.size() < 2)
            {
                return normalized;
            }

            std::wstring out = prefix + segments[0] + L"\\" + segments[1];
            for (size_t i = 2; i < segments.size(); i++)
            {
                out += L"\\" + segments[i];
            }
            return out;
        }

        if (kind == Kind::Drive || kind == Kind::ExtDrive)
        {
            std::wstring out = prefix;
            for (size_t i = 0; i < segments.size(); i++)
            {
                out += segments[i];
                if (i + 1 < segments.size())
                {
                    out += L"\\";
                }
            }
            return out;
        }

        // Relative / Other
        std::wstring out;
        for (size_t i = 0; i < segments.size(); i++)
        {
            out += segments[i];
            if (i + 1 < segments.size())
            {
                out += L"\\";
            }
        }
        return out.empty() ? normalized : out;
    }

#ifndef FIND_FIRST_EX_LARGE_FETCH
#define FIND_FIRST_EX_LARGE_FETCH 0x00000002
#endif

    static HANDLE FindFirstFileExCompatible(const std::wstring& searchPath, WIN32_FIND_DATAW* findData, bool enableLargeFetch)
    {
        if (findData == nullptr)
        {
            return INVALID_HANDLE_VALUE;
        }

        DWORD flags = enableLargeFetch ? FIND_FIRST_EX_LARGE_FETCH : 0;

        // FindExInfoBasic 在某些旧系统/文件系统上可能不支持；必要时回退到 Standard。
        const FINDEX_SEARCH_OPS searchOp = FindExSearchNameMatch;

        FINDEX_INFO_LEVELS infoLevels[2] = { FindExInfoBasic, FindExInfoStandard };
        DWORD flagsCandidates[2] = { flags, 0 };

        for (int i = 0; i < 2; i++)
        {
            for (int j = 0; j < 2; j++)
            {
                ::ZeroMemory(findData, sizeof(*findData));

                HANDLE handle = ::FindFirstFileExW(
                    searchPath.c_str(),
                    infoLevels[i],
                    findData,
                    searchOp,
                    nullptr,
                    flagsCandidates[j]);

                if (handle != INVALID_HANDLE_VALUE)
                {
                    return handle;
                }

                const DWORD err = ::GetLastError();
                if (err != ERROR_INVALID_PARAMETER)
                {
                    // 非“参数不支持”，不需要继续尝试别的组合
                    return INVALID_HANDLE_VALUE;
                }
            }
        }

        return INVALID_HANDLE_VALUE;
    }

    static bool TryGetFileSizeByPath(const std::wstring& path, uint64_t* fileSize)
    {
        if (fileSize == nullptr)
        {
            return false;
        }

        WIN32_FILE_ATTRIBUTE_DATA data;
        ::ZeroMemory(&data, sizeof(data));

        const BOOL ok = ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data);
        if (!ok)
        {
            return false;
        }

        const uint64_t high = static_cast<uint64_t>(data.nFileSizeHigh);
        const uint64_t low = static_cast<uint64_t>(data.nFileSizeLow);
        *fileSize = (high << 32) | low;
        return true;
    }
#ifndef FIND_FIRST_EX_LARGE_FETCH
#define FIND_FIRST_EX_LARGE_FETCH 0x00000002
#endif
#ifndef COPY_FILE_NO_BUFFERING
#define COPY_FILE_NO_BUFFERING 0x00001000
#endif
#ifndef COPY_FILE_FAIL_IF_EXISTS
#define COPY_FILE_FAIL_IF_EXISTS 0x00000001
#endif
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0602)
#ifndef GB_COPYFILE2_EXTENDED_PARAMETERS_DEFINED
#define GB_COPYFILE2_EXTENDED_PARAMETERS_DEFINED
    typedef struct _COPYFILE2_EXTENDED_PARAMETERS
    {
        DWORD dwSize;
        DWORD dwCopyFlags;
        void* pProgressRoutine;
        void* pvCallbackContext;
        HANDLE hCancel;
    } COPYFILE2_EXTENDED_PARAMETERS;
#endif
#endif

    typedef HRESULT(WINAPI* CopyFile2Func)(PCWSTR existingFileName, PCWSTR newFileName, COPYFILE2_EXTENDED_PARAMETERS* extendedParameters);

    static CopyFile2Func GetCopyFile2Func()
    {
        static std::once_flag onceFlag;
        static CopyFile2Func func = nullptr;

        std::call_once(onceFlag, []()
            {
                HMODULE kernel32 = ::GetModuleHandleW(L"Kernel32.dll");
                if (kernel32 != nullptr)
                {
                    func = reinterpret_cast<CopyFile2Func>(::GetProcAddress(kernel32, "CopyFile2"));
                }
            });

        return func;
    }

    static std::wstring FormatWin32ErrorMessageLocal(uint32_t errorCode)
    {
        wchar_t* buffer = nullptr;

        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD langId = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);

        const DWORD len = ::FormatMessageW(flags,
            nullptr,
            errorCode,
            langId,
            reinterpret_cast<LPWSTR>(&buffer),
            0,
            nullptr);

        std::wstring message;
        if (len != 0 && buffer != nullptr)
        {
            message.assign(buffer, buffer + len);
            ::LocalFree(buffer);
        }
        else
        {
            message = L"Error=" + std::to_wstring(errorCode);
        }
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
        {
            message.pop_back();
        }
        return message;
    }

    static bool CopyFileBestEffort(const std::wstring& sourcePath, const std::wstring& destPath, bool overwrite, bool useNoBuffering, std::wstring* errorMessage, const wchar_t* context)
    {
        if (useNoBuffering)
        {
            CopyFile2Func copyFile2 = GetCopyFile2Func();
            if (copyFile2 != nullptr)
            {
                COPYFILE2_EXTENDED_PARAMETERS params;
                ::ZeroMemory(&params, sizeof(params));
                params.dwSize = sizeof(params);
                params.dwCopyFlags = COPY_FILE_NO_BUFFERING;
                if (!overwrite)
                {
                    params.dwCopyFlags |= COPY_FILE_FAIL_IF_EXISTS;
                }

                const HRESULT hr = copyFile2(sourcePath.c_str(), destPath.c_str(), &params);
                if (SUCCEEDED(hr))
                {
                    return true;
                }

                // 某些环境下 COPY_FILE_NO_BUFFERING 可能不支持（或被文件系统拒绝），这里退回 CopyFileW。
                // 失败原因如果最终仍失败，会在 CopyFileW 中报出 GetLastError。
                (void)hr;
            }
        }

        const BOOL ok = ::CopyFileW(sourcePath.c_str(), destPath.c_str(), overwrite ? FALSE : TRUE);
        if (!ok)
        {
            const DWORD err = ::GetLastError();
            if (errorMessage != nullptr)
            {
                std::wstring msg = context;
                msg += L" failed: ";
                msg += FormatWin32ErrorMessageLocal(err);

                // 若 CopyFile2 存在但失败且后续 CopyFileW 也失败，追加一个提示，方便定位。
                if (useNoBuffering && GetCopyFile2Func() != nullptr)
                {
                    msg += L" (CopyFile2+COPY_FILE_NO_BUFFERING attempt was made)";
                }

                *errorMessage = msg;
            }
            return false;
        }

        return true;
    }

    static const uint64_t kLargeFileNoBufferingThresholdBytes = 1ull << 30; // 1 GiB

    // 对“单文件分段并行拷贝”而言，太小的文件并行开销大于收益；
    // 这里给一个保守阈值，避免把 4KB/1MB 之类的小文件拆成很多线程任务。
    static const uint64_t kParallelFileMinBytes = 64ull << 20; // 64 MiB

    // 分段大小：在 SMB / 本地磁盘上通常 4~16 MiB 都比较常见；取 8 MiB 作为折中。
    static const uint32_t kParallelChunkBytes = 8u << 20; // 8 MiB

    static size_t NormalizeParallelThreadCount(size_t threadCount)
    {
        if (threadCount == 0)
        {
            unsigned int hw = std::thread::hardware_concurrency();
            if (hw == 0)
            {
                hw = 4;
            }
            size_t out = static_cast<size_t>(hw);
            out = std::max<size_t>(2, std::min<size_t>(8, out));
            return out;
        }

        return std::max<size_t>(1, threadCount);
    }

    static size_t NormalizeParallelMaxQueueSize(size_t threadCount)
    {
        // 让枚举线程能够提前生产一点任务，但又避免一次性把“几十万文件”全塞进队列。
        // 经验上 32x 线程数的队列深度对 I/O 工作负载比较稳。
        const size_t maxQueueSize = threadCount * 32;
        return std::max<size_t>(64, std::min<size_t>(4096, maxQueueSize));
    }

    class WinHandleGuard
    {
    public:
        explicit WinHandleGuard(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle)
        {
        }

        ~WinHandleGuard()
        {
            Close();
        }

        WinHandleGuard(const WinHandleGuard&) = delete;
        WinHandleGuard& operator=(const WinHandleGuard&) = delete;

        WinHandleGuard(WinHandleGuard&& other) noexcept : handle_(other.handle_)
        {
            other.handle_ = INVALID_HANDLE_VALUE;
        }

        WinHandleGuard& operator=(WinHandleGuard&& other) noexcept
        {
            if (this == &other)
            {
                return *this;
            }

            Close();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
            return *this;
        }

        HANDLE Get() const
        {
            return handle_;
        }

        bool IsValid() const
        {
            return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
        }

        void Reset(HANDLE handle)
        {
            if (handle_ == handle)
            {
                return;
            }
            Close();
            handle_ = handle;
        }

        void Close()
        {
            if (IsValid())
            {
                ::CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
            }
        }

    private:
        HANDLE handle_;
    };

    struct ParallelFirstError
    {
        std::atomic<bool> hasError{ false };
        std::mutex mutex;
        std::wstring message;

        void SetOnce(const std::wstring& msg)
        {
            bool expected = false;
            if (!hasError.compare_exchange_strong(expected, true))
            {
                return;
            }

            std::lock_guard<std::mutex> lock(mutex);
            message = msg;
        }
    };

    static bool TryCloneFileTimesAndAttributes(const std::wstring& sourcePath, const std::wstring& destPath)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        ::ZeroMemory(&data, sizeof(data));

        if (!::GetFileAttributesExW(sourcePath.c_str(), GetFileExInfoStandard, &data))
        {
            return true;
        }

        // 先设置时间（需要句柄），再设置属性（只读属性如果先设置可能影响写入）。
        {
            const DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
            WinHandleGuard destHandle(::CreateFileW(destPath.c_str(), FILE_WRITE_ATTRIBUTES, shareMode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (destHandle.IsValid())
            {
                (void)::SetFileTime(destHandle.Get(), &data.ftCreationTime, &data.ftLastAccessTime, &data.ftLastWriteTime);
            }
        }

        (void)::SetFileAttributesW(destPath.c_str(), data.dwFileAttributes);
        return true;
    }

    static bool SetFileSizeByHandle(HANDLE handle, uint64_t fileSize, std::wstring* errorMessage, const wchar_t* context)
    {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(fileSize);
        if (!::SetFilePointerEx(handle, li, nullptr, FILE_BEGIN))
        {
            const DWORD err = ::GetLastError();
            if (errorMessage != nullptr)
            {
                *errorMessage = std::wstring(context) + L" SetFilePointerEx failed: " + FormatWin32ErrorMessageLocal(err);
            }
            return false;
        }

        if (!::SetEndOfFile(handle))
        {
            const DWORD err = ::GetLastError();
            if (errorMessage != nullptr)
            {
                *errorMessage = std::wstring(context) + L" SetEndOfFile failed: " + FormatWin32ErrorMessageLocal(err);
            }
            return false;
        }

        return true;
    }

    static bool CopyFileBySegmentsParallel(const std::wstring& sourcePath, const std::wstring& destPath, bool overwrite,
        size_t threadCount, std::wstring* errorMessage, const wchar_t* context)
    {
        if (errorMessage != nullptr)
        {
            errorMessage->clear();
        }

        uint64_t fileSize = 0;
        const bool gotSize = TryGetFileSizeByPath(sourcePath, &fileSize);
        if (!gotSize)
        {
            // 获取大小失败则退回系统拷贝。
            return CopyFileBestEffort(sourcePath, destPath, overwrite, false, errorMessage, context);
        }

        if (threadCount <= 1 || fileSize < kParallelFileMinBytes)
        {
            return CopyFileBestEffort(sourcePath, destPath, overwrite, false, errorMessage, context);
        }

        const DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

        // 先创建/截断目标文件并预设大小（避免 worker 竞争创建）。
        {
            const DWORD disposition = overwrite ? CREATE_ALWAYS : CREATE_NEW;
            WinHandleGuard destHandle(::CreateFileW(destPath.c_str(), GENERIC_WRITE, shareMode, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!destHandle.IsValid())
            {
                const DWORD err = ::GetLastError();
                if (errorMessage != nullptr)
                {
                    *errorMessage = std::wstring(context) + L" CreateFileW(dest) failed: " + FormatWin32ErrorMessageLocal(err);
                }
                return false;
            }

            const bool sized = SetFileSizeByHandle(destHandle.Get(), fileSize, errorMessage, context);
            if (!sized)
            {
                return false;
            }
        }

        ParallelFirstError firstError;
        std::atomic<uint64_t> nextChunkIndex{ 0 };

        const size_t maxQueueSize = NormalizeParallelMaxQueueSize(threadCount);
        GB_ThreadPool pool(threadCount, maxQueueSize);

        for (size_t workerId = 0; workerId < threadCount; workerId++)
        {
            pool.Post([&, workerId]() {
                (void)workerId;
                if (firstError.hasError.load(std::memory_order_acquire))
                {
                    return;
                }
            
                WinHandleGuard sourceHandle(::CreateFileW(sourcePath.c_str(), GENERIC_READ, shareMode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (!sourceHandle.IsValid())
                {
                    const DWORD err = ::GetLastError();
                    firstError.SetOnce(std::wstring(context) + L" CreateFileW(source) failed: " + FormatWin32ErrorMessageLocal(err));
                    return;
                }
            
                WinHandleGuard destHandle(::CreateFileW(destPath.c_str(), GENERIC_WRITE, shareMode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (!destHandle.IsValid())
                {
                    const DWORD err = ::GetLastError();
                    firstError.SetOnce(std::wstring(context) + L" CreateFileW(dest worker) failed: " + FormatWin32ErrorMessageLocal(err));
                    return;
                }
            
                std::vector<uint8_t> buffer;
                buffer.resize(static_cast<size_t>(kParallelChunkBytes));
            
                while (!firstError.hasError.load(std::memory_order_acquire))
                {
                    const uint64_t chunkIndex = nextChunkIndex.fetch_add(1, std::memory_order_acq_rel);
                    const uint64_t offset = chunkIndex * static_cast<uint64_t>(kParallelChunkBytes);
                    if (offset >= fileSize)
                    {
                        break;
                    }
            
                    const uint32_t bytesToCopy = static_cast<uint32_t>(std::min<uint64_t>(kParallelChunkBytes, fileSize - offset));
            
                    LARGE_INTEGER li;
                    li.QuadPart = static_cast<LONGLONG>(offset);
                    if (!::SetFilePointerEx(sourceHandle.Get(), li, nullptr, FILE_BEGIN))
                    {
                        const DWORD err = ::GetLastError();
                        firstError.SetOnce(std::wstring(context) + L" SetFilePointerEx(source) failed: " + FormatWin32ErrorMessageLocal(err));
                        break;
                    }
            
                    uint32_t totalRead = 0;
                    while (totalRead < bytesToCopy && !firstError.hasError.load(std::memory_order_acquire))
                    {
                        DWORD readBytes = 0;
                        const BOOL ok = ::ReadFile(sourceHandle.Get(), buffer.data() + totalRead, bytesToCopy - totalRead, &readBytes, nullptr);
                        if (!ok)
                        {
                            const DWORD err = ::GetLastError();
                            firstError.SetOnce(std::wstring(context) + L" ReadFile failed: " + FormatWin32ErrorMessageLocal(err));
                            break;
                        }
                        if (readBytes == 0)
                        {
                            firstError.SetOnce(std::wstring(context) + L" ReadFile returned 0 before expected EOF");
                            break;
                        }
                        totalRead += static_cast<uint32_t>(readBytes);
                    }
            
                    if (firstError.hasError.load(std::memory_order_acquire))
                    {
                        break;
                    }
            
                    li.QuadPart = static_cast<LONGLONG>(offset);
                    if (!::SetFilePointerEx(destHandle.Get(), li, nullptr, FILE_BEGIN))
                    {
                        const DWORD err = ::GetLastError();
                        firstError.SetOnce(std::wstring(context) + L" SetFilePointerEx(dest) failed: " + FormatWin32ErrorMessageLocal(err));
                        break;
                    }
            
                    uint32_t totalWritten = 0;
                    while (totalWritten < bytesToCopy && !firstError.hasError.load(std::memory_order_acquire))
                    {
                        DWORD writtenBytes = 0;
                        const BOOL ok = ::WriteFile(destHandle.Get(), buffer.data() + totalWritten, bytesToCopy - totalWritten, &writtenBytes, nullptr);
                        if (!ok)
                        {
                            const DWORD err = ::GetLastError();
                            firstError.SetOnce(std::wstring(context) + L" WriteFile failed: " + FormatWin32ErrorMessageLocal(err));
                            break;
                        }
                        if (writtenBytes == 0)
                        {
                            firstError.SetOnce(std::wstring(context) + L" WriteFile returned 0 unexpectedly");
                            break;
                        }
                        totalWritten += static_cast<uint32_t>(writtenBytes);
                    }
                }
            });
        }

        pool.WaitIdle();

        if (firstError.hasError.load(std::memory_order_acquire))
        {
            if (errorMessage != nullptr)
            {
                std::lock_guard<std::mutex> lock(firstError.mutex);
                *errorMessage = firstError.message;
            }
            return false;
        }

        (void)TryCloneFileTimesAndAttributes(sourcePath, destPath);
        return true;
    }
}

GB_SmbAccessor::GB_SmbAccessor(const std::wstring& hostOrIp, AddressType addressType): hostOrIp_(hostOrIp), addressType_(addressType), credentials_(), useLongPathPrefix_(false)
{
}

GB_SmbAccessor::GB_SmbAccessor(const std::wstring& hostOrIp, AddressType addressType, const Credentials& credentials)
    : hostOrIp_(hostOrIp), addressType_(addressType), credentials_(credentials), useLongPathPrefix_(false)
{
}

GB_SmbAccessor::~GB_SmbAccessor()
{
    std::vector<ConnectedShareRecord> sharesToDisconnect;
    {
        std::lock_guard<std::mutex> lock(connectedSharesMutex_);
        sharesToDisconnect = connectedShares_;
    }

    for (size_t i = 0; i < sharesToDisconnect.size(); i++)
    {
        std::wstring dummy;
        DisconnectShare(sharesToDisconnect[i].shareName, true, &dummy);
    }
}

void GB_SmbAccessor::SetCredentials(const Credentials& credentials)
{
    credentials_ = credentials;
}

void GB_SmbAccessor::SetUseLongPathPrefix(bool useLongPathPrefix)
{
    useLongPathPrefix_ = useLongPathPrefix;
}

bool GB_SmbAccessor::TestTcp445(int timeoutMs, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    WSADATA wsaData;
    const int wsaResult = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"WSAStartup failed: " + std::to_wstring(wsaResult);
        }
        return false;
    }

    addrinfoW hints;
    ::ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const static std::wstring port = L"445";
    addrinfoW* resultList = nullptr;
    const std::wstring hostForSocket = hostOrIp_; // 直接用原始 host/ip；IPv6 literal 在 getaddrinfo 里是合法的

    const int gaiResult = ::GetAddrInfoW(hostForSocket.c_str(), port.c_str(), &hints, &resultList);
    if (gaiResult != 0)
    {
        ::WSACleanup();
        if (errorMessage != nullptr)
        {
            *errorMessage = L"GetAddrInfoW failed: " + std::to_wstring(gaiResult);
        }
        return false;
    }

    bool connected = false;
    int lastSocketError = 0;

    for (addrinfoW* addr = resultList; addr != nullptr; addr = addr->ai_next)
    {
        const SOCKET socketHandle = ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (socketHandle == INVALID_SOCKET)
        {
            continue;
        }

        u_long nonBlocking = 1;
        ::ioctlsocket(socketHandle, FIONBIO, &nonBlocking);

        const int connectResult = ::connect(socketHandle, addr->ai_addr, static_cast<int>(addr->ai_addrlen));
        if (connectResult == 0)
        {
            connected = true;
            ::closesocket(socketHandle);
            break;
        }

        const int lastError = ::WSAGetLastError();
        lastSocketError = lastError;
        if (lastError == WSAEWOULDBLOCK || lastError == WSAEINPROGRESS || lastError == WSAEINVAL)
        {
            fd_set writeSet;
            fd_set exceptSet;
            FD_ZERO(&writeSet);
            FD_ZERO(&exceptSet);
            FD_SET(socketHandle, &writeSet);
            FD_SET(socketHandle, &exceptSet);

            timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;

            const int selectResult = ::select(0, nullptr, &writeSet, &exceptSet, &tv);
            if (selectResult == 0)
            {
                lastSocketError = WSAETIMEDOUT;
            }
            else if (selectResult == SOCKET_ERROR)
            {
                lastSocketError = ::WSAGetLastError();
            }
            if (selectResult > 0 && (FD_ISSET(socketHandle, &writeSet) || FD_ISSET(socketHandle, &exceptSet)))
            {
                int soError = 0;
                int soErrorLen = sizeof(soError);
                const int getOptResult = ::getsockopt(socketHandle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soErrorLen);
                if (getOptResult == SOCKET_ERROR)
                {
                    soError = ::WSAGetLastError();
                }
                lastSocketError = soError;
                if (soError == 0)
                {
                    connected = true;
                    ::closesocket(socketHandle);
                    break;
                }
            }
        }

        ::closesocket(socketHandle);
    }

    ::FreeAddrInfoW(resultList);
    ::WSACleanup();

    if (!connected && errorMessage != nullptr)
    {
        if (lastSocketError != 0)
        {
            *errorMessage = L"TCP 445 connect failed: " + FormatWin32ErrorMessageLocal(static_cast<uint32_t>(lastSocketError));
        }
        else
        {
            *errorMessage = L"TCP 445 connect failed.";
        }
    }
    return connected;
}

bool GB_SmbAccessor::TestSmbConnection(std::wstring* errorMessage) const
{
    // IPC$ 属于特殊共享；测试连通性时常用它
    const bool ok = ConnectShare(L"IPC$", false, errorMessage);
    if (ok)
    {
        // 不强制断开：有时上层希望保持会话
        std::wstring dummy;
        DisconnectShare(L"IPC$", true, &dummy);
    }
    return ok;
}

bool GB_SmbAccessor::GetShares(std::vector<std::wstring>* shareNames, bool includeSpecialShares, std::wstring* errorMessage) const
{
    if (shareNames == nullptr)
    {
        return false;
    }
    shareNames->clear();

    std::vector<ShareInfo> shares;
    const bool ok = GetShareInfos(&shares, includeSpecialShares, errorMessage);
    if (!ok)
    {
        return false;
    }

    shareNames->reserve(shares.size());
    for (size_t i = 0; i < shares.size(); i++)
    {
        shareNames->push_back(shares[i].name);
    }
    return true;
}

bool GB_SmbAccessor::GetShareInfos(std::vector<ShareInfo>* shares, bool includeSpecialShares, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }
    if (shares == nullptr)
    {
        return false;
    }
    shares->clear();

    const std::wstring serverName = GetServerNameForNetApi(); // 形如 \\server
    DWORD level = 1;
    DWORD resumeHandle = 0;

    while (true)
    {
        LPBYTE bufferRaw = nullptr;
        DWORD entriesRead = 0;
        DWORD totalEntries = 0;

        const NET_API_STATUS status = ::NetShareEnum(
            const_cast<LPWSTR>(serverName.c_str()),
            level,
            &bufferRaw,
            MAX_PREFERRED_LENGTH,
            &entriesRead,
            &totalEntries,
            &resumeHandle);

        std::unique_ptr<BYTE, NetApiBufferDeleter> buffer(bufferRaw);

        if (status != NERR_Success && status != ERROR_MORE_DATA)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = L"NetShareEnum failed: " + FormatWin32ErrorMessage(static_cast<uint32_t>(status));
            }
            return false;
        }

        const SHARE_INFO_1* shareInfo = reinterpret_cast<const SHARE_INFO_1*>(buffer.get());
        for (DWORD i = 0; i < entriesRead; i++)
        {
            const uint32_t type = static_cast<uint32_t>(shareInfo[i].shi1_type);
            const uint32_t baseType = (type & 0xFFu); // DISKTREE/PRINTQ/DEVICE/IPC 只有很小的值
            const bool isSpecial = (type & STYPE_SPECIAL) != 0;
            const bool isDiskShare = (baseType == STYPE_DISKTREE);

            if (!isDiskShare)
            {
                continue;
            }
            if (!includeSpecialShares && isSpecial)
            {
                continue;
            }

            ShareInfo info;
            if (shareInfo[i].shi1_netname != nullptr)
            {
                info.name = shareInfo[i].shi1_netname;
            }
            info.type = type;
            if (shareInfo[i].shi1_remark != nullptr)
            {
                info.remark = shareInfo[i].shi1_remark;
            }
            shares->push_back(info);
        }

        if (status == NERR_Success)
        {
            break;
        }
        // ERROR_MORE_DATA：继续用 resumeHandle 拉下一页
    }

    return true;
}

bool GB_SmbAccessor::ConnectShare(const std::wstring& shareName, bool persistent, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    const std::wstring remoteName = GetUncRoot(shareName); // \\server\share

    NETRESOURCEW netResource;
    ::ZeroMemory(&netResource, sizeof(netResource));
    netResource.dwType = RESOURCETYPE_ANY;
    netResource.lpRemoteName = const_cast<LPWSTR>(remoteName.c_str());

    std::wstring fullUserName;
    if (!credentials_.userName.empty())
    {
        const bool hasBackslash = (credentials_.userName.find(L'\\') != std::wstring::npos);
        const bool hasAt = (credentials_.userName.find(L'@') != std::wstring::npos);

        if (!hasBackslash && !hasAt && !credentials_.domain.empty())
        {
            fullUserName = credentials_.domain + L"\\" + credentials_.userName;
        }
        else
        {
            fullUserName = credentials_.userName;
        }
    }

    const wchar_t* userNamePtr = fullUserName.empty() ? nullptr : fullUserName.c_str();
    const wchar_t* passwordPtr = credentials_.password.empty() ? nullptr : credentials_.password.c_str();
    const DWORD flags = persistent ? CONNECT_UPDATE_PROFILE : 0;

    const DWORD result = ::WNetAddConnection2W(&netResource, passwordPtr, userNamePtr, flags);
    if (result != NO_ERROR)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"WNetAddConnection2W failed: " + FormatWin32ErrorMessage(result);
        }
        return false;
    }

    RememberConnectedShare(shareName, persistent);
    return true;
}

bool GB_SmbAccessor::DisconnectShare(const std::wstring& shareName, bool force, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    const std::wstring remoteName = GetUncRoot(shareName);

    const DWORD result = ::WNetCancelConnection2W(remoteName.c_str(), 0, force ? TRUE : FALSE);
    if (result != NO_ERROR)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"WNetCancelConnection2W failed: " + FormatWin32ErrorMessage(result);
        }
        return false;
    }

    ForgetConnectedShare(shareName);
    return true;
}

bool GB_SmbAccessor::ListDirectory(const std::wstring& shareName, const std::wstring& relativeDir, std::vector<std::wstring>* childNames, bool includeDirectories, bool includeFiles, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }
    if (childNames == nullptr)
    {
        return false;
    }
    childNames->clear();

    std::wstring dirPath = BuildUncPathInternal(shareName, relativeDir, useLongPathPrefix_);
    dirPath = NormalizeSlashes(dirPath);

    if (!dirPath.empty() && dirPath.back() != L'\\')
    {
        dirPath += L"\\";
    }
    const std::wstring searchPattern = dirPath + L"*";

    WIN32_FIND_DATAW findData;
    ::ZeroMemory(&findData, sizeof(findData));

    HANDLE handle = ::FindFirstFileW(searchPattern.c_str(), &findData);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD err = ::GetLastError();
        if (errorMessage != nullptr)
        {
            *errorMessage = L"FindFirstFileW failed: " + FormatWin32ErrorMessage(err);
        }
        return false;
    }

    FindHandleGuard guard(handle);

    while (true)
    {
        const std::wstring name = findData.cFileName;

        if (name != L"." && name != L"..")
        {
            const bool isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            if (isDirectory && includeDirectories)
            {
                childNames->push_back(name);
            }
            else if (!isDirectory && includeFiles)
            {
                childNames->push_back(name);
            }
        }

        const BOOL ok = ::FindNextFileW(handle, &findData);
        if (!ok)
        {
            const DWORD err = ::GetLastError();
            if (err == ERROR_NO_MORE_FILES)
            {
                break;
            }
            if (errorMessage != nullptr)
            {
                *errorMessage = L"FindNextFileW failed: " + FormatWin32ErrorMessage(err);
            }
            return false;
        }
    }

    return true;
}

bool GB_SmbAccessor::FileExists(const std::wstring& shareName, const std::wstring& relativePath) const
{
    const std::wstring fullPath = BuildUncPathInternal(shareName, relativePath, useLongPathPrefix_);
    const DWORD attrs = ::GetFileAttributesW(fullPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool GB_SmbAccessor::DirectoryExists(const std::wstring& shareName, const std::wstring& relativePath) const
{
    const std::wstring fullPath = BuildUncPathInternal(shareName, relativePath, useLongPathPrefix_);
    const DWORD attrs = ::GetFileAttributesW(fullPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool GB_SmbAccessor::CreateDirectoryRecursive(const std::wstring& shareName, const std::wstring& relativeDir, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    std::wstring fullDir = BuildUncPathInternal(shareName, relativeDir, useLongPathPrefix_);
    fullDir = NormalizeSlashes(fullDir);

    // 去掉末尾反斜杠，避免空段
    while (!fullDir.empty() && (fullDir.back() == L'\\' || fullDir.back() == L'/'))
    {
        fullDir.pop_back();
    }

    // 注意：递归创建是“按段”创建目录；必须先把 "."/".." 折叠掉。
    // 对于 "\\?\\" 前缀路径，Win32 不会自动折叠这些导航段。
    fullDir = CanonicalizeDotsInPath(fullDir, true);

    // 如果已经存在
    const DWORD existingAttrs = ::GetFileAttributesW(fullDir.c_str());
    if (existingAttrs != INVALID_FILE_ATTRIBUTES && (existingAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return true;
    }

    // 从 UNC 根开始逐段创建：\\?\UNC\server\share\dir1\dir2 或 \\server\share\dir1\dir2
    std::wstring prefix;
    size_t pos = 0;

    if (StartsWith(fullDir, L"\\\\?\\UNC\\"))
    {
        // \\?\UNC\server\share\...
        prefix = L"\\\\?\\UNC\\";
        pos = prefix.size();

        // server
        const size_t serverEnd = fullDir.find(L'\\', pos);
        if (serverEnd == std::wstring::npos)
        {
            return false;
        }
        pos = serverEnd + 1;

        // share
        const size_t shareEnd = fullDir.find(L'\\', pos);
        if (shareEnd == std::wstring::npos)
        {
            // 只有 share 根，无需创建
            return true;
        }
        pos = shareEnd + 1;

        prefix = fullDir.substr(0, pos);
    }
    else if (StartsWith(fullDir, L"\\\\"))
    {
        // \\server\share\...
        prefix = L"\\\\";
        pos = prefix.size();

        const size_t serverEnd = fullDir.find(L'\\', pos);
        if (serverEnd == std::wstring::npos)
        {
            return false;
        }
        pos = serverEnd + 1;

        const size_t shareEnd = fullDir.find(L'\\', pos);
        if (shareEnd == std::wstring::npos)
        {
            return true;
        }
        pos = shareEnd + 1;

        prefix = fullDir.substr(0, pos);
    }
    else
    {
        // 非 UNC，不符合本类定位
        return false;
    }

    std::wstring current = prefix;
    while (pos < fullDir.size())
    {
        const size_t nextSep = fullDir.find(L'\\', pos);
        const std::wstring segment = (nextSep == std::wstring::npos) ? fullDir.substr(pos) : fullDir.substr(pos, nextSep - pos);

        if (!segment.empty())
        {
            current = JoinPath(current, segment);

            const DWORD attrs = ::GetFileAttributesW(current.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES)
            {
                const BOOL ok = ::CreateDirectoryW(current.c_str(), nullptr);
                if (!ok)
                {
                    const DWORD err = ::GetLastError();
                    if (err != ERROR_ALREADY_EXISTS)
                    {
                        if (errorMessage != nullptr)
                        {
                            *errorMessage = L"CreateDirectoryW failed: " + FormatWin32ErrorMessage(err);
                        }
                        return false;
                    }
                }
            }
        }

        if (nextSep == std::wstring::npos)
        {
            break;
        }
        pos = nextSep + 1;
    }

    return true;
}

bool GB_SmbAccessor::DeleteFileRemote(const std::wstring& shareName, const std::wstring& relativePath, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    const std::wstring fullPath = BuildUncPathInternal(shareName, relativePath, useLongPathPrefix_);
    const BOOL ok = ::DeleteFileW(fullPath.c_str());
    if (!ok)
    {
        const DWORD err = ::GetLastError();
        if (errorMessage != nullptr)
        {
            *errorMessage = L"DeleteFileW failed: " + FormatWin32ErrorMessage(err);
        }
        return false;
    }

    return true;
}


bool GB_SmbAccessor::CopyFileFromLocalInternal(const std::wstring& localPath, const std::wstring& shareName, const std::wstring& remoteRelativePath, bool overwrite, bool skipEnsureParentDir, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    if (!skipEnsureParentDir)
    {
        const std::wstring remoteParentDir = GetParentPath(remoteRelativePath);
        if (!remoteParentDir.empty())
        {
            const bool created = CreateDirectoryRecursive(shareName, remoteParentDir, errorMessage);
            if (!created)
            {
                return false;
            }
        }
    }

    const std::wstring remotePath = BuildUncPathInternal(shareName, remoteRelativePath, useLongPathPrefix_);

    bool useNoBuffering = false;
    uint64_t fileSize = 0;
    if (TryGetFileSizeByPath(localPath, &fileSize) && fileSize >= kLargeFileNoBufferingThresholdBytes)
    {
        useNoBuffering = true;
    }

    return CopyFileBestEffort(localPath, remotePath, overwrite, useNoBuffering, errorMessage, L"CopyFile(local->remote)");
}

bool GB_SmbAccessor::CopyFileToLocalInternal(const std::wstring& shareName, const std::wstring& remoteRelativePath, const std::wstring& localPath, bool overwrite, bool skipEnsureParentDir, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    if (!skipEnsureParentDir)
    {
        const std::wstring localParentDir = GetParentPath(localPath);
        if (!localParentDir.empty())
        {
            const bool created = CreateLocalDirectoryRecursive(localParentDir, errorMessage);
            if (!created)
            {
                return false;
            }
        }
    }

    const std::wstring remotePath = BuildUncPathInternal(shareName, remoteRelativePath, useLongPathPrefix_);

    bool useNoBuffering = false;
    uint64_t fileSize = 0;
    if (TryGetFileSizeByPath(remotePath, &fileSize) && fileSize >= kLargeFileNoBufferingThresholdBytes)
    {
        useNoBuffering = true;
    }

    return CopyFileBestEffort(remotePath, localPath, overwrite, useNoBuffering, errorMessage, L"CopyFile(remote->local)");
}

bool GB_SmbAccessor::CopyFileFromLocal(const std::wstring& localPath, const std::wstring& shareName, const std::wstring& remoteRelativePath, bool overwrite, std::wstring* errorMessage) const
{
    return CopyFileFromLocalInternal(localPath, shareName, remoteRelativePath, overwrite, false, errorMessage);
}


bool GB_SmbAccessor::CopyFileToLocal(const std::wstring& shareName, const std::wstring& remoteRelativePath, const std::wstring& localPath, bool overwrite, std::wstring* errorMessage) const
{
    return CopyFileToLocalInternal(shareName, remoteRelativePath, localPath, overwrite, false, errorMessage);
}


bool GB_SmbAccessor::CopyFileFromLocalParallel(const std::wstring& localPath, const std::wstring& shareName, const std::wstring& remoteRelativePath,
    bool overwrite, std::wstring* errorMessage, size_t threadCount) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    // 先确保远端父目录存在（与 CopyFileFromLocalInternal 的行为一致）。
    const std::wstring parentDir = GetParentPath(remoteRelativePath);
    if (!parentDir.empty())
    {
        const bool ok = CreateDirectoryRecursive(shareName, parentDir, errorMessage);
        if (!ok)
        {
            return false;
        }
    }

    const std::wstring remotePath = BuildUncPathInternal(shareName, remoteRelativePath, useLongPathPrefix_);

    // 超大文件场景：优先沿用现有 CopyFile2 + COPY_FILE_NO_BUFFERING 的路径。
    uint64_t fileSize = 0;
    if (TryGetFileSizeByPath(localPath, &fileSize) && fileSize >= kLargeFileNoBufferingThresholdBytes)
    {
        return CopyFileBestEffort(localPath, remotePath, overwrite, true, errorMessage, L"CopyFileParallel(local->remote)");
    }

    const size_t normalizedThreadCount = NormalizeParallelThreadCount(threadCount);
    return CopyFileBySegmentsParallel(localPath, remotePath, overwrite, normalizedThreadCount, errorMessage, L"CopyFileParallel(local->remote)");
}


bool GB_SmbAccessor::CopyFileToLocalParallel(const std::wstring& shareName, const std::wstring& remoteRelativePath, const std::wstring& localPath,
    bool overwrite, std::wstring* errorMessage, size_t threadCount) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    // 先确保本地父目录存在（与 CopyFileToLocalInternal 的行为一致）。
    const std::wstring localParentDir = GetParentPath(localPath);
    if (!localParentDir.empty())
    {
        const bool ok = CreateLocalDirectoryRecursive(localParentDir, errorMessage);
        if (!ok)
        {
            return false;
        }
    }

    const std::wstring remotePath = BuildUncPathInternal(shareName, remoteRelativePath, useLongPathPrefix_);

    // 超大文件场景：优先沿用现有 CopyFile2 + COPY_FILE_NO_BUFFERING 的路径。
    uint64_t fileSize = 0;
    if (TryGetFileSizeByPath(remotePath, &fileSize) && fileSize >= kLargeFileNoBufferingThresholdBytes)
    {
        return CopyFileBestEffort(remotePath, localPath, overwrite, true, errorMessage, L"CopyFileParallel(remote->local)");
    }

    const size_t normalizedThreadCount = NormalizeParallelThreadCount(threadCount);
    return CopyFileBySegmentsParallel(remotePath, localPath, overwrite, normalizedThreadCount, errorMessage, L"CopyFileParallel(remote->local)");
}


bool GB_SmbAccessor::CopyDirectoryFromLocalParallel(const std::wstring& localDirectory, const std::wstring& shareName, const std::wstring& remoteRelativePath,
    bool overwrite, std::wstring* errorMessage, size_t threadCount) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    std::wstring localDir = NormalizeSlashes(localDirectory);
    while (!localDir.empty() && localDir.back() == L'\\')
    {
        localDir.pop_back();
    }

    const DWORD attrs = ::GetFileAttributesW(localDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Local directory not found: " + localDir;
        }
        return false;
    }

    // 只对“根目标目录”做一次递归创建（保证中间目录存在）。
    const bool createdRemoteDir = CreateDirectoryRecursive(shareName, remoteRelativePath, errorMessage);
    if (!createdRemoteDir)
    {
        return false;
    }

    const size_t normalizedThreadCount = NormalizeParallelThreadCount(threadCount);
    const size_t maxQueueSize = NormalizeParallelMaxQueueSize(normalizedThreadCount);
    GB_ThreadPool pool(normalizedThreadCount, maxQueueSize);

    ParallelFirstError firstError;

    struct DirTask
    {
        std::wstring localDir;
        std::wstring remoteRel;
        bool skipEnsureRemoteDir = false;
    };

    auto EnsureRemoteDirOneLevel = [&](const std::wstring& remoteRel, std::wstring* ensureErrorMessage) -> bool {
        if (remoteRel.empty())
        {
            return true;
        }
    
        std::wstring fullDir = BuildUncPathInternal(shareName, remoteRel, useLongPathPrefix_);
        fullDir = NormalizeSlashes(fullDir);
        while (!fullDir.empty() && fullDir.back() == L'\\')
        {
            fullDir.pop_back();
        }
    
        const BOOL ok = ::CreateDirectoryW(fullDir.c_str(), nullptr);
        if (ok)
        {
            return true;
        }
    
        const DWORD err = ::GetLastError();
        if (err == ERROR_ALREADY_EXISTS)
        {
            const DWORD attrs = ::GetFileAttributesW(fullDir.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                return true;
            }
            if (ensureErrorMessage != nullptr)
            {
                *ensureErrorMessage = L"Destination path exists but is not a directory: " + fullDir;
            }
            return false;
        }
    
        if (ensureErrorMessage != nullptr)
        {
            *ensureErrorMessage = L"CreateDirectoryW(remote) failed: " + FormatWin32ErrorMessage(err);
        }
        return false;
    };

    std::vector<DirTask> taskStack;
    taskStack.reserve(64);
    {
        DirTask root;
        root.localDir = localDir;
        root.remoteRel = remoteRelativePath;
        root.skipEnsureRemoteDir = true; // 根目录已递归创建
        taskStack.push_back(root);
    }

    while (!taskStack.empty())
    {
        if (firstError.hasError.load(std::memory_order_acquire))
        {
            break;
        }

        const DirTask task = taskStack.back();
        taskStack.pop_back();

        if (!task.skipEnsureRemoteDir)
        {
            const bool ensured = EnsureRemoteDirOneLevel(task.remoteRel, errorMessage);
            if (!ensured)
            {
                firstError.SetOnce(errorMessage != nullptr ? *errorMessage : L"EnsureRemoteDirOneLevel failed");
                break;
            }
        }

        std::wstring searchPath = NormalizeSlashes(task.localDir);
        if (!searchPath.empty() && searchPath.back() != L'\\')
        {
            searchPath += L"\\";
        }
        searchPath += L"*";

        WIN32_FIND_DATAW findData;
        ::ZeroMemory(&findData, sizeof(findData));

        HANDLE handle = FindFirstFileExCompatible(searchPath, &findData, false);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const DWORD err = ::GetLastError();
            if (errorMessage != nullptr)
            {
                *errorMessage = L"FindFirstFileExW(local) failed: " + FormatWin32ErrorMessage(err);
            }
            firstError.SetOnce(errorMessage != nullptr ? *errorMessage : L"FindFirstFileExW(local) failed");
            break;
        }

        FindHandleGuard guard(handle);

        while (true)
        {
            if (firstError.hasError.load(std::memory_order_acquire))
            {
                break;
            }

            const std::wstring name = findData.cFileName;
            if (name != L"." && name != L"..")
            {
                const DWORD attrs = findData.dwFileAttributes;
                const bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const bool isReparsePoint = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

                const std::wstring childLocalPath = JoinPath(task.localDir, name);
                const std::wstring childRemoteRel = task.remoteRel.empty() ? name : JoinPath(task.remoteRel, name);

                if (isDirectory)
                {
                    if (!isReparsePoint)
                    {
                        DirTask child;
                        child.localDir = childLocalPath;
                        child.remoteRel = childRemoteRel;
                        child.skipEnsureRemoteDir = false;
                        taskStack.push_back(child);
                    }
                    else
                    {
                        const bool ensured = EnsureRemoteDirOneLevel(childRemoteRel, errorMessage);
                        if (!ensured)
                        {
                            firstError.SetOnce(errorMessage != nullptr ? *errorMessage : L"EnsureRemoteDirOneLevel failed");
                            break;
                        }
                    }
                }
                else
                {
                    const std::wstring localPathCopy = childLocalPath;
                    const std::wstring remoteRelCopy = childRemoteRel;
                    pool.Post([&, localPathCopy, remoteRelCopy]() {
                        if (firstError.hasError.load(std::memory_order_acquire))
                        {
                            return;
                        }

                        std::wstring localError;
                        const bool ok = CopyFileFromLocalInternal(localPathCopy, shareName, remoteRelCopy, overwrite, true, &localError);
                        if (!ok)
                        {
                            firstError.SetOnce(localError);
                        }
                    });
                }
            }

            const BOOL ok = ::FindNextFileW(handle, &findData);
            if (!ok)
            {
                const DWORD err = ::GetLastError();
                if (err == ERROR_NO_MORE_FILES)
                {
                    break;
                }
                if (errorMessage != nullptr)
                {
                    *errorMessage = L"FindNextFileW(local) failed: " + FormatWin32ErrorMessage(err);
                }
                firstError.SetOnce(errorMessage != nullptr ? *errorMessage : L"FindNextFileW(local) failed");
                break;
            }
        }
    }

    pool.WaitIdle();

    if (firstError.hasError.load(std::memory_order_acquire))
    {
        if (errorMessage != nullptr)
        {
            std::lock_guard<std::mutex> lock(firstError.mutex);
            *errorMessage = firstError.message;
        }
        return false;
    }

    return true;
}


bool GB_SmbAccessor::CopyDirectoryToLocalParallel(const std::wstring& shareName, const std::wstring& remoteRelativePath, const std::wstring& localDirectory,
    bool overwrite, std::wstring* errorMessage, size_t threadCount) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    std::wstring localDir = NormalizeSlashes(localDirectory);
    while (!localDir.empty() && localDir.back() == L'\\')
    {
        localDir.pop_back();
    }

    const bool createdLocalDir = CreateLocalDirectoryRecursive(localDir, errorMessage);
    if (!createdLocalDir)
    {
        return false;
    }

    const std::wstring remoteDirPath = BuildUncPathInternal(shareName, remoteRelativePath, useLongPathPrefix_);
    const DWORD attrs = ::GetFileAttributesW(remoteDirPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Remote directory not found: " + remoteDirPath;
        }
        return false;
    }

    // 目录拷贝的“自动并发”不宜太激进：SMB 多流并发可能会触发更多往返/竞争，导致吞吐下降。
    // 若外部显式指定 threadCount，则尊重；否则把自动并发上限收敛到 4。
    const size_t requestedThreadCount = threadCount;
    const size_t normalizedThreadCount = NormalizeParallelThreadCount(threadCount);
    const size_t poolThreadCount = (requestedThreadCount == 0) ? std::min<size_t>(normalizedThreadCount, 4) : normalizedThreadCount;

    const size_t maxQueueSize = NormalizeParallelMaxQueueSize(poolThreadCount);
    GB_ThreadPool pool(poolThreadCount, maxQueueSize);

    ParallelFirstError firstError;

    struct DirTask
    {
        std::wstring remoteRel;
        std::wstring localDir;
        bool skipEnsureLocalDir = false;
    };

    auto EnsureLocalDirOneLevel = [&](const std::wstring& fullLocalDir, std::wstring* ensureErrorMessage) -> bool {
        std::wstring dirPath = NormalizeSlashes(fullLocalDir);
        while (!dirPath.empty() && dirPath.back() == L'\\')
        {
            dirPath.pop_back();
        }

        const BOOL ok = ::CreateDirectoryW(dirPath.c_str(), nullptr);
        if (ok)
        {
            return true;
        }

        const DWORD err = ::GetLastError();
        if (err == ERROR_ALREADY_EXISTS)
        {
            const DWORD attrs = ::GetFileAttributesW(dirPath.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                return true;
            }
            if (ensureErrorMessage != nullptr)
            {
                *ensureErrorMessage = L"Destination path exists but is not a directory: " + dirPath;
            }
            return false;
        }

        if (ensureErrorMessage != nullptr)
        {
            *ensureErrorMessage = L"CreateDirectoryW(local) failed: " + FormatWin32ErrorMessage(err);
        }
        return false;
        };

    // 目录拷贝的“文件任务”中避免再调用 GetFileAttributesExW(UNC) 做远端元数据探测：
    // - 枚举阶段的 WIN32_FIND_DATAW 已经带了文件大小；
    // - 直接走 CopyFile2/CopyFileW 的拷贝路径即可，减少一次 SMB 往返。
    auto CopyFileRemoteToLocalFast = [&](const std::wstring& remoteRel, const std::wstring& localPath, uint64_t fileSizeBytes,
        std::wstring* localErrorMessage) -> bool
        {
            const std::wstring sourcePath = BuildUncPathInternal(shareName, remoteRel, useLongPathPrefix_);
            const std::wstring destPath = NormalizeSlashes(localPath);

            // 超大文件：保留原先的 NO_BUFFERING 优化尝试（若不支持会自动回退）。
            const bool useNoBuffering = (fileSizeBytes >= kLargeFileNoBufferingThresholdBytes);

            bool triedCopyFile2 = false;
            CopyFile2Func copyFile2 = GetCopyFile2Func();
            if (copyFile2 != nullptr)
            {
                triedCopyFile2 = true;

                COPYFILE2_EXTENDED_PARAMETERS params;
                ::ZeroMemory(&params, sizeof(params));
                params.dwSize = sizeof(params);
                params.dwCopyFlags = 0;
                if (!overwrite)
                {
                    params.dwCopyFlags |= COPY_FILE_FAIL_IF_EXISTS;
                }
                if (useNoBuffering)
                {
                    params.dwCopyFlags |= COPY_FILE_NO_BUFFERING;
                }

                const HRESULT hr = copyFile2(sourcePath.c_str(), destPath.c_str(), &params);
                if (SUCCEEDED(hr))
                {
                    (void)TryCloneFileTimesAndAttributes(sourcePath, destPath);
                    return true;
                }
            }

            const BOOL ok = ::CopyFileW(sourcePath.c_str(), destPath.c_str(), overwrite ? FALSE : TRUE);
            if (!ok)
            {
                const DWORD err = ::GetLastError();
                if (localErrorMessage != nullptr)
                {
                    std::wstring msg = L"CopyFile(remote->local) failed: ";
                    msg += sourcePath;
                    msg += L" -> ";
                    msg += destPath;
                    msg += L", ";
                    msg += FormatWin32ErrorMessageLocal(err);
                    if (triedCopyFile2)
                    {
                        msg += L" (CopyFile2 attempt was made)";
                    }
                    *localErrorMessage = msg;
                }
                return false;
            }

            (void)TryCloneFileTimesAndAttributes(sourcePath, destPath);
            return true;
        };

    std::vector<DirTask> taskStack;
    taskStack.reserve(64);
    {
        DirTask root;
        root.remoteRel = remoteRelativePath;
        root.localDir = localDir;
        root.skipEnsureLocalDir = true;
        taskStack.push_back(root);
    }

    while (!taskStack.empty())
    {
        if (firstError.hasError.load(std::memory_order_acquire))
        {
            break;
        }

        const DirTask task = taskStack.back();
        taskStack.pop_back();

        if (!task.skipEnsureLocalDir)
        {
            const bool ensured = EnsureLocalDirOneLevel(task.localDir, errorMessage);
            if (!ensured)
            {
                firstError.SetOnce(errorMessage != nullptr ? *errorMessage : L"EnsureLocalDirOneLevel failed");
                break;
            }
        }

        const std::wstring currentRemoteDirPath = BuildUncPathInternal(shareName, task.remoteRel, useLongPathPrefix_);

        std::wstring searchPath = NormalizeSlashes(currentRemoteDirPath);
        if (!searchPath.empty() && searchPath.back() != L'\\')
        {
            searchPath += L"\\";
        }
        searchPath += L"*";

        WIN32_FIND_DATAW findData;
        ::ZeroMemory(&findData, sizeof(findData));

        HANDLE handle = FindFirstFileExCompatible(searchPath, &findData, true);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const DWORD err = ::GetLastError();
            if (errorMessage != nullptr)
            {
                *errorMessage = L"FindFirstFileExW(remote) failed: " + FormatWin32ErrorMessage(err);
            }
            firstError.SetOnce(errorMessage != nullptr ? *errorMessage : L"FindFirstFileExW(remote) failed");
            break;
        }

        FindHandleGuard guard(handle);

        while (true)
        {
            if (firstError.hasError.load(std::memory_order_acquire))
            {
                break;
            }

            const std::wstring name = findData.cFileName;
            if (name != L"." && name != L"..")
            {
                const DWORD attrs = findData.dwFileAttributes;
                const bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const bool isReparsePoint = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

                const std::wstring childLocalPath = JoinPath(task.localDir, name);
                const std::wstring childRemoteRel = task.remoteRel.empty() ? name : JoinPath(task.remoteRel, name);

                if (isDirectory)
                {
                    if (!isReparsePoint)
                    {
                        DirTask child;
                        child.remoteRel = childRemoteRel;
                        child.localDir = childLocalPath;
                        child.skipEnsureLocalDir = false;
                        taskStack.push_back(child);
                    }
                    else
                    {
                        const bool ensured = EnsureLocalDirOneLevel(childLocalPath, errorMessage);
                        if (!ensured)
                        {
                            firstError.SetOnce(errorMessage != nullptr ? *errorMessage : L"EnsureLocalDirOneLevel failed");
                            break;
                        }
                    }
                }
                else
                {
                    const uint64_t high = static_cast<uint64_t>(findData.nFileSizeHigh);
                    const uint64_t low = static_cast<uint64_t>(findData.nFileSizeLow);
                    const uint64_t fileSizeBytes = (high << 32) | low;

                    const std::wstring localPathCopy = childLocalPath;
                    const std::wstring remoteRelCopy = childRemoteRel;
                    pool.Post([&, localPathCopy, remoteRelCopy, fileSizeBytes]()
                        {
                            if (firstError.hasError.load(std::memory_order_acquire))
                            {
                                return;
                            }

                            std::wstring localError;
                            const bool ok = CopyFileRemoteToLocalFast(remoteRelCopy, localPathCopy, fileSizeBytes, &localError);
                            if (!ok)
                            {
                                firstError.SetOnce(localError);
                            }
                        });
                }
            }

            const BOOL ok = ::FindNextFileW(handle, &findData);
            if (!ok)
            {
                const DWORD err = ::GetLastError();
                if (err == ERROR_NO_MORE_FILES)
                {
                    break;
                }
                if (errorMessage != nullptr)
                {
                    *errorMessage = L"FindNextFileW(remote) failed: " + FormatWin32ErrorMessage(err);
                }
                firstError.SetOnce(errorMessage != nullptr ? *errorMessage : L"FindNextFileW(remote) failed");
                break;
            }
        }
    }

    pool.WaitIdle();

    if (firstError.hasError.load(std::memory_order_acquire))
    {
        if (errorMessage != nullptr)
        {
            std::lock_guard<std::mutex> lock(firstError.mutex);
            *errorMessage = firstError.message;
        }
        return false;
    }

    return true;
}


bool GB_SmbAccessor::CopyDirectoryFromLocal(const std::wstring& localDirectory, const std::wstring& shareName, const std::wstring& remoteRelativePath, bool overwrite, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    std::wstring localDir = NormalizeSlashes(localDirectory);
    while (!localDir.empty() && localDir.back() == L'\\')
    {
        localDir.pop_back();
    }

    const DWORD attrs = ::GetFileAttributesW(localDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Local directory not found: " + localDir;
        }
        return false;
    }

    // 只对“根目标目录”做一次递归创建（保证中间目录存在）。
    // 之后的子目录创建全部走“一层创建”快路径，避免每次都从 UNC 根逐段探测/创建。
    const bool createdRemoteDir = CreateDirectoryRecursive(shareName, remoteRelativePath, errorMessage);
    if (!createdRemoteDir)
    {
        return false;
    }

    struct DirTask
    {
        std::wstring localDir;
        std::wstring remoteRel;
        bool skipEnsureRemoteDir = false;
    };

    auto EnsureRemoteDirOneLevel = [&](const std::wstring& remoteRel, std::wstring* ensureErrorMessage) -> bool {
        if (remoteRel.empty())
        {
            // share 根目录无需创建
            return true;
        }

        std::wstring fullDir = BuildUncPathInternal(shareName, remoteRel, useLongPathPrefix_);
        fullDir = NormalizeSlashes(fullDir);
        while (!fullDir.empty() && fullDir.back() == L'\\')
        {
            fullDir.pop_back();
        }

        const BOOL ok = ::CreateDirectoryW(fullDir.c_str(), nullptr);
        if (ok)
        {
            return true;
        }

        const DWORD err = ::GetLastError();
        if (err == ERROR_ALREADY_EXISTS)
        {
            const DWORD attrs = ::GetFileAttributesW(fullDir.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                return true;
            }
            if (ensureErrorMessage != nullptr)
            {
                *ensureErrorMessage = L"Destination path exists but is not a directory: " + fullDir;
            }
            return false;
        }

        if (ensureErrorMessage != nullptr)
        {
            *ensureErrorMessage = L"CreateDirectoryW(remote) failed: " + FormatWin32ErrorMessage(err);
        }
        return false;
    };

    std::vector<DirTask> taskStack;
    taskStack.reserve(64);
    {
        DirTask root;
        root.localDir = localDir;
        root.remoteRel = remoteRelativePath;
        root.skipEnsureRemoteDir = true; // 根目录已递归创建
        taskStack.push_back(root);
    }

    while (!taskStack.empty())
    {
        const DirTask task = taskStack.back();
        taskStack.pop_back();

        if (!task.skipEnsureRemoteDir)
        {
            const bool ensured = EnsureRemoteDirOneLevel(task.remoteRel, errorMessage);
            if (!ensured)
            {
                return false;
            }
        }

        std::wstring searchPath = task.localDir;
        if (!searchPath.empty() && searchPath.back() != L'\\')
        {
            searchPath += L"\\";
        }
        searchPath += L"*";

        WIN32_FIND_DATAW findData;
        ::ZeroMemory(&findData, sizeof(findData));

        HANDLE handle = FindFirstFileExCompatible(searchPath, &findData, false);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const DWORD err = ::GetLastError();
            if (errorMessage != nullptr)
            {
                *errorMessage = L"FindFirstFileExW(local) failed: " + FormatWin32ErrorMessage(err);
            }
            return false;
        }

        FindHandleGuard guard(handle);

        while (true)
        {
            const std::wstring name = findData.cFileName;
            if (name != L"." && name != L"..")
            {
                const DWORD attrs = findData.dwFileAttributes;
                const bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const bool isReparsePoint = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

                const std::wstring childLocalPath = JoinPath(task.localDir, name);
                const std::wstring childRemoteRel = task.remoteRel.empty() ? name : JoinPath(task.remoteRel, name);

                if (isDirectory)
                {
                    // 避免跟随 Junction/Symlink 目录导致死循环/逃逸目录树：默认不递归 reparse point。
                    // 如确实需要跟随，移除该判断或改为可配置选项。
                    if (!isReparsePoint)
                    {
                        DirTask child;
                        child.localDir = childLocalPath;
                        child.remoteRel = childRemoteRel;
                        child.skipEnsureRemoteDir = false;
                        taskStack.push_back(child);
                    }
                    else
                    {
                        const bool ensured = EnsureRemoteDirOneLevel(childRemoteRel, errorMessage);
                        if (!ensured)
                        {
                            return false;
                        }
                    }
                }
                else
                {
                    const bool ok = CopyFileFromLocalInternal(childLocalPath, shareName, childRemoteRel, overwrite, true, errorMessage);
                    if (!ok)
                    {
                        return false;
                    }
                }
            }

            const BOOL ok = ::FindNextFileW(handle, &findData);
            if (!ok)
            {
                const DWORD err = ::GetLastError();
                if (err == ERROR_NO_MORE_FILES)
                {
                    break;
                }
                if (errorMessage != nullptr)
                {
                    *errorMessage = L"FindNextFileW(local) failed: " + FormatWin32ErrorMessage(err);
                }
                return false;
            }
        }
    }

    return true;
}

bool GB_SmbAccessor::CopyDirectoryToLocal(const std::wstring& shareName, const std::wstring& remoteRelativePath, const std::wstring& localDirectory, bool overwrite, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    std::wstring localDir = NormalizeSlashes(localDirectory);
    while (!localDir.empty() && localDir.back() == L'\\')
    {
        localDir.pop_back();
    }

    const bool createdLocalDir = CreateLocalDirectoryRecursive(localDir, errorMessage);
    if (!createdLocalDir)
    {
        return false;
    }

    const std::wstring remoteDirPath = BuildUncPathInternal(shareName, remoteRelativePath, useLongPathPrefix_);
    const DWORD attrs = ::GetFileAttributesW(remoteDirPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = L"Remote directory not found: " + remoteDirPath;
        }
        return false;
    }

    // 只对“根目标目录”做一次递归创建（保证中间目录存在）。
    // 之后的子目录创建全部走“一层创建”快路径，避免重复逐段探测/创建。
    struct DirTask
    {
        std::wstring remoteRel;
        std::wstring localDir;
        bool skipEnsureLocalDir = false;
    };

    auto EnsureLocalDirOneLevel = [&](const std::wstring& fullLocalDir, std::wstring* ensureErrorMessage) -> bool {
        std::wstring dirPath = NormalizeSlashes(fullLocalDir);
        while (!dirPath.empty() && dirPath.back() == L'\\')
        {
            dirPath.pop_back();
        }

        const BOOL ok = ::CreateDirectoryW(dirPath.c_str(), nullptr);
        if (ok)
        {
            return true;
        }

        const DWORD err = ::GetLastError();
        if (err == ERROR_ALREADY_EXISTS)
        {
            const DWORD attrs = ::GetFileAttributesW(dirPath.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                return true;
            }
            if (ensureErrorMessage != nullptr)
            {
                *ensureErrorMessage = L"Destination path exists but is not a directory: " + dirPath;
            }
            return false;
        }

        if (ensureErrorMessage != nullptr)
        {
            *ensureErrorMessage = L"CreateDirectoryW(local) failed: " + FormatWin32ErrorMessage(err);
        }
        return false;
    };

    std::vector<DirTask> taskStack;
    taskStack.reserve(64);
    {
        DirTask root;
        root.remoteRel = remoteRelativePath;
        root.localDir = localDir;
        root.skipEnsureLocalDir = true; // 根目录已递归创建
        taskStack.push_back(root);
    }

    while (!taskStack.empty())
    {
        const DirTask task = taskStack.back();
        taskStack.pop_back();

        if (!task.skipEnsureLocalDir)
        {
            const bool ensured = EnsureLocalDirOneLevel(task.localDir, errorMessage);
            if (!ensured)
            {
                return false;
            }
        }

        const std::wstring currentRemoteDirPath = BuildUncPathInternal(shareName, task.remoteRel, useLongPathPrefix_);

        std::wstring searchPath = NormalizeSlashes(currentRemoteDirPath);
        if (!searchPath.empty() && searchPath.back() != L'\\')
        {
            searchPath += L"\\";
        }
        searchPath += L"*";

        WIN32_FIND_DATAW findData;
        ::ZeroMemory(&findData, sizeof(findData));

        HANDLE handle = FindFirstFileExCompatible(searchPath, &findData, true);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const DWORD err = ::GetLastError();
            if (errorMessage != nullptr)
            {
                *errorMessage = L"FindFirstFileExW(remote) failed: " + FormatWin32ErrorMessage(err);
            }
            return false;
        }

        FindHandleGuard guard(handle);

        while (true)
        {
            const std::wstring name = findData.cFileName;
            if (name != L"." && name != L"..")
            {
                const DWORD attrs = findData.dwFileAttributes;
                const bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const bool isReparsePoint = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

                const std::wstring childLocalPath = JoinPath(task.localDir, name);
                const std::wstring childRemoteRel = task.remoteRel.empty() ? name : JoinPath(task.remoteRel, name);

                if (isDirectory)
                {
                    // 避免跟随 Junction/Symlink 目录导致死循环/逃逸目录树：默认不递归 reparse point。
                    if (!isReparsePoint)
                    {
                        DirTask child;
                        child.remoteRel = childRemoteRel;
                        child.localDir = childLocalPath;
                        child.skipEnsureLocalDir = false;
                        taskStack.push_back(child);
                    }
                    else
                    {
                        const bool ensured = EnsureLocalDirOneLevel(childLocalPath, errorMessage);
                        if (!ensured)
                        {
                            return false;
                        }
                    }
                }
                else
                {
                    const bool ok = CopyFileToLocalInternal(shareName, childRemoteRel, childLocalPath, overwrite, true, errorMessage);
                    if (!ok)
                    {
                        return false;
                    }
                }
            }

            const BOOL ok = ::FindNextFileW(handle, &findData);
            if (!ok)
            {
                const DWORD err = ::GetLastError();
                if (err == ERROR_NO_MORE_FILES)
                {
                    break;
                }
                if (errorMessage != nullptr)
                {
                    *errorMessage = L"FindNextFileW(remote) failed: " + FormatWin32ErrorMessage(err);
                }
                return false;
            }
        }
    }

    return true;
}

bool GB_SmbAccessor::GetFileSizeRemote(const std::wstring& shareName, const std::wstring& remoteRelativePath, uint64_t* fileSize, std::wstring* errorMessage) const
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }
    if (fileSize == nullptr)
    {
        return false;
    }

    const std::wstring remotePath = BuildUncPathInternal(shareName, remoteRelativePath, useLongPathPrefix_);

    WIN32_FILE_ATTRIBUTE_DATA data;
    ::ZeroMemory(&data, sizeof(data));

    const BOOL ok = ::GetFileAttributesExW(remotePath.c_str(), GetFileExInfoStandard, &data);
    if (!ok)
    {
        const DWORD err = ::GetLastError();
        if (errorMessage != nullptr)
        {
            *errorMessage = L"GetFileAttributesExW failed: " + FormatWin32ErrorMessage(err);
        }
        return false;
    }

    const uint64_t high = static_cast<uint64_t>(data.nFileSizeHigh);
    const uint64_t low = static_cast<uint64_t>(data.nFileSizeLow);
    *fileSize = (high << 32) | low;
    return true;
}

std::wstring GB_SmbAccessor::GetUncRoot(const std::wstring& shareName) const
{
    return BuildUncRootInternal(shareName, false);
}

std::wstring GB_SmbAccessor::GetUncPath(const std::wstring& shareName, const std::wstring& relativePath) const
{
    return BuildUncPathInternal(shareName, relativePath, false);
}

std::wstring GB_SmbAccessor::FormatWin32ErrorMessage(uint32_t errorCode)
{
    wchar_t* buffer = nullptr;

    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD langId = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);

    const DWORD len = ::FormatMessageW(flags,
        nullptr,
        errorCode,
        langId,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message;
    if (len != 0 && buffer != nullptr)
    {
        message.assign(buffer, buffer + len);
        ::LocalFree(buffer);
    }
    else
    {
        message = L"Error=" + std::to_wstring(errorCode);
    }

    // 去掉末尾的换行/句点
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
    {
        message.pop_back();
    }
    return message;
}

std::wstring GB_SmbAccessor::NormalizeSlashes(const std::wstring& path)
{
    std::wstring out = path;
    for (size_t i = 0; i < out.size(); i++)
    {
        if (out[i] == L'/')
        {
            out[i] = L'\\';
        }
    }
    return out;
}

std::wstring GB_SmbAccessor::JoinPath(const std::wstring& left, const std::wstring& right)
{
    if (left.empty())
    {
        return right;
    }
    if (right.empty())
    {
        return left;
    }

    if (left.back() == L'\\')
    {
        return left + right;
    }
    return left + L"\\" + right;
}

std::wstring GB_SmbAccessor::GetParentPath(const std::wstring& path)
{
    std::wstring normalized = NormalizeSlashes(path);

    while (!normalized.empty() && normalized.back() == L'\\')
    {
        normalized.pop_back();
    }

    const size_t pos = normalized.find_last_of(L'\\');
    if (pos == std::wstring::npos)
    {
        return L"";
    }

    // Drive root: "C:\\xxx" -> parent should keep the trailing slash: "C:\\"
    if (pos == 2 && normalized.size() >= 3 && normalized[1] == L':')
    {
        return normalized.substr(0, 3);
    }
    // Extended-length drive root: "\\?\\C:\\xxx" -> parent should be "\\?\\C:\\"
    if (StartsWith(normalized, L"\\\\?\\") && normalized.size() >= 7)
    {
        const size_t prefixLen = 4; // "\\?\\"
        if (pos == prefixLen + 2 && normalized[prefixLen + 1] == L':')
        {
            return normalized.substr(0, prefixLen + 3);
        }
    }

    return normalized.substr(0, pos);
}

bool GB_SmbAccessor::CreateLocalDirectoryRecursive(const std::wstring& fullDirectory, std::wstring* errorMessage)
{
    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }

    std::wstring dirPath = NormalizeSlashes(fullDirectory);

    while (!dirPath.empty() && dirPath.back() == L'\\')
    {
        dirPath.pop_back();
    }

    // 递归创建时按段逐级创建，必须先折叠 "."/".."，避免把 ".." 当成目录名去创建。
    dirPath = CanonicalizeDotsInPath(dirPath, true);

    if (dirPath.empty())
    {
        return true;
    }

    const DWORD existingAttrs = ::GetFileAttributesW(dirPath.c_str());
    if (existingAttrs != INVALID_FILE_ATTRIBUTES && (existingAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return true;
    }

    std::wstring current;
    size_t pos = 0;

    if (StartsWith(dirPath, L"\\\\?\\UNC\\"))
    {
        // \\?\\UNC\\server\\share\\...
        const size_t prefixLen = 8; // "\\?\\UNC\\"
        pos = prefixLen;

        const size_t serverEnd = dirPath.find(L'\\', pos);
        if (serverEnd == std::wstring::npos)
        {
            return false;
        }
        pos = serverEnd + 1;

        const size_t shareEnd = dirPath.find(L'\\', pos);
        if (shareEnd == std::wstring::npos)
        {
            return true;
        }
        pos = shareEnd + 1;
        current = dirPath.substr(0, pos);
    }
    else if (StartsWith(dirPath, L"\\\\?\\"))
    {
        // \\?\\C:\\...
        const size_t prefixLen = 4; // "\\?\\"
        if (dirPath.size() >= prefixLen + 3 && dirPath[prefixLen + 1] == L':' && dirPath[prefixLen + 2] == L'\\')
        {
            pos = prefixLen + 3;
            current = dirPath.substr(0, pos);
        }
        else
        {
            // 其它 \\?\\ 形式，不做特殊处理
            current.clear();
            pos = 0;
        }
    }
    else if (StartsWith(dirPath, L"\\\\"))
    {
        // \\server\\share\\...
        pos = 2;
        const size_t serverEnd = dirPath.find(L'\\', pos);
        if (serverEnd == std::wstring::npos)
        {
            return false;
        }
        pos = serverEnd + 1;

        const size_t shareEnd = dirPath.find(L'\\', pos);
        if (shareEnd == std::wstring::npos)
        {
            return true;
        }
        pos = shareEnd + 1;
        current = dirPath.substr(0, pos);
    }
    else if (dirPath.size() >= 3 && dirPath[1] == L':' && dirPath[2] == L'\\')
    {
        // C:\\...
        pos = 3;
        current = dirPath.substr(0, pos);
    }
    else
    {
        // 相对路径
        current.clear();
        pos = 0;
    }

    while (pos < dirPath.size())
    {
        const size_t nextSep = dirPath.find(L'\\', pos);
        const std::wstring segment = (nextSep == std::wstring::npos)
            ? dirPath.substr(pos)
            : dirPath.substr(pos, nextSep - pos);

        if (!segment.empty())
        {
            current = current.empty() ? segment : JoinPath(current, segment);

            const DWORD attrs = ::GetFileAttributesW(current.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES)
            {
                const BOOL ok = ::CreateDirectoryW(current.c_str(), nullptr);
                if (!ok)
                {
                    const DWORD err = ::GetLastError();
                    if (err != ERROR_ALREADY_EXISTS)
                    {
                        if (errorMessage != nullptr)
                        {
                            *errorMessage = L"CreateDirectoryW(local) failed: " + FormatWin32ErrorMessage(err);
                        }
                        return false;
                    }
                }
            }
        }

        if (nextSep == std::wstring::npos)
        {
            break;
        }
        pos = nextSep + 1;
    }

    return true;
}

void GB_SmbAccessor::RememberConnectedShare(const std::wstring& shareName, bool persistent) const
{
    std::lock_guard<std::mutex> lock(connectedSharesMutex_);

    for (size_t i = 0; i < connectedShares_.size(); i++)
    {
        if (connectedShares_[i].shareName == shareName)
        {
            connectedShares_[i].persistent = connectedShares_[i].persistent || persistent;
            return;
        }
    }

    ConnectedShareRecord record;
    record.shareName = shareName;
    record.persistent = persistent;
    connectedShares_.push_back(record);
}

void GB_SmbAccessor::ForgetConnectedShare(const std::wstring& shareName) const
{
    std::lock_guard<std::mutex> lock(connectedSharesMutex_);

    for (size_t i = 0; i < connectedShares_.size(); i++)
    {
        if (connectedShares_[i].shareName == shareName)
        {
            connectedShares_.erase(connectedShares_.begin() + i);
            return;
        }
    }
}

std::wstring GB_SmbAccessor::GetServerNameForUnc() const
{
    if (addressType_ == AddressType::IPv6Literal)
    {
        return ConvertIpv6LiteralToUncHost(hostOrIp_);
    }
    return hostOrIp_;
}

std::wstring GB_SmbAccessor::GetServerNameForNetApi() const
{
    // NetShareEnum 的 servername 形如 \\server
    const std::wstring server = GetServerNameForUnc();
    return L"\\\\" + server;
}

std::wstring GB_SmbAccessor::BuildUncRootInternal(const std::wstring& shareName, bool useLongPathPrefix) const
{
    const std::wstring server = GetServerNameForUnc();

    if (useLongPathPrefix)
    {
        // \\?\UNC\server\share
        return L"\\\\?\\UNC\\" + server + L"\\" + shareName;
    }
    // \\server\share
    return L"\\\\" + server + L"\\" + shareName;
}

std::wstring GB_SmbAccessor::BuildUncPathInternal(const std::wstring& shareName, const std::wstring& relativePath, bool useLongPathPrefix) const
{
    std::wstring root = BuildUncRootInternal(shareName, useLongPathPrefix);
    std::wstring rel = NormalizeSlashes(relativePath);

    if (rel.empty())
    {
        return root;
    }

    if (rel.front() == L'\\')
    {
        rel.erase(rel.begin());
    }

    // 对于 "\\?\\" 前缀的 UNC，Win32 不会自动折叠 "."/".."。
    // 为了让所有远端文件 API 的语义一致，这里把相对路径进行词法规范化。
    // shareName + relativePath 的组合语义应当被“钉死”在 share 根之下，
    // 因而不保留多余的 ".." 段。
    rel = CanonicalizeDotsInPath(rel, false);

    return JoinPath(root, rel);
}

std::wstring GB_SmbAccessor::ConvertIpv6LiteralToUncHost(const std::wstring& ipv6Literal)
{
    // UNC 下 IPv6 的常用写法：把 ':' -> '-'，追加 ".ipv6-literal.net"
    // scope id 的 '%' 需要转写（常见规则：'%' -> 's'）
    std::wstring out;
    out.reserve(ipv6Literal.size() + 32);

    for (size_t i = 0; i < ipv6Literal.size(); i++)
    {
        const wchar_t ch = ipv6Literal[i];
        if (ch == L':')
        {
            out.push_back(L'-');
        }
        else if (ch == L'%')
        {
            out.push_back(L's');
        }
        else
        {
            out.push_back(ch);
        }
    }

    out += L".ipv6-literal.net";
    return out;
}


#endif