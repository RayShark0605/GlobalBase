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

namespace
{
    const size_t GB_DbgMaxReasonableFrameCount = 1024;

#if defined(_WIN32) || defined(_WIN64)
    const size_t GB_DbgInitialPathBufferLength = 1024;
    const size_t GB_DbgMaxPathBufferLength = 32768;
    const uint32_t GB_DbgExceptionPossibleDeadlock = 0xC0000194u;
    const uint32_t GB_DbgExceptionCppException = 0xE06D7363u;
    const uint32_t GB_DbgExceptionClrException = 0xE0434352u;
#endif

    size_t NormalizeFrameCount(size_t maxFrameCount)
    {
        return (std::min)(maxFrameCount, GB_DbgMaxReasonableFrameCount);
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
    std::wstring Utf8ToWideString(const std::string& textUtf8)
    {
        if (textUtf8.empty())
        {
            return std::wstring();
        }

        if (textUtf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        {
            return std::wstring();
        }

        const int byteCount = static_cast<int>(textUtf8.size());
        const int wideLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, textUtf8.data(), byteCount, nullptr, 0);
        if (wideLength <= 0)
        {
            return std::wstring();
        }

        std::wstring wideString(static_cast<size_t>(wideLength), L'\0');
        const int convertedLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, textUtf8.data(), byteCount, &wideString[0], wideLength);
        if (convertedLength != wideLength)
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

    std::wstring GetModuleFilePath(HMODULE moduleHandle)
    {
        if (moduleHandle == nullptr)
        {
            return std::wstring();
        }

        for (size_t bufferLength = GB_DbgInitialPathBufferLength; bufferLength <= GB_DbgMaxPathBufferLength; bufferLength *= 2)
        {
            std::vector<wchar_t> buffer(bufferLength, L'\0');
            ::SetLastError(ERROR_SUCCESS);
            const DWORD copiedLength = ::GetModuleFileNameW(moduleHandle, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copiedLength == 0)
            {
                return std::wstring();
            }

            if (copiedLength + 1 < buffer.size())
            {
                return std::wstring(buffer.data(), copiedLength);
            }
        }

        return std::wstring();
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
        if (address == 0)
        {
            return false;
        }

        MEMORY_BASIC_INFORMATION memoryInfo;
        std::memset(&memoryInfo, 0, sizeof(memoryInfo));
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), &memoryInfo, sizeof(memoryInfo)) == 0)
        {
            return false;
        }

        const HMODULE moduleHandle = reinterpret_cast<HMODULE>(memoryInfo.AllocationBase);
        if (moduleHandle == nullptr)
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
            if (m_initialized)
            {
                if (m_referenceCount < (std::numeric_limits<size_t>::max)())
                {
                    m_referenceCount++;
                }
                return true;
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

        GB_DbgStackTrace CaptureCurrentStackTrace(size_t skipFrameCount, size_t maxFrameCount)
        {
            GB_DbgStackTrace stackTrace;

#if defined(_M_X64)
            const size_t normalizedMaxFrameCount = NormalizeFrameCount(maxFrameCount);
            if (normalizedMaxFrameCount == 0)
            {
                return stackTrace;
            }

            std::vector<void*> rawAddresses(normalizedMaxFrameCount, nullptr);
            const size_t internalSkipFrameCount = skipFrameCount + 2;
            const USHORT capturedFrameCount = ::CaptureStackBackTrace(SafeToUlong(internalSkipFrameCount), SafeToUlong(normalizedMaxFrameCount), rawAddresses.data(), nullptr);

            std::vector<uint64_t> addresses;
            addresses.reserve(capturedFrameCount);
            for (USHORT i = 0; i < capturedFrameCount; i++)
            {
                const uint64_t address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rawAddresses[i]));
                AppendFrameAddress(addresses, address, normalizedMaxFrameCount);
            }

            ResolveAddresses(addresses, stackTrace);
#else
            (void)skipFrameCount;
            (void)maxFrameCount;
