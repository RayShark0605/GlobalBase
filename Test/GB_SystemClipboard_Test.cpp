#include "GB_RunTests.h"
#include "Desktop/GB_SystemClipboard.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
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
    int totalSystemClipboardCaseCount = 0;

    [[noreturn]] void FailSystemClipboardTest(const std::string& caseName, const std::string& detail)
    {
        std::cerr << "[FAILED] " << caseName << "\n" << detail << std::endl;
        std::exit(1);
    }

    void RequireTrueSystemClipboard(const bool condition, const std::string& caseName, const std::string& detail)
    {
        totalSystemClipboardCaseCount++;
        if (!condition)
        {
            FailSystemClipboardTest(caseName, detail);
        }
    }

    void RequireClipboardResultSucceeded(const GB_SystemResult& result, const std::string& caseName)
    {
        totalSystemClipboardCaseCount++;
        if (result.IsFailed())
        {
            FailSystemClipboardTest(caseName, result.ToString());
        }
    }

#if defined(_WIN32)
    bool PrepareIsolatedWindowStation(std::string& errorMessage)
    {
        errorMessage.clear();
        SECURITY_DESCRIPTOR securityDescriptor = {};
        if (::InitializeSecurityDescriptor(&securityDescriptor, SECURITY_DESCRIPTOR_REVISION) == FALSE || ::SetSecurityDescriptorDacl(&securityDescriptor, TRUE, nullptr, FALSE) == FALSE)
        {
            errorMessage = "Preparing window station security failed with error " + std::to_string(::GetLastError()) + ".";
            return false;
        }
        SECURITY_ATTRIBUTES securityAttributes = {};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.lpSecurityDescriptor = &securityDescriptor;
        const HWINSTA windowStation = ::CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, &securityAttributes);
        if (windowStation == nullptr)
        {
            errorMessage = "CreateWindowStationW failed with error " + std::to_string(::GetLastError()) + ".";
            return false;
        }
        if (::SetProcessWindowStation(windowStation) == FALSE)
        {
            errorMessage = "SetProcessWindowStation failed with error " + std::to_string(::GetLastError()) + ".";
            return false;
        }

        const HDESK desktop = ::CreateDesktopW(L"ClipboardTestDesktop", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (desktop == nullptr)
        {
            errorMessage = "CreateDesktopW failed with error " + std::to_string(::GetLastError()) + ".";
            return false;
        }
        if (::SetThreadDesktop(desktop) == FALSE)
        {
            errorMessage = "SetThreadDesktop failed with error " + std::to_string(::GetLastError()) + ".";
            return false;
        }
        return true;
    }

    int LaunchIsolatedClipboardTestProcess()
    {
        wchar_t executablePath[MAX_PATH] = {};
        const DWORD executablePathLength = ::GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(sizeof(executablePath) / sizeof(executablePath[0])));
        if (executablePathLength == 0 || executablePathLength >= sizeof(executablePath) / sizeof(executablePath[0]))
        {
            std::cerr << "[FAILED] GetModuleFileNameW for isolated clipboard tests. Error: " << ::GetLastError() << std::endl;
            return 1;
        }

        std::wstring commandLine = L"\"";
        commandLine.append(executablePath, executablePathLength);
        commandLine += L"\" --gb-system-clipboard-isolated-child";
        std::vector<wchar_t> commandLineBuffer(commandLine.begin(), commandLine.end());
        commandLineBuffer.push_back(L'\0');

        STARTUPINFOW startupInfo = {};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInformation = {};
        if (::CreateProcessW(executablePath, commandLineBuffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startupInfo, &processInformation) == FALSE)
        {
            std::cerr << "[FAILED] CreateProcessW for isolated clipboard tests. Error: " << ::GetLastError() << std::endl;
            return 1;
        }

        const DWORD waitResult = ::WaitForSingleObject(processInformation.hProcess, 120000);
        DWORD exitCode = 1;
        if (waitResult != WAIT_OBJECT_0 || ::GetExitCodeProcess(processInformation.hProcess, &exitCode) == FALSE)
        {
            std::cerr << "[FAILED] Waiting for isolated clipboard tests. Error: " << ::GetLastError() << std::endl;
            exitCode = 1;
        }
        (void)::CloseHandle(processInformation.hThread);
        (void)::CloseHandle(processInformation.hProcess);
        return static_cast<int>(exitCode);
    }

    bool SetExternalBitmap()
    {
        const HDC screenDeviceContext = ::GetDC(nullptr);
        if (screenDeviceContext == nullptr)
        {
            return false;
        }
        const HDC memoryDeviceContext = ::CreateCompatibleDC(screenDeviceContext);
        BITMAPINFO bitmapInfo = {};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = 2;
        bitmapInfo.bmiHeader.biHeight = -1;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        void* bitmapBits = nullptr;
        const HBITMAP bitmapHandle = memoryDeviceContext == nullptr ? nullptr : ::CreateDIBSection(screenDeviceContext, &bitmapInfo, DIB_RGB_COLORS, &bitmapBits, nullptr, 0);
        if (memoryDeviceContext == nullptr || bitmapHandle == nullptr)
        {
            if (memoryDeviceContext != nullptr)
            {
                (void)::DeleteDC(memoryDeviceContext);
            }
            (void)::ReleaseDC(nullptr, screenDeviceContext);
            return false;
        }
        const HGDIOBJ previousObject = ::SelectObject(memoryDeviceContext, bitmapHandle);
        const bool pixelsSet = previousObject != nullptr && previousObject != HGDI_ERROR && ::SetPixelV(memoryDeviceContext, 0, 0, RGB(10, 20, 30)) != FALSE && ::SetPixelV(memoryDeviceContext, 1, 0, RGB(40, 50, 60)) != FALSE;
        if (pixelsSet && bitmapBits != nullptr)
        {
            const unsigned char expectedPixels[] =
            {
                30, 20, 10, 0,
                60, 50, 40, 0
            };
            std::memcpy(bitmapBits, expectedPixels, sizeof(expectedPixels));
        }
        if (previousObject != nullptr && previousObject != HGDI_ERROR)
        {
            (void)::SelectObject(memoryDeviceContext, previousObject);
        }
        (void)::DeleteDC(memoryDeviceContext);
        (void)::ReleaseDC(nullptr, screenDeviceContext);
        if (!pixelsSet)
        {
            (void)::DeleteObject(bitmapHandle);
            return false;
        }

        const HWND ownerWindow = ::CreateWindowExW(0, L"STATIC", L"External Bitmap Owner", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (ownerWindow == nullptr || ::OpenClipboard(ownerWindow) == FALSE)
        {
            (void)::DeleteObject(bitmapHandle);
            return false;
        }
        bool succeeded = false;
        if (::EmptyClipboard() != FALSE && ::SetClipboardData(CF_BITMAP, bitmapHandle) != nullptr)
        {
            succeeded = true;
        }
        (void)::CloseClipboard();
        (void)::DestroyWindow(ownerWindow);
        if (!succeeded)
        {
            (void)::DeleteObject(bitmapHandle);
        }
        return succeeded;
    }

    bool SetMalformedDibV5()
    {
        const HGLOBAL memoryHandle = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 32);
        if (memoryHandle == nullptr)
        {
            return false;
        }
        const HWND ownerWindow = ::CreateWindowExW(0, L"STATIC", L"Malformed DIB Owner", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (ownerWindow == nullptr || ::OpenClipboard(ownerWindow) == FALSE)
        {
            (void)::GlobalFree(memoryHandle);
            return false;
        }
        bool succeeded = false;
        if (::EmptyClipboard() != FALSE && ::SetClipboardData(CF_DIBV5, memoryHandle) != nullptr)
        {
            succeeded = true;
        }
        (void)::CloseClipboard();
        (void)::DestroyWindow(ownerWindow);
        if (!succeeded)
        {
            (void)::GlobalFree(memoryHandle);
        }
        return succeeded;
    }

    bool SetExternalUnicodeText(const std::wstring& text)
    {
        const size_t dataSize = (text.size() + 1) * sizeof(wchar_t);
        const HGLOBAL memoryHandle = ::GlobalAlloc(GMEM_MOVEABLE, dataSize);
        if (memoryHandle == nullptr)
        {
            return false;
        }
        void* memoryData = ::GlobalLock(memoryHandle);
        if (memoryData == nullptr)
        {
            (void)::GlobalFree(memoryHandle);
            return false;
        }
        std::memcpy(memoryData, text.c_str(), dataSize);
        (void)::GlobalUnlock(memoryHandle);

        const HWND ownerWindow = ::CreateWindowExW(0, L"STATIC", L"External Text Owner", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (ownerWindow == nullptr || ::OpenClipboard(ownerWindow) == FALSE)
        {
            (void)::GlobalFree(memoryHandle);
            return false;
        }
        bool succeeded = false;
        if (::EmptyClipboard() != FALSE && ::SetClipboardData(CF_UNICODETEXT, memoryHandle) != nullptr)
        {
            succeeded = true;
        }
        (void)::CloseClipboard();
        (void)::DestroyWindow(ownerWindow);
        if (!succeeded)
        {
            (void)::GlobalFree(memoryHandle);
        }
        return succeeded;
    }

    bool SetExternalRegisteredFormat(const wchar_t* formatName)
    {
        const UINT formatId = ::RegisterClipboardFormatW(formatName);
        if (formatId == 0)
        {
            return false;
        }
        const char formatData[] = "x";
        const HGLOBAL memoryHandle = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(formatData));
        if (memoryHandle == nullptr)
        {
            return false;
        }
        void* memoryData = ::GlobalLock(memoryHandle);
        if (memoryData == nullptr)
        {
            (void)::GlobalFree(memoryHandle);
            return false;
        }
        std::memcpy(memoryData, formatData, sizeof(formatData));
        (void)::GlobalUnlock(memoryHandle);

        const HWND ownerWindow = ::CreateWindowExW(0, L"STATIC", L"External Registered Format Owner", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (ownerWindow == nullptr || ::OpenClipboard(ownerWindow) == FALSE)
        {
            (void)::GlobalFree(memoryHandle);
            return false;
        }
        bool succeeded = false;
        if (::EmptyClipboard() != FALSE && ::SetClipboardData(formatId, memoryHandle) != nullptr)
        {
            succeeded = true;
        }
        (void)::CloseClipboard();
        (void)::DestroyWindow(ownerWindow);
        if (!succeeded)
        {
            (void)::GlobalFree(memoryHandle);
        }
        return succeeded;
    }
#endif

    void TestClipboardBasics()
    {
        uint64_t sequenceBefore = 0;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetSequenceNumber(sequenceBefore), "GetSequenceNumber before clear");
        RequireClipboardResultSucceeded(GB_SystemClipboard::Clear(), "Clear clipboard");

        uint64_t sequenceAfter = 0;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetSequenceNumber(sequenceAfter), "GetSequenceNumber after clear");
        RequireTrueSystemClipboard(sequenceAfter != sequenceBefore || sequenceAfter != 0, "Clipboard sequence changes", "Clearing the clipboard should produce or retain a valid sequence number.");

        bool hasText = true;
        bool hasImage = true;
        bool hasFilePaths = true;
        RequireClipboardResultSucceeded(GB_SystemClipboard::HasText(hasText), "HasText after clear");
        RequireClipboardResultSucceeded(GB_SystemClipboard::HasImage(hasImage), "HasImage after clear");
        RequireClipboardResultSucceeded(GB_SystemClipboard::HasFilePaths(hasFilePaths), "HasFilePaths after clear");
        RequireTrueSystemClipboard(!hasText && !hasImage && !hasFilePaths, "Clipboard empty formats", "A cleared isolated clipboard should not expose text, image, or file paths.");

        bool unchangedAvailability = true;
        const GB_SystemResult invalidFormatResult = GB_SystemClipboard::IsFormatAvailable(0, unchangedAvailability);
        RequireTrueSystemClipboard(invalidFormatResult.IsFailed() && unchangedAvailability, "Format query output unchanged on failure", invalidFormatResult.ToString());
    }

    void TestClipboardText()
    {
        const std::string textCases[] =
        {
            "",
            "ASCII clipboard text",
            u8"\u4E2D\u6587\u526A\u8D34\u677F",
            u8"Emoji \U0001F600 \U0001F680",
            "line1\nline2\r\nline3\rline4"
        };
        for (size_t index = 0; index < sizeof(textCases) / sizeof(textCases[0]); index++)
        {
            RequireClipboardResultSucceeded(GB_SystemClipboard::SetText(textCases[index]), "SetText round trip");
            std::string readText = "unchanged";
            RequireClipboardResultSucceeded(GB_SystemClipboard::GetText(readText), "GetText round trip");
            RequireTrueSystemClipboard(readText == textCases[index], "Text exact round trip", "Clipboard text did not round trip exactly.");
        }

        GB_SystemClipboardTextWriteOptions crlfWriteOptions;
        crlfWriteOptions.newlineMode = GB_SystemClipboardNewlineMode::CrLf;
        RequireClipboardResultSucceeded(GB_SystemClipboard::SetText("a\nb\rc\r\nd", crlfWriteOptions), "SetText CRLF normalization");
        std::string crlfText;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetText(crlfText), "GetText CRLF preserve");
        RequireTrueSystemClipboard(crlfText == "a\r\nb\r\nc\r\nd", "CRLF normalized write", "CRLF write normalization produced unexpected text.");

        GB_SystemClipboardTextReadOptions lfReadOptions;
        lfReadOptions.newlineMode = GB_SystemClipboardNewlineMode::Lf;
        std::string lfText;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetText(lfText, lfReadOptions), "GetText LF normalization");
        RequireTrueSystemClipboard(lfText == "a\nb\nc\nd", "LF normalized read", "LF read normalization produced unexpected text.");

        const std::string invalidUtf8("\xC3\x28", 2);
        const GB_SystemResult invalidUtf8Result = GB_SystemClipboard::SetText(invalidUtf8);
        RequireTrueSystemClipboard(invalidUtf8Result.IsFailed() && invalidUtf8Result.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject invalid UTF-8", invalidUtf8Result.ToString());

        const std::string embeddedNull("left\0right", 10);
        const GB_SystemResult embeddedNullResult = GB_SystemClipboard::SetText(embeddedNull);
        RequireTrueSystemClipboard(embeddedNullResult.IsFailed() && embeddedNullResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject embedded NUL", embeddedNullResult.ToString());

        GB_SystemClipboardTextWriteOptions noEmptyOptions;
        noEmptyOptions.allowEmptyText = false;
        const GB_SystemResult emptyResult = GB_SystemClipboard::SetText("", noEmptyOptions);
        RequireTrueSystemClipboard(emptyResult.IsFailed() && emptyResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject disabled empty text", emptyResult.ToString());

        const std::string largeText(static_cast<size_t>(1024) * 1024, 'x');
        RequireClipboardResultSucceeded(GB_SystemClipboard::SetText(largeText), "SetText large text");
        std::string readLargeText;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetText(readLargeText), "GetText large text");
        RequireTrueSystemClipboard(readLargeText == largeText, "Large text round trip", "Large clipboard text did not round trip.");

        GB_SystemClipboardTextWriteOptions exactLimitWriteOptions;
        exactLimitWriteOptions.maxTextBytes = 5;
        RequireClipboardResultSucceeded(GB_SystemClipboard::SetText("12345", exactLimitWriteOptions), "SetText exact UTF-8 byte limit");
        GB_SystemClipboardTextReadOptions exactLimitReadOptions;
        exactLimitReadOptions.maxTextBytes = 5;
        std::string exactLimitText;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetText(exactLimitText, exactLimitReadOptions), "GetText exact UTF-8 byte limit");
        RequireTrueSystemClipboard(exactLimitText == "12345", "Text byte limit excludes terminator", "The text size limit incorrectly counted UTF-16 storage or the terminator.");
    }

    void TestClipboardBusy()
    {
#if defined(_WIN32)
        std::mutex busyMutex;
        std::condition_variable busyCondition;
        bool clipboardOpened = false;
        bool ownerSucceeded = false;
        std::thread ownerThread([&]()
            {
                const HWND ownerWindow = ::CreateWindowExW(0, L"STATIC", L"Busy Clipboard Owner", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
                ownerSucceeded = ownerWindow != nullptr && ::OpenClipboard(ownerWindow) != FALSE;
                {
                    std::lock_guard<std::mutex> lock(busyMutex);
                    clipboardOpened = true;
                }
                busyCondition.notify_all();
                if (ownerSucceeded)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    (void)::CloseClipboard();
                }
                if (ownerWindow != nullptr)
                {
                    (void)::DestroyWindow(ownerWindow);
                }
            });

        {
            std::unique_lock<std::mutex> lock(busyMutex);
            busyCondition.wait(lock, [&]()
                {
                    return clipboardOpened;
                });
        }
        RequireTrueSystemClipboard(ownerSucceeded, "Open clipboard in owner thread", "The busy clipboard test could not acquire the clipboard.");

        GB_SystemClipboardTextWriteOptions writeOptions;
        writeOptions.accessOptions.retryCount = 2;
        writeOptions.accessOptions.initialRetryDelayMilliseconds = 1;
        writeOptions.accessOptions.maxRetryDelayMilliseconds = 2;
        const GB_SystemResult busyResult = GB_SystemClipboard::SetText("busy", writeOptions);
        RequireTrueSystemClipboard(busyResult.IsFailed() && busyResult.errorCode == GB_SystemErrorCode::ResourceBusy, "Clipboard busy error", busyResult.ToString());
        ownerThread.join();
#endif
    }

    void TestClipboardFilePaths()
    {
        const std::string longPath = std::string("C:\\") + std::string(400, 'a') + "\\file.bin";
        const std::vector<std::string> filePaths =
        {
            "C:\\Folder With Spaces\\file.txt",
            u8"C:\\\u4E2D\u6587\u76EE\u5F55\\\u6587\u4EF6.bin",
            "\\\\server\\share\\missing file.dat",
            "C:\\does-not-need-to-exist\\item.tmp",
            longPath
        };
        RequireClipboardResultSucceeded(GB_SystemClipboard::SetFilePaths(filePaths), "SetFilePaths");

        std::vector<std::string> readFilePaths;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetFilePaths(readFilePaths), "GetFilePaths");
        RequireTrueSystemClipboard(readFilePaths == filePaths, "File path round trip", "CF_HDROP paths did not round trip exactly.");

        const std::vector<std::string> emptyPaths;
        const GB_SystemResult emptyPathsResult = GB_SystemClipboard::SetFilePaths(emptyPaths);
        RequireTrueSystemClipboard(emptyPathsResult.IsFailed() && emptyPathsResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject empty file path list", emptyPathsResult.ToString());
    }

    void TestClipboardImages()
    {
        GB_Image sourceImage;
        RequireTrueSystemClipboard(sourceImage.Create(2, 2, GB_ImageDepth::UInt8, 4, false), "Create source BGRA image", "GB_Image::Create failed.");
        RequireTrueSystemClipboard(sourceImage.SetPixelColor(0, 0, GB_ColorRGBA(10, 20, 30, 40)), "Set image pixel 0", "SetPixelColor failed.");
        RequireTrueSystemClipboard(sourceImage.SetPixelColor(0, 1, GB_ColorRGBA(50, 60, 70, 80)), "Set image pixel 1", "SetPixelColor failed.");
        RequireTrueSystemClipboard(sourceImage.SetPixelColor(1, 0, GB_ColorRGBA(90, 100, 110, 120)), "Set image pixel 2", "SetPixelColor failed.");
        RequireTrueSystemClipboard(sourceImage.SetPixelColor(1, 1, GB_ColorRGBA(130, 140, 150, 160)), "Set image pixel 3", "SetPixelColor failed.");

        RequireClipboardResultSucceeded(GB_SystemClipboard::SetImage(sourceImage), "SetImage BGRA");
        GB_Image readImage;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetImage(readImage), "GetImage BGRA");
        RequireTrueSystemClipboard(readImage.GetWidth() == 2 && readImage.GetHeight() == 2 && readImage.GetChannels() == 4 && readImage.GetDepth() == GB_ImageDepth::UInt8, "BGRA image shape", "Clipboard image shape changed.");
        for (size_t rowIndex = 0; rowIndex < 2; rowIndex++)
        {
            for (size_t columnIndex = 0; columnIndex < 2; columnIndex++)
            {
                GB_ColorRGBA sourceColor;
                GB_ColorRGBA readColor;
                RequireTrueSystemClipboard(sourceImage.GetPixelColor(rowIndex, columnIndex, sourceColor), "Read source pixel", "GetPixelColor failed.");
                RequireTrueSystemClipboard(readImage.GetPixelColor(rowIndex, columnIndex, readColor), "Read clipboard pixel", "GetPixelColor failed.");
                RequireTrueSystemClipboard(sourceColor == readColor, "BGRA alpha round trip", "Clipboard DIBV5 pixel or alpha changed.");
            }
        }

        GB_Image grayImage;
        RequireTrueSystemClipboard(grayImage.Create(1, 2, GB_ImageDepth::UInt8, 1, false), "Create gray image", "GB_Image::Create failed.");
        grayImage.GetRowData(0)[0] = 25;
        grayImage.GetRowData(0)[1] = 200;
        RequireClipboardResultSucceeded(GB_SystemClipboard::SetImage(grayImage), "SetImage gray");
        GB_Image readGrayImage;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetImage(readGrayImage), "GetImage gray");
        GB_ColorRGBA grayColor;
        RequireTrueSystemClipboard(readGrayImage.GetPixelColor(0, 1, grayColor) && grayColor.r == 200 && grayColor.g == 200 && grayColor.b == 200 && grayColor.a == 255, "Gray to BGRA conversion", "Gray clipboard conversion was incorrect.");

        GB_Image emptyImage;
        const GB_SystemResult emptyImageResult = GB_SystemClipboard::SetImage(emptyImage);
        RequireTrueSystemClipboard(emptyImageResult.IsFailed() && emptyImageResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Reject empty image", emptyImageResult.ToString());

#if defined(_WIN32)
        RequireTrueSystemClipboard(SetExternalBitmap(), "Set external CF_BITMAP", "Creating external CF_BITMAP clipboard data failed.");
        GB_Image externalBitmapImage;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetImage(externalBitmapImage), "Get external CF_BITMAP");
        RequireTrueSystemClipboard(externalBitmapImage.GetWidth() == 2 && externalBitmapImage.GetHeight() == 1 && externalBitmapImage.GetChannels() == 4, "External bitmap shape", "CF_BITMAP dimensions or channel count changed.");
        GB_ColorRGBA firstColor;
        GB_ColorRGBA secondColor;
        RequireTrueSystemClipboard(externalBitmapImage.GetPixelColor(0, 0, firstColor) && externalBitmapImage.GetPixelColor(0, 1, secondColor), "Read external bitmap pixels", "GetPixelColor failed.");
        const std::string externalColorDetail = "First RGBA=(" + std::to_string(firstColor.r) + "," + std::to_string(firstColor.g) + "," + std::to_string(firstColor.b) + "," + std::to_string(firstColor.a) + "), second RGBA=(" + std::to_string(secondColor.r) + "," + std::to_string(secondColor.g) + "," + std::to_string(secondColor.b) + "," + std::to_string(secondColor.a) + ").";
        RequireTrueSystemClipboard(firstColor.a == 255 && secondColor.a == 255, "External bitmap opaque alpha", externalColorDetail);

        RequireTrueSystemClipboard(SetMalformedDibV5(), "Set malformed CF_DIBV5", "Creating malformed CF_DIBV5 failed.");
        GB_Image unchangedImage = sourceImage.Clone();
        const GB_SystemResult malformedResult = GB_SystemClipboard::GetImage(unchangedImage);
        RequireTrueSystemClipboard(malformedResult.IsFailed(), "Reject malformed CF_DIBV5", "Malformed CF_DIBV5 unexpectedly succeeded.");
        RequireTrueSystemClipboard(unchangedImage.GetWidth() == sourceImage.GetWidth() && unchangedImage.GetHeight() == sourceImage.GetHeight(), "Image output unchanged on failure", "GetImage modified the output image after failure.");
