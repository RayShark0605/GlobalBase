#include "GB_Mouse.h"

#include "GB_Screen.h"
#include "../Geometry/GB_Rectangle.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
#if defined(_WIN32)
    namespace internal
    {
        class ScreenDcScope
        {
        public:
            ScreenDcScope()
            {
                screenDc = ::GetDC(nullptr);
            }

            ~ScreenDcScope()
            {
                if (screenDc != nullptr)
                {
                    ::ReleaseDC(nullptr, screenDc);
                    screenDc = nullptr;
                }
            }

            HDC Get() const
            {
                return screenDc;
            }

            bool IsValid() const
            {
                return screenDc != nullptr;
            }

        private:
            HDC screenDc = nullptr;
        };

        class CompatibleDcScope
        {
        public:
            explicit CompatibleDcScope(HDC sourceDc)
            {
                memoryDc = ::CreateCompatibleDC(sourceDc);
            }

            ~CompatibleDcScope()
            {
                if (memoryDc != nullptr)
                {
                    ::DeleteDC(memoryDc);
                    memoryDc = nullptr;
                }
            }

            HDC Get() const
            {
                return memoryDc;
            }

            bool IsValid() const
            {
                return memoryDc != nullptr;
            }

        private:
            HDC memoryDc = nullptr;
        };

        class BitmapScope
        {
        public:
            explicit BitmapScope(HBITMAP bitmapHandle) : bitmapHandle(bitmapHandle)
            {
            }

            ~BitmapScope()
            {
                if (bitmapHandle != nullptr)
                {
                    ::DeleteObject(bitmapHandle);
                    bitmapHandle = nullptr;
                }
            }

            HBITMAP Get() const
            {
                return bitmapHandle;
            }

            bool IsValid() const
            {
                return bitmapHandle != nullptr;
            }

        private:
            HBITMAP bitmapHandle = nullptr;
        };

        class SelectObjectScope
        {
        public:
            SelectObjectScope(HDC dcHandle, HGDIOBJ objectHandle) : dcHandle(dcHandle)
            {
                if (dcHandle != nullptr && objectHandle != nullptr)
                {
                    const HGDIOBJ oldObject = ::SelectObject(dcHandle, objectHandle);
                    if (oldObject != nullptr && oldObject != HGDI_ERROR)
                    {
                        oldObjectHandle = oldObject;
                        isValid = true;
                    }
                }
            }

            ~SelectObjectScope()
            {
                if (isValid && dcHandle != nullptr)
                {
                    (void)::SelectObject(dcHandle, oldObjectHandle);
                }
            }

            bool IsValid() const
            {
                return isValid;
            }

        private:
            HDC dcHandle = nullptr;
            HGDIOBJ oldObjectHandle = nullptr;
            bool isValid = false;
        };

        class IconBitmapScope
        {
        public:
            explicit IconBitmapScope(HBITMAP bitmapHandle) : bitmapHandle(bitmapHandle)
            {
            }

            ~IconBitmapScope()
            {
                if (bitmapHandle != nullptr)
                {
                    ::DeleteObject(bitmapHandle);
                    bitmapHandle = nullptr;
                }
            }

            HBITMAP Get() const
            {
                return bitmapHandle;
            }

        private:
            HBITMAP bitmapHandle = nullptr;
        };

        class DpiAwarenessScope
        {
        public:
            DpiAwarenessScope()
            {
                const HMODULE user32Module = ::GetModuleHandleW(L"user32.dll");
                if (user32Module == nullptr)
                {
                    return;
                }

                const auto setThreadDpiAwarenessContext = reinterpret_cast<SetThreadDpiAwarenessContextFunction>(::GetProcAddress(user32Module, "SetThreadDpiAwarenessContext"));
                if (setThreadDpiAwarenessContext == nullptr)
                {
                    return;
                }

                setThreadDpiAwarenessContextFunction = setThreadDpiAwarenessContext;
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
                previousContext = setThreadDpiAwarenessContextFunction(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#else
                previousContext = setThreadDpiAwarenessContextFunction(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4));
#endif
                isActive = (previousContext != nullptr);
            }

            ~DpiAwarenessScope()
            {
                if (isActive && setThreadDpiAwarenessContextFunction != nullptr)
                {
                    (void)setThreadDpiAwarenessContextFunction(previousContext);
                }
            }

        private:
            using SetThreadDpiAwarenessContextFunction = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT dpiContext);

            SetThreadDpiAwarenessContextFunction setThreadDpiAwarenessContextFunction = nullptr;
            DPI_AWARENESS_CONTEXT previousContext = nullptr;
            bool isActive = false;
        };

        static bool TryCalculatePixelBufferByteCount(const int width, const int height, size_t& byteCount)
        {
            byteCount = 0;

            if (width <= 0 || height <= 0)
            {
                return false;
            }

            const uint64_t widthValue = static_cast<uint64_t>(width);
            const uint64_t heightValue = static_cast<uint64_t>(height);
            const uint64_t pixelByteCount = widthValue * heightValue * 4ull;
            if (pixelByteCount > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
            {
                return false;
            }

            byteCount = static_cast<size_t>(pixelByteCount);
            return true;
        }

        static bool IsFinitePoint(const GB_Point2d& point)
        {
            return std::isfinite(point.x) && std::isfinite(point.y);
        }

        static bool IsPointInHalfOpenRectangle(const GB_Point2d& point, const GB_Rectangle& rectangle)
        {
            if (!IsFinitePoint(point) || !rectangle.IsValid())
            {
                return false;
            }

            return point.x >= rectangle.minX && point.x < rectangle.maxX && point.y >= rectangle.minY && point.y < rectangle.maxY;
        }

        static bool TryGetCurrentCursorInfo(CURSORINFO& cursorInfo)
        {
            std::memset(&cursorInfo, 0, sizeof(cursorInfo));
            cursorInfo.cbSize = sizeof(cursorInfo);
            return ::GetCursorInfo(&cursorInfo) != FALSE;
        }

        static bool TryGetCurrentVisibleCursorHandle(HCURSOR& cursorHandle)
        {
            cursorHandle = nullptr;

            CURSORINFO cursorInfo = {};
            if (!TryGetCurrentCursorInfo(cursorInfo))
            {
                return false;
            }

            if ((cursorInfo.flags & CURSOR_SHOWING) == 0 || cursorInfo.hCursor == nullptr)
            {
                return false;
            }

            cursorHandle = cursorInfo.hCursor;
            return true;
        }

        static bool TryGetCurrentPhysicalCursorPosition(POINT& cursorPoint)
        {
            cursorPoint.x = 0;
            cursorPoint.y = 0;

            const HMODULE user32Module = ::GetModuleHandleW(L"user32.dll");
            if (user32Module != nullptr)
            {
                using GetPhysicalCursorPosFunction = BOOL(WINAPI*)(LPPOINT lpPoint);
                const auto getPhysicalCursorPos = reinterpret_cast<GetPhysicalCursorPosFunction>(::GetProcAddress(user32Module, "GetPhysicalCursorPos"));
                if (getPhysicalCursorPos != nullptr && getPhysicalCursorPos(&cursorPoint) != FALSE)
                {
                    return true;
                }
            }

            if (::GetCursorPos(&cursorPoint) != FALSE)
            {
                return true;
            }

            CURSORINFO cursorInfo = {};
            if (TryGetCurrentCursorInfo(cursorInfo))
            {
                cursorPoint = cursorInfo.ptScreenPos;
                return true;
            }

            return false;
        }

        static void CleanupIconInfoBitmaps(ICONINFO& iconInfo)
        {
            if (iconInfo.hbmMask != nullptr)
            {
                ::DeleteObject(iconInfo.hbmMask);
                iconInfo.hbmMask = nullptr;
            }

            if (iconInfo.hbmColor != nullptr)
            {
                ::DeleteObject(iconInfo.hbmColor);
                iconInfo.hbmColor = nullptr;
            }
        }

        static bool TryGetCursorBitmapSize(const HCURSOR cursorHandle, int& cursorWidth, int& cursorHeight, ICONINFO& iconInfo)
        {
            cursorWidth = 0;
            cursorHeight = 0;
            std::memset(&iconInfo, 0, sizeof(iconInfo));

            if (cursorHandle == nullptr)
            {
                return false;
            }

            if (::GetIconInfo(cursorHandle, &iconInfo) == FALSE)
            {
                return false;
            }

            BITMAP bitmapInfo = {};
            if (iconInfo.hbmColor != nullptr)
            {
                if (::GetObjectW(iconInfo.hbmColor, sizeof(bitmapInfo), &bitmapInfo) <= 0)
                {
                    CleanupIconInfoBitmaps(iconInfo);
                    return false;
                }

                cursorWidth = bitmapInfo.bmWidth;
                cursorHeight = std::abs(bitmapInfo.bmHeight);
                if (!(cursorWidth > 0 && cursorHeight > 0))
                {
                    CleanupIconInfoBitmaps(iconInfo);
                    return false;
                }

                return true;
            }

            if (iconInfo.hbmMask == nullptr)
            {
                CleanupIconInfoBitmaps(iconInfo);
                return false;
            }

            if (::GetObjectW(iconInfo.hbmMask, sizeof(bitmapInfo), &bitmapInfo) <= 0)
            {
                CleanupIconInfoBitmaps(iconInfo);
                return false;
            }

            const int maskBitmapHeight = std::abs(bitmapInfo.bmHeight);
            if (bitmapInfo.bmWidth <= 0 || maskBitmapHeight <= 0 || (maskBitmapHeight % 2) != 0)
            {
                CleanupIconInfoBitmaps(iconInfo);
                return false;
            }

            cursorWidth = bitmapInfo.bmWidth;
            cursorHeight = maskBitmapHeight / 2;
            if (!(cursorWidth > 0 && cursorHeight > 0))
            {
                CleanupIconInfoBitmaps(iconInfo);
                return false;
            }

            return true;
        }

        static void FillBgraBuffer(const GB_ColorRGBA& backgroundColor, std::vector<uint8_t>& bgraBuffer)
        {
            const uint8_t blue = backgroundColor.b;
            const uint8_t green = backgroundColor.g;
            const uint8_t red = backgroundColor.r;
            const uint8_t alpha = backgroundColor.a;

            for (size_t pixelOffset = 0; pixelOffset + 3 < bgraBuffer.size(); pixelOffset += 4)
            {
                bgraBuffer[pixelOffset + 0] = blue;
                bgraBuffer[pixelOffset + 1] = green;
                bgraBuffer[pixelOffset + 2] = red;
                bgraBuffer[pixelOffset + 3] = alpha;
            }
        }

        static bool TryRenderCursorToBgraBuffer(const HCURSOR cursorHandle, const int cursorWidth, const int cursorHeight, const GB_ColorRGBA& backgroundColor, std::vector<uint8_t>& bgraBuffer)
        {
            bgraBuffer.clear();

            size_t pixelBufferByteCount = 0;
            if (cursorHandle == nullptr || !TryCalculatePixelBufferByteCount(cursorWidth, cursorHeight, pixelBufferByteCount))
            {
                return false;
            }

            try
            {
                bgraBuffer.resize(pixelBufferByteCount);
            }
            catch (...)
            {
                bgraBuffer.clear();
                return false;
            }

            FillBgraBuffer(backgroundColor, bgraBuffer);

            ScreenDcScope screenDc;
            if (!screenDc.IsValid())
            {
                bgraBuffer.clear();
                return false;
            }

            CompatibleDcScope memoryDc(screenDc.Get());
            if (!memoryDc.IsValid())
            {
                bgraBuffer.clear();
                return false;
            }

            BITMAPINFO bitmapInfo = {};
            bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
            bitmapInfo.bmiHeader.biWidth = cursorWidth;
            bitmapInfo.bmiHeader.biHeight = -cursorHeight;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            void* bitmapBits = nullptr;
            BitmapScope bitmap(::CreateDIBSection(screenDc.Get(), &bitmapInfo, DIB_RGB_COLORS, &bitmapBits, nullptr, 0));
            if (!bitmap.IsValid() || bitmapBits == nullptr)
            {
                bgraBuffer.clear();
                return false;
            }

            std::memcpy(bitmapBits, bgraBuffer.data(), pixelBufferByteCount);

            SelectObjectScope selectBitmap(memoryDc.Get(), bitmap.Get());
            if (!selectBitmap.IsValid())
            {
                bgraBuffer.clear();
                return false;
            }

            if (::DrawIconEx(memoryDc.Get(), 0, 0, cursorHandle, cursorWidth, cursorHeight, 0, nullptr, DI_NORMAL) == FALSE)
            {
                bgraBuffer.clear();
                return false;
            }

            ::GdiFlush();
            std::memcpy(bgraBuffer.data(), bitmapBits, pixelBufferByteCount);
            return true;
        }

        static uint8_t ClampDoubleToByte(const double value)
        {
            if (!(value > 0.0))
            {
                return 0;
            }

            if (value >= 255.0)
            {
                return 255;
            }

            return static_cast<uint8_t>(std::llround(value));
        }

        static uint8_t CalculatePixelAlpha(const uint8_t blackBackgroundValue, const uint8_t whiteBackgroundValue)
        {
            int colorDelta = static_cast<int>(whiteBackgroundValue) - static_cast<int>(blackBackgroundValue);
            if (colorDelta < 0)
            {
                colorDelta = 0;
            }
            if (colorDelta > 255)
            {
                colorDelta = 255;
            }

            const int alphaValue = 255 - colorDelta;
            return static_cast<uint8_t>(alphaValue < 0 ? 0 : alphaValue);
        }

        static uint8_t RecoverForegroundChannelValue(const uint8_t blackBackgroundValue, const uint8_t alphaValue)
        {
            if (alphaValue == 0)
            {
                return 0;
            }

            if (alphaValue == 255)
            {
                return blackBackgroundValue;
            }

            const double recoveredValue = static_cast<double>(blackBackgroundValue) * 255.0 / static_cast<double>(alphaValue);
            return ClampDoubleToByte(recoveredValue);
        }

        static bool IsPixelRepresentableAsStraightAlpha(const uint8_t blackBackgroundValue, const uint8_t whiteBackgroundValue)
        {
            return whiteBackgroundValue >= blackBackgroundValue;
        }

        static bool TryBuildCursorImageFromRenderedBuffers(const int cursorWidth, const int cursorHeight, const std::vector<uint8_t>& blackBackgroundBuffer, const std::vector<uint8_t>& whiteBackgroundBuffer, GB_Image& cursorImage)
        {
            if (cursorWidth <= 0 || cursorHeight <= 0)
            {
                return false;
            }

            size_t pixelBufferByteCount = 0;
            if (!TryCalculatePixelBufferByteCount(cursorWidth, cursorHeight, pixelBufferByteCount) || blackBackgroundBuffer.size() != pixelBufferByteCount || whiteBackgroundBuffer.size() != pixelBufferByteCount)
            {
                return false;
            }

            GB_Image newCursorImage;
            if (!newCursorImage.Create(static_cast<size_t>(cursorHeight), static_cast<size_t>(cursorWidth), GB_ImageDepth::UInt8, 4, false))
            {
                return false;
            }

            bool hasVisiblePixel = false;
            for (int rowIndex = 0; rowIndex < cursorHeight; rowIndex++)
            {
                unsigned char* rowData = newCursorImage.GetRowData(static_cast<size_t>(rowIndex));
                if (rowData == nullptr)
                {
                    return false;
                }

                for (int colIndex = 0; colIndex < cursorWidth; colIndex++)
                {
                    const size_t pixelOffset = (static_cast<size_t>(rowIndex) * static_cast<size_t>(cursorWidth) + static_cast<size_t>(colIndex)) * 4;

                    const uint8_t blackBlue = blackBackgroundBuffer[pixelOffset + 0];
                    const uint8_t blackGreen = blackBackgroundBuffer[pixelOffset + 1];
                    const uint8_t blackRed = blackBackgroundBuffer[pixelOffset + 2];

                    const uint8_t whiteBlue = whiteBackgroundBuffer[pixelOffset + 0];
                    const uint8_t whiteGreen = whiteBackgroundBuffer[pixelOffset + 1];
                    const uint8_t whiteRed = whiteBackgroundBuffer[pixelOffset + 2];

                    if (!IsPixelRepresentableAsStraightAlpha(blackBlue, whiteBlue) || !IsPixelRepresentableAsStraightAlpha(blackGreen, whiteGreen) || !IsPixelRepresentableAsStraightAlpha(blackRed, whiteRed))
                    {
                        return false;
                    }

                    const uint8_t alphaBlue = CalculatePixelAlpha(blackBlue, whiteBlue);
                    const uint8_t alphaGreen = CalculatePixelAlpha(blackGreen, whiteGreen);
                    const uint8_t alphaRed = CalculatePixelAlpha(blackRed, whiteRed);
                    const uint8_t alphaValue = static_cast<uint8_t>((static_cast<unsigned int>(alphaBlue) + static_cast<unsigned int>(alphaGreen) + static_cast<unsigned int>(alphaRed)) / 3u);

                    const uint8_t blueValue = RecoverForegroundChannelValue(blackBlue, alphaValue);
                    const uint8_t greenValue = RecoverForegroundChannelValue(blackGreen, alphaValue);
                    const uint8_t redValue = RecoverForegroundChannelValue(blackRed, alphaValue);

                    rowData[colIndex * 4 + 0] = blueValue;
                    rowData[colIndex * 4 + 1] = greenValue;
                    rowData[colIndex * 4 + 2] = redValue;
                    rowData[colIndex * 4 + 3] = alphaValue;

                    if (alphaValue != 0 || blueValue != 0 || greenValue != 0 || redValue != 0)
                    {
                        hasVisiblePixel = true;
                    }
                }
            }

            if (!hasVisiblePixel)
            {
                return false;
            }

            cursorImage = std::move(newCursorImage);
            return true;
        }

        static bool TryGetContainingScreenIndex(const std::vector<GB_ScreenInfo>& screenInfos, const GB_Point2d& physicalPixelPoint, int& screenIndex)
        {
            screenIndex = -1;

            int firstMatchedScreenIndex = -1;
            for (size_t i = 0; i < screenInfos.size(); i++)
            {
                if (!IsPointInHalfOpenRectangle(physicalPixelPoint, screenInfos[i].virtualScreenRectangle))
                {
                    continue;
                }

                if (screenInfos[i].isPrimary)
                {
                    screenIndex = static_cast<int>(i);
                    return true;
                }

                if (firstMatchedScreenIndex < 0)
                {
                    firstMatchedScreenIndex = static_cast<int>(i);
                }
            }

            if (firstMatchedScreenIndex >= 0)
            {
                screenIndex = firstMatchedScreenIndex;
                return true;
            }

            return false;
        }

        static bool TryGetMousePhysicalPixelPointOnScreen(int& screenIndex, GB_Point2d& physicalPixelPointOnScreen)
        {
            screenIndex = -1;
            physicalPixelPointOnScreen = GB_Point2d();

            GB_Point2d physicalPixelPoint;
            if (!GB_Mouse::GetMousePhysicalPosition(physicalPixelPoint))
            {
                return false;
            }

            const std::vector<GB_ScreenInfo> screenInfos = GB_Screen::GetAllScreens();
            if (!TryGetContainingScreenIndex(screenInfos, physicalPixelPoint, screenIndex))
            {
                screenIndex = -1;
                return false;
            }

            const GB_ScreenInfo& screenInfo = screenInfos[static_cast<size_t>(screenIndex)];
            if (!screenInfo.virtualScreenRectangle.IsValid())
            {
                screenIndex = -1;
                return false;
            }

            physicalPixelPointOnScreen.Set(physicalPixelPoint.x - screenInfo.virtualScreenRectangle.minX, physicalPixelPoint.y - screenInfo.virtualScreenRectangle.minY);
            return true;
        }

        static bool TryCaptureDisplayedCursorFallbackImage(const int fallbackCaptureRadius, GB_Image& cursorImage, GB_Point2d& hotSpot)
        {
            cursorImage.Clear();
            hotSpot = GB_Point2d();

            const int captureRadius = std::max(fallbackCaptureRadius, 1);

            GB_Point2d physicalPixelPoint;
            if (!GB_Mouse::GetMousePhysicalPosition(physicalPixelPoint) || !IsFinitePoint(physicalPixelPoint))
            {
                return false;
            }

            const double centerX = static_cast<double>(std::llround(physicalPixelPoint.x));
            const double centerY = static_cast<double>(std::llround(physicalPixelPoint.y));

            const GB_Rectangle virtualScreenRectangle = GB_Screen::GetVirtualScreenRectangle();
            if (!virtualScreenRectangle.IsValid())
            {
                return false;
            }

            const GB_Rectangle desiredRectangle(centerX - static_cast<double>(captureRadius), centerY - static_cast<double>(captureRadius), centerX + static_cast<double>(captureRadius) + 1.0, centerY + static_cast<double>(captureRadius) + 1.0);
            const GB_Rectangle clippedRectangle = desiredRectangle.Intersected(virtualScreenRectangle);
            if (!clippedRectangle.IsValid() || clippedRectangle.Width() <= 0.0 || clippedRectangle.Height() <= 0.0)
            {
                return false;
            }

            GB_Image capturedImage;
            if (!GB_Screen::CaptureVirtualScreen(clippedRectangle, capturedImage) || capturedImage.IsEmpty())
            {
                return false;
            }

            hotSpot.Set(centerX - clippedRectangle.minX, centerY - clippedRectangle.minY);
            cursorImage = std::move(capturedImage);
            return true;
        }
    }
