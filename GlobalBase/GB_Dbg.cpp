#include "GB_Dbg.h"
#include "GB_Utf8String.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   define DBGHELP_TRANSLATE_TCHAR
#   include <Windows.h>
#   include <DbgHelp.h>
#   if defined(_MSC_VER)
#       include <intrin.h>
#   endif
#   pragma comment(lib, "Dbghelp.lib")
#endif

#if (defined(_WIN32) || defined(_WIN64)) && (defined(_M_X64) || defined(__x86_64__))
#   define GB_DBG_WINDOWS_X64 1
#endif

namespace
{
    const size_t GB_DbgMaxReasonableFrameCount = 1024;
    const size_t GB_DbgStackBufferFrameCount = 128;

#if defined(_WIN32) || defined(_WIN64)
    const size_t GB_DbgInitialPathBufferLength = 1024;
    const size_t GB_DbgMaxPathBufferLength = 32768;
    const DWORD GB_DbgMaxSymbolNameLength = 1024;
    const uint32_t GB_DbgExceptionPossibleDeadlock = 0xC0000194u;
    const uint32_t GB_DbgExceptionCppException = 0xE06D7363u;
    const uint32_t GB_DbgExceptionClrException = 0xE0434352u;
#endif

#if defined(_WIN32) || defined(_WIN64)
    size_t NormalizeFrameCount(size_t maxFrameCount)
    {
        return (std::min)(maxFrameCount, GB_DbgMaxReasonableFrameCount);
    }
#endif

    size_t SafeAddSizeT(size_t leftValue, size_t rightValue)
    {
        const size_t maxValue = (std::numeric_limits<size_t>::max)();
        if (leftValue > maxValue - rightValue)
        {
            return maxValue;
        }

        return leftValue + rightValue;
    }

    std::string FormatHexValue(uint64_t value, size_t width)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::uppercase << std::setw(static_cast<int>(width)) << std::setfill('0') << value;
        return stream.str();
    }

    std::string FormatAddress(uint64_t address)
    {
        return FormatHexValue(address, 16);
    }