#endif
    }

    void TestClipboardFormats()
    {
        RequireClipboardResultSucceeded(GB_SystemClipboard::SetText("format test"), "SetText for format enumeration");
        std::vector<GB_SystemClipboardFormatInfo> formats;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetFormats(formats), "GetFormats text");
        bool foundUnicodeText = false;
        for (size_t index = 0; index < formats.size(); index++)
        {
            foundUnicodeText = foundUnicodeText || (formats[index].formatId == 13 && formats[index].formatType == GB_SystemClipboardFormatType::UnicodeText && formats[index].formatName == "CF_UNICODETEXT");
        }
        RequireTrueSystemClipboard(foundUnicodeText, "Enumerate CF_UNICODETEXT", "Text clipboard formats did not contain a correctly classified CF_UNICODETEXT.");

        bool available = false;
        RequireClipboardResultSucceeded(GB_SystemClipboard::IsFormatAvailable(13, available), "IsFormatAvailable CF_UNICODETEXT");
        RequireTrueSystemClipboard(available, "CF_UNICODETEXT available", "CF_UNICODETEXT should be available after SetText.");
        RequireTrueSystemClipboard(GB_SystemClipboard::GetFormatTypeName(GB_SystemClipboardFormatType::DibV5) == "DibV5", "Format type name", "DibV5 format type name is incorrect.");