#endif
}

namespace
{
    static bool TryGetPureCursorImage(GB_Image& cursorImage, GB_Point2d& hotSpot)
    {
        hotSpot = GB_Point2d();

#if !defined(_WIN32)
        cursorImage.Clear();
        return false;
#else
        internal::DpiAwarenessScope dpiAwarenessScope;

        HCURSOR cursorHandle = nullptr;
        if (!internal::TryGetCurrentVisibleCursorHandle(cursorHandle))
        {
            cursorImage.Clear();
            return false;
        }

        int cursorWidth = 0;
        int cursorHeight = 0;
        ICONINFO iconInfo = {};
        if (!internal::TryGetCursorBitmapSize(cursorHandle, cursorWidth, cursorHeight, iconInfo))
        {
            cursorImage.Clear();
            return false;
        }

        internal::IconBitmapScope maskBitmapScope(iconInfo.hbmMask);
        internal::IconBitmapScope colorBitmapScope(iconInfo.hbmColor);

        std::vector<uint8_t> blackBackgroundBuffer;
        std::vector<uint8_t> whiteBackgroundBuffer;
        if (!internal::TryRenderCursorToBgraBuffer(cursorHandle, cursorWidth, cursorHeight, GB_ColorRGBA::Black, blackBackgroundBuffer) ||
            !internal::TryRenderCursorToBgraBuffer(cursorHandle, cursorWidth, cursorHeight, GB_ColorRGBA::White, whiteBackgroundBuffer) ||
            !internal::TryBuildCursorImageFromRenderedBuffers(cursorWidth, cursorHeight, blackBackgroundBuffer, whiteBackgroundBuffer, cursorImage))
        {
            cursorImage.Clear();
            return false;
        }

        hotSpot.Set(static_cast<double>(iconInfo.xHotspot), static_cast<double>(iconInfo.yHotspot));
        return true;
#endif
    }
}