#if defined(_WIN32) || defined(_WIN64)
    bool TryUtf8ToWideString(const std::string& textUtf8, std::wstring& wideString)
    {
        wideString.clear();
        if (textUtf8.empty())
        {
            return true;
        }

        if (textUtf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        {
            return false;
        }

        const int byteCount = static_cast<int>(textUtf8.size());
        const int wideLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, textUtf8.data(), byteCount, nullptr, 0);
        if (wideLength <= 0)
        {
            return false;
        }

        std::wstring result(static_cast<size_t>(wideLength), L'\0');
        const int convertedLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, textUtf8.data(), byteCount, &result[0], wideLength);
        if (convertedLength != wideLength)
        {
            return false;
        }

        wideString.swap(result);
        return true;
    }

    std::wstring Utf8ToWideString(const std::string& textUtf8)
    {
        std::wstring wideString;
        if (!TryUtf8ToWideString(textUtf8, wideString))
        {
            return std::wstring();
        }

        return wideString;
    }

    std::string WideStringToUtf8(const std::wstring& wideString)
    {
        if (wideString.empty())
        {
            return std::string();
        }

        if (wideString.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        {
            return std::string();
        }

        const int wideCount = static_cast<int>(wideString.size());
        const int utf8Length = ::WideCharToMultiByte(CP_UTF8, 0, wideString.data(), wideCount, nullptr, 0, nullptr, nullptr);
        if (utf8Length <= 0)
        {
            return std::string();
        }

        std::string utf8String(static_cast<size_t>(utf8Length), '\0');
        const int convertedLength = ::WideCharToMultiByte(CP_UTF8, 0, wideString.data(), wideCount, &utf8String[0], utf8Length, nullptr, nullptr);
        if (convertedLength != utf8Length)
        {
            return std::string();
        }

        return utf8String;
    }

    std::string WideCharArrayToUtf8(const wchar_t* text)
    {
        if (text == nullptr || text[0] == L'\0')
        {
            return std::string();
        }

        return WideStringToUtf8(std::wstring(text));
    }

    std::wstring TrimSystemMessage(const std::wstring& text)
    {
        size_t endPos = text.size();
        while (endPos > 0)
        {
            const wchar_t currentChar = text[endPos - 1];
            if (currentChar != L'\r' && currentChar != L'\n' && currentChar != L' ' && currentChar != L'\t' && currentChar != L'.')
            {
                break;
            }
            endPos--;
        }

        return text.substr(0, endPos);
    }

    std::wstring GetFileNameFromPath(const std::wstring& filePath)
    {
        const size_t pos = filePath.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
        {
            return filePath;
        }

        return filePath.substr(pos + 1);
    }

    std::wstring GetDirectoryFromPath(const std::wstring& filePath)
    {
        const size_t pos = filePath.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
        {
            return std::wstring();
        }

        return filePath.substr(0, pos);
    }

    void AppendSymbolPathPart(std::wstring& symbolSearchPath, const std::wstring& pathPart)
    {
        if (pathPart.empty())
        {
            return;
        }

        if (!symbolSearchPath.empty())
        {
            symbolSearchPath.push_back(L';');
        }

        symbolSearchPath.append(pathPart);
    }

    std::wstring GetModuleFilePath(HMODULE moduleHandle)
    {
        for (size_t bufferLength = GB_DbgInitialPathBufferLength; bufferLength <= GB_DbgMaxPathBufferLength; bufferLength *= 2)
        {
            std::vector<wchar_t> buffer(bufferLength, L'\0');
            ::SetLastError(ERROR_SUCCESS);
            const DWORD copiedLength = ::GetModuleFileNameW(moduleHandle, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copiedLength == 0)
            {
                return std::wstring();
            }

            if (copiedLength < buffer.size())
            {
                return std::wstring(buffer.data(), copiedLength);
            }
        }

        return std::wstring();
    }

    std::wstring GetCurrentDirectoryString()
    {
        const DWORD requiredLength = ::GetCurrentDirectoryW(0, nullptr);
        if (requiredLength == 0)
        {
            return std::wstring();
        }

        std::vector<wchar_t> buffer(static_cast<size_t>(requiredLength) + 1, L'\0');
        const DWORD copiedLength = ::GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (copiedLength == 0 || copiedLength >= buffer.size())
        {
            return std::wstring();
        }

        return std::wstring(buffer.data(), copiedLength);
    }

    std::wstring GetEnvironmentVariableString(const wchar_t* variableName)
    {
        if (variableName == nullptr || variableName[0] == L'\0')
        {
            return std::wstring();
        }

        const DWORD requiredLength = ::GetEnvironmentVariableW(variableName, nullptr, 0);
        if (requiredLength == 0)
        {
            return std::wstring();
        }

        std::vector<wchar_t> buffer(static_cast<size_t>(requiredLength) + 1, L'\0');
        const DWORD copiedLength = ::GetEnvironmentVariableW(variableName, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copiedLength == 0 || copiedLength >= buffer.size())
        {
            return std::wstring();
        }

        return std::wstring(buffer.data(), copiedLength);
    }

    std::wstring BuildDefaultSymbolSearchPath()
    {
        std::wstring symbolSearchPath;

        const std::wstring executablePath = GetModuleFilePath(nullptr);
        AppendSymbolPathPart(symbolSearchPath, GetDirectoryFromPath(executablePath));
        AppendSymbolPathPart(symbolSearchPath, GetCurrentDirectoryString());
        AppendSymbolPathPart(symbolSearchPath, GetEnvironmentVariableString(L"_NT_SYMBOL_PATH"));
        AppendSymbolPathPart(symbolSearchPath, GetEnvironmentVariableString(L"_NT_ALTERNATE_SYMBOL_PATH"));

        return symbolSearchPath;
    }

    uint32_t GetCurrentSystemErrorCode()
    {
        return static_cast<uint32_t>(::GetLastError());
    }

    MINIDUMP_TYPE ToMiniDumpType(GB_DbgMiniDumpLevel level)
    {
        DWORD miniDumpType = MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData | MiniDumpIgnoreInaccessibleMemory;

        if (level == GB_DbgMiniDumpLevel::WithDataSegments)
        {
            miniDumpType |= MiniDumpWithDataSegs;
        }
        else if (level == GB_DbgMiniDumpLevel::WithFullMemory)
        {
            miniDumpType |= MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo | MiniDumpWithHandleData | MiniDumpWithDataSegs;
        }

        return static_cast<MINIDUMP_TYPE>(miniDumpType);
    }

    HANDLE DuplicateCurrentProcessHandle()
    {
        HANDLE processHandle = nullptr;
        const HANDLE currentProcessPseudoHandle = ::GetCurrentProcess();
        const BOOL success = ::DuplicateHandle(currentProcessPseudoHandle, currentProcessPseudoHandle, currentProcessPseudoHandle, &processHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
        if (!success || processHandle == nullptr)
        {
            return currentProcessPseudoHandle;
        }

        return processHandle;
    }

    bool IsPseudoCurrentProcessHandle(HANDLE processHandle)
    {
        return processHandle == ::GetCurrentProcess();
    }

    ULONG SafeToUlong(size_t value)
    {
        const size_t maxUlongValue = static_cast<size_t>((std::numeric_limits<ULONG>::max)());
        return static_cast<ULONG>((std::min)(value, maxUlongValue));
    }

    void* AddressToPointer(uint64_t address)
    {
        if (address == 0 || address > static_cast<uint64_t>((std::numeric_limits<uintptr_t>::max)()))
        {
            return nullptr;
        }

        return reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    }

    void AppendFrameAddress(std::vector<uint64_t>& addresses, uint64_t address, size_t maxFrameCount)
    {
        if (address == 0 || addresses.size() >= maxFrameCount)
        {
            return;
        }

        if (!addresses.empty() && addresses.back() == address)
        {
            return;
        }

        addresses.push_back(address);
    }

    bool ReadUint64FromCurrentProcess(uint64_t address, uint64_t& value)
    {
        value = 0;
        if (address == 0 || address > static_cast<uint64_t>((std::numeric_limits<uintptr_t>::max)()))
        {
            return false;
        }

        SIZE_T bytesRead = 0;
        const BOOL success = ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), &value, sizeof(value), &bytesRead);
        return success == TRUE && bytesRead == sizeof(value);
    }

    bool UnwindOneFrameX64(CONTEXT& context)
    {
#if defined(GB_DBG_WINDOWS_X64)
        const uint64_t oldRip = static_cast<uint64_t>(context.Rip);
        if (oldRip == 0)
        {
            return false;
        }

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION runtimeFunction = ::RtlLookupFunctionEntry(static_cast<DWORD64>(context.Rip), &imageBase, nullptr);
        if (runtimeFunction != nullptr)
        {
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, static_cast<DWORD64>(context.Rip), runtimeFunction, &context, &handlerData, &establisherFrame, nullptr);
            return context.Rip != 0 && static_cast<uint64_t>(context.Rip) != oldRip;
        }

        uint64_t returnAddress = 0;
        if (!ReadUint64FromCurrentProcess(static_cast<uint64_t>(context.Rsp), returnAddress))
        {
            return false;
        }

        context.Rip = static_cast<DWORD64>(returnAddress);
        context.Rsp = static_cast<DWORD64>(context.Rsp + sizeof(uint64_t));
        return returnAddress != 0 && returnAddress != oldRip;
#else
        (void)context;
        return false;