#if defined(_WIN32)
        RequireTrueSystemClipboard(SetExternalRegisteredFormat(L"HTML Format"), "Set external HTML format", "Registering or setting HTML Format failed.");
        formats.clear();
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetFormats(formats), "GetFormats HTML");
        bool foundHtml = false;
        for (size_t index = 0; index < formats.size(); index++)
        {
            foundHtml = foundHtml || (formats[index].formatName == "HTML Format" && formats[index].formatType == GB_SystemClipboardFormatType::Html && formats[index].isRegisteredFormat);
        }
        RequireTrueSystemClipboard(foundHtml, "Classify HTML format", "HTML Format was not classified as a registered HTML clipboard format.");

        RequireTrueSystemClipboard(SetExternalRegisteredFormat(L"Rich Text Format"), "Set external RTF format", "Registering or setting Rich Text Format failed.");
        formats.clear();
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetFormats(formats), "GetFormats RTF");
        bool foundRichText = false;
        for (size_t index = 0; index < formats.size(); index++)
        {
            foundRichText = foundRichText || (formats[index].formatName == "Rich Text Format" && formats[index].formatType == GB_SystemClipboardFormatType::RichText && formats[index].isRegisteredFormat);
        }
        RequireTrueSystemClipboard(foundRichText, "Classify RTF format", "Rich Text Format was not classified as a registered rich-text clipboard format.");