bool GB_Mouse::GetCursorImage(GB_Image& cursorImage, const int fallbackCaptureRadius)
{
    GB_Point2d hotSpot;
    return GetCursorImage(cursorImage, hotSpot, fallbackCaptureRadius);
}

bool GB_Mouse::GetCursorImage(GB_Image& cursorImage, GB_Point2d& hotSpot, const int fallbackCaptureRadius)
{
    hotSpot = GB_Point2d();

    if (TryGetPureCursorImage(cursorImage, hotSpot))
    {
        return true;
    }

#if !defined(_WIN32)
    cursorImage.Clear();
    return false;
#else
    return internal::TryCaptureDisplayedCursorFallbackImage(fallbackCaptureRadius, cursorImage, hotSpot);
#endif
}

bool GB_Mouse::GetMousePosition(GB_Point2d& logicalPixelPoint)
{
    logicalPixelPoint = GB_Point2d();

#if !defined(_WIN32)
    return false;
#else
    int screenIndex = -1;
    GB_Point2d physicalPixelPointOnScreen;
    if (!internal::TryGetMousePhysicalPixelPointOnScreen(screenIndex, physicalPixelPointOnScreen))
    {
        return false;
    }

    return GB_Screen::PhysicalPixelToLogicalPixel(screenIndex, physicalPixelPointOnScreen, logicalPixelPoint);
#endif
}

bool GB_Mouse::GetMousePhysicalPosition(GB_Point2d& physicalPixelPoint)
{
    physicalPixelPoint = GB_Point2d();

#if !defined(_WIN32)
    return false;
#else
    internal::DpiAwarenessScope dpiAwarenessScope;

    POINT cursorPoint = { 0, 0 };
    if (!internal::TryGetCurrentPhysicalCursorPosition(cursorPoint))
    {
        return false;
    }

    physicalPixelPoint.Set(static_cast<double>(cursorPoint.x), static_cast<double>(cursorPoint.y));
    return true;
#endif
}

bool GB_Mouse::GetMousePhysicalPosition(int& screenIndex, GB_Point2d& physicalPixelPointOnScreen)
{
    screenIndex = -1;
    physicalPixelPointOnScreen = GB_Point2d();

#if !defined(_WIN32)
    return false;
#else
    GB_Point2d logicalPixelPoint;
    if (GetMousePosition(logicalPixelPoint) && GB_Screen::LogicalPixelToPhysicalPixel(logicalPixelPoint, screenIndex, physicalPixelPointOnScreen))
    {
        return true;
    }

    return internal::TryGetMousePhysicalPixelPointOnScreen(screenIndex, physicalPixelPointOnScreen);
#endif
}