#endif
    }

    std::vector<uint64_t> CaptureStackAddressesFromContextByUnwind(const void* contextRecord, size_t maxFrameCount)
    {
#if defined(GB_DBG_WINDOWS_X64)
        const size_t normalizedMaxFrameCount = NormalizeFrameCount(maxFrameCount);
        if (contextRecord == nullptr || normalizedMaxFrameCount == 0)
        {
            return std::vector<uint64_t>();
        }

        CONTEXT context = *reinterpret_cast<const CONTEXT*>(contextRecord);

        std::vector<uint64_t> addresses;
        addresses.reserve(normalizedMaxFrameCount);
        AppendFrameAddress(addresses, static_cast<uint64_t>(context.Rip), normalizedMaxFrameCount);

        size_t duplicateFrameCount = 0;
        while (addresses.size() < normalizedMaxFrameCount)
        {
            const bool success = UnwindOneFrameX64(context);
            if (!success || context.Rip == 0)
            {
                break;
            }

            const size_t oldFrameCount = addresses.size();
            AppendFrameAddress(addresses, static_cast<uint64_t>(context.Rip), normalizedMaxFrameCount);
            if (addresses.size() == oldFrameCount)
            {
                duplicateFrameCount++;
                if (duplicateFrameCount > 1)
                {
                    break;
                }
                continue;
            }

            duplicateFrameCount = 0;
        }

        return addresses;
#else
        (void)contextRecord;
        (void)maxFrameCount;
        return std::vector<uint64_t>();
#endif
    }

    struct GB_DbgRuntimeModuleInfo
    {
        HMODULE moduleHandle = nullptr;
        uint64_t moduleBase = 0;
        std::wstring modulePath;
        std::wstring moduleName;
        bool isValid = false;
    };

    bool GetRuntimeModuleInfo(uint64_t address, GB_DbgRuntimeModuleInfo& moduleInfo)
    {
        moduleInfo = GB_DbgRuntimeModuleInfo();

        void* addressPointer = AddressToPointer(address);
        if (addressPointer == nullptr)
        {
            return false;
        }

        HMODULE moduleHandle = nullptr;
        const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (!::GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(addressPointer), &moduleHandle) || moduleHandle == nullptr)
        {
            return false;
        }

        moduleInfo.moduleHandle = moduleHandle;
        moduleInfo.moduleBase = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(moduleHandle));
        moduleInfo.modulePath = GetModuleFilePath(moduleHandle);
        moduleInfo.moduleName = GetFileNameFromPath(moduleInfo.modulePath);
        moduleInfo.isValid = moduleInfo.moduleBase != 0;
        return moduleInfo.isValid;
    }

    void FillFrameModuleByRuntimeInfo(const GB_DbgRuntimeModuleInfo& moduleInfo, GB_DbgStackFrame& frame)
    {
        if (!moduleInfo.isValid || frame.hasModule)
        {
            return;
        }

        frame.modulePathUtf8 = WideStringToUtf8(moduleInfo.modulePath);
        frame.moduleNameUtf8 = WideStringToUtf8(moduleInfo.moduleName);
        frame.hasModule = !frame.modulePathUtf8.empty() || !frame.moduleNameUtf8.empty();
    }

    class GB_DbgFileHandleScope
    {
    public:
        explicit GB_DbgFileHandleScope(HANDLE fileHandle)
            : m_fileHandle(fileHandle)
        {
        }

        ~GB_DbgFileHandleScope()
        {
            if (m_fileHandle != nullptr && m_fileHandle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(m_fileHandle);
                m_fileHandle = nullptr;
            }
        }

        GB_DbgFileHandleScope(const GB_DbgFileHandleScope&) = delete;
        GB_DbgFileHandleScope& operator = (const GB_DbgFileHandleScope&) = delete;

        HANDLE Get() const
        {
            return m_fileHandle;
        }

        bool IsValid() const
        {
            return m_fileHandle != nullptr && m_fileHandle != INVALID_HANDLE_VALUE;
        }

    private:
        HANDLE m_fileHandle = nullptr;
    };


    class GB_DbgLocalMemoryScope
    {
    public:
        explicit GB_DbgLocalMemoryScope(HLOCAL localMemory)
            : m_localMemory(localMemory)
        {
        }

        ~GB_DbgLocalMemoryScope()
        {
            if (m_localMemory != nullptr)
            {
                ::LocalFree(m_localMemory);
                m_localMemory = nullptr;
            }
        }

        GB_DbgLocalMemoryScope(const GB_DbgLocalMemoryScope&) = delete;
        GB_DbgLocalMemoryScope& operator = (const GB_DbgLocalMemoryScope&) = delete;

    private:
        HLOCAL m_localMemory = nullptr;
    };

    class GB_DbgSymbolEngine
    {
    public:
        static GB_DbgSymbolEngine& Instance()
        {
            static GB_DbgSymbolEngine engine;
            return engine;
        }

        bool Initialize(const GB_DbgSymbolOptions& options)
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            if (m_initialized && m_referenceCount > 0)
            {
                if (m_referenceCount == (std::numeric_limits<size_t>::max)())
                {
                    return false;
                }

                m_referenceCount++;
                return true;
            }

            if (m_initialized && m_referenceCount == 0)
            {
                ::SymCleanup(m_processHandle);
                m_initialized = false;
                m_initializedOptions = GB_DbgSymbolOptions();
                m_symbolBuffer.clear();
            }

            if (!InitializeLocked(options))
            {
                return false;
            }

            m_referenceCount = 1;
            return true;
        }

        void Cleanup()
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            if (!m_initialized)
            {
                return;
            }

            if (m_referenceCount > 0)
            {
                m_referenceCount--;
            }

            if (m_referenceCount > 0)
            {
                return;
            }

            ::SymCleanup(m_processHandle);
            m_initialized = false;
            m_referenceCount = 0;
            m_initializedOptions = GB_DbgSymbolOptions();
            m_symbolBuffer.clear();
        }

        bool RefreshModuleList()
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            if (!EnsureInitializedLocked())
            {
                return false;
            }

            return ::SymRefreshModuleList(m_processHandle) == TRUE;
        }

        bool IsInitialized() const
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            return m_initialized;
        }

        std::vector<uint64_t> CaptureCurrentStackAddresses(size_t skipFrameCount, size_t maxFrameCount)
        {
#if defined(GB_DBG_WINDOWS_X64)
            return CaptureCurrentStackAddressesInternal(skipFrameCount, 3, maxFrameCount);
#else
            (void)skipFrameCount;
            (void)maxFrameCount;
            return std::vector<uint64_t>();
#endif
        }

        GB_DbgStackTrace CaptureCurrentStackTrace(size_t skipFrameCount, size_t maxFrameCount, bool resolveSymbols)
        {
#if defined(GB_DBG_WINDOWS_X64)
            const std::vector<uint64_t> addresses = CaptureCurrentStackAddressesInternal(skipFrameCount, 3, maxFrameCount);
            if (resolveSymbols)
            {
                return ResolveAddresses(addresses);
            }

            return BuildUnresolvedStackTrace(addresses);
#else
            (void)skipFrameCount;
            (void)maxFrameCount;
            (void)resolveSymbols;
            return GB_DbgStackTrace();
#endif
        }

        std::vector<uint64_t> CaptureStackAddressesFromContext(const void* contextRecord, size_t maxFrameCount)
        {
#if defined(GB_DBG_WINDOWS_X64)
            return CaptureStackAddressesFromContextByUnwind(contextRecord, maxFrameCount);
#else
            (void)contextRecord;
            (void)maxFrameCount;
            return std::vector<uint64_t>();
#endif
        }

        GB_DbgStackTrace CaptureStackTraceFromContext(const void* contextRecord, size_t maxFrameCount, bool resolveSymbols)
        {
#if defined(GB_DBG_WINDOWS_X64)
            const std::vector<uint64_t> addresses = CaptureStackAddressesFromContext(contextRecord, maxFrameCount);
            if (resolveSymbols)
            {
                return ResolveAddresses(addresses);
            }

            return BuildUnresolvedStackTrace(addresses);
#else
            (void)contextRecord;
            (void)maxFrameCount;
            (void)resolveSymbols;
            return GB_DbgStackTrace();
#endif
        }

        GB_DbgStackTrace ResolveStackTraceAddresses(const std::vector<uint64_t>& addresses)
        {
            return ResolveAddresses(addresses);
        }

        bool ResolveAddress(uint64_t address, GB_DbgStackFrame& frame)
        {
            frame = GB_DbgStackFrame();
            frame.address = address;

            if (address == 0)
            {
                return false;
            }

            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            if (EnsureInitializedLocked())
            {
                ResolveAddressLocked(address, frame);
            }
            else
            {
                GB_DbgRuntimeModuleInfo runtimeModuleInfo;
                if (GetRuntimeModuleInfo(address, runtimeModuleInfo))
                {
                    FillFrameModuleByRuntimeInfo(runtimeModuleInfo, frame);
                }
            }

            return frame.hasModule || frame.hasFunction || frame.hasSourceLocation;
        }

        bool WriteMiniDump(const std::string& dumpFilePathUtf8, const void* exceptionPointers, uint32_t exceptionThreadId, GB_DbgMiniDumpLevel level)
        {
            if (dumpFilePathUtf8.empty())
            {
                return false;
            }

            std::wstring dumpFilePath;
            if (!TryUtf8ToWideString(dumpFilePathUtf8, dumpFilePath) || dumpFilePath.empty())
            {
                return false;
            }

            GB_DbgFileHandleScope dumpFileHandle(::CreateFileW(dumpFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!dumpFileHandle.IsValid())
            {
                return false;
            }

            MINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
            std::memset(&exceptionInfo, 0, sizeof(exceptionInfo));
            MINIDUMP_EXCEPTION_INFORMATION* exceptionInfoPtr = nullptr;

            if (exceptionPointers != nullptr)
            {
                exceptionInfo.ThreadId = exceptionThreadId == 0 ? ::GetCurrentThreadId() : static_cast<DWORD>(exceptionThreadId);
                exceptionInfo.ExceptionPointers = const_cast<PEXCEPTION_POINTERS>(reinterpret_cast<const EXCEPTION_POINTERS*>(exceptionPointers));
                exceptionInfo.ClientPointers = FALSE;
                exceptionInfoPtr = &exceptionInfo;
            }

            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            const BOOL success = ::MiniDumpWriteDump(m_processHandle, ::GetCurrentProcessId(), dumpFileHandle.Get(), ToMiniDumpType(level), exceptionInfoPtr, nullptr, nullptr);
            return success == TRUE;
        }

    private:
        GB_DbgSymbolEngine()
            : m_processHandle(DuplicateCurrentProcessHandle())
        {
        }

        ~GB_DbgSymbolEngine()
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            if (m_initialized)
            {
                ::SymCleanup(m_processHandle);
                m_initialized = false;
                m_referenceCount = 0;
            }

            if (m_processHandle != nullptr && !IsPseudoCurrentProcessHandle(m_processHandle))
            {
                ::CloseHandle(m_processHandle);
                m_processHandle = nullptr;
            }
        }

        bool EnsureInitializedLocked()
        {
            if (m_initialized)
            {
                return true;
            }

            return InitializeLocked(GB_DbgSymbolOptions());
        }

        bool InitializeLocked(const GB_DbgSymbolOptions& options)
        {
            if (m_initialized)
            {
                return true;
            }

            DWORD symOptions = ::SymGetOptions();
            if (options.loadLineInfo)
            {
                symOptions |= SYMOPT_LOAD_LINES;
            }
            else
            {
                symOptions &= ~SYMOPT_LOAD_LINES;
            }

            if (options.undecorateSymbolNames)
            {
                symOptions |= SYMOPT_UNDNAME;
            }
            else
            {
                symOptions &= ~SYMOPT_UNDNAME;
            }

            if (options.deferredLoadSymbols)
            {
                symOptions |= SYMOPT_DEFERRED_LOADS;
            }
            else
            {
                symOptions &= ~SYMOPT_DEFERRED_LOADS;
            }

            if (options.failCriticalErrors)
            {
                symOptions |= SYMOPT_FAIL_CRITICAL_ERRORS;
            }
            else
            {
                symOptions &= ~SYMOPT_FAIL_CRITICAL_ERRORS;
            }

            if (options.noSymbolPrompts)
            {
                symOptions |= SYMOPT_NO_PROMPTS;
            }
            else
            {
                symOptions &= ~SYMOPT_NO_PROMPTS;
            }

            ::SymSetOptions(symOptions);

            std::wstring symbolSearchPath;
            if (!options.symbolSearchPathUtf8.empty())
            {
                if (!TryUtf8ToWideString(options.symbolSearchPathUtf8, symbolSearchPath))
                {
                    return false;
                }
            }
            else
            {
                symbolSearchPath = BuildDefaultSymbolSearchPath();
            }

            const wchar_t* symbolSearchPathPtr = symbolSearchPath.empty() ? nullptr : symbolSearchPath.c_str();
            const BOOL invadeProcess = options.loadModules ? TRUE : FALSE;

            if (!::SymInitializeW(m_processHandle, symbolSearchPathPtr, invadeProcess))
            {
                return false;
            }

            m_initialized = true;
            m_initializedOptions = options;
            return true;
        }

        std::vector<uint64_t> CaptureCurrentStackAddressesInternal(size_t skipFrameCount, size_t internalFrameCount, size_t maxFrameCount)
        {
            const size_t normalizedMaxFrameCount = NormalizeFrameCount(maxFrameCount);
            if (normalizedMaxFrameCount == 0)
            {
                return std::vector<uint64_t>();
            }

            void* stackRawAddresses[GB_DbgStackBufferFrameCount] = {};
            std::vector<void*> heapRawAddresses;
            void** rawAddresses = stackRawAddresses;
            if (normalizedMaxFrameCount > GB_DbgStackBufferFrameCount)
            {
                heapRawAddresses.assign(normalizedMaxFrameCount, nullptr);
                rawAddresses = heapRawAddresses.data();
            }

            const size_t internalSkipFrameCount = SafeAddSizeT(skipFrameCount, internalFrameCount);
            const USHORT capturedFrameCount = ::CaptureStackBackTrace(SafeToUlong(internalSkipFrameCount), SafeToUlong(normalizedMaxFrameCount), rawAddresses, nullptr);

            std::vector<uint64_t> addresses;
            addresses.reserve(capturedFrameCount);
            for (USHORT i = 0; i < capturedFrameCount; i++)
            {
                const uint64_t address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rawAddresses[i]));
                AppendFrameAddress(addresses, address, normalizedMaxFrameCount);
            }

            return addresses;
        }

        GB_DbgStackTrace BuildUnresolvedStackTrace(const std::vector<uint64_t>& addresses) const
        {
            GB_DbgStackTrace stackTrace;
            stackTrace.frames.reserve(addresses.size());

            for (const uint64_t address : addresses)
            {
                GB_DbgStackFrame frame;
                frame.address = address;
                stackTrace.frames.push_back(frame);
            }

            return stackTrace;
        }

        GB_DbgStackTrace ResolveAddresses(const std::vector<uint64_t>& addresses)
        {
            GB_DbgStackTrace stackTrace;
            stackTrace.frames.reserve(addresses.size());

            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            const bool canResolveSymbols = EnsureInitializedLocked();
            for (const uint64_t address : addresses)
            {
                GB_DbgStackFrame frame;
                frame.address = address;
                if (canResolveSymbols)
                {
                    ResolveAddressLocked(address, frame);
                }
                else
                {
                    GB_DbgRuntimeModuleInfo runtimeModuleInfo;
                    if (GetRuntimeModuleInfo(address, runtimeModuleInfo))
                    {
                        FillFrameModuleByRuntimeInfo(runtimeModuleInfo, frame);
                    }
                }
                stackTrace.frames.push_back(frame);
            }

            return stackTrace;
        }

        void ResolveAddressLocked(uint64_t address, GB_DbgStackFrame& frame)
        {
            frame = GB_DbgStackFrame();
            frame.address = address;

            if (address == 0)
            {
                return;
            }

            GB_DbgRuntimeModuleInfo runtimeModuleInfo;
            bool hasRuntimeModuleInfo = false;
            if (::SymGetModuleBase64(m_processHandle, static_cast<DWORD64>(address)) == 0)
            {
                hasRuntimeModuleInfo = GetRuntimeModuleInfo(address, runtimeModuleInfo);
                if (hasRuntimeModuleInfo)
                {
                    const wchar_t* modulePath = runtimeModuleInfo.modulePath.empty() ? nullptr : runtimeModuleInfo.modulePath.c_str();
                    const wchar_t* moduleName = runtimeModuleInfo.moduleName.empty() ? nullptr : runtimeModuleInfo.moduleName.c_str();
                    ::SymLoadModuleExW(m_processHandle, nullptr, modulePath, moduleName, static_cast<DWORD64>(runtimeModuleInfo.moduleBase), 0, nullptr, 0);
                }
            }

            ResolveModuleByDbgHelpLocked(address, frame);
            if (!frame.hasModule)
            {
                if (!hasRuntimeModuleInfo)
                {
                    hasRuntimeModuleInfo = GetRuntimeModuleInfo(address, runtimeModuleInfo);
                }

                if (hasRuntimeModuleInfo)
                {
                    FillFrameModuleByRuntimeInfo(runtimeModuleInfo, frame);
                }
            }

            ResolveSymbolByDbgHelpLocked(address, frame);
            ResolveLineByDbgHelpLocked(address, frame);
        }

        void ResolveModuleByDbgHelpLocked(uint64_t address, GB_DbgStackFrame& frame)
        {
            IMAGEHLP_MODULEW64 moduleInfo;
            std::memset(&moduleInfo, 0, sizeof(moduleInfo));
            moduleInfo.SizeOfStruct = sizeof(moduleInfo);
            if (!::SymGetModuleInfoW64(m_processHandle, static_cast<DWORD64>(address), &moduleInfo))
            {
                return;
            }

            frame.moduleNameUtf8 = WideCharArrayToUtf8(moduleInfo.ModuleName);
            frame.modulePathUtf8 = WideCharArrayToUtf8(moduleInfo.LoadedImageName);
            if (frame.modulePathUtf8.empty())
            {
                frame.modulePathUtf8 = WideCharArrayToUtf8(moduleInfo.ImageName);
            }

            if (frame.moduleNameUtf8.empty() && !frame.modulePathUtf8.empty())
            {
                const std::wstring modulePath = Utf8ToWideString(frame.modulePathUtf8);
                frame.moduleNameUtf8 = WideStringToUtf8(GetFileNameFromPath(modulePath));
            }

            frame.hasModule = !frame.moduleNameUtf8.empty() || !frame.modulePathUtf8.empty();
        }

        bool EnsureSymbolBufferLocked()
        {
            const size_t symbolBufferSize = sizeof(SYMBOL_INFOW) + (static_cast<size_t>(GB_DbgMaxSymbolNameLength) + 1) * sizeof(wchar_t);
            if (m_symbolBuffer.size() >= symbolBufferSize)
            {
                return true;
            }

            try
            {
                m_symbolBuffer.resize(symbolBufferSize);
            }
            catch (...)
            {
                m_symbolBuffer.clear();
                return false;
            }

            return true;
        }

        void ResolveSymbolByDbgHelpLocked(uint64_t address, GB_DbgStackFrame& frame)
        {
            if (!EnsureSymbolBufferLocked())
            {
                return;
            }

            std::memset(m_symbolBuffer.data(), 0, m_symbolBuffer.size());
            SYMBOL_INFOW* symbolInfo = reinterpret_cast<SYMBOL_INFOW*>(m_symbolBuffer.data());
            symbolInfo->SizeOfStruct = sizeof(SYMBOL_INFOW);
            symbolInfo->MaxNameLen = GB_DbgMaxSymbolNameLength;

            DWORD64 symbolDisplacement = 0;
            if (!::SymFromAddrW(m_processHandle, static_cast<DWORD64>(address), &symbolDisplacement, symbolInfo))
            {
                return;
            }

            if (symbolInfo->NameLen > 0)
            {
                frame.functionNameUtf8 = WideStringToUtf8(std::wstring(symbolInfo->Name, symbolInfo->NameLen));
            }
            else
            {
                frame.functionNameUtf8 = WideCharArrayToUtf8(symbolInfo->Name);
            }
            frame.symbolDisplacement = static_cast<uint64_t>(symbolDisplacement);
            frame.hasFunction = !frame.functionNameUtf8.empty();
        }

        void ResolveLineByDbgHelpLocked(uint64_t address, GB_DbgStackFrame& frame)
        {
            if (!m_initializedOptions.loadLineInfo)
            {
                return;
            }

            IMAGEHLP_LINEW64 lineInfo;
            std::memset(&lineInfo, 0, sizeof(lineInfo));
            lineInfo.SizeOfStruct = sizeof(lineInfo);

            DWORD lineDisplacement = 0;
            if (!::SymGetLineFromAddrW64(m_processHandle, static_cast<DWORD64>(address), &lineDisplacement, &lineInfo))
            {
                return;
            }

            frame.sourceFilePathUtf8 = WideCharArrayToUtf8(lineInfo.FileName);
            frame.sourceLine = static_cast<uint32_t>(lineInfo.LineNumber);
            frame.lineDisplacement = static_cast<uint32_t>(lineDisplacement);
            frame.hasSourceLocation = !frame.sourceFilePathUtf8.empty() && frame.sourceLine > 0;
        }

    private:
        HANDLE m_processHandle = nullptr;
        bool m_initialized = false;
        size_t m_referenceCount = 0;
        GB_DbgSymbolOptions m_initializedOptions;
        std::vector<unsigned char> m_symbolBuffer;
        mutable std::recursive_mutex m_mutex;
    };