#endif
    }

    void TestClipboardWatcher()
    {
        GB_SystemClipboardWatcher watcher;
        std::mutex eventMutex;
        std::condition_variable eventCondition;
        std::vector<GB_SystemClipboardEvent> receivedEvents;
        watcher.SetClipboardEventCallback([&](const GB_SystemClipboardEvent& event)
            {
                {
                    std::lock_guard<std::mutex> lock(eventMutex);
                    receivedEvents.push_back(event);
                }
                eventCondition.notify_all();
            });

        std::atomic<int> publicEventCount(0);
        GB_EventSubscriptionToken subscriptionToken;
        RequireClipboardResultSucceeded(watcher.GetEventDispatcher().SubscribeAll([&](const GB_Event& event)
            {
                if (event.eventName == "SystemClipboard.Changed")
                {
                    publicEventCount++;
                }
            }, subscriptionToken), "Subscribe watcher public dispatcher");

        RequireClipboardResultSucceeded(watcher.Start(), "Watcher Start first");
        RequireTrueSystemClipboard(watcher.IsRunning(), "Watcher running first", "Watcher should be running.");
        RequireClipboardResultSucceeded(watcher.Start(), "Watcher Start duplicate");
        RequireClipboardResultSucceeded(GB_SystemClipboard::SetText("watcher self write"), "SetText watcher event");

        {
            std::unique_lock<std::mutex> lock(eventMutex);
            const bool eventReceived = eventCondition.wait_for(lock, std::chrono::seconds(5), [&]()
                {
                    return !receivedEvents.empty();
                });
            RequireTrueSystemClipboard(eventReceived, "Watcher receives event", "No clipboard event was received within five seconds.");
        }

        GB_SystemClipboardEvent receivedEvent;
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            receivedEvent = receivedEvents.back();
        }
        RequireTrueSystemClipboard(receivedEvent.eventName == "SystemClipboard.Changed", "Watcher event name", "Clipboard watcher event name is incorrect.");
        RequireTrueSystemClipboard(receivedEvent.sequenceNumber != 0, "Watcher event sequence", "Clipboard watcher sequence number should be nonzero.");
        RequireTrueSystemClipboard(receivedEvent.isSelfWrite, "Watcher self-write detection", "A GB_SystemClipboard write was not marked as a self write.");
        RequireTrueSystemClipboard(receivedEvent.formatsConsistent, "Watcher consistent formats", "Watcher format snapshot was not sequence-consistent.");
        RequireTrueSystemClipboard(!receivedEvent.formats.empty(), "Watcher format snapshot", "Watcher format snapshot should contain formats.");

        const std::chrono::steady_clock::time_point publicDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (publicEventCount.load() == 0 && std::chrono::steady_clock::now() < publicDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        RequireTrueSystemClipboard(publicEventCount.load() > 0, "Watcher public event", "The public event dispatcher did not publish the clipboard event.");

        {
            std::lock_guard<std::mutex> lock(eventMutex);
            receivedEvents.clear();
        }
#if defined(_WIN32)
        RequireTrueSystemClipboard(SetExternalUnicodeText(L"watcher external write"), "Set external watcher text", "Writing clipboard text outside GB_SystemClipboard failed.");
        uint64_t externalSequence = 0;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetSequenceNumber(externalSequence), "Get external watcher sequence");
        {
            std::unique_lock<std::mutex> lock(eventMutex);
            const bool externalEventReceived = eventCondition.wait_for(lock, std::chrono::seconds(5), [&]()
                {
                    for (size_t index = 0; index < receivedEvents.size(); index++)
                    {
                        if (receivedEvents[index].sequenceNumber == externalSequence)
                        {
                            return true;
                        }
                    }
                    return false;
                });
            RequireTrueSystemClipboard(externalEventReceived, "Watcher receives external event", "No matching external clipboard event was received.");
        }
        bool externalMarkedSelf = true;
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            for (size_t index = 0; index < receivedEvents.size(); index++)
            {
                if (receivedEvents[index].sequenceNumber == externalSequence)
                {
                    externalMarkedSelf = receivedEvents[index].isSelfWrite;
                }
            }
        }
        RequireTrueSystemClipboard(!externalMarkedSelf, "Watcher external-write detection", "An external clipboard write was incorrectly marked as a self write.");
