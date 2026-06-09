#include "GB_RunTests.h"
#include "Desktop/GB_SystemProcess.h"
#include "Desktop/GB_WindowsCommandLineInternal.h"
#include "GB_Process.h"
#include "GB_Utf8String.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
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
#  include <shellapi.h>
#endif

namespace
{
    int totalSystemProcessCaseCount = 0;

    [[noreturn]] void FailSystemProcessTest(const std::string& caseName, const std::string& detail)
    {
        std::cerr << "[FAILED] " << caseName << "\n" << detail << std::endl;
        std::exit(1);
    }

    void RequireTrueSystemProcess(const bool condition, const std::string& caseName, const std::string& detail)
    {
        totalSystemProcessCaseCount++;
        if (!condition)
        {
            FailSystemProcessTest(caseName, detail);
        }
    }

    void RequireProcessResultSucceeded(const GB_SystemResult& result, const std::string& caseName)
    {
        totalSystemProcessCaseCount++;
        if (result.IsFailed())
        {
            FailSystemProcessTest(caseName, result.ToString());
        }
    }

#if defined(_WIN32)
    std::vector<std::string> GetWideArgumentsUtf8()
    {
        int argumentCount = 0;
        LPWSTR* arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argumentCount);
        std::vector<std::string> result;
        if (arguments == nullptr)
        {
            return result;
        }
        for (int argumentIndex = 0; argumentIndex < argumentCount; argumentIndex++)
        {
            result.push_back(GB_WStringToUtf8(arguments[argumentIndex]));
        }
        ::LocalFree(arguments);
        return result;
    }

    int RunSystemProcessChild(const std::vector<std::string>& arguments, const size_t modeIndex)
    {
        if (modeIndex >= arguments.size())
        {
            return 91;
        }
        const std::string& mode = arguments[modeIndex];
        if (mode == "arguments")
        {
            for (size_t argumentIndex = modeIndex + 1; argumentIndex < arguments.size(); argumentIndex++)
            {
                std::cout << "ARG=" << arguments[argumentIndex].size() << ":" << arguments[argumentIndex] << "\n";
            }
            return 0;
        }
        if (mode == "environment")
        {
            wchar_t environmentBuffer[1024] = {};
            const DWORD environmentLength = ::GetEnvironmentVariableW(L"GB_SYSTEM_PROCESS_TEST_VALUE", environmentBuffer, static_cast<DWORD>(sizeof(environmentBuffer) / sizeof(environmentBuffer[0])));
            std::vector<wchar_t> directoryBuffer(32768);
            const DWORD directoryLength = ::GetCurrentDirectoryW(static_cast<DWORD>(directoryBuffer.size()), directoryBuffer.data());
            std::cout << "ENV=" << (environmentLength == 0 ? std::string() : GB_WStringToUtf8(std::wstring(environmentBuffer, environmentLength))) << "\n";
            std::cout << "CWD=" << (directoryLength == 0 ? std::string() : GB_WStringToUtf8(std::wstring(directoryBuffer.data(), directoryLength))) << "\n";
            return 0;
        }
        if (mode == "streams")
        {
            const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
            std::cout << "OUT:" << input;
            std::cerr << "ERR:" << input;
            return 17;
        }
        if (mode == "merge")
        {
            std::cout << "stdout-line\n";
            std::cerr << "stderr-line\n";
            return 0;
        }
        if (mode == "large")
        {
            const size_t byteCount = modeIndex + 1 < arguments.size() ? static_cast<size_t>(std::strtoull(arguments[modeIndex + 1].c_str(), nullptr, 10)) : 0;
            const std::string block(65536, 'X');
            size_t written = 0;
            while (written < byteCount)
            {
                const size_t currentCount = (std::min)(block.size(), byteCount - written);
                std::cout.write(block.data(), static_cast<std::streamsize>(currentCount));
                written += currentCount;
            }
            return 0;
        }
        if (mode == "dual-large")
        {
            const size_t byteCount = modeIndex + 1 < arguments.size() ? static_cast<size_t>(std::strtoull(arguments[modeIndex + 1].c_str(), nullptr, 10)) : 0;
            const auto WriteStream = [byteCount](const HANDLE streamHandle, const char fillCharacter)
                {
                    const std::string block(65536, fillCharacter);
                    size_t writtenTotal = 0;
                    while (writtenTotal < byteCount)
                    {
                        const DWORD requestedCount = static_cast<DWORD>((std::min)(block.size(), byteCount - writtenTotal));
                        DWORD writtenCount = 0;
                        if (::WriteFile(streamHandle, block.data(), requestedCount, &writtenCount, nullptr) == FALSE || writtenCount == 0)
                        {
                            return;
                        }
                        writtenTotal += writtenCount;
                    }
                };
            std::thread outputThread(WriteStream, ::GetStdHandle(STD_OUTPUT_HANDLE), 'O');
            std::thread errorThread(WriteStream, ::GetStdHandle(STD_ERROR_HANDLE), 'E');
            outputThread.join();
            errorThread.join();
            return 0;
        }
        if (mode == "utf16")
        {
            const std::wstring text = L"UTF16-\x4E2D\x6587";
            DWORD bytesWritten = 0;
            (void)::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &bytesWritten, nullptr);
            return bytesWritten == text.size() * sizeof(wchar_t) ? 0 : 92;
        }
        if (mode == "invalid-utf8")
        {
            const unsigned char bytes[] = { 0xC3, 0x28 };
            DWORD bytesWritten = 0;
            (void)::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), bytes, static_cast<DWORD>(sizeof(bytes)), &bytesWritten, nullptr);
            return 23;
        }
        if (mode == "truncated-utf8")
        {
            const unsigned char bytes[] = { 'A', 0xE2, 0x82, 0xAC, 'B' };
            DWORD bytesWritten = 0;
            (void)::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), bytes, static_cast<DWORD>(sizeof(bytes)), &bytesWritten, nullptr);
            return 0;
        }
        if (mode == "exit-code")
        {
            return modeIndex + 1 < arguments.size() ? std::atoi(arguments[modeIndex + 1].c_str()) : 0;
        }
        if (mode == "sleep")
        {
            const DWORD sleepMilliseconds = modeIndex + 1 < arguments.size() ? static_cast<DWORD>(std::strtoul(arguments[modeIndex + 1].c_str(), nullptr, 10)) : 1000;
            ::Sleep(sleepMilliseconds);
            return 0;
        }
        if (mode == "spawn-grandchild" || mode == "spawn-grandchild-exit")
        {
            std::string executablePath;
            if (GB_SystemProcess::GetCurrentExecutablePath(executablePath).IsFailed())
            {
                std::cout << "SPAWN_FAILED\n";
                return 0;
            }
            GB_ProcessStartOptions options;
            options.executablePath = executablePath;
            options.arguments.push_back("--gb-system-process-child");
            options.arguments.push_back("sleep");
            options.arguments.push_back("60000");
            GB_ProcessInstance processInstance;
            if (GB_SystemProcess::Start(options, processInstance).IsFailed())
            {
                std::cout << "SPAWN_FAILED\n";
                return 0;
            }
            std::cout << "GRANDCHILD=" << processInstance.GetProcessId() << "\n";
            std::cout.flush();
            if (mode == "spawn-grandchild")
            {
                ::Sleep(60000);
            }
            return 0;
        }
        if (mode == "gui")
        {
            const wchar_t* className = L"GB_SystemProcess_Test_Window";
            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = [](HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT
                {
                    if (message == WM_CLOSE)
                    {
                        (void)::DestroyWindow(windowHandle);
                        return 0;
                    }
                    if (message == WM_DESTROY)
                    {
                        ::PostQuitMessage(0);
                        return 0;
                    }
                    return ::DefWindowProcW(windowHandle, message, wParam, lParam);
                };
            windowClass.hInstance = ::GetModuleHandleW(nullptr);
            windowClass.lpszClassName = className;
            if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                return 93;
            }
            const HWND windowHandle = ::CreateWindowExW(WS_EX_APPWINDOW, className, L"GB System Process GUI Child", WS_OVERLAPPEDWINDOW, 100, 100, 480, 320, nullptr, nullptr, windowClass.hInstance, nullptr);
            if (windowHandle == nullptr)
            {
                return 94;
            }
            (void)::ShowWindow(windowHandle, SW_SHOWNOACTIVATE);
            (void)::UpdateWindow(windowHandle);
            MSG message = {};
            while (::GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            return 0;
        }
        if (mode == "hidden-tool-window")
        {
            const wchar_t* className = L"GB_SystemProcess_Test_HiddenToolWindow";
            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = ::DefWindowProcW;
            windowClass.hInstance = ::GetModuleHandleW(nullptr);
            windowClass.lpszClassName = className;
            if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                return 96;
            }
            const HWND windowHandle = ::CreateWindowExW(WS_EX_TOOLWINDOW, className, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, windowClass.hInstance, nullptr);
            if (windowHandle == nullptr)
            {
                return 97;
            }
            ::Sleep(60000);
            return 0;
        }
        return 95;
    }

    GB_ProcessStartOptions MakeChildOptions(const std::string& mode)
    {
        std::string executablePath;
        const GB_SystemResult pathResult = GB_SystemProcess::GetCurrentExecutablePath(executablePath);
        if (pathResult.IsFailed())
        {
            FailSystemProcessTest("Get current executable path", pathResult.ToString());
        }
        GB_ProcessStartOptions options;
        options.executablePath = executablePath;
        options.arguments.push_back("--gb-system-process-child");
        options.arguments.push_back(mode);
        return options;
    }

    bool WaitUntilProcessMissing(const GB_ProcessIdentity& identity, const uint32_t timeoutMilliseconds)
    {
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            GB_ProcessInfo processInfo;
            const GB_SystemResult result = GB_SystemProcess::GetProcessInfo(identity, processInfo);
            if (result.IsFailed() && result.errorCode == GB_SystemErrorCode::NotFound)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    GB_SystemResult WaitForOutputMarker(GB_ProcessInstance& processInstance, const std::string& marker, std::string& output, bool& truncated, const uint32_t timeoutMilliseconds)
    {
        output.clear();
        truncated = false;
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        while (std::chrono::steady_clock::now() < deadline)
        {
            GB_SystemResult result = processInstance.GetStandardOutput(output, truncated);
            if (result.IsFailed())
            {
                return result.WithOperationName("WaitForOutputMarker");
            }
            if (output.find(marker) != std::string::npos)
            {
                return GB_SystemResult::Succeeded("WaitForOutputMarker");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return GB_SystemResult::Failed(GB_SystemErrorCode::Timeout, "WaitForOutputMarker", "等待子进程输出标记超时。");
    }

    void TestCommandLineEscapingCorpus()
    {
        const std::vector<std::wstring> expectedArguments = { L"", L"plain", L"space argument", L"\t", L"\"", L"\\", L"trailing\\", L"\\\"", L"\\\\\"", L"a\\\"b", L"\x4E2D\x6587" };
        std::wstring commandLine = L"\"C:\\Program Files\\GlobalBase Test.exe\" ";
        commandLine += GB_WindowsCommandLineInternal::BuildParameters(expectedArguments);
        int argumentCount = 0;
        LPWSTR* parsedArguments = ::CommandLineToArgvW(commandLine.c_str(), &argumentCount);
        RequireTrueSystemProcess(parsedArguments != nullptr, "Parse command-line escaping corpus", std::to_string(::GetLastError()));
        RequireTrueSystemProcess(argumentCount == static_cast<int>(expectedArguments.size() + 1), "Command-line escaping argument count", std::to_string(argumentCount));
        for (size_t argumentIndex = 0; argumentIndex < expectedArguments.size(); argumentIndex++)
        {
            RequireTrueSystemProcess(std::wstring(parsedArguments[argumentIndex + 1]) == expectedArguments[argumentIndex], "Command-line escaping corpus item", std::to_string(argumentIndex));
        }
        ::LocalFree(parsedArguments);
    }

    void TestOptionValidation()
    {
        GB_ProcessStartOptions options = MakeChildOptions("sleep");
        options.rawCommandLine = "raw";
        options.arguments.push_back("conflict");
        GB_ProcessInstance processInstance;
        const GB_SystemResult conflictResult = GB_SystemProcess::Start(options, processInstance);
        RequireTrueSystemProcess(conflictResult.IsFailed() && conflictResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject raw command line and arguments", conflictResult.ToString());

        options = MakeChildOptions("sleep");
        options.executablePath = std::string("\xC3\x28", 2);
        const GB_SystemResult utf8Result = GB_SystemProcess::Start(options, processInstance);
        RequireTrueSystemProcess(utf8Result.IsFailed() && utf8Result.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject invalid UTF-8 executable path", utf8Result.ToString());

        options = MakeChildOptions("sleep");
        options.jobOptions.enabled = true;
        options.jobOptions.cpuRatePercent = 101;
        const GB_SystemResult cpuResult = GB_SystemProcess::Start(options, processInstance);
        RequireTrueSystemProcess(cpuResult.IsFailed() && cpuResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject invalid Job CPU rate", cpuResult.ToString());

        options = MakeChildOptions("sleep");
        options.maximumCapturedBytesPerStream = static_cast<size_t>((std::numeric_limits<int>::max)()) + 1;
        const GB_SystemResult captureLimitResult = GB_SystemProcess::Start(options, processInstance);
        RequireTrueSystemProcess(captureLimitResult.IsFailed() && captureLimitResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject unrepresentable capture limit", captureLimitResult.ToString());

        options = MakeChildOptions("sleep");
        options.startSuspended = true;
        GB_ProcessRunResult runResult;
        const GB_SystemResult suspendedRunResult = GB_SystemProcess::Run(options, runResult);
        RequireTrueSystemProcess(suspendedRunResult.IsFailed() && suspendedRunResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject suspended Run", suspendedRunResult.ToString());
    }

    void TestArgumentsEnvironmentAndWorkingDirectory()
    {
        GB_ProcessStartOptions options = MakeChildOptions("arguments");
        options.arguments.push_back("");
        options.arguments.push_back("space argument");
        options.arguments.push_back("quote\"and\\slash\\\\");
        options.arguments.push_back(GB_MakeUtf8String(u8"\u4E2D\u6587\u53C2\u6570"));
        options.redirectStandardOutput = true;
        GB_ProcessRunResult runResult;
        RequireProcessResultSucceeded(GB_SystemProcess::Run(options, runResult), "Run argument child");
        RequireTrueSystemProcess(runResult.hasExitCode && runResult.exitCode == 0, "Argument child exit code", std::to_string(runResult.exitCode));
        RequireTrueSystemProcess(runResult.standardOutput.find("ARG=0:\r\n") != std::string::npos, "Preserve empty argument", runResult.standardOutput);
        RequireTrueSystemProcess(runResult.standardOutput.find("space argument") != std::string::npos, "Preserve spaced argument", runResult.standardOutput);
        RequireTrueSystemProcess(runResult.standardOutput.find("quote\"and\\slash\\\\") != std::string::npos, "Preserve quotes and slashes", runResult.standardOutput);
        RequireTrueSystemProcess(runResult.standardOutput.find(GB_MakeUtf8String(u8"\u4E2D\u6587\u53C2\u6570")) != std::string::npos, "Preserve Unicode argument", runResult.standardOutput);

        std::string currentDirectory;
        RequireProcessResultSucceeded(GB_SystemProcess::GetCurrentWorkingDirectory(currentDirectory), "Get current directory");
        options = MakeChildOptions("environment");
        options.redirectStandardOutput = true;
        options.workingDirectory = currentDirectory;
        GB_ProcessEnvironmentVariable variable;
        variable.name = "GB_SYSTEM_PROCESS_TEST_VALUE";
        variable.value = GB_MakeUtf8String(u8"\u73AF\u5883\u503C");
        options.environmentVariables.push_back(variable);
        GB_ProcessEnvironmentVariable replacementVariable;
        replacementVariable.name = "gb_system_process_test_value";
        replacementVariable.value = GB_MakeUtf8String(u8"\u8986\u76D6\u503C");
        options.environmentVariables.push_back(replacementVariable);
        RequireProcessResultSucceeded(GB_SystemProcess::Run(options, runResult), "Run environment child");
        RequireTrueSystemProcess(runResult.standardOutput.find("ENV=" + replacementVariable.value) != std::string::npos, "Case-insensitive environment override", runResult.standardOutput);
        RequireTrueSystemProcess(runResult.standardOutput.find("CWD=" + currentDirectory) != std::string::npos, "Working directory", runResult.standardOutput);
    }

    void TestStandardStreamsAndEncoding()
    {
        GB_ProcessStartOptions options = MakeChildOptions("streams");
        options.redirectStandardInput = true;
        options.redirectStandardOutput = true;
        options.redirectStandardError = true;
        GB_ProcessInstance processInstance;
        RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start streams child");
        RequireProcessResultSucceeded(processInstance.WriteStandardInput("input-data"), "Write standard input");
        RequireProcessResultSucceeded(processInstance.CloseStandardInput(), "Close standard input");
        GB_ProcessWaitOptions waitOptions;
        waitOptions.timeoutMilliseconds = 5000;
        uint32_t exitCode = 0;
        RequireProcessResultSucceeded(processInstance.WaitForExit(exitCode, waitOptions), "Wait streams child");
        RequireTrueSystemProcess(exitCode == 17, "Streams exit code", std::to_string(exitCode));
        std::string standardOutput;
        std::string standardError;
        bool outputTruncated = false;
        bool errorTruncated = false;
        RequireProcessResultSucceeded(processInstance.GetStandardOutput(standardOutput, outputTruncated), "Read standard output");
        RequireProcessResultSucceeded(processInstance.GetStandardError(standardError, errorTruncated), "Read standard error");
        RequireTrueSystemProcess(standardOutput == "OUT:input-data", "Standard output content", standardOutput);
        RequireTrueSystemProcess(standardError == "ERR:input-data", "Standard error content", standardError);

        options = MakeChildOptions("merge");
        options.redirectStandardOutput = true;
        options.mergeStandardErrorToOutput = true;
        GB_ProcessRunResult runResult;
        RequireProcessResultSucceeded(GB_SystemProcess::Run(options, runResult), "Run merged output child");
        RequireTrueSystemProcess(runResult.standardOutput.find("stdout-line") != std::string::npos && runResult.standardOutput.find("stderr-line") != std::string::npos && runResult.standardError.empty(), "Merged output content", runResult.standardOutput);

        options = MakeChildOptions("utf16");
        options.redirectStandardOutput = true;
        options.outputEncoding = GB_ProcessOutputEncoding::Utf16LittleEndian;
        RequireProcessResultSucceeded(GB_SystemProcess::Run(options, runResult), "Run UTF-16 child");
        RequireTrueSystemProcess(runResult.standardOutput == GB_MakeUtf8String(u8"UTF16-\u4E2D\u6587"), "UTF-16 output conversion", runResult.standardOutput);

        options = MakeChildOptions("truncated-utf8");
        options.redirectStandardOutput = true;
        options.maximumCapturedBytesPerStream = 2;
        RequireProcessResultSucceeded(GB_SystemProcess::Run(options, runResult), "Run truncated UTF-8 child");
        RequireTrueSystemProcess(runResult.standardOutput == "A" && runResult.standardOutputTruncated && runResult.standardOutputEncodingValid, "Truncated UTF-8 keeps valid prefix", runResult.standardOutput);

        options = MakeChildOptions("invalid-utf8");
        options.redirectStandardOutput = true;
        RequireProcessResultSucceeded(GB_SystemProcess::Run(options, runResult), "Run invalid UTF-8 child");
        RequireTrueSystemProcess(runResult.hasExitCode && runResult.exitCode == 23 && !runResult.standardOutputEncodingValid && !runResult.standardOutputEncodingError.empty(), "Encoding error preserves process exit result", runResult.standardOutputEncodingError);

        options = MakeChildOptions("exit-code");
        options.arguments.push_back("259");
        RequireProcessResultSucceeded(GB_SystemProcess::Run(options, runResult), "Run STILL_ACTIVE-valued exit code child");
        RequireTrueSystemProcess(runResult.hasExitCode && runResult.exitCode == STILL_ACTIVE, "Exit code 259 is not treated as running", std::to_string(runResult.exitCode));
    }

    void TestLargeOutputTimeoutAndCallbacks()
    {
        GB_ProcessStartOptions options = MakeChildOptions("large");
        options.arguments.push_back("1048576");
        options.redirectStandardOutput = true;
        options.maximumCapturedBytesPerStream = 4096;
        options.maximumPendingOutputCallbacks = 1;
        options.outputCallback = [](const GB_ProcessOutputEvent&) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); };
        GB_ProcessRunResult runResult;
        RequireProcessResultSucceeded(GB_SystemProcess::Run(options, runResult), "Run large output child");
        RequireTrueSystemProcess(runResult.standardOutput.size() == 4096 && runResult.standardOutputTruncated, "Large output truncation", std::to_string(runResult.standardOutput.size()));
        RequireTrueSystemProcess(runResult.droppedOutputCallbackCount > 0, "Output callback backpressure", std::to_string(runResult.droppedOutputCallbackCount));

        options = MakeChildOptions("dual-large");
        options.arguments.push_back("8388608");
        options.redirectStandardOutput = true;
        options.redirectStandardError = true;
        options.maximumCapturedBytesPerStream = 4096;
        options.runTimeoutMilliseconds = 10000;
        RequireProcessResultSucceeded(GB_SystemProcess::Run(options, runResult), "Run concurrent dual-stream output child");
        RequireTrueSystemProcess(runResult.hasExitCode && runResult.exitCode == 0 && runResult.standardOutput.size() == 4096 && runResult.standardError.size() == 4096 && runResult.standardOutputTruncated && runResult.standardErrorTruncated, "Concurrent stdout and stderr drain", std::to_string(runResult.standardOutput.size()) + "," + std::to_string(runResult.standardError.size()));

        options = MakeChildOptions("sleep");
        options.arguments.push_back("5000");
        options.runTimeoutMilliseconds = 50;
        options.terminateOnRunTimeout = true;
        options.jobOptions.enabled = true;
        options.jobOptions.terminateOnJobClose = true;
        const GB_SystemResult timeoutResult = GB_SystemProcess::Run(options, runResult);
        RequireTrueSystemProcess(timeoutResult.IsFailed() && timeoutResult.errorCode == GB_SystemErrorCode::Timeout && runResult.timedOut && runResult.terminatedByModule, "Run timeout termination", timeoutResult.ToString());

        options = MakeChildOptions("sleep");
        options.arguments.push_back("60000");
        options.redirectStandardOutput = true;
        options.runTimeoutMilliseconds = 50;
        options.terminateOnRunTimeout = false;
        const GB_SystemResult detachedTimeoutResult = GB_SystemProcess::Run(options, runResult);
        RequireTrueSystemProcess(detachedTimeoutResult.IsFailed() && detachedTimeoutResult.errorCode == GB_SystemErrorCode::Timeout && runResult.timedOut && !runResult.terminatedByModule && runResult.standardOutputTruncated, "Detached Run timeout marks partial output", detachedTimeoutResult.ToString());
        GB_ProcessInfo detachedProcessInfo;
        RequireProcessResultSucceeded(GB_SystemProcess::GetProcessInfo(runResult.processId, detachedProcessInfo), "Query detached Run timeout child");
        RequireProcessResultSucceeded(GB_SystemProcess::TerminateProcess(detachedProcessInfo.identity, 29), "Cleanup detached Run timeout child");
    }

    void TestSuspendedQueryAndCancellation()
    {
        GB_ProcessStartOptions options = MakeChildOptions("sleep");
        options.arguments.push_back("60000");
        options.startSuspended = true;
        GB_ProcessInstance processInstance;
        RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start suspended child");
        GB_ProcessWaitOptions immediateWait;
        immediateWait.timeoutMilliseconds = 0;
        uint32_t exitCode = 0;
        const GB_SystemResult immediateResult = processInstance.WaitForExit(exitCode, immediateWait);
        RequireTrueSystemProcess(immediateResult.IsFailed() && immediateResult.errorCode == GB_SystemErrorCode::Timeout, "Suspended child still running", immediateResult.ToString());

        GB_ProcessInfo processInfo;
        RequireProcessResultSucceeded(GB_SystemProcess::GetProcessInfo(processInstance.GetIdentity(), processInfo), "Query suspended child");
        RequireTrueSystemProcess(processInfo.identity == processInstance.GetIdentity(), "Strong identity query", std::to_string(processInfo.processId));
        RequireTrueSystemProcess(processInfo.processState == GB_ProcessState::Running && processInfo.parentProcessId == static_cast<int>(::GetCurrentProcessId()), "Suspended child state and parent", processInfo.stateUtf8);

        GB_ProcessFindOptions findOptions;
        findOptions.processId = processInstance.GetProcessId();
        std::vector<GB_ProcessInfo> foundProcesses;
        RequireProcessResultSucceeded(GB_SystemProcess::FindProcesses(findOptions, foundProcesses), "Find child by PID");
        RequireTrueSystemProcess(foundProcesses.size() == 1, "Find child count", std::to_string(foundProcesses.size()));

        std::atomic<bool> cancellationFlag(true);
        GB_ProcessWaitOptions cancelledWait;
        cancelledWait.timeoutMilliseconds = 5000;
        cancelledWait.cancellationFlag = &cancellationFlag;
        const GB_SystemResult cancelledResult = GB_SystemProcess::WaitForProcessExit(processInstance.GetIdentity(), exitCode, cancelledWait);
        RequireTrueSystemProcess(cancelledResult.IsFailed() && cancelledResult.errorCode == GB_SystemErrorCode::Cancelled, "Cancelled process wait", cancelledResult.ToString());

        cancellationFlag.store(false);
        cancelledWait.pollIntervalMilliseconds = 5000;
        const std::chrono::steady_clock::time_point cancellationStart = std::chrono::steady_clock::now();
        std::thread cancellationThread([&cancellationFlag]()
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                cancellationFlag.store(true);
            });
        const GB_SystemResult responsiveCancellationResult = GB_SystemProcess::WaitForProcessExit(processInstance.GetIdentity(), exitCode, cancelledWait);
        cancellationThread.join();
        const uint64_t cancellationElapsedMilliseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cancellationStart).count());
        RequireTrueSystemProcess(responsiveCancellationResult.IsFailed() && responsiveCancellationResult.errorCode == GB_SystemErrorCode::Cancelled && cancellationElapsedMilliseconds < 1000, "Cancellation remains responsive with large poll interval", std::to_string(cancellationElapsedMilliseconds));

        GB_ProcessIdentity staleIdentity = processInstance.GetIdentity();
        staleIdentity.creationTime100Nanoseconds++;
        const GB_SystemResult staleResult = GB_SystemProcess::GetProcessInfo(staleIdentity, processInfo);
        RequireTrueSystemProcess(staleResult.IsFailed() && staleResult.errorCode == GB_SystemErrorCode::NotFound, "Reject mismatched process identity", staleResult.ToString());

        std::atomic<int> resumeReadyCount(0);
        std::atomic<bool> beginResume(false);
        std::atomic<int> resumeSuccessCount(0);
        std::atomic<int> resumeInvalidStateCount(0);
        const auto ResumeConcurrently = [&]()
            {
                resumeReadyCount++;
                while (!beginResume.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                const GB_SystemResult resumeResult = processInstance.ResumeMainThread();
                if (resumeResult.IsSucceeded())
                {
                    resumeSuccessCount++;
                }
                else if (resumeResult.errorCode == GB_SystemErrorCode::InvalidState)
                {
                    resumeInvalidStateCount++;
                }
            };
        std::thread firstResumeThread(ResumeConcurrently);
        std::thread secondResumeThread(ResumeConcurrently);
        while (resumeReadyCount.load(std::memory_order_acquire) != 2)
        {
            std::this_thread::yield();
        }
        beginResume.store(true, std::memory_order_release);
        firstResumeThread.join();
        secondResumeThread.join();
        RequireTrueSystemProcess(resumeSuccessCount.load() == 1 && resumeInvalidStateCount.load() == 1, "Concurrent resume executes exactly once", std::to_string(resumeSuccessCount.load()) + "/" + std::to_string(resumeInvalidStateCount.load()));
        RequireProcessResultSucceeded(processInstance.Terminate(31), "Terminate resumed child");

        options = MakeChildOptions("sleep");
        options.arguments.push_back("60000");
        options.startSuspended = true;
        options.jobOptions.enabled = true;
        options.jobOptions.terminateOnJobClose = true;
        RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start suspended Job child");
        const GB_SystemResult suspendedJobWaitResult = processInstance.WaitForExit(exitCode, immediateWait);
        RequireTrueSystemProcess(suspendedJobWaitResult.IsFailed() && suspendedJobWaitResult.errorCode == GB_SystemErrorCode::Timeout, "Job association preserves caller suspension", suspendedJobWaitResult.ToString());
        RequireProcessResultSucceeded(processInstance.ResumeMainThread(), "Resume suspended Job child");
        RequireProcessResultSucceeded(processInstance.Terminate(32, true), "Terminate resumed Job child");
    }

    void TestWindowCloseAndSafety()
    {
        GB_ProcessStartOptions options = MakeChildOptions("gui");
        GB_ProcessInstance processInstance;
        RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start GUI child");
        GB_ProcessWaitOptions windowWait;
        windowWait.timeoutMilliseconds = 5000;
        windowWait.pollIntervalMilliseconds = 10;
        GB_WindowInfo windowInfo;
        RequireProcessResultSucceeded(processInstance.WaitForMainWindow(windowInfo, windowWait), "Wait GUI child main window");
        RequireTrueSystemProcess(windowInfo.windowId.processId == processInstance.GetProcessId(), "GUI child window process", std::to_string(windowInfo.windowId.processId));
        GB_ProcessCloseOptions closeOptions;
        closeOptions.waitForExitMilliseconds = 5000;
        closeOptions.forceTerminateAfterTimeout = true;
        RequireProcessResultSucceeded(processInstance.Close(closeOptions), "Close GUI child");

        options = MakeChildOptions("exit-code");
        options.arguments.push_back("0");
        RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start non-GUI child for main-window wait");
        const GB_SystemResult missingWindowResult = processInstance.WaitForMainWindow(windowInfo, windowWait);
        RequireTrueSystemProcess(missingWindowResult.IsFailed() && missingWindowResult.errorCode == GB_SystemErrorCode::NotFound, "Main-window wait stops when process exits", missingWindowResult.ToString());

        options = MakeChildOptions("hidden-tool-window");
        RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start hidden-tool-window child");
        GB_ProcessWaitOptions shortWindowWait;
        shortWindowWait.timeoutMilliseconds = 200;
        shortWindowWait.pollIntervalMilliseconds = 10;
        const GB_SystemResult hiddenWindowWaitResult = processInstance.WaitForMainWindow(windowInfo, shortWindowWait);
        RequireTrueSystemProcess(hiddenWindowWaitResult.IsFailed() && hiddenWindowWaitResult.errorCode == GB_SystemErrorCode::Timeout, "Hidden tool window is not a main window", hiddenWindowWaitResult.ToString());
        GB_ProcessInfo hiddenWindowProcessInfo;
        RequireProcessResultSucceeded(GB_SystemProcess::GetProcessInfo(processInstance.GetIdentity(), hiddenWindowProcessInfo), "Query hidden-tool-window child");
        RequireTrueSystemProcess(!hiddenWindowProcessInfo.hasMainWindow, "Process query excludes hidden tool window", hiddenWindowProcessInfo.mainWindowTitle);
        RequireProcessResultSucceeded(processInstance.Terminate(33), "Terminate hidden-tool-window child");

        GB_ProcessInfo currentProcessInfo;
        RequireProcessResultSucceeded(GB_SystemProcess::GetCurrentProcessInfo(currentProcessInfo), "Query current process");
        const GB_SystemResult selfTerminateResult = GB_SystemProcess::TerminateProcess(currentProcessInfo.identity, 1);
        RequireTrueSystemProcess(selfTerminateResult.IsFailed() && selfTerminateResult.errorCode == GB_SystemErrorCode::PermissionDenied, "Reject current process termination", selfTerminateResult.ToString());
    }

    void TestJobAndDestructorPolicies()
    {
        GB_ProcessIdentity detachedIdentity;
        {
            GB_ProcessStartOptions options = MakeChildOptions("sleep");
            options.arguments.push_back("60000");
            GB_ProcessInstance processInstance;
            RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start ordinary destructor child");
            detachedIdentity = processInstance.GetIdentity();
        }
        GB_ProcessInfo processInfo;
        RequireProcessResultSucceeded(GB_SystemProcess::GetProcessInfo(detachedIdentity, processInfo), "Ordinary destructor leaves process running");
        RequireProcessResultSucceeded(GB_SystemProcess::TerminateProcess(detachedIdentity, 41), "Cleanup ordinary destructor child");

        GB_ProcessIdentity jobIdentity;
        {
            GB_ProcessStartOptions options = MakeChildOptions("sleep");
            options.arguments.push_back("60000");
            options.jobOptions.enabled = true;
            options.jobOptions.terminateOnJobClose = true;
            GB_ProcessInstance processInstance;
            RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start Job destructor child");
            jobIdentity = processInstance.GetIdentity();
        }
        RequireTrueSystemProcess(WaitUntilProcessMissing(jobIdentity, 5000), "Job destructor terminates child", std::to_string(jobIdentity.processId));

        GB_ProcessStartOptions options = MakeChildOptions("spawn-grandchild");
        options.redirectStandardOutput = true;
        options.jobOptions.enabled = true;
        options.jobOptions.terminateOnJobClose = true;
        GB_ProcessInstance processInstance;
        RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start Job tree child");
        std::string output;
        bool truncated = false;
        RequireProcessResultSucceeded(WaitForOutputMarker(processInstance, "GRANDCHILD=", output, truncated, 5000), "Wait Job tree output");
        const size_t markerPosition = output.find("GRANDCHILD=");
        RequireTrueSystemProcess(markerPosition != std::string::npos, "Grandchild PID reported", output);
        const uint32_t grandchildProcessId = static_cast<uint32_t>(std::strtoul(output.c_str() + markerPosition + 11, nullptr, 10));
        GB_ProcessInfo grandchildInfo;
        RequireProcessResultSucceeded(GB_SystemProcess::GetProcessInfo(grandchildProcessId, grandchildInfo), "Query Job grandchild");
        RequireProcessResultSucceeded(processInstance.Terminate(42, true), "Terminate entire Job");
        RequireTrueSystemProcess(WaitUntilProcessMissing(grandchildInfo.identity, 5000), "Job termination removes grandchild", std::to_string(grandchildProcessId));

        options = MakeChildOptions("spawn-grandchild");
        options.redirectStandardOutput = true;
        options.jobOptions.enabled = true;
        options.jobOptions.terminateOnJobClose = true;
        options.jobOptions.processMemoryLimitBytes = 256ULL * 1024ULL * 1024ULL;
        options.jobOptions.jobMemoryLimitBytes = 512ULL * 1024ULL * 1024ULL;
        options.jobOptions.cpuRatePercent = 50;
        options.jobOptions.breakawayAllowed = true;
        RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start resource-limited Job tree");
        RequireProcessResultSucceeded(WaitForOutputMarker(processInstance, "GRANDCHILD=", output, truncated, 5000), "Wait close Job tree output");
        const size_t closeMarkerPosition = output.find("GRANDCHILD=");
        RequireTrueSystemProcess(closeMarkerPosition != std::string::npos, "Close Job grandchild PID reported", output);
        const uint32_t closeGrandchildProcessId = static_cast<uint32_t>(std::strtoul(output.c_str() + closeMarkerPosition + 11, nullptr, 10));
        GB_ProcessInfo closeGrandchildInfo;
        RequireProcessResultSucceeded(GB_SystemProcess::GetProcessInfo(closeGrandchildProcessId, closeGrandchildInfo), "Query close Job grandchild");
        const GB_ProcessIdentity closeRootIdentity = processInstance.GetIdentity();
        GB_ProcessCloseOptions closeOptions;
        closeOptions.forceTerminateAfterTimeout = true;
        closeOptions.terminateJobWhenAvailable = true;
        RequireProcessResultSucceeded(processInstance.Close(closeOptions), "Force close entire Job without GUI window");
        RequireTrueSystemProcess(WaitUntilProcessMissing(closeRootIdentity, 5000) && WaitUntilProcessMissing(closeGrandchildInfo.identity, 5000), "Instance close removes entire Job", std::to_string(closeRootIdentity.processId));

        options = MakeChildOptions("spawn-grandchild");
        options.redirectStandardOutput = true;
        options.jobOptions.enabled = true;
        options.jobOptions.maximumActiveProcessCount = 1;
        GB_ProcessRunResult runResult;
        RequireProcessResultSucceeded(GB_SystemProcess::Run(options, runResult), "Run active-process-limited Job");
        RequireTrueSystemProcess(runResult.standardOutput.find("SPAWN_FAILED") != std::string::npos, "Job active process limit", runResult.standardOutput);

        options = MakeChildOptions("spawn-grandchild");
        options.redirectStandardOutput = true;
        RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start snapshot process tree");
        RequireProcessResultSucceeded(WaitForOutputMarker(processInstance, "GRANDCHILD=", output, truncated, 5000), "Wait snapshot tree output");
        const size_t snapshotMarkerPosition = output.find("GRANDCHILD=");
        RequireTrueSystemProcess(snapshotMarkerPosition != std::string::npos, "Snapshot grandchild PID reported", output);
        const uint32_t snapshotGrandchildProcessId = static_cast<uint32_t>(std::strtoul(output.c_str() + snapshotMarkerPosition + 11, nullptr, 10));
        GB_ProcessInfo snapshotGrandchildInfo;
        RequireProcessResultSucceeded(GB_SystemProcess::GetProcessInfo(snapshotGrandchildProcessId, snapshotGrandchildInfo), "Query snapshot grandchild");
        const GB_ProcessIdentity snapshotRootIdentity = processInstance.GetIdentity();
        RequireProcessResultSucceeded(GB_SystemProcess::TerminateProcessTreeSnapshot(snapshotRootIdentity, 43), "Terminate snapshot process tree");
        RequireTrueSystemProcess(WaitUntilProcessMissing(snapshotRootIdentity, 5000) && WaitUntilProcessMissing(snapshotGrandchildInfo.identity, 5000), "Snapshot termination removes entire tree", std::to_string(snapshotRootIdentity.processId));

        options = MakeChildOptions("spawn-grandchild-exit");
        options.redirectStandardOutput = true;
        RequireProcessResultSucceeded(GB_SystemProcess::Start(options, processInstance), "Start exiting snapshot root");
        RequireProcessResultSucceeded(WaitForOutputMarker(processInstance, "GRANDCHILD=", output, truncated, 5000), "Wait exiting snapshot root output");
        const size_t exitingRootMarkerPosition = output.find("GRANDCHILD=");
        RequireTrueSystemProcess(exitingRootMarkerPosition != std::string::npos, "Exiting root grandchild PID reported", output);
        const uint32_t orphanProcessId = static_cast<uint32_t>(std::strtoul(output.c_str() + exitingRootMarkerPosition + 11, nullptr, 10));
        GB_ProcessInfo orphanProcessInfo;
        RequireProcessResultSucceeded(GB_SystemProcess::GetProcessInfo(orphanProcessId, orphanProcessInfo), "Query orphan snapshot child");
        const GB_ProcessIdentity exitedRootIdentity = processInstance.GetIdentity();
        uint32_t exitedRootCode = 0;
        GB_ProcessWaitOptions rootExitWait;
        rootExitWait.timeoutMilliseconds = 5000;
        RequireProcessResultSucceeded(processInstance.WaitForExit(exitedRootCode, rootExitWait), "Wait snapshot root exit");
        RequireProcessResultSucceeded(GB_SystemProcess::TerminateProcessTreeSnapshot(exitedRootIdentity, 44), "Terminate snapshot descendants after root exit");
        RequireTrueSystemProcess(WaitUntilProcessMissing(orphanProcessInfo.identity, 5000), "Snapshot termination skips exited root and removes descendant", std::to_string(orphanProcessId));
    }

    void TestConcurrentLaunchAndLegacyCompatibility()
    {
        GB_ProcessStartOptions concurrentOutputOptions = MakeChildOptions("dual-large");
        concurrentOutputOptions.arguments.push_back(std::to_string(4 * 1024 * 1024));
        concurrentOutputOptions.redirectStandardOutput = true;
        concurrentOutputOptions.redirectStandardError = true;
        GB_ProcessInstance concurrentOutputProcess;
        RequireProcessResultSucceeded(GB_SystemProcess::Start(concurrentOutputOptions, concurrentOutputProcess), "Start concurrent output query child");
        std::atomic<bool> outputWaitCompleted(false);
        std::atomic<bool> outputWaitFailed(false);
        std::thread outputWaitThread([&]()
            {
                uint32_t exitCode = 0;
                GB_ProcessWaitOptions waitOptions;
                waitOptions.timeoutMilliseconds = 10000;
                outputWaitFailed.store(concurrentOutputProcess.WaitForExit(exitCode, waitOptions).IsFailed() || exitCode != 0, std::memory_order_release);
                outputWaitCompleted.store(true, std::memory_order_release);
            });
        bool concurrentOutputReadFailed = false;
        while (!outputWaitCompleted.load(std::memory_order_acquire))
        {
            std::string output;
            bool truncated = false;
            if (concurrentOutputProcess.GetStandardOutput(output, truncated).IsFailed())
            {
                concurrentOutputReadFailed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        outputWaitThread.join();
        RequireTrueSystemProcess(!concurrentOutputReadFailed && !outputWaitFailed.load(std::memory_order_acquire), "Concurrent output query and exit wait", "concurrent read or wait failed");

        DWORD handleCountBefore = 0;
        RequireTrueSystemProcess(::GetProcessHandleCount(::GetCurrentProcess(), &handleCountBefore) != FALSE, "Read handle count before launch stress", std::to_string(::GetLastError()));
        for (size_t launchIndex = 0; launchIndex < 32; launchIndex++)
        {
            GB_ProcessStartOptions stressOptions = MakeChildOptions("exit-code");
            stressOptions.arguments.push_back("0");
            stressOptions.redirectStandardOutput = true;
            stressOptions.redirectStandardError = true;
            stressOptions.jobOptions.enabled = true;
            GB_ProcessRunResult stressResult;
            RequireProcessResultSucceeded(GB_SystemProcess::Run(stressOptions, stressResult), "Sequential process handle stress");
        }
        DWORD handleCountAfter = 0;
        RequireTrueSystemProcess(::GetProcessHandleCount(::GetCurrentProcess(), &handleCountAfter) != FALSE, "Read handle count after launch stress", std::to_string(::GetLastError()));
        RequireTrueSystemProcess(handleCountAfter <= handleCountBefore + 4, "No persistent handle leak after launch stress", std::to_string(handleCountBefore) + " -> " + std::to_string(handleCountAfter));

        const size_t threadCount = 8;
        std::atomic<int> failureCount(0);
        std::vector<std::thread> threads;
        for (size_t threadIndex = 0; threadIndex < threadCount; threadIndex++)
        {
            threads.push_back(std::thread([threadIndex, &failureCount]()
                {
                    GB_ProcessStartOptions options = MakeChildOptions("arguments");
                    const std::string token = "concurrent-" + std::to_string(threadIndex);
                    options.arguments.push_back(token);
                    options.redirectStandardOutput = true;
                    GB_ProcessRunResult runResult;
                    if (GB_SystemProcess::Run(options, runResult).IsFailed() || runResult.standardOutput.find(token) == std::string::npos)
                    {
                        failureCount++;
                    }
                }));
        }
        for (size_t threadIndex = 0; threadIndex < threads.size(); threadIndex++)
        {
            threads[threadIndex].join();
        }
        RequireTrueSystemProcess(failureCount.load() == 0, "Concurrent process launch and handle inheritance", std::to_string(failureCount.load()));

        std::string executablePath;
        RequireProcessResultSucceeded(GB_SystemProcess::GetCurrentExecutablePath(executablePath), "Get executable path for legacy API");
        int processId = 0;
        const std::vector<std::string> arguments = { "--gb-system-process-child", "sleep", "60000" };
        RequireTrueSystemProcess(GB_StartProcess(executablePath, arguments, std::string(), &processId) && processId > 0, "Legacy GB_StartProcess", std::to_string(processId));
        GB_ProcessInfo processInfo;
        RequireTrueSystemProcess(GB_GetProcessInfo(processId, processInfo) && processInfo.identity.IsStrong(), "Legacy GB_GetProcessInfo", std::to_string(processId));
        RequireTrueSystemProcess(GB_TerminateProcessById(processId, 1000, true), "Legacy GB_TerminateProcessById", std::to_string(processId));

        const std::vector<std::string> guiArguments = { "--gb-system-process-child", "gui" };
        RequireTrueSystemProcess(GB_StartProcess(executablePath, guiArguments, std::string(), &processId) && processId > 0, "Legacy start GUI process", std::to_string(processId));
        RequireTrueSystemProcess(GB_GetProcessInfo(processId, processInfo) && processInfo.identity.IsStrong(), "Legacy query GUI process", std::to_string(processId));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        RequireTrueSystemProcess(GB_TerminateProcessById(processId, 0, false), "Legacy close request reports success without waiting", std::to_string(processId));
        RequireTrueSystemProcess(WaitUntilProcessMissing(processInfo.identity, 5000), "Legacy GUI close exits process", std::to_string(processId));
    }
#endif
}

int RunGB_SystemProcessChildMode(int argc, char* argv[], bool& handled)
{
    handled = false;
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    return 0;
#else
    (void)argc;
    (void)argv;
    const std::vector<std::string> arguments = GetWideArgumentsUtf8();
    for (size_t argumentIndex = 1; argumentIndex < arguments.size(); argumentIndex++)
    {
        if (arguments[argumentIndex] == "--gb-system-process-child")
        {
            handled = true;
            return RunSystemProcessChild(arguments, argumentIndex + 1);
        }
    }
    return 0;
#endif
}

int RunGB_SystemProcessTests()
{
#if !defined(_WIN32)
    std::cout << "GB_SystemProcess tests skipped on non-Windows." << std::endl;
    return 0;
#else
    try
    {
        TestCommandLineEscapingCorpus();
        TestOptionValidation();
        TestArgumentsEnvironmentAndWorkingDirectory();
        TestStandardStreamsAndEncoding();
        TestLargeOutputTimeoutAndCallbacks();
        TestSuspendedQueryAndCancellation();
        TestWindowCloseAndSafety();
        TestJobAndDestructorPolicies();
        TestConcurrentLaunchAndLegacyCompatibility();
    }
    catch (const std::exception& exceptionObject)
    {
        std::cerr << "[FAILED] Unexpected system process test exception\n" << exceptionObject.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "[FAILED] Unknown system process test exception" << std::endl;
        return 1;
    }
    std::cout << "GB_SystemProcess tests passed. Total checks: " << totalSystemProcessCaseCount << std::endl;
    return 0;
#endif
}