#endif
}

GB_DbgSymbolScope::GB_DbgSymbolScope(const GB_DbgSymbolOptions& options)
    : m_initialized(GB_DbgInitializeSymbols(options))
{
}

GB_DbgSymbolScope::~GB_DbgSymbolScope()
{
    if (m_initialized)
    {
        GB_DbgCleanupSymbols();
    }
}

bool GB_DbgSymbolScope::IsInitialized() const
{
    return m_initialized;
}

bool GB_DbgInitializeSymbols(const GB_DbgSymbolOptions& options)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        return GB_DbgSymbolEngine::Instance().Initialize(options);
    }
    catch (...)
    {
        return false;
    }
#else
    (void)options;
    return false;
#endif
}

void GB_DbgCleanupSymbols()
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        GB_DbgSymbolEngine::Instance().Cleanup();
    }
    catch (...)
    {
    }
#endif
}

bool GB_DbgRefreshSymbols()
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        return GB_DbgSymbolEngine::Instance().RefreshModuleList();
    }
    catch (...)
    {
        return false;
    }
#else
    return false;
#endif
}

bool GB_DbgIsSymbolEngineInitialized()
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        return GB_DbgSymbolEngine::Instance().IsInitialized();
    }
    catch (...)
    {
        return false;
    }
#else
    return false;