#endif
            return stackTrace;
        }

        GB_DbgStackTrace CaptureStackTraceFromContext(const void* contextRecord, size_t maxFrameCount)
        {
            GB_DbgStackTrace stackTrace;

#if defined(_M_X64)
            const size_t normalizedMaxFrameCount = NormalizeFrameCount(maxFrameCount);
            if (contextRecord == nullptr || normalizedMaxFrameCount == 0)
            {
                return stackTrace;
            }

            CONTEXT context = *reinterpret_cast<const CONTEXT*>(contextRecord);

            std::vector<uint64_t> addresses;
            addresses.reserve(normalizedMaxFrameCount);
            AppendFrameAddress(addresses, static_cast<uint64_t>(context.Rip), normalizedMaxFrameCount);

            {
                std::lock_guard<std::recursive_mutex> lock(m_mutex);
                if (EnsureInitializedLocked())
                {
                    CaptureStackAddressesFromContextLocked(context, normalizedMaxFrameCount, addresses);
                }
            }

            ResolveAddresses(addresses, stackTrace);
#else
            (void)contextRecord;
            (void)maxFrameCount;
#endif
            return stackTrace;
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

            const std::wstring dumpFilePath = Utf8ToWideString(dumpFilePathUtf8);
            if (dumpFilePath.empty())
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

            ::SymSetOptions(symOptions);

            const std::wstring symbolSearchPath = Utf8ToWideString(options.symbolSearchPathUtf8);
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

        void CaptureStackAddressesFromContextLocked(CONTEXT context, size_t maxFrameCount, std::vector<uint64_t>& addresses)
        {
#if defined(_M_X64)
            STACKFRAME64 stackFrame;
            std::memset(&stackFrame, 0, sizeof(stackFrame));
            stackFrame.AddrPC.Offset = context.Rip;
            stackFrame.AddrPC.Mode = AddrModeFlat;
            stackFrame.AddrStack.Offset = context.Rsp;
            stackFrame.AddrStack.Mode = AddrModeFlat;
            stackFrame.AddrFrame.Offset = context.Rbp;
            stackFrame.AddrFrame.Mode = AddrModeFlat;

            size_t duplicateFrameCount = 0;
            while (addresses.size() < maxFrameCount)
            {
                const BOOL success = ::StackWalk64(IMAGE_FILE_MACHINE_AMD64, m_processHandle, ::GetCurrentThread(), &stackFrame, &context, nullptr, ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr);
                if (!success || stackFrame.AddrPC.Offset == 0)
                {
                    break;
                }

                const size_t oldFrameCount = addresses.size();
                AppendFrameAddress(addresses, static_cast<uint64_t>(stackFrame.AddrPC.Offset), maxFrameCount);
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
#else
            (void)context;
            (void)maxFrameCount;
            (void)addresses;
#endif
        }

        void ResolveAddresses(const std::vector<uint64_t>& addresses, GB_DbgStackTrace& stackTrace)
        {
            stackTrace.frames.clear();
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
            const bool hasRuntimeModuleInfo = GetRuntimeModuleInfo(address, runtimeModuleInfo);
            if (::SymGetModuleBase64(m_processHandle, static_cast<DWORD64>(address)) == 0 && hasRuntimeModuleInfo)
            {
                const wchar_t* modulePath = runtimeModuleInfo.modulePath.empty() ? nullptr : runtimeModuleInfo.modulePath.c_str();
                const wchar_t* moduleName = runtimeModuleInfo.moduleName.empty() ? nullptr : runtimeModuleInfo.moduleName.c_str();
                ::SymLoadModuleExW(m_processHandle, nullptr, modulePath, moduleName, static_cast<DWORD64>(runtimeModuleInfo.moduleBase), 0, nullptr, 0);
            }

            ResolveModuleByDbgHelpLocked(address, frame);
            if (!frame.hasModule && hasRuntimeModuleInfo)
            {
                FillFrameModuleByRuntimeInfo(runtimeModuleInfo, frame);
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

        void ResolveSymbolByDbgHelpLocked(uint64_t address, GB_DbgStackFrame& frame)
        {
            const DWORD maxSymbolNameLength = 1024;
            const size_t symbolBufferSize = sizeof(SYMBOL_INFOW) + static_cast<size_t>(maxSymbolNameLength) * sizeof(wchar_t);
            std::unique_ptr<unsigned char[]> symbolBuffer(new (std::nothrow) unsigned char[symbolBufferSize]);
            if (!symbolBuffer)
            {
                return;
            }

            std::memset(symbolBuffer.get(), 0, symbolBufferSize);
            SYMBOL_INFOW* symbolInfo = reinterpret_cast<SYMBOL_INFOW*>(symbolBuffer.get());
            symbolInfo->SizeOfStruct = sizeof(SYMBOL_INFOW);
            symbolInfo->MaxNameLen = maxSymbolNameLength;

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
    return GB_DbgSymbolEngine::Instance().Initialize(options);
#else
    (void)options;
    return false;
#endif
}

void GB_DbgCleanupSymbols()
{
#if defined(_WIN32) || defined(_WIN64)
    GB_DbgSymbolEngine::Instance().Cleanup();
#endif
}

bool GB_DbgRefreshSymbols()
{
#if defined(_WIN32) || defined(_WIN64)
    return GB_DbgSymbolEngine::Instance().RefreshModuleList();
#else
    return false;
#endif
}

bool GB_DbgIsSymbolEngineInitialized()
{
#if defined(_WIN32) || defined(_WIN64)
    return GB_DbgSymbolEngine::Instance().IsInitialized();
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

GB_DbgStackTrace GB_DbgCaptureStackTrace(size_t skipFrameCount, size_t maxFrameCount)
{
#if defined(_WIN32) || defined(_WIN64)
    return GB_DbgSymbolEngine::Instance().CaptureCurrentStackTrace(skipFrameCount, maxFrameCount);
#else
    (void)skipFrameCount;
    (void)maxFrameCount;
    return GB_DbgStackTrace();
#endif
}

GB_DbgStackTrace GB_DbgCaptureStackTraceFromContext(const void* contextRecord, size_t maxFrameCount)
{
#if defined(_WIN32) || defined(_WIN64)
    return GB_DbgSymbolEngine::Instance().CaptureStackTraceFromContext(contextRecord, maxFrameCount);
#else
    (void)contextRecord;
    (void)maxFrameCount;
    return GB_DbgStackTrace();
#endif
}

GB_DbgStackTrace GB_DbgCaptureStackTraceFromExceptionPointers(const void* exceptionPointers, size_t maxFrameCount)
{
#if defined(_WIN32) || defined(_WIN64)
    if (exceptionPointers == nullptr)
    {
        return GB_DbgStackTrace();
    }

    const EXCEPTION_POINTERS* sehExceptionPointers = reinterpret_cast<const EXCEPTION_POINTERS*>(exceptionPointers);
    if (sehExceptionPointers->ContextRecord == nullptr)
    {
        return GB_DbgStackTrace();
    }

    return GB_DbgCaptureStackTraceFromContext(sehExceptionPointers->ContextRecord, maxFrameCount);
#else
    (void)exceptionPointers;
    (void)maxFrameCount;
    return GB_DbgStackTrace();
#endif
}

bool GB_DbgResolveAddress(uint64_t address, GB_DbgStackFrame& frame)
{
#if defined(_WIN32) || defined(_WIN64)
    return GB_DbgSymbolEngine::Instance().ResolveAddress(address, frame);
#else
    frame = GB_DbgStackFrame();
    frame.address = address;
    return false;
#endif
}

std::string GB_DbgFormatStackTrace(const GB_DbgStackTrace& stackTrace, bool withFrameIndex)
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

std::string GB_DbgFormatSystemErrorMessage(uint32_t errorCode)
{
#if defined(_WIN32) || defined(_WIN64)
    wchar_t* messageBuffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD messageLength = ::FormatMessageW(flags, nullptr, static_cast<DWORD>(errorCode), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&messageBuffer), 0, nullptr);

    if (messageLength > 0 && messageBuffer != nullptr)
    {
        const std::wstring message = TrimSystemMessage(std::wstring(messageBuffer, messageLength));
        ::LocalFree(messageBuffer);

        std::ostringstream stream;
        stream << WideStringToUtf8(message) << " (" << FormatHexValue(errorCode, 8) << ")";
        return stream.str();
    }

    if (messageBuffer != nullptr)
    {
        ::LocalFree(messageBuffer);
    }
#endif

    std::ostringstream stream;
    stream << "Unknown system error (" << FormatHexValue(errorCode, 8) << ")";
    return stream.str();
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
#else
    (void)textUtf8;
#endif
}

void GB_DbgOutputCurrentStackTrace(size_t skipFrameCount, size_t maxFrameCount)
{
    const GB_DbgStackTrace stackTrace = GB_DbgCaptureStackTrace(skipFrameCount + 1, maxFrameCount);
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
    return GB_DbgSymbolEngine::Instance().WriteMiniDump(dumpFilePathUtf8, exceptionPointers, 0, level);
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
    return GB_DbgSymbolEngine::Instance().WriteMiniDump(dumpFilePathUtf8, exceptionPointers, exceptionThreadId, level);
#else
    (void)dumpFilePathUtf8;
    (void)exceptionPointers;
    (void)exceptionThreadId;
    (void)level;
    return false;
#endif
}
