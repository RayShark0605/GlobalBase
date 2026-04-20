#include "GB_Mouse.h"

#include "GB_Screen.h"
#include "../Geometry/GB_Rectangle.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <thread>
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


        static int64_t ClampInt64(const int64_t value, const int64_t minValue, const int64_t maxValue)
        {
            if (value < minValue)
            {
                return minValue;
            }

            if (value > maxValue)
            {
                return maxValue;
            }

            return value;
        }

        static bool TryRoundDoubleToInt64(const double value, int64_t& roundedValue)
        {
            roundedValue = 0;

            if (!std::isfinite(value))
            {
                return false;
            }

            const double minValue = static_cast<double>(std::numeric_limits<int64_t>::min());
            const double maxValue = static_cast<double>(std::numeric_limits<int64_t>::max());
            if (value < minValue || value > maxValue)
            {
                return false;
            }

            roundedValue = static_cast<int64_t>(std::llround(value));
            return true;
        }

        static bool TryConvertInt64ToLong(const int64_t value, LONG& longValue)
        {
            longValue = 0;

            if (value < static_cast<int64_t>(std::numeric_limits<LONG>::min()) || value > static_cast<int64_t>(std::numeric_limits<LONG>::max()))
            {
                return false;
            }

            longValue = static_cast<LONG>(value);
            return true;
        }

        static bool TryGetRectanglePixelBounds(const GB_Rectangle& rectangle, int64_t& minX, int64_t& minY, int64_t& maxX, int64_t& maxY)
        {
            minX = 0;
            minY = 0;
            maxX = -1;
            maxY = -1;

            if (!rectangle.IsValid() || rectangle.Width() <= 0.0 || rectangle.Height() <= 0.0)
            {
                return false;
            }

            const double rectangleMinX = std::floor(rectangle.minX);
            const double rectangleMinY = std::floor(rectangle.minY);
            const double rectangleMaxXExclusive = std::ceil(rectangle.maxX);
            const double rectangleMaxYExclusive = std::ceil(rectangle.maxY);

            if (!(rectangleMaxXExclusive > rectangleMinX) || !(rectangleMaxYExclusive > rectangleMinY))
            {
                return false;
            }

            if (!TryRoundDoubleToInt64(rectangleMinX, minX) || !TryRoundDoubleToInt64(rectangleMinY, minY) ||
                !TryRoundDoubleToInt64(rectangleMaxXExclusive - 1.0, maxX) || !TryRoundDoubleToInt64(rectangleMaxYExclusive - 1.0, maxY))
            {
                return false;
            }

            return maxX >= minX && maxY >= minY;
        }
        static bool IsFinitePoint(const GB_Point2d& point)
        {
            return std::isfinite(point.x) && std::isfinite(point.y);
        }

        static bool TryClampPointToRectangle(const GB_Point2d& point, const GB_Rectangle& rectangle, POINT& clampedPoint)
        {
            clampedPoint.x = 0;
            clampedPoint.y = 0;

            int64_t minX = 0;
            int64_t minY = 0;
            int64_t maxX = 0;
            int64_t maxY = 0;
            if (!IsFinitePoint(point) || !TryGetRectanglePixelBounds(rectangle, minX, minY, maxX, maxY))
            {
                return false;
            }

            int64_t targetX = 0;
            int64_t targetY = 0;
            if (!TryRoundDoubleToInt64(point.x, targetX) || !TryRoundDoubleToInt64(point.y, targetY))
            {
                return false;
            }

            const int64_t clampedX = ClampInt64(targetX, minX, maxX);
            const int64_t clampedY = ClampInt64(targetY, minY, maxY);
            return TryConvertInt64ToLong(clampedX, clampedPoint.x) && TryConvertInt64ToLong(clampedY, clampedPoint.y);
        }

        static bool TrySetCurrentPhysicalCursorPosition(const POINT& cursorPoint)
        {
            const HMODULE user32Module = ::GetModuleHandleW(L"user32.dll");
            if (user32Module != nullptr)
            {
                using SetPhysicalCursorPosFunction = BOOL(WINAPI*)(int x, int y);
                const auto setPhysicalCursorPos = reinterpret_cast<SetPhysicalCursorPosFunction>(::GetProcAddress(user32Module, "SetPhysicalCursorPos"));
                if (setPhysicalCursorPos != nullptr && setPhysicalCursorPos(static_cast<int>(cursorPoint.x), static_cast<int>(cursorPoint.y)) != FALSE)
                {
                    return true;
                }
            }

            return ::SetCursorPos(static_cast<int>(cursorPoint.x), static_cast<int>(cursorPoint.y)) != FALSE;
        }

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

        static bool TryGetCurrentVisibleCursorInfo(CURSORINFO& cursorInfo)
        {
            if (!TryGetCurrentCursorInfo(cursorInfo))
            {
                return false;
            }

            if ((cursorInfo.flags & CURSOR_SHOWING) == 0 || cursorInfo.hCursor == nullptr)
            {
                std::memset(&cursorInfo, 0, sizeof(cursorInfo));
                cursorInfo.cbSize = sizeof(cursorInfo);
                return false;
            }

            return true;
        }

        static bool TryGetCurrentVisibleCursorHandle(HCURSOR& cursorHandle)
        {
            cursorHandle = nullptr;

            CURSORINFO cursorInfo = {};
            if (!TryGetCurrentVisibleCursorInfo(cursorInfo))
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

                    if (alphaValue != 0)
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

        static size_t GetMonochromeBitmapRowStrideBytes(const int bitmapWidth)
        {
            if (bitmapWidth <= 0)
            {
                return 0;
            }

            const size_t widthValue = static_cast<size_t>(bitmapWidth);
            return ((widthValue + 31u) / 32u) * 4u;
        }

        static bool TryReadMonochromeBitmapBits(const HBITMAP bitmapHandle, const int bitmapWidth, const int bitmapHeight, std::vector<uint8_t>& bitmapBits)
        {
            bitmapBits.clear();

            if (bitmapHandle == nullptr || bitmapWidth <= 0 || bitmapHeight <= 0)
            {
                return false;
            }

            const size_t rowStrideBytes = GetMonochromeBitmapRowStrideBytes(bitmapWidth);
            if (rowStrideBytes == 0 || rowStrideBytes > (std::numeric_limits<size_t>::max)() / static_cast<size_t>(bitmapHeight))
            {
                return false;
            }

            const size_t bufferByteCount = rowStrideBytes * static_cast<size_t>(bitmapHeight);

            try
            {
                bitmapBits.resize(bufferByteCount);
            }
            catch (...)
            {
                bitmapBits.clear();
                return false;
            }

            ScreenDcScope screenDc;
            if (!screenDc.IsValid())
            {
                bitmapBits.clear();
                return false;
            }

            struct MonochromeBitmapInfo
            {
                BITMAPINFOHEADER bitmapHeader;
                RGBQUAD colorTable[2];
            };

            MonochromeBitmapInfo bitmapInfo = {};
            bitmapInfo.bitmapHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bitmapHeader.biWidth = bitmapWidth;
            bitmapInfo.bitmapHeader.biHeight = -bitmapHeight;
            bitmapInfo.bitmapHeader.biPlanes = 1;
            bitmapInfo.bitmapHeader.biBitCount = 1;
            bitmapInfo.bitmapHeader.biCompression = BI_RGB;
            bitmapInfo.bitmapHeader.biSizeImage = static_cast<DWORD>(bufferByteCount);
            bitmapInfo.bitmapHeader.biClrUsed = 2;
            bitmapInfo.bitmapHeader.biClrImportant = 2;
            bitmapInfo.colorTable[0].rgbBlue = 0;
            bitmapInfo.colorTable[0].rgbGreen = 0;
            bitmapInfo.colorTable[0].rgbRed = 0;
            bitmapInfo.colorTable[0].rgbReserved = 0;
            bitmapInfo.colorTable[1].rgbBlue = 255;
            bitmapInfo.colorTable[1].rgbGreen = 255;
            bitmapInfo.colorTable[1].rgbRed = 255;
            bitmapInfo.colorTable[1].rgbReserved = 0;

            if (::GetDIBits(screenDc.Get(), bitmapHandle, 0, static_cast<UINT>(bitmapHeight), bitmapBits.data(), reinterpret_cast<BITMAPINFO*>(&bitmapInfo), DIB_RGB_COLORS) == 0)
            {
                bitmapBits.clear();
                return false;
            }

            return true;
        }

        static bool TryGetMonochromeBitmapPixel(const std::vector<uint8_t>& bitmapBits, const size_t rowStrideBytes, const int bitmapWidth, const int bitmapHeight, const int x, const int y, bool& pixelValue)
        {
            pixelValue = false;

            if (bitmapWidth <= 0 || bitmapHeight <= 0 || x < 0 || x >= bitmapWidth || y < 0 || y >= bitmapHeight)
            {
                return false;
            }

            if (rowStrideBytes == 0 || bitmapBits.size() < rowStrideBytes * static_cast<size_t>(bitmapHeight))
            {
                return false;
            }

            const size_t byteOffset = static_cast<size_t>(y) * rowStrideBytes + static_cast<size_t>(x / 8);
            if (byteOffset >= bitmapBits.size())
            {
                return false;
            }

            const uint8_t bitMask = static_cast<uint8_t>(0x80u >> (x & 7));
            pixelValue = (bitmapBits[byteOffset] & bitMask) != 0;
            return true;
        }

        static bool TryTrimTransparentBorder(GB_Image& image, GB_Point2d* hotSpot)
        {
            if (image.IsEmpty() || image.GetDepth() != GB_ImageDepth::UInt8 || image.GetChannels() != 4)
            {
                return false;
            }

            const size_t imageWidth = image.GetWidth();
            const size_t imageHeight = image.GetHeight();
            if (imageWidth == 0 || imageHeight == 0)
            {
                return false;
            }

            size_t minVisibleX = imageWidth;
            size_t minVisibleY = imageHeight;
            size_t maxVisibleX = 0;
            size_t maxVisibleY = 0;
            bool hasVisiblePixel = false;

            for (size_t rowIndex = 0; rowIndex < imageHeight; rowIndex++)
            {
                const unsigned char* rowData = image.GetRowData(rowIndex);
                if (rowData == nullptr)
                {
                    return false;
                }

                for (size_t colIndex = 0; colIndex < imageWidth; colIndex++)
                {
                    if (rowData[colIndex * 4 + 3] == 0)
                    {
                        continue;
                    }

                    if (!hasVisiblePixel)
                    {
                        minVisibleX = colIndex;
                        minVisibleY = rowIndex;
                        maxVisibleX = colIndex;
                        maxVisibleY = rowIndex;
                        hasVisiblePixel = true;
                    }
                    else
                    {
                        minVisibleX = (std::min)(minVisibleX, colIndex);
                        minVisibleY = (std::min)(minVisibleY, rowIndex);
                        maxVisibleX = (std::max)(maxVisibleX, colIndex);
                        maxVisibleY = (std::max)(maxVisibleY, rowIndex);
                    }
                }
            }

            if (!hasVisiblePixel)
            {
                return false;
            }

            if (minVisibleX == 0 && minVisibleY == 0 && maxVisibleX + 1 == imageWidth && maxVisibleY + 1 == imageHeight)
            {
                return true;
            }

            const size_t trimmedWidth = maxVisibleX - minVisibleX + 1;
            const size_t trimmedHeight = maxVisibleY - minVisibleY + 1;

            GB_Image trimmedImage;
            if (!trimmedImage.Create(trimmedHeight, trimmedWidth, GB_ImageDepth::UInt8, 4, false))
            {
                return false;
            }

            const size_t trimmedRowByteCount = trimmedWidth * 4u;
            for (size_t rowIndex = 0; rowIndex < trimmedHeight; rowIndex++)
            {
                const unsigned char* sourceRowData = image.GetRowData(minVisibleY + rowIndex);
                unsigned char* targetRowData = trimmedImage.GetRowData(rowIndex);
                if (sourceRowData == nullptr || targetRowData == nullptr)
                {
                    return false;
                }

                std::memcpy(targetRowData, sourceRowData + minVisibleX * 4u, trimmedRowByteCount);
            }

            image = std::move(trimmedImage);
            if (hotSpot != nullptr)
            {
                hotSpot->x -= static_cast<double>(minVisibleX);
                hotSpot->y -= static_cast<double>(minVisibleY);
            }

            return true;
        }

        static bool TryBuildMonochromeCursorImageFromMaskBitmap(const ICONINFO& iconInfo, const int cursorWidth, const int cursorHeight, GB_Image& cursorImage)
        {
            cursorImage.Clear();

            if (iconInfo.hbmMask == nullptr || iconInfo.hbmColor != nullptr || cursorWidth <= 0 || cursorHeight <= 0)
            {
                return false;
            }

            std::vector<uint8_t> maskBitmapBits;
            if (!TryReadMonochromeBitmapBits(iconInfo.hbmMask, cursorWidth, cursorHeight * 2, maskBitmapBits))
            {
                return false;
            }

            const size_t rowStrideBytes = GetMonochromeBitmapRowStrideBytes(cursorWidth);
            if (rowStrideBytes == 0)
            {
                return false;
            }

            GB_Image newCursorImage;
            if (!newCursorImage.Create(static_cast<size_t>(cursorHeight), static_cast<size_t>(cursorWidth), GB_ImageDepth::UInt8, 4, false))
            {
                return false;
            }

            bool hasOpaquePixel = false;
            for (int rowIndex = 0; rowIndex < cursorHeight; rowIndex++)
            {
                unsigned char* rowData = newCursorImage.GetRowData(static_cast<size_t>(rowIndex));
                if (rowData == nullptr)
                {
                    return false;
                }

                for (int colIndex = 0; colIndex < cursorWidth; colIndex++)
                {
                    bool andMaskBit = false;
                    bool xorMaskBit = false;
                    if (!TryGetMonochromeBitmapPixel(maskBitmapBits, rowStrideBytes, cursorWidth, cursorHeight * 2, colIndex, rowIndex, andMaskBit) ||
                        !TryGetMonochromeBitmapPixel(maskBitmapBits, rowStrideBytes, cursorWidth, cursorHeight * 2, colIndex, rowIndex + cursorHeight, xorMaskBit))
                    {
                        return false;
                    }

                    unsigned char* pixelData = rowData + static_cast<size_t>(colIndex) * 4u;
                    uint8_t blueValue = 0;
                    uint8_t greenValue = 0;
                    uint8_t redValue = 0;
                    uint8_t alphaValue = 255;

                    if (andMaskBit)
                    {
                        if (xorMaskBit)
                        {
                            // AND=1 XOR=1 表示 Reverse screen。
                            // 该像素依赖背景，无法还原为唯一的背景无关 RGBA 值。
                            // 这里将其规范化为不透明白色，以得到一张“纯光标图”。
                            blueValue = 255;
                            greenValue = 255;
                            redValue = 255;
                            alphaValue = 255;
                        }
                        else
                        {
                            // AND=1 XOR=0 表示 Screen，不改变背景，这里将其规范化为透明。
                            blueValue = 0;
                            greenValue = 0;
                            redValue = 0;
                            alphaValue = 0;
                        }
                    }
                    else
                    {
                        // AND=0 时为可直接规范化的实体像素：XOR=0 为黑，XOR=1 为白。
                        const uint8_t grayValue = xorMaskBit ? 255u : 0u;
                        blueValue = grayValue;
                        greenValue = grayValue;
                        redValue = grayValue;
                        alphaValue = 255;
                    }

                    pixelData[0] = blueValue;
                    pixelData[1] = greenValue;
                    pixelData[2] = redValue;
                    pixelData[3] = alphaValue;
                    if (alphaValue != 0)
                    {
                        hasOpaquePixel = true;
                    }
                }
            }

            if (!hasOpaquePixel)
            {
                return false;
            }

            cursorImage = std::move(newCursorImage);
            return true;
        }

        static bool TryForceImageOpaqueAlpha(GB_Image& image)
        {
            if (image.IsEmpty() || image.GetDepth() != GB_ImageDepth::UInt8 || image.GetChannels() != 4)
            {
                return false;
            }

            const size_t imageWidth = image.GetWidth();
            const size_t imageHeight = image.GetHeight();

            for (size_t rowIndex = 0; rowIndex < imageHeight; rowIndex++)
            {
                unsigned char* rowData = image.GetRowData(rowIndex);
                if (rowData == nullptr)
                {
                    return false;
                }

                for (size_t colIndex = 0; colIndex < imageWidth; colIndex++)
                {
                    rowData[colIndex * 4 + 3] = 255;
                }
            }

            return true;
        }

        static bool TryOverlayCursorOnBgraImage(const HCURSOR cursorHandle, const int cursorLeftOnImage, const int cursorTopOnImage, const int cursorWidth, const int cursorHeight, GB_Image& backgroundImage)
        {
            if (cursorHandle == nullptr || cursorWidth <= 0 || cursorHeight <= 0 || backgroundImage.IsEmpty() || backgroundImage.GetDepth() != GB_ImageDepth::UInt8 || backgroundImage.GetChannels() != 4)
            {
                return false;
            }

            if (backgroundImage.GetWidth() > static_cast<size_t>(std::numeric_limits<int>::max()) || backgroundImage.GetHeight() > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                return false;
            }

            const int imageWidth = static_cast<int>(backgroundImage.GetWidth());
            const int imageHeight = static_cast<int>(backgroundImage.GetHeight());
            const size_t sourceRowStrideBytes = backgroundImage.GetRowStrideBytes();
            const size_t rowByteCount = static_cast<size_t>(imageWidth) * 4u;
            if (sourceRowStrideBytes < rowByteCount)
            {
                return false;
            }

            size_t pixelBufferByteCount = 0;
            if (!TryCalculatePixelBufferByteCount(imageWidth, imageHeight, pixelBufferByteCount))
            {
                return false;
            }

            std::vector<uint8_t> bgraBuffer;
            try
            {
                bgraBuffer.resize(pixelBufferByteCount);
            }
            catch (...)
            {
                return false;
            }

            for (int rowIndex = 0; rowIndex < imageHeight; rowIndex++)
            {
                const unsigned char* rowData = backgroundImage.GetRowData(static_cast<size_t>(rowIndex));
                if (rowData == nullptr)
                {
                    return false;
                }

                std::memcpy(bgraBuffer.data() + static_cast<size_t>(rowIndex) * rowByteCount, rowData, rowByteCount);
            }

            for (size_t pixelOffset = 3; pixelOffset < bgraBuffer.size(); pixelOffset += 4)
            {
                bgraBuffer[pixelOffset] = 255;
            }

            ScreenDcScope screenDc;
            if (!screenDc.IsValid())
            {
                return false;
            }

            CompatibleDcScope memoryDc(screenDc.Get());
            if (!memoryDc.IsValid())
            {
                return false;
            }

            BITMAPINFO bitmapInfo = {};
            bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
            bitmapInfo.bmiHeader.biWidth = imageWidth;
            bitmapInfo.bmiHeader.biHeight = -imageHeight;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            void* bitmapBits = nullptr;
            BitmapScope bitmap(::CreateDIBSection(screenDc.Get(), &bitmapInfo, DIB_RGB_COLORS, &bitmapBits, nullptr, 0));
            if (!bitmap.IsValid() || bitmapBits == nullptr)
            {
                return false;
            }

            std::memcpy(bitmapBits, bgraBuffer.data(), pixelBufferByteCount);

            SelectObjectScope selectBitmap(memoryDc.Get(), bitmap.Get());
            if (!selectBitmap.IsValid())
            {
                return false;
            }

            if (::DrawIconEx(memoryDc.Get(), cursorLeftOnImage, cursorTopOnImage, cursorHandle, cursorWidth, cursorHeight, 0, nullptr, DI_NORMAL) == FALSE)
            {
                return false;
            }

            ::GdiFlush();
            std::memcpy(bgraBuffer.data(), bitmapBits, pixelBufferByteCount);

            for (size_t pixelOffset = 3; pixelOffset < bgraBuffer.size(); pixelOffset += 4)
            {
                bgraBuffer[pixelOffset] = 255;
            }

            for (int rowIndex = 0; rowIndex < imageHeight; rowIndex++)
            {
                unsigned char* rowData = backgroundImage.GetRowData(static_cast<size_t>(rowIndex));
                if (rowData == nullptr)
                {
                    return false;
                }

                std::memcpy(rowData, bgraBuffer.data() + static_cast<size_t>(rowIndex) * rowByteCount, rowByteCount);
            }

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

        static bool TryGetPhysicalPixelPointOnScreenFromCursorPoint(const std::vector<GB_ScreenInfo>& screenInfos, const POINT& physicalCursorPoint, int& screenIndex, GB_Point2d& physicalPixelPointOnScreen)
        {
            screenIndex = -1;
            physicalPixelPointOnScreen = GB_Point2d();

            const GB_Point2d physicalPixelPoint(static_cast<double>(physicalCursorPoint.x), static_cast<double>(physicalCursorPoint.y));
            if (!TryGetContainingScreenIndex(screenInfos, physicalPixelPoint, screenIndex))
            {
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

        static bool TryGetScreenRectangle(const std::vector<GB_ScreenInfo>& screenInfos, const int screenIndex, GB_Rectangle& screenRectangle)
        {
            screenRectangle = GB_Rectangle();

            if (screenIndex < 0 || static_cast<size_t>(screenIndex) >= screenInfos.size())
            {
                return false;
            }

            const GB_Rectangle& rectangle = screenInfos[static_cast<size_t>(screenIndex)].virtualScreenRectangle;
            if (!rectangle.IsValid() || rectangle.Width() <= 0.0 || rectangle.Height() <= 0.0)
            {
                return false;
            }

            screenRectangle = rectangle;
            return true;
        }

        static bool TryResolveScreenLocalTargetPoint(const std::vector<GB_ScreenInfo>& screenInfos, const int screenIndex, const GB_Point2d& physicalPixelPointOnScreen, POINT& targetPoint)
        {
            targetPoint.x = 0;
            targetPoint.y = 0;

            GB_Rectangle screenRectangle;
            if (!TryGetScreenRectangle(screenInfos, screenIndex, screenRectangle))
            {
                return false;
            }

            const GB_Point2d targetPhysicalPixelPoint(screenRectangle.minX + physicalPixelPointOnScreen.x, screenRectangle.minY + physicalPixelPointOnScreen.y);
            return TryClampPointToRectangle(targetPhysicalPixelPoint, screenRectangle, targetPoint);
        }

        static bool TryProjectPointToNearestScreenPixel(const std::vector<GB_ScreenInfo>& screenInfos, const GB_Point2d& physicalPixelPoint, POINT& projectedPoint, int& projectedScreenIndex)
        {
            projectedPoint.x = 0;
            projectedPoint.y = 0;
            projectedScreenIndex = -1;

            if (!IsFinitePoint(physicalPixelPoint) || screenInfos.empty())
            {
                return false;
            }

            int containingScreenIndex = -1;
            if (TryGetContainingScreenIndex(screenInfos, physicalPixelPoint, containingScreenIndex))
            {
                GB_Rectangle containingRectangle;
                if (!TryGetScreenRectangle(screenInfos, containingScreenIndex, containingRectangle) || !TryClampPointToRectangle(physicalPixelPoint, containingRectangle, projectedPoint))
                {
                    return false;
                }

                projectedScreenIndex = containingScreenIndex;
                return true;
            }

            bool hasBestPoint = false;
            double bestDistanceSquared = 0;
            POINT bestPoint = {};
            int bestScreenIndex = -1;

            for (size_t i = 0; i < screenInfos.size(); i++)
            {
                const GB_Rectangle& screenRectangle = screenInfos[i].virtualScreenRectangle;
                if (!screenRectangle.IsValid() || screenRectangle.Width() <= 0.0 || screenRectangle.Height() <= 0.0)
                {
                    continue;
                }

                const double clampedX = std::max(screenRectangle.minX, std::min(physicalPixelPoint.x, screenRectangle.maxX - 1.0));
                const double clampedY = std::max(screenRectangle.minY, std::min(physicalPixelPoint.y, screenRectangle.maxY - 1.0));
                const double deltaX = physicalPixelPoint.x - clampedX;
                const double deltaY = physicalPixelPoint.y - clampedY;
                const double distanceSquared = deltaX * deltaX + deltaY * deltaY;

                POINT clampedPoint = {};
                if (!TryClampPointToRectangle(GB_Point2d(clampedX, clampedY), screenRectangle, clampedPoint))
                {
                    continue;
                }

                if (!hasBestPoint || distanceSquared < bestDistanceSquared || (distanceSquared == bestDistanceSquared && screenInfos[i].isPrimary))
                {
                    hasBestPoint = true;
                    bestDistanceSquared = distanceSquared;
                    bestPoint = clampedPoint;
                    bestScreenIndex = static_cast<int>(i);
                }
            }

            if (!hasBestPoint)
            {
                return false;
            }

            projectedPoint = bestPoint;
            projectedScreenIndex = bestScreenIndex;
            return true;
        }

        static double GetPositiveOptionOrDefault(const double value, const double defaultValue)
        {
            return (std::isfinite(value) && value > 0.0) ? value : defaultValue;
        }

        static double GetDefaultMoveSpeedPixelPerSecond(const GB_MouseMoveMode moveMode)
        {
            switch (moveMode)
            {
            case GB_MouseMoveMode::Linear:
                return 4200.0;

            case GB_MouseMoveMode::HumanLike:
                return 2600.0;

            case GB_MouseMoveMode::Teleport:
            default:
                break;
            }

            return std::numeric_limits<double>::infinity();
        }

        static double ResolveMoveDurationMs(const double distancePixel, const GB_MouseMoveOptions& moveOptions)
        {
            if (!(distancePixel > 0.0))
            {
                return 0.0;
            }

            const double defaultSpeedPixelPerSecond = GetDefaultMoveSpeedPixelPerSecond(moveOptions.moveMode);
            double baseDurationMs = 0.0;
            if (std::isfinite(defaultSpeedPixelPerSecond) && defaultSpeedPixelPerSecond > 0.0)
            {
                baseDurationMs = distancePixel * 1000.0 / defaultSpeedPixelPerSecond;
            }

            double lowerBoundMs = 0.0;
            double upperBoundMs = std::numeric_limits<double>::infinity();

            if (moveOptions.specifyMinDurationMs && std::isfinite(moveOptions.minDurationMs) && moveOptions.minDurationMs > 0.0)
            {
                lowerBoundMs = std::max(lowerBoundMs, moveOptions.minDurationMs);
            }

            if (moveOptions.specifyMaxDurationMs && std::isfinite(moveOptions.maxDurationMs) && moveOptions.maxDurationMs > 0.0)
            {
                upperBoundMs = std::min(upperBoundMs, moveOptions.maxDurationMs);
            }

            if (moveOptions.specifyMaxSpeedPixelPerSecond && std::isfinite(moveOptions.maxSpeedPixelPerSecond) && moveOptions.maxSpeedPixelPerSecond > 0.0)
            {
                lowerBoundMs = std::max(lowerBoundMs, distancePixel * 1000.0 / moveOptions.maxSpeedPixelPerSecond);
            }

            if (moveOptions.specifyMinSpeedPixelPerSecond && std::isfinite(moveOptions.minSpeedPixelPerSecond) && moveOptions.minSpeedPixelPerSecond > 0.0)
            {
                upperBoundMs = std::min(upperBoundMs, distancePixel * 1000.0 / moveOptions.minSpeedPixelPerSecond);
            }

            if (lowerBoundMs <= upperBoundMs)
            {
                return std::min(std::max(baseDurationMs, lowerBoundMs), upperBoundMs);
            }

            if (std::abs(baseDurationMs - lowerBoundMs) <= std::abs(baseDurationMs - upperBoundMs))
            {
                return lowerBoundMs;
            }

            return upperBoundMs;
        }

        static double SampleMoveProgress(const GB_MouseMoveMode moveMode, const double linearProgress)
        {
            const double t = std::max(0.0, std::min(1.0, linearProgress));

            switch (moveMode)
            {
            case GB_MouseMoveMode::HumanLike:
                return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);

            case GB_MouseMoveMode::Linear:
            case GB_MouseMoveMode::Teleport:
            default:
                break;
            }

            return t;
        }

        static bool ExecuteCursorMove(const POINT& startPoint, const POINT& targetPoint, const GB_MouseMoveOptions& moveOptions)
        {
            if (moveOptions.moveMode == GB_MouseMoveMode::Teleport)
            {
                return TrySetCurrentPhysicalCursorPosition(targetPoint);
            }

            const double deltaX = static_cast<double>(targetPoint.x) - static_cast<double>(startPoint.x);
            const double deltaY = static_cast<double>(targetPoint.y) - static_cast<double>(startPoint.y);
            const double distancePixel = std::sqrt(deltaX * deltaX + deltaY * deltaY);
            if (!(distancePixel > 0.0))
            {
                return TrySetCurrentPhysicalCursorPosition(targetPoint);
            }

            const double durationMs = ResolveMoveDurationMs(distancePixel, moveOptions);
            const double samplingIntervalMs = GetPositiveOptionOrDefault(moveOptions.samplingIntervalMs, 4.0);
            const double maxStepPixelDistance = GetPositiveOptionOrDefault(moveOptions.maxStepPixelDistance, 8.0);

            const int stepCountByDuration = durationMs > 0.0 ? std::max(1, static_cast<int>(std::ceil(durationMs / samplingIntervalMs))) : 1;
            const int stepCountByDistance = std::max(1, static_cast<int>(std::ceil(distancePixel / maxStepPixelDistance)));
            const int stepCount = std::max(stepCountByDuration, stepCountByDistance);

            const std::vector<GB_ScreenInfo> screenInfos = GB_Screen::GetAllScreens();
            if (screenInfos.empty())
            {
                return false;
            }

            const auto moveStartTime = std::chrono::steady_clock::now();
            for (int stepIndex = 1; stepIndex <= stepCount; stepIndex++)
            {
                const double linearProgress = static_cast<double>(stepIndex) / static_cast<double>(stepCount);
                const double sampledProgress = SampleMoveProgress(moveOptions.moveMode, linearProgress);

                const GB_Point2d rawSamplePoint(static_cast<double>(startPoint.x) + deltaX * sampledProgress, static_cast<double>(startPoint.y) + deltaY * sampledProgress);
                POINT sampledPoint = {};
                int sampledScreenIndex = -1;
                if (!TryProjectPointToNearestScreenPixel(screenInfos, rawSamplePoint, sampledPoint, sampledScreenIndex) || !TrySetCurrentPhysicalCursorPosition(sampledPoint))
                {
                    return false;
                }
                (void)sampledScreenIndex;

                if (stepIndex >= stepCount || !(durationMs > 0.0))
                {
                    continue;
                }

                const auto expectedTime = moveStartTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(durationMs * linearProgress));
                std::this_thread::sleep_until(expectedTime);
            }

            return TrySetCurrentPhysicalCursorPosition(targetPoint);
        }

        static bool TryGetMousePhysicalPixelPointOnScreen(int& screenIndex, GB_Point2d& physicalPixelPointOnScreen)
        {
            screenIndex = -1;
            physicalPixelPointOnScreen = GB_Point2d();

            DpiAwarenessScope dpiAwarenessScope;

            POINT physicalCursorPoint = { 0, 0 };
            if (!TryGetCurrentPhysicalCursorPosition(physicalCursorPoint))
            {
                return false;
            }

            const std::vector<GB_ScreenInfo> screenInfos = GB_Screen::GetAllScreens();
            if (screenInfos.empty())
            {
                return false;
            }

            return TryGetPhysicalPixelPointOnScreenFromCursorPoint(screenInfos, physicalCursorPoint, screenIndex, physicalPixelPointOnScreen);
        }

        static bool TryCaptureDisplayedCursorFallbackImage(const int fallbackCaptureRadius, GB_Image& cursorImage, GB_Point2d& hotSpot)
        {
            cursorImage.Clear();
            hotSpot = GB_Point2d();

            const int captureRadius = std::max(fallbackCaptureRadius, 1);

            DpiAwarenessScope dpiAwarenessScope;

            CURSORINFO cursorInfo = {};
            if (!TryGetCurrentVisibleCursorInfo(cursorInfo))
            {
                return false;
            }

            const double centerX = static_cast<double>(cursorInfo.ptScreenPos.x);
            const double centerY = static_cast<double>(cursorInfo.ptScreenPos.y);

            int cursorWidth = 0;
            int cursorHeight = 0;
            ICONINFO iconInfo = {};
            const bool hasCursorGeometry = TryGetCursorBitmapSize(cursorInfo.hCursor, cursorWidth, cursorHeight, iconInfo);
            IconBitmapScope maskBitmapScope(iconInfo.hbmMask);
            IconBitmapScope colorBitmapScope(iconInfo.hbmColor);

            GB_Rectangle desiredRectangle(centerX - static_cast<double>(captureRadius), centerY - static_cast<double>(captureRadius), centerX + static_cast<double>(captureRadius) + 1.0, centerY + static_cast<double>(captureRadius) + 1.0);
            if (hasCursorGeometry)
            {
                const double cursorLeft = centerX - static_cast<double>(iconInfo.xHotspot);
                const double cursorTop = centerY - static_cast<double>(iconInfo.yHotspot);
                desiredRectangle.Expand(GB_Rectangle(cursorLeft, cursorTop, cursorLeft + static_cast<double>(cursorWidth), cursorTop + static_cast<double>(cursorHeight)));
            }

            const GB_Rectangle virtualScreenRectangle = GB_Screen::GetVirtualScreenRectangle();
            if (!virtualScreenRectangle.IsValid())
            {
                return false;
            }

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

            if (capturedImage.GetDepth() == GB_ImageDepth::UInt8 && capturedImage.GetChannels() == 4 && !TryForceImageOpaqueAlpha(capturedImage))
            {
                return false;
            }

            hotSpot.Set(centerX - clippedRectangle.minX, centerY - clippedRectangle.minY);

            if (hasCursorGeometry)
            {
                const int cursorLeftOnImage = static_cast<int>(cursorInfo.ptScreenPos.x) - static_cast<int>(iconInfo.xHotspot) - static_cast<int>(std::llround(clippedRectangle.minX));
                const int cursorTopOnImage = static_cast<int>(cursorInfo.ptScreenPos.y) - static_cast<int>(iconInfo.yHotspot) - static_cast<int>(std::llround(clippedRectangle.minY));
                if (!TryOverlayCursorOnBgraImage(cursorInfo.hCursor, cursorLeftOnImage, cursorTopOnImage, cursorWidth, cursorHeight, capturedImage))
                {
                    return false;
                }
            }

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

        if (iconInfo.hbmColor == nullptr)
        {
            if (internal::TryBuildMonochromeCursorImageFromMaskBitmap(iconInfo, cursorWidth, cursorHeight, cursorImage))
            {
                hotSpot.Set(static_cast<double>(iconInfo.xHotspot), static_cast<double>(iconInfo.yHotspot));
                if (!internal::TryTrimTransparentBorder(cursorImage, &hotSpot))
                {
                    cursorImage.Clear();
                    hotSpot = GB_Point2d();
                    return false;
                }
                return true;
            }
        }

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
        if (!internal::TryTrimTransparentBorder(cursorImage, &hotSpot))
        {
            cursorImage.Clear();
            hotSpot = GB_Point2d();
            return false;
        }
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
    return internal::TryGetMousePhysicalPixelPointOnScreen(screenIndex, physicalPixelPointOnScreen);
#endif
}

bool GB_Mouse::MoveTo(const GB_Point2d& physicalPixelPoint, const GB_MouseMoveCoordinateType coordinateType, const GB_MouseMoveOptions& moveOptions)
{
#if !defined(_WIN32)
    (void)physicalPixelPoint;
    (void)coordinateType;
    (void)moveOptions;
    return false;
#else
    internal::DpiAwarenessScope dpiAwarenessScope;

    POINT startPoint = { 0, 0 };
    if (!internal::TryGetCurrentPhysicalCursorPosition(startPoint))
    {
        return false;
    }

    const std::vector<GB_ScreenInfo> screenInfos = GB_Screen::GetAllScreens();
    if (screenInfos.empty())
    {
        return false;
    }

    switch (coordinateType)
    {
    case GB_MouseMoveCoordinateType::VirtualScreenPhysicalPixel:
    {
        POINT targetPoint = {};
        int projectedScreenIndex = -1;
        if (!internal::TryProjectPointToNearestScreenPixel(screenInfos, physicalPixelPoint, targetPoint, projectedScreenIndex))
        {
            return false;
        }
        (void)projectedScreenIndex;

        return internal::ExecuteCursorMove(startPoint, targetPoint, moveOptions);
    }

    case GB_MouseMoveCoordinateType::CurrentScreenPhysicalPixel:
    {
        int currentScreenIndex = -1;
        GB_Point2d currentPhysicalPixelPointOnScreen;
        if (!internal::TryGetPhysicalPixelPointOnScreenFromCursorPoint(screenInfos, startPoint, currentScreenIndex, currentPhysicalPixelPointOnScreen))
        {
            return false;
        }
        (void)currentPhysicalPixelPointOnScreen;

        POINT targetPoint = {};
        if (!internal::TryResolveScreenLocalTargetPoint(screenInfos, currentScreenIndex, physicalPixelPoint, targetPoint))
        {
            return false;
        }

        return internal::ExecuteCursorMove(startPoint, targetPoint, moveOptions);
    }

    default:
        break;
    }

    return false;
#endif
}

bool GB_Mouse::MoveTo(const int screenIndex, const GB_Point2d& physicalPixelPointOnScreen, const GB_MouseMoveOptions& moveOptions)
{
#if !defined(_WIN32)
    (void)screenIndex;
    (void)physicalPixelPointOnScreen;
    (void)moveOptions;
    return false;
#else
    internal::DpiAwarenessScope dpiAwarenessScope;

    POINT startPoint = { 0, 0 };
    if (!internal::TryGetCurrentPhysicalCursorPosition(startPoint))
    {
        return false;
    }

    const std::vector<GB_ScreenInfo> screenInfos = GB_Screen::GetAllScreens();
    POINT targetPoint = {};
    if (!internal::TryResolveScreenLocalTargetPoint(screenInfos, screenIndex, physicalPixelPointOnScreen, targetPoint))
    {
        return false;
    }

    return internal::ExecuteCursorMove(startPoint, targetPoint, moveOptions);
#endif
}

bool GB_Mouse::Move(const GB_Vector2d& physicalPixelOffset, const bool allowMoveToOtherScreens, const GB_MouseMoveOptions& moveOptions)
{
#if !defined(_WIN32)
    (void)physicalPixelOffset;
    (void)allowMoveToOtherScreens;
    (void)moveOptions;
    return false;
#else
    if (!physicalPixelOffset.IsValid())
    {
        return false;
    }

    internal::DpiAwarenessScope dpiAwarenessScope;

    POINT startPoint = { 0, 0 };
    if (!internal::TryGetCurrentPhysicalCursorPosition(startPoint))
    {
        return false;
    }

    const std::vector<GB_ScreenInfo> screenInfos = GB_Screen::GetAllScreens();
    if (screenInfos.empty())
    {
        return false;
    }

    const GB_Point2d startPhysicalPixelPoint(static_cast<double>(startPoint.x), static_cast<double>(startPoint.y));

    if (allowMoveToOtherScreens)
    {
        POINT targetPoint = {};
        int projectedScreenIndex = -1;
        if (!internal::TryProjectPointToNearestScreenPixel(screenInfos, startPhysicalPixelPoint + physicalPixelOffset, targetPoint, projectedScreenIndex))
        {
            return false;
        }
        (void)projectedScreenIndex;

        return internal::ExecuteCursorMove(startPoint, targetPoint, moveOptions);
    }

    int currentScreenIndex = -1;
    GB_Point2d currentPhysicalPixelPointOnScreen;
    if (!internal::TryGetPhysicalPixelPointOnScreenFromCursorPoint(screenInfos, startPoint, currentScreenIndex, currentPhysicalPixelPointOnScreen))
    {
        return false;
    }
    (void)currentPhysicalPixelPointOnScreen;

    GB_Rectangle currentScreenRectangle;
    if (!internal::TryGetScreenRectangle(screenInfos, currentScreenIndex, currentScreenRectangle))
    {
        return false;
    }

    POINT targetPoint = {};
    if (!internal::TryClampPointToRectangle(startPhysicalPixelPoint + physicalPixelOffset, currentScreenRectangle, targetPoint))
    {
        return false;
    }

    return internal::ExecuteCursorMove(startPoint, targetPoint, moveOptions);
#endif
}