#endif
}

bool GB_DbgIsDebuggerPresent()
{
#if defined(_WIN32) || defined(_WIN64)
    return ::IsDebuggerPresent() == TRUE;
#else
    return false;
#endif
}

void GB_DbgBreak()
{
#if defined(_MSC_VER)
    __debugbreak();
#elif defined(_WIN32) || defined(_WIN64)
    ::DebugBreak();
#endif
}

void GB_DbgBreakIfDebuggerPresent()
{
    if (GB_DbgIsDebuggerPresent())
    {
        GB_DbgBreak();
    }
}

GB_DbgStackTrace GB_DbgCaptureStackTrace(size_t skipFrameCount, size_t maxFrameCount, bool resolveSymbols)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        return GB_DbgSymbolEngine::Instance().CaptureCurrentStackTrace(skipFrameCount, maxFrameCount, resolveSymbols);
    }
    catch (...)
    {
        return GB_DbgStackTrace();
    }
#else
    (void)skipFrameCount;
    (void)maxFrameCount;
    (void)resolveSymbols;
    return GB_DbgStackTrace();
#endif
}

std::vector<uint64_t> GB_DbgCaptureStackTraceAddresses(size_t skipFrameCount, size_t maxFrameCount)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        return GB_DbgSymbolEngine::Instance().CaptureCurrentStackAddresses(skipFrameCount, maxFrameCount);
    }
    catch (...)
    {
        return std::vector<uint64_t>();
    }