#endif

        {
            std::lock_guard<std::mutex> lock(eventMutex);
            receivedEvents.clear();
        }
        for (size_t index = 0; index < 128; index++)
        {
            RequireClipboardResultSucceeded(GB_SystemClipboard::SetText("burst-" + std::to_string(index)), "SetText watcher burst");
        }
        uint64_t burstSequence = 0;
        RequireClipboardResultSucceeded(GB_SystemClipboard::GetSequenceNumber(burstSequence), "Get watcher burst sequence");
        {
            std::unique_lock<std::mutex> lock(eventMutex);
            const bool burstEventReceived = eventCondition.wait_for(lock, std::chrono::seconds(10), [&]()
                {
                    for (size_t index = 0; index < receivedEvents.size(); index++)
                    {
                        if (receivedEvents[index].sequenceNumber == burstSequence && receivedEvents[index].isSelfWrite)
                        {
                            return true;
                        }
                    }
                    return false;
                });
            RequireTrueSystemClipboard(burstEventReceived, "Watcher handles burst updates", "The latest high-frequency self write was not delivered or was misclassified.");
        }

        RequireClipboardResultSucceeded(watcher.Stop(), "Watcher Stop first");
        RequireTrueSystemClipboard(!watcher.IsRunning(), "Watcher stopped first", "Watcher should be stopped.");
        RequireClipboardResultSucceeded(watcher.Stop(), "Watcher Stop duplicate");
        RequireClipboardResultSucceeded(watcher.Start(), "Watcher restart");
        RequireTrueSystemClipboard(watcher.IsRunning(), "Watcher running after restart", "Watcher should restart.");
        RequireClipboardResultSucceeded(watcher.Stop(), "Watcher Stop after restart");

        GB_SystemClipboardWatcherOptions invalidOptions;
        invalidOptions.formatAccessOptions.retryCount = 1001;
        GB_SystemClipboardWatcher invalidWatcher(invalidOptions);
        const GB_SystemResult invalidStartResult = invalidWatcher.Start();
        RequireTrueSystemClipboard(invalidStartResult.IsFailed() && invalidStartResult.errorCode == GB_SystemErrorCode::InvalidArgument, "Watcher validates access options", invalidStartResult.ToString());

        GB_SystemClipboardWatcher callbackStopWatcher;
        std::atomic<int> callbackStopState(0);
        callbackStopWatcher.SetClipboardEventCallback([&](const GB_SystemClipboardEvent&)
            {
                const GB_SystemResult stopResult = callbackStopWatcher.Stop();
                callbackStopState.store(stopResult.IsSucceeded() ? 1 : -1);
            });
        RequireClipboardResultSucceeded(callbackStopWatcher.Start(), "Watcher Start for callback stop");
        RequireClipboardResultSucceeded(GB_SystemClipboard::SetText("callback stop"), "SetText for callback stop");
        const std::chrono::steady_clock::time_point callbackStopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (callbackStopState.load() == 0 && std::chrono::steady_clock::now() < callbackStopDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        RequireTrueSystemClipboard(callbackStopState.load() == 1 && !callbackStopWatcher.IsRunning(), "Watcher Stop from callback", "Stopping the watcher from its callback failed.");
        callbackStopWatcher.SetClipboardEventCallback(GB_SystemClipboardWatcher::ClipboardEventCallback());
        GB_SystemResult callbackRestartResult = GB_SystemResult::Failed(GB_SystemErrorCode::InvalidState, "callback restart");
        const std::chrono::steady_clock::time_point callbackRestartDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (callbackRestartResult.IsFailed() && std::chrono::steady_clock::now() < callbackRestartDeadline)
        {
            callbackRestartResult = callbackStopWatcher.Start();
            if (callbackRestartResult.IsFailed())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        RequireClipboardResultSucceeded(callbackRestartResult, "Watcher restart after callback stop");
        RequireClipboardResultSucceeded(callbackStopWatcher.Stop(), "Watcher final stop after callback stop");

        {
            GB_SystemClipboardWatcher destructorWatcher;
            RequireClipboardResultSucceeded(destructorWatcher.Start(), "Watcher Start for destructor stop");
        }
    }
}

int RunGB_SystemClipboardTests()
{
#if !defined(_WIN32)
    std::cout << "GB_SystemClipboard tests skipped on non-Windows." << std::endl;
    return 0;
#else
    return LaunchIsolatedClipboardTestProcess();
#endif
}

int RunGB_SystemClipboardIsolatedTests()
{
#if !defined(_WIN32)
    return 0;
#else
    std::string isolationError;
    if (!PrepareIsolatedWindowStation(isolationError))
    {
        std::cerr << "[FAILED] Isolated clipboard window station\n" << isolationError << std::endl;
        return 1;
    }

    try
    {
        TestClipboardBasics();
        TestClipboardText();
        TestClipboardBusy();
        TestClipboardFilePaths();
        TestClipboardImages();
        TestClipboardFormats();
        TestClipboardWatcher();
    }
    catch (const std::exception& exceptionObject)
    {
        std::cerr << "[FAILED] Unexpected clipboard test exception\n" << exceptionObject.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "[FAILED] Unknown clipboard test exception" << std::endl;
        return 1;
    }

    std::cout << "GB_SystemClipboard tests passed. Total checks: " << totalSystemClipboardCaseCount << std::endl;
    return 0;
#endif
}