#else
    (void)skipFrameCount;
    (void)maxFrameCount;
    return std::vector<uint64_t>();
#endif
}

GB_DbgStackTrace GB_DbgCaptureStackTraceFromContext(const void* contextRecord, size_t maxFrameCount, bool resolveSymbols)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        return GB_DbgSymbolEngine::Instance().CaptureStackTraceFromContext(contextRecord, maxFrameCount, resolveSymbols);
    }
    catch (...)
    {
        return GB_DbgStackTrace();
    }
#else
    (void)contextRecord;
    (void)maxFrameCount;
    (void)resolveSymbols;
    return GB_DbgStackTrace();
#endif
}

std::vector<uint64_t> GB_DbgCaptureStackTraceAddressesFromContext(const void* contextRecord, size_t maxFrameCount)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        return GB_DbgSymbolEngine::Instance().CaptureStackAddressesFromContext(contextRecord, maxFrameCount);
    }
    catch (...)
    {
        return std::vector<uint64_t>();
    }
#else
    (void)contextRecord;
    (void)maxFrameCount;
    return std::vector<uint64_t>();
#endif
}

GB_DbgStackTrace GB_DbgCaptureStackTraceFromExceptionPointers(const void* exceptionPointers, size_t maxFrameCount, bool resolveSymbols)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        if (exceptionPointers == nullptr)
        {
            return GB_DbgStackTrace();
        }

        const EXCEPTION_POINTERS* sehExceptionPointers = reinterpret_cast<const EXCEPTION_POINTERS*>(exceptionPointers);
        if (sehExceptionPointers->ContextRecord == nullptr)
        {
            return GB_DbgStackTrace();
        }

        return GB_DbgCaptureStackTraceFromContext(sehExceptionPointers->ContextRecord, maxFrameCount, resolveSymbols);
    }
    catch (...)
    {
        return GB_DbgStackTrace();
    }
#else
    (void)exceptionPointers;
    (void)maxFrameCount;
    (void)resolveSymbols;
    return GB_DbgStackTrace();
#endif
}

std::vector<uint64_t> GB_DbgCaptureStackTraceAddressesFromExceptionPointers(const void* exceptionPointers, size_t maxFrameCount)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        if (exceptionPointers == nullptr)
        {
            return std::vector<uint64_t>();
        }

        const EXCEPTION_POINTERS* sehExceptionPointers = reinterpret_cast<const EXCEPTION_POINTERS*>(exceptionPointers);
        if (sehExceptionPointers->ContextRecord == nullptr)
        {
            return std::vector<uint64_t>();
        }

        return GB_DbgCaptureStackTraceAddressesFromContext(sehExceptionPointers->ContextRecord, maxFrameCount);
    }
    catch (...)
    {
        return std::vector<uint64_t>();
    }
#else
    (void)exceptionPointers;
    (void)maxFrameCount;
    return std::vector<uint64_t>();
#endif
}

bool GB_DbgResolveAddress(uint64_t address, GB_DbgStackFrame& frame)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        return GB_DbgSymbolEngine::Instance().ResolveAddress(address, frame);
    }
    catch (...)
    {
        frame = GB_DbgStackFrame();
        frame.address = address;
        return false;
    }
#else
    frame = GB_DbgStackFrame();
    frame.address = address;
    return false;
#endif
}


GB_DbgStackTrace GB_DbgResolveStackTraceAddresses(const std::vector<uint64_t>& addresses)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        return GB_DbgSymbolEngine::Instance().ResolveStackTraceAddresses(addresses);
    }
    catch (...)
    {
        GB_DbgStackTrace stackTrace;
        stackTrace.frames.reserve(addresses.size());
        for (const uint64_t address : addresses)
        {
            GB_DbgStackFrame frame;
            frame.address = address;
            stackTrace.frames.push_back(frame);
        }
        return stackTrace;
    }
#else
    GB_DbgStackTrace stackTrace;
    stackTrace.frames.reserve(addresses.size());
    for (const uint64_t address : addresses)
    {
        GB_DbgStackFrame frame;
        frame.address = address;
        stackTrace.frames.push_back(frame);
    }
    return stackTrace;
#endif
}

std::string GB_DbgFormatStackTrace(const GB_DbgStackTrace& stackTrace, bool withFrameIndex)
{
    try
    {
        std::ostringstream stream;

        for (size_t i = 0; i < stackTrace.frames.size(); i++)
        {
            const GB_DbgStackFrame& frame = stackTrace.frames[i];

            if (withFrameIndex)
            {
                stream << "#" << std::setw(2) << std::setfill('0') << i << " ";
            }

            stream << FormatAddress(frame.address);

            if (frame.hasModule && !frame.moduleNameUtf8.empty())
            {
                stream << " " << frame.moduleNameUtf8 << "!";
            }
            else
            {
                stream << " ";
            }

            if (frame.hasFunction)
            {
                stream << frame.functionNameUtf8;
                if (frame.symbolDisplacement != 0)
                {
                    stream << " + " << FormatHexValue(frame.symbolDisplacement, 0);
                }
            }
            else
            {
                stream << "(Function name unavailable)";
            }

            if (frame.hasSourceLocation)
            {
                stream << " [" << frame.sourceFilePathUtf8 << ":" << frame.sourceLine;
                if (frame.lineDisplacement != 0)
                {
                    stream << " + " << frame.lineDisplacement;
                }
                stream << "]";
            }
            else if (frame.hasModule && frame.moduleNameUtf8.empty() && !frame.modulePathUtf8.empty())
            {
                stream << " [" << frame.modulePathUtf8 << "]";
            }

            stream << "\n";
        }

        return stream.str();
    }
    catch (...)
    {
        return std::string();
    }
}

std::string GB_DbgFormatSystemErrorMessage(uint32_t errorCode)
{
    try
    {
#if defined(_WIN32) || defined(_WIN64)
        wchar_t* messageBuffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD messageLength = ::FormatMessageW(flags, nullptr, static_cast<DWORD>(errorCode), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&messageBuffer), 0, nullptr);

        GB_DbgLocalMemoryScope messageBufferScope(messageBuffer);
        if (messageLength > 0 && messageBuffer != nullptr)
        {
            const std::wstring message = TrimSystemMessage(std::wstring(messageBuffer, messageLength));

            std::ostringstream stream;
            stream << WideStringToUtf8(message) << " (" << FormatHexValue(errorCode, 8) << ")";
            return stream.str();
        }
#endif

        std::ostringstream stream;
        stream << "Unknown system error (" << FormatHexValue(errorCode, 8) << ")";
        return stream.str();
    }
    catch (...)
    {
        return std::string("Unknown system error");
    }
}

std::string GB_DbgGetLastSystemErrorMessage()
{
#if defined(_WIN32) || defined(_WIN64)
    return GB_DbgFormatSystemErrorMessage(GetCurrentSystemErrorCode());
#else
    return GB_DbgFormatSystemErrorMessage(0);
#endif
}

void GB_DbgOutputDebugStringUtf8(const std::string& textUtf8)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        if (textUtf8.empty())
        {
            return;
        }

        const std::wstring wideText = Utf8ToWideString(textUtf8);
        if (!wideText.empty())
        {
            ::OutputDebugStringW(wideText.c_str());
        }
        else
        {
            ::OutputDebugStringA(textUtf8.c_str());
        }
    }
    catch (...)
    {
    }
#else
    (void)textUtf8;
#endif
}

void GB_DbgOutputCurrentStackTrace(size_t skipFrameCount, size_t maxFrameCount)
{
    const GB_DbgStackTrace stackTrace = GB_DbgCaptureStackTrace(SafeAddSizeT(skipFrameCount, 1), maxFrameCount);
    GB_DbgOutputDebugStringUtf8(GB_DbgFormatStackTrace(stackTrace));
}

std::string GB_DbgGetExceptionCodeName(uint32_t exceptionCode)
{
#if defined(_WIN32) || defined(_WIN64)
    switch (exceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:
        return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:
        return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:
        return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:
        return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:
        return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:
        return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_GUARD_PAGE:
        return "EXCEPTION_GUARD_PAGE";
    case EXCEPTION_INVALID_HANDLE:
        return "EXCEPTION_INVALID_HANDLE";
    case CONTROL_C_EXIT:
        return "CONTROL_C_EXIT";
    case GB_DbgExceptionPossibleDeadlock:
        return "EXCEPTION_POSSIBLE_DEADLOCK";
    case GB_DbgExceptionCppException:
        return "MSVC_CPP_EXCEPTION";
    case GB_DbgExceptionClrException:
        return "CLR_EXCEPTION";
    default:
        return "UNKNOWN_EXCEPTION";
    }
#else
    (void)exceptionCode;
    return "UNKNOWN_EXCEPTION";
#endif
}

std::string GB_DbgGetExceptionCodeDescription(uint32_t exceptionCode)
{
#if defined(_WIN32) || defined(_WIN64)
    switch (exceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return GB_STR("访问冲突：通常是空指针、野指针、越界读写或访问无权限内存。");
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return GB_STR("数组越界异常。");
    case EXCEPTION_BREAKPOINT:
        return GB_STR("断点异常。");
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return GB_STR("数据未按要求对齐。");
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return GB_STR("浮点非规格化操作数异常。");
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return GB_STR("浮点除零异常。");
    case EXCEPTION_FLT_INEXACT_RESULT:
        return GB_STR("浮点结果不精确异常。");
    case EXCEPTION_FLT_INVALID_OPERATION:
        return GB_STR("无效浮点操作异常。");
    case EXCEPTION_FLT_OVERFLOW:
        return GB_STR("浮点上溢异常。");
    case EXCEPTION_FLT_STACK_CHECK:
        return GB_STR("浮点栈检查异常。");
    case EXCEPTION_FLT_UNDERFLOW:
        return GB_STR("浮点下溢异常。");
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return GB_STR("非法指令异常。");
    case EXCEPTION_IN_PAGE_ERROR:
        return GB_STR("页错误：访问的页无法调入内存，可能与映射文件、磁盘或硬件错误有关。");
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return GB_STR("整数除零异常。");
    case EXCEPTION_INT_OVERFLOW:
        return GB_STR("整数溢出异常。");
    case EXCEPTION_INVALID_DISPOSITION:
        return GB_STR("异常处理器返回了无效处置结果。");
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return GB_STR("不可继续异常。");
    case EXCEPTION_PRIV_INSTRUCTION:
        return GB_STR("特权指令异常。");
    case EXCEPTION_SINGLE_STEP:
        return GB_STR("单步调试异常。");
    case EXCEPTION_STACK_OVERFLOW:
        return GB_STR("栈溢出异常。");
    case EXCEPTION_GUARD_PAGE:
        return GB_STR("保护页异常。");
    case EXCEPTION_INVALID_HANDLE:
        return GB_STR("无效句柄异常。");
    case CONTROL_C_EXIT:
        return GB_STR("控制台 Ctrl+C 或 Ctrl+Break 退出事件。");
    case GB_DbgExceptionPossibleDeadlock:
        return GB_STR("可能发生死锁。");
    case GB_DbgExceptionCppException:
        return GB_STR("MSVC C++ 异常。若该异常最终被捕获，通常不代表崩溃；若未处理，则需要结合调用栈和异常对象继续分析。");
    case GB_DbgExceptionClrException:
        return GB_STR("CLR/.NET 异常。通常出现在托管代码或混合托管/非托管进程中。");
    default:
        return GB_STR("未知 Windows SEH 异常。");
    }
#else
    (void)exceptionCode;
    return GB_STR("未知异常。");
#endif
}

bool GB_DbgWriteMiniDump(const std::string& dumpFilePathUtf8, const void* exceptionPointers, GB_DbgMiniDumpLevel level)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        return GB_DbgSymbolEngine::Instance().WriteMiniDump(dumpFilePathUtf8, exceptionPointers, 0, level);
    }
    catch (...)
    {
        return false;
    }
#else
    (void)dumpFilePathUtf8;
    (void)exceptionPointers;
    (void)level;
    return false;
#endif
}

bool GB_DbgWriteMiniDumpEx(const std::string& dumpFilePathUtf8, const void* exceptionPointers, uint32_t exceptionThreadId, GB_DbgMiniDumpLevel level)
{
#if defined(_WIN32) || defined(_WIN64)
    try
    {
        return GB_DbgSymbolEngine::Instance().WriteMiniDump(dumpFilePathUtf8, exceptionPointers, exceptionThreadId, level);
    }
    catch (...)
    {
        return false;
    }
#else
    (void)dumpFilePathUtf8;
    (void)exceptionPointers;
    (void)exceptionThreadId;
    (void)level;
    return false;
#endif
}
