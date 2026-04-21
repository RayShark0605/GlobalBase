#include "GB_Mouse.h"

#include "GB_Screen.h"
#include "../Geometry/GB_Rectangle.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
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

            constexpr double minValue = static_cast<double>(std::numeric_limits<int64_t>::min());
            constexpr double maxValue = static_cast<double>(std::numeric_limits<int64_t>::max());
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

        static bool TryGetImagePixelBrightness(const GB_Image& image, const int x, const int y, double& brightness)
        {
            brightness = 0.0;

            if (image.IsEmpty() || image.GetDepth() != GB_ImageDepth::UInt8 || x < 0 || y < 0)
            {
                return false;
            }

            if (image.GetWidth() > static_cast<size_t>(std::numeric_limits<int>::max()) || image.GetHeight() > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                return false;
            }

            if (x >= static_cast<int>(image.GetWidth()) || y >= static_cast<int>(image.GetHeight()))
            {
                return false;
            }

            const size_t channelCount = image.GetChannels();
            if (channelCount == 0)
            {
                return false;
            }

            const unsigned char* rowData = image.GetRowData(static_cast<size_t>(y));
            if (rowData == nullptr)
            {
                return false;
            }

            const unsigned char* pixelData = rowData + static_cast<size_t>(x) * channelCount;
            if (channelCount == 1)
            {
                brightness = static_cast<double>(pixelData[0]);
                return true;
            }

            const double blueValue = static_cast<double>(pixelData[0]);
            const double greenValue = static_cast<double>(channelCount >= 2 ? pixelData[1] : pixelData[0]);
            const double redValue = static_cast<double>(channelCount >= 3 ? pixelData[2] : pixelData[0]);
            brightness = redValue * 0.299 + greenValue * 0.587 + blueValue * 0.114;
            return true;
        }

        static bool TryCaptureCursorBackgroundImage(const POINT& physicalCursorPoint, const ICONINFO& iconInfo, const int cursorWidth, const int cursorHeight, GB_Image& backgroundImage, int& backgroundOffsetX, int& backgroundOffsetY)
        {
            backgroundImage.Clear();
            backgroundOffsetX = 0;
            backgroundOffsetY = 0;

            if (cursorWidth <= 0 || cursorHeight <= 0)
            {
                return false;
            }

            const double cursorLeft = static_cast<double>(physicalCursorPoint.x) - static_cast<double>(iconInfo.xHotspot);
            const double cursorTop = static_cast<double>(physicalCursorPoint.y) - static_cast<double>(iconInfo.yHotspot);
            const GB_Rectangle cursorRectangle(cursorLeft, cursorTop, cursorLeft + static_cast<double>(cursorWidth), cursorTop + static_cast<double>(cursorHeight));

            const GB_Rectangle virtualScreenRectangle = GB_Screen::GetVirtualScreenRectangle();
            if (!virtualScreenRectangle.IsValid())
            {
                return false;
            }

            const GB_Rectangle clippedRectangle = cursorRectangle.Intersected(virtualScreenRectangle);
            if (!clippedRectangle.IsValid() || clippedRectangle.Width() <= 0.0 || clippedRectangle.Height() <= 0.0)
            {
                return false;
            }

            if (!GB_Screen::CaptureVirtualScreen(clippedRectangle, backgroundImage) || backgroundImage.IsEmpty())
            {
                backgroundImage.Clear();
                return false;
            }

            const double offsetX = clippedRectangle.minX - cursorRectangle.minX;
            const double offsetY = clippedRectangle.minY - cursorRectangle.minY;
            int64_t roundedOffsetX = 0;
            int64_t roundedOffsetY = 0;
            if (!TryRoundDoubleToInt64(offsetX, roundedOffsetX) || !TryRoundDoubleToInt64(offsetY, roundedOffsetY))
            {
                backgroundImage.Clear();
                return false;
            }

            if (roundedOffsetX < static_cast<int64_t>(std::numeric_limits<int>::min()) || roundedOffsetX > static_cast<int64_t>(std::numeric_limits<int>::max()) ||
                roundedOffsetY < static_cast<int64_t>(std::numeric_limits<int>::min()) || roundedOffsetY > static_cast<int64_t>(std::numeric_limits<int>::max()))
            {
                backgroundImage.Clear();
                return false;
            }

            backgroundOffsetX = static_cast<int>(roundedOffsetX);
            backgroundOffsetY = static_cast<int>(roundedOffsetY);
            return true;
        }

        static uint8_t ResolveMonochromeCursorReverseScreenGrayValueFromBrightness(const double brightness)
        {
            return brightness >= 128.0 ? 0u : 255u;
        }

        static bool TryResolveMonochromeCursorReverseScreenDefaultGrayValue(const std::vector<uint8_t>& maskBitmapBits, const size_t rowStrideBytes, const int cursorWidth, const int cursorHeight, const GB_Image* backgroundImage, const int backgroundOffsetX, const int backgroundOffsetY, uint8_t& grayValue)
        {
            grayValue = 255;

            if (rowStrideBytes == 0 || cursorWidth <= 0 || cursorHeight <= 0)
            {
                return false;
            }

            bool hasReverseScreenPixel = false;
            uint64_t brightSampleCount = 0;
            uint64_t darkSampleCount = 0;
            double brightnessSum = 0.0;
            uint64_t brightnessSampleCount = 0;

            for (int rowIndex = 0; rowIndex < cursorHeight; rowIndex++)
            {
                for (int colIndex = 0; colIndex < cursorWidth; colIndex++)
                {
                    bool andMaskBit = false;
                    bool xorMaskBit = false;
                    if (!TryGetMonochromeBitmapPixel(maskBitmapBits, rowStrideBytes, cursorWidth, cursorHeight * 2, colIndex, rowIndex, andMaskBit) ||
                        !TryGetMonochromeBitmapPixel(maskBitmapBits, rowStrideBytes, cursorWidth, cursorHeight * 2, colIndex, rowIndex + cursorHeight, xorMaskBit))
                    {
                        return false;
                    }

                    if (!(andMaskBit && xorMaskBit))
                    {
                        continue;
                    }

                    hasReverseScreenPixel = true;
                    if (backgroundImage == nullptr)
                    {
                        continue;
                    }

                    const int backgroundX = colIndex - backgroundOffsetX;
                    const int backgroundY = rowIndex - backgroundOffsetY;
                    double brightness = 0.0;
                    if (!TryGetImagePixelBrightness(*backgroundImage, backgroundX, backgroundY, brightness))
                    {
                        continue;
                    }

                    brightnessSum += brightness;
                    brightnessSampleCount++;
                    if (brightness >= 128.0)
                    {
                        brightSampleCount++;
                    }
                    else
                    {
                        darkSampleCount++;
                    }
                }
            }

            if (!hasReverseScreenPixel)
            {
                return true;
            }

            if (brightnessSampleCount == 0 && backgroundImage != nullptr && !backgroundImage->IsEmpty())
            {
                const int backgroundWidth = static_cast<int>(backgroundImage->GetWidth());
                const int backgroundHeight = static_cast<int>(backgroundImage->GetHeight());
                for (int rowIndex = 0; rowIndex < backgroundHeight; rowIndex++)
                {
                    for (int colIndex = 0; colIndex < backgroundWidth; colIndex++)
                    {
                        double brightness = 0.0;
                        if (!TryGetImagePixelBrightness(*backgroundImage, colIndex, rowIndex, brightness))
                        {
                            continue;
                        }

                        brightnessSum += brightness;
                        brightnessSampleCount++;
                        if (brightness >= 128.0)
                        {
                            brightSampleCount++;
                        }
                        else
                        {
                            darkSampleCount++;
                        }
                    }
                }
            }

            if (brightnessSampleCount == 0)
            {
                grayValue = 255;
                return true;
            }

            if (brightSampleCount > darkSampleCount)
            {
                grayValue = 0;
                return true;
            }

            if (darkSampleCount > brightSampleCount)
            {
                grayValue = 255;
                return true;
            }

            grayValue = ResolveMonochromeCursorReverseScreenGrayValueFromBrightness(brightnessSum / static_cast<double>(brightnessSampleCount));
            return true;
        }

        static uint8_t ResolveMonochromeCursorReverseScreenGrayValueAtPixel(const GB_Image* backgroundImage, const int backgroundX, const int backgroundY, const uint8_t defaultGrayValue)
        {
            if (backgroundImage != nullptr)
            {
                double brightness = 0.0;
                if (TryGetImagePixelBrightness(*backgroundImage, backgroundX, backgroundY, brightness))
                {
                    return ResolveMonochromeCursorReverseScreenGrayValueFromBrightness(brightness);
                }
            }

            return defaultGrayValue;
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

        static bool TryBuildMonochromeCursorImageFromMaskBitmap(const ICONINFO& iconInfo, const int cursorWidth, const int cursorHeight, const GB_Image* backgroundImage, const int backgroundOffsetX, const int backgroundOffsetY, GB_Image& cursorImage)
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

            uint8_t defaultReverseScreenGrayValue = 255;
            if (!TryResolveMonochromeCursorReverseScreenDefaultGrayValue(maskBitmapBits, rowStrideBytes, cursorWidth, cursorHeight, backgroundImage, backgroundOffsetX, backgroundOffsetY, defaultReverseScreenGrayValue))
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
                            // 这里优先根据当前像素位置的局部背景亮度进行规范化；若局部背景不可获取，则退化为整体背景亮度对应的默认颜色。
                            const int backgroundX = colIndex - backgroundOffsetX;
                            const int backgroundY = rowIndex - backgroundOffsetY;
                            const uint8_t reverseScreenGrayValue = ResolveMonochromeCursorReverseScreenGrayValueAtPixel(backgroundImage, backgroundX, backgroundY, defaultReverseScreenGrayValue);
                            blueValue = reverseScreenGrayValue;
                            greenValue = reverseScreenGrayValue;
                            redValue = reverseScreenGrayValue;
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

        static bool TryGetCursorClipRectangle(GB_Rectangle& clipRectangle)
        {
            clipRectangle = GB_Rectangle();

            RECT clipRect = {};
            if (::GetClipCursor(&clipRect) == FALSE)
            {
                return false;
            }

            if (clipRect.right <= clipRect.left || clipRect.bottom <= clipRect.top)
            {
                return false;
            }

            clipRectangle = GB_Rectangle(static_cast<double>(clipRect.left), static_cast<double>(clipRect.top), static_cast<double>(clipRect.right), static_cast<double>(clipRect.bottom));
            return clipRectangle.IsValid() && clipRectangle.Width() > 0.0 && clipRectangle.Height() > 0.0;
        }

        static bool TryClampPointToClipCursorRectangle(const POINT& sourcePoint, POINT& clampedPoint)
        {
            clampedPoint = sourcePoint;

            GB_Rectangle clipRectangle;
            if (!TryGetCursorClipRectangle(clipRectangle))
            {
                return true;
            }

            return TryClampPointToRectangle(GB_Point2d(static_cast<double>(sourcePoint.x), static_cast<double>(sourcePoint.y)), clipRectangle, clampedPoint);
        }

        static bool TryProjectPointToSpecificScreenPixel(const std::vector<GB_ScreenInfo>& screenInfos, const GB_Point2d& physicalPixelPoint, const int screenIndex, POINT& projectedPoint, double& distanceSquared)
        {
            projectedPoint.x = 0;
            projectedPoint.y = 0;
            distanceSquared = 0.0;

            if (!IsFinitePoint(physicalPixelPoint))
            {
                return false;
            }

            GB_Rectangle screenRectangle;
            if (!TryGetScreenRectangle(screenInfos, screenIndex, screenRectangle))
            {
                return false;
            }

            GB_Rectangle clipRectangle;
            if (TryGetCursorClipRectangle(clipRectangle))
            {
                screenRectangle = screenRectangle.Intersected(clipRectangle);
                if (!screenRectangle.IsValid() || screenRectangle.Width() <= 0.0 || screenRectangle.Height() <= 0.0)
                {
                    return false;
                }
            }

            const double clampedX = std::max(screenRectangle.minX, std::min(physicalPixelPoint.x, screenRectangle.maxX - 1.0));
            const double clampedY = std::max(screenRectangle.minY, std::min(physicalPixelPoint.y, screenRectangle.maxY - 1.0));
            const double deltaX = physicalPixelPoint.x - clampedX;
            const double deltaY = physicalPixelPoint.y - clampedY;
            distanceSquared = deltaX * deltaX + deltaY * deltaY;

            return TryClampPointToRectangle(GB_Point2d(clampedX, clampedY), screenRectangle, projectedPoint);
        }

        static bool TryProjectPointToNearestScreenPixel(const std::vector<GB_ScreenInfo>& screenInfos, const GB_Point2d& physicalPixelPoint, POINT& projectedPoint, int& projectedScreenIndex, const int preferredScreenIndex = -1)
        {
            projectedPoint.x = 0;
            projectedPoint.y = 0;
            projectedScreenIndex = -1;

            if (!IsFinitePoint(physicalPixelPoint) || screenInfos.empty())
            {
                return false;
            }

            bool hasBestPoint = false;
            double bestDistanceSquared = 0.0;
            POINT bestPoint = {};
            int bestScreenIndex = -1;

            for (size_t i = 0; i < screenInfos.size(); i++)
            {
                POINT candidatePoint = {};
                double candidateDistanceSquared = 0.0;
                if (!TryProjectPointToSpecificScreenPixel(screenInfos, physicalPixelPoint, static_cast<int>(i), candidatePoint, candidateDistanceSquared))
                {
                    continue;
                }

                if (!hasBestPoint || candidateDistanceSquared < bestDistanceSquared || (candidateDistanceSquared == bestDistanceSquared && screenInfos[i].isPrimary))
                {
                    hasBestPoint = true;
                    bestDistanceSquared = candidateDistanceSquared;
                    bestPoint = candidatePoint;
                    bestScreenIndex = static_cast<int>(i);
                }
            }

            if (!hasBestPoint)
            {
                return false;
            }

            if (preferredScreenIndex >= 0 && static_cast<size_t>(preferredScreenIndex) < screenInfos.size() && preferredScreenIndex != bestScreenIndex)
            {
                POINT preferredPoint = {};
                double preferredDistanceSquared = 0.0;
                if (TryProjectPointToSpecificScreenPixel(screenInfos, physicalPixelPoint, preferredScreenIndex, preferredPoint, preferredDistanceSquared))
                {
                    constexpr double screenSwitchHysteresisDistanceSquared = 64.0;
                    if (preferredDistanceSquared <= bestDistanceSquared + screenSwitchHysteresisDistanceSquared)
                    {
                        projectedPoint = preferredPoint;
                        projectedScreenIndex = preferredScreenIndex;
                        return true;
                    }
                }
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

        static double GetHumanLikeMinimumDurationMs(const double distancePixel)
        {
            if (!(distancePixel > 0.0) || !std::isfinite(distancePixel))
            {
                return 0.0;
            }

            return 18.0 + std::sqrt(distancePixel) * 6.5;
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

            if (moveOptions.moveMode == GB_MouseMoveMode::HumanLike)
            {
                baseDurationMs = std::max(baseDurationMs, GetHumanLikeMinimumDurationMs(distancePixel));
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

        static uint64_t MixUint64(uint64_t value)
        {
            value ^= value >> 30;
            value *= 0xbf58476d1ce4e5b9ull;
            value ^= value >> 27;
            value *= 0x94d049bb133111ebull;
            value ^= value >> 31;
            return value;
        }

        static double GetDeterministicUnitValueForMove(const POINT& startPoint, const POINT& targetPoint)
        {
            uint64_t mixedValue = 0x9e3779b97f4a7c15ull;
            mixedValue ^= static_cast<uint64_t>(static_cast<uint32_t>(startPoint.x)) + 0x9e3779b97f4a7c15ull + (mixedValue << 6) + (mixedValue >> 2);
            mixedValue ^= static_cast<uint64_t>(static_cast<uint32_t>(startPoint.y)) + 0x9e3779b97f4a7c15ull + (mixedValue << 6) + (mixedValue >> 2);
            mixedValue ^= static_cast<uint64_t>(static_cast<uint32_t>(targetPoint.x)) + 0x9e3779b97f4a7c15ull + (mixedValue << 6) + (mixedValue >> 2);
            mixedValue ^= static_cast<uint64_t>(static_cast<uint32_t>(targetPoint.y)) + 0x9e3779b97f4a7c15ull + (mixedValue << 6) + (mixedValue >> 2);
            const uint64_t mixedResult = MixUint64(mixedValue);
            return static_cast<double>(mixedResult) / static_cast<double>((std::numeric_limits<uint64_t>::max)());
        }

        static GB_Point2d EvaluateCubicBezierPoint(const GB_Point2d& startPoint, const GB_Point2d& controlPoint1, const GB_Point2d& controlPoint2, const GB_Point2d& endPoint, const double progress)
        {
            const double t = std::max(0.0, std::min(1.0, progress));
            const double u = 1.0 - t;
            const double uu = u * u;
            const double tt = t * t;
            const double uuu = uu * u;
            const double ttt = tt * t;

            return GB_Point2d(
                uuu * startPoint.x + 3.0 * uu * t * controlPoint1.x + 3.0 * u * tt * controlPoint2.x + ttt * endPoint.x,
                uuu * startPoint.y + 3.0 * uu * t * controlPoint1.y + 3.0 * u * tt * controlPoint2.y + ttt * endPoint.y);
        }

        static bool TryBuildHumanLikeBezierControlPoints(const POINT& startPoint, const POINT& targetPoint, GB_Point2d& controlPoint1, GB_Point2d& controlPoint2)
        {
            controlPoint1 = GB_Point2d();
            controlPoint2 = GB_Point2d();

            const double deltaX = static_cast<double>(targetPoint.x) - static_cast<double>(startPoint.x);
            const double deltaY = static_cast<double>(targetPoint.y) - static_cast<double>(startPoint.y);
            const double distancePixel = std::sqrt(deltaX * deltaX + deltaY * deltaY);
            if (!(distancePixel > 12.0) || !std::isfinite(distancePixel))
            {
                return false;
            }

            const double inverseDistance = 1.0 / distancePixel;
            const double directionX = deltaX * inverseDistance;
            const double directionY = deltaY * inverseDistance;
            const double normalX = -directionY;
            const double normalY = directionX;

            const double unitValue = GetDeterministicUnitValueForMove(startPoint, targetPoint);
            const double sideSign = unitValue >= 0.5 ? 1.0 : -1.0;
            const double curvatureScale = 0.75 + unitValue * 0.5;
            const double lateralOffsetPixel = std::min(36.0, std::max(2.0, distancePixel * 0.12 * curvatureScale));
            const double firstControlAlongDistance = distancePixel * (0.24 + unitValue * 0.06);
            const double secondControlAlongDistance = distancePixel * (0.70 + unitValue * 0.06);

            controlPoint1.Set(
                static_cast<double>(startPoint.x) + directionX * firstControlAlongDistance + normalX * sideSign * lateralOffsetPixel,
                static_cast<double>(startPoint.y) + directionY * firstControlAlongDistance + normalY * sideSign * lateralOffsetPixel);
            controlPoint2.Set(
                static_cast<double>(startPoint.x) + directionX * secondControlAlongDistance + normalX * sideSign * lateralOffsetPixel * 0.45,
                static_cast<double>(startPoint.y) + directionY * secondControlAlongDistance + normalY * sideSign * lateralOffsetPixel * 0.45);
            return true;
        }

        static GB_Point2d EvaluateMovePathPoint(const POINT& startPoint, const POINT& targetPoint, const GB_MouseMoveMode moveMode, const double linearProgress)
        {
            const double sampledProgress = SampleMoveProgress(moveMode, linearProgress);
            const GB_Point2d startPhysicalPixelPoint(static_cast<double>(startPoint.x), static_cast<double>(startPoint.y));
            const GB_Point2d targetPhysicalPixelPoint(static_cast<double>(targetPoint.x), static_cast<double>(targetPoint.y));

            if (moveMode != GB_MouseMoveMode::HumanLike)
            {
                return GB_Point2d(
                    startPhysicalPixelPoint.x + (targetPhysicalPixelPoint.x - startPhysicalPixelPoint.x) * sampledProgress,
                    startPhysicalPixelPoint.y + (targetPhysicalPixelPoint.y - startPhysicalPixelPoint.y) * sampledProgress);
            }

            GB_Point2d controlPoint1;
            GB_Point2d controlPoint2;
            if (!TryBuildHumanLikeBezierControlPoints(startPoint, targetPoint, controlPoint1, controlPoint2))
            {
                return GB_Point2d(
                    startPhysicalPixelPoint.x + (targetPhysicalPixelPoint.x - startPhysicalPixelPoint.x) * sampledProgress,
                    startPhysicalPixelPoint.y + (targetPhysicalPixelPoint.y - startPhysicalPixelPoint.y) * sampledProgress);
            }

            return EvaluateCubicBezierPoint(startPhysicalPixelPoint, controlPoint1, controlPoint2, targetPhysicalPixelPoint, sampledProgress);
        }

        static int ResolveMoveStepCount(const double distancePixel, const double durationMs, const GB_MouseMoveOptions& moveOptions)
        {
            const double samplingIntervalMs = GetPositiveOptionOrDefault(moveOptions.samplingIntervalMs, 4.0);
            const double maxStepPixelDistance = GetPositiveOptionOrDefault(moveOptions.maxStepPixelDistance, 8.0);

            int64_t stepCountByDuration = 1;
            int64_t stepCountByDistance = 1;

            if (durationMs > 0.0 && std::isfinite(durationMs))
            {
                const double rawStepCountByDuration = std::ceil(durationMs / samplingIntervalMs);
                if (std::isfinite(rawStepCountByDuration) && rawStepCountByDuration > 1.0)
                {
                    stepCountByDuration = static_cast<int64_t>(std::min(rawStepCountByDuration, static_cast<double>(std::numeric_limits<int64_t>::max())));
                }
            }

            if (distancePixel > 0.0 && std::isfinite(distancePixel))
            {
                const double rawStepCountByDistance = std::ceil(distancePixel / maxStepPixelDistance);
                if (std::isfinite(rawStepCountByDistance) && rawStepCountByDistance > 1.0)
                {
                    stepCountByDistance = static_cast<int64_t>(std::min(rawStepCountByDistance, static_cast<double>(std::numeric_limits<int64_t>::max())));
                }
            }

            const int64_t maxReasonableStepCount = 100000;
            const int64_t stepCount = std::max<int64_t>(1, std::min<int64_t>(maxReasonableStepCount, std::max(stepCountByDuration, stepCountByDistance)));
            return static_cast<int>(stepCount);
        }

        static bool ExecuteCursorMove(const POINT& startPoint, const POINT& targetPoint, const std::vector<GB_ScreenInfo>& screenInfos, const GB_MouseMoveOptions& moveOptions)
        {
            if (screenInfos.empty())
            {
                return false;
            }

            POINT clipAdjustedTargetPoint = {};
            if (!TryClampPointToClipCursorRectangle(targetPoint, clipAdjustedTargetPoint))
            {
                return false;
            }

            POINT effectiveTargetPoint = {};
            int targetScreenIndex = -1;
            if (!TryProjectPointToNearestScreenPixel(screenInfos, GB_Point2d(static_cast<double>(clipAdjustedTargetPoint.x), static_cast<double>(clipAdjustedTargetPoint.y)), effectiveTargetPoint, targetScreenIndex))
            {
                return false;
            }

            if (moveOptions.moveMode == GB_MouseMoveMode::Teleport)
            {
                if (!TrySetCurrentPhysicalCursorPosition(effectiveTargetPoint))
                {
                    return false;
                }

                POINT actualPoint = {};
                if (TryGetCurrentPhysicalCursorPosition(actualPoint))
                {
                    return actualPoint.x == effectiveTargetPoint.x && actualPoint.y == effectiveTargetPoint.y;
                }

                return true;
            }

            const double deltaX = static_cast<double>(effectiveTargetPoint.x) - static_cast<double>(startPoint.x);
            const double deltaY = static_cast<double>(effectiveTargetPoint.y) - static_cast<double>(startPoint.y);
            const double distancePixel = std::sqrt(deltaX * deltaX + deltaY * deltaY);
            if (!(distancePixel > 0.0))
            {
                if (!TrySetCurrentPhysicalCursorPosition(effectiveTargetPoint))
                {
                    return false;
                }

                POINT actualPoint = {};
                if (TryGetCurrentPhysicalCursorPosition(actualPoint))
                {
                    return actualPoint.x == effectiveTargetPoint.x && actualPoint.y == effectiveTargetPoint.y;
                }

                return true;
            }

            int startScreenIndex = -1;
            (void)TryGetContainingScreenIndex(screenInfos, GB_Point2d(static_cast<double>(startPoint.x), static_cast<double>(startPoint.y)), startScreenIndex);

            const bool useCurvedHumanLikePath = (moveOptions.moveMode == GB_MouseMoveMode::HumanLike && startScreenIndex >= 0 && targetScreenIndex >= 0 && startScreenIndex == targetScreenIndex);
            const GB_MouseMoveMode pathMoveMode = useCurvedHumanLikePath ? moveOptions.moveMode : (moveOptions.moveMode == GB_MouseMoveMode::HumanLike ? GB_MouseMoveMode::Linear : moveOptions.moveMode);

            const double durationMs = ResolveMoveDurationMs(distancePixel, moveOptions);
            const int stepCount = ResolveMoveStepCount(distancePixel, durationMs, moveOptions);

            struct MoveStepSample
            {
                POINT cursorPoint = {};
                double linearProgress = 0.0;
            };

            std::vector<MoveStepSample> moveStepSamples;
            try
            {
                moveStepSamples.reserve(static_cast<size_t>(stepCount));
            }
            catch (...)
            {
                return false;
            }

            int preferredScreenIndex = startScreenIndex;
            for (int stepIndex = 1; stepIndex <= stepCount; stepIndex++)
            {
                const double linearProgress = static_cast<double>(stepIndex) / static_cast<double>(stepCount);
                const GB_Point2d rawSamplePoint = EvaluateMovePathPoint(startPoint, effectiveTargetPoint, pathMoveMode, linearProgress);

                POINT sampledPoint = {};
                int sampledScreenIndex = -1;
                if (!TryProjectPointToNearestScreenPixel(screenInfos, rawSamplePoint, sampledPoint, sampledScreenIndex, preferredScreenIndex))
                {
                    return false;
                }

                preferredScreenIndex = sampledScreenIndex;

                if (!moveStepSamples.empty() && moveStepSamples.back().cursorPoint.x == sampledPoint.x && moveStepSamples.back().cursorPoint.y == sampledPoint.y)
                {
                    moveStepSamples.back().linearProgress = linearProgress;
                    continue;
                }

                MoveStepSample sample;
                sample.cursorPoint = sampledPoint;
                sample.linearProgress = linearProgress;
                moveStepSamples.push_back(sample);
            }

            const auto moveStartTime = std::chrono::steady_clock::now();
            for (size_t stepIndex = 0; stepIndex < moveStepSamples.size(); stepIndex++)
            {
                if (!TrySetCurrentPhysicalCursorPosition(moveStepSamples[stepIndex].cursorPoint))
                {
                    return false;
                }

                if (stepIndex + 1 >= moveStepSamples.size() || !(durationMs > 0.0))
                {
                    continue;
                }

                const auto expectedTime = moveStartTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(durationMs * moveStepSamples[stepIndex].linearProgress));
                std::this_thread::sleep_until(expectedTime);
            }

            if (!TrySetCurrentPhysicalCursorPosition(effectiveTargetPoint))
            {
                return false;
            }

            POINT actualPoint = {};
            if (TryGetCurrentPhysicalCursorPosition(actualPoint))
            {
                return actualPoint.x == effectiveTargetPoint.x && actualPoint.y == effectiveTargetPoint.y;
            }

            return true;
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

        CURSORINFO cursorInfo = {};
        if (!internal::TryGetCurrentVisibleCursorInfo(cursorInfo))
        {
            cursorImage.Clear();
            return false;
        }

        const HCURSOR cursorHandle = cursorInfo.hCursor;

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
            GB_Image backgroundImage;
            int backgroundOffsetX = 0;
            int backgroundOffsetY = 0;
            const bool hasBackgroundImage = internal::TryCaptureCursorBackgroundImage(cursorInfo.ptScreenPos, iconInfo, cursorWidth, cursorHeight, backgroundImage, backgroundOffsetX, backgroundOffsetY);

            if (internal::TryBuildMonochromeCursorImageFromMaskBitmap(iconInfo, cursorWidth, cursorHeight, hasBackgroundImage ? &backgroundImage : nullptr, backgroundOffsetX, backgroundOffsetY, cursorImage))
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

        return internal::ExecuteCursorMove(startPoint, targetPoint, screenInfos, moveOptions);
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

        return internal::ExecuteCursorMove(startPoint, targetPoint, screenInfos, moveOptions);
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

    return internal::ExecuteCursorMove(startPoint, targetPoint, screenInfos, moveOptions);
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

        return internal::ExecuteCursorMove(startPoint, targetPoint, screenInfos, moveOptions);
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

    return internal::ExecuteCursorMove(startPoint, targetPoint, screenInfos, moveOptions);
#endif
}

#if defined(_WIN32)
namespace
{
    namespace internal
    {
        static bool TrySendMouseInput(const DWORD mouseFlags, const DWORD mouseData = 0)
        {
            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dx = 0;
            input.mi.dy = 0;
            input.mi.mouseData = mouseData;
            input.mi.dwFlags = mouseFlags;
            input.mi.time = 0;
            input.mi.dwExtraInfo = static_cast<ULONG_PTR>(::GetMessageExtraInfo());
            return ::SendInput(1, &input, sizeof(INPUT)) == 1;
        }

        static int ClampNonNegativeDelayMs(const int delayMs)
        {
            return std::max(delayMs, 0);
        }

        static void SleepForDelayMs(const int delayMs)
        {
            const int clampedDelayMs = ClampNonNegativeDelayMs(delayMs);
            if (clampedDelayMs <= 0)
            {
                return;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(clampedDelayMs));
        }

        static bool TrySendMouseInputWithRetry(const DWORD mouseFlags, const DWORD mouseData = 0, const int maxAttemptCount = 2, const int retryDelayMs = 1)
        {
            const int attemptCount = std::max(maxAttemptCount, 1);
            for (int attemptIndex = 0; attemptIndex < attemptCount; attemptIndex++)
            {
                if (TrySendMouseInput(mouseFlags, mouseData))
                {
                    return true;
                }

                if (attemptIndex + 1 < attemptCount)
                {
                    SleepForDelayMs(retryDelayMs);
                }
            }

            return false;
        }

        static int GetSafeSystemDoubleClickTimeMs()
        {
            const UINT systemDoubleClickTimeMs = ::GetDoubleClickTime();
            const UINT safeSystemDoubleClickTimeMs = (systemDoubleClickTimeMs > 0 ? systemDoubleClickTimeMs : 500u);
            return static_cast<int>(safeSystemDoubleClickTimeMs);
        }

        static int ResolveSafeDoubleClickPressDurationMs(const int downUpIntervalMs)
        {
            const int normalizedDownUpIntervalMs = ClampNonNegativeDelayMs(downUpIntervalMs);
            const int safetyMarginMs = 8;
            const int safeSystemDoubleClickTimeMs = GetSafeSystemDoubleClickTimeMs();
            const int maxPressDurationMs = std::max(0, (safeSystemDoubleClickTimeMs - safetyMarginMs) / 2);
            return std::min(normalizedDownUpIntervalMs, maxPressDurationMs);
        }

        static int ResolveHumanLikeDoubleClickIntervalMs(const int interClickIntervalMs, const int downUpIntervalMs)
        {
            const int safeSystemDoubleClickTimeMs = GetSafeSystemDoubleClickTimeMs();
            int resolvedIntervalMs = 0;
            if (interClickIntervalMs >= 0)
            {
                resolvedIntervalMs = interClickIntervalMs;
            }
            else
            {
                resolvedIntervalMs = std::max(40, std::min(120, safeSystemDoubleClickTimeMs / 5));
            }

            const int safetyMarginMs = 8;
            const int maxInterClickIntervalMs = std::max(0, safeSystemDoubleClickTimeMs - safetyMarginMs - downUpIntervalMs * 2);
            return std::min(resolvedIntervalMs, maxInterClickIntervalMs);
        }

        static bool TryClickMouseButton(const DWORD downFlags, const DWORD upFlags, const int downUpIntervalMs)
        {
            if (!TrySendMouseInputWithRetry(downFlags))
            {
                return false;
            }

            SleepForDelayMs(downUpIntervalMs);

            if (TrySendMouseInputWithRetry(upFlags))
            {
                return true;
            }

            SleepForDelayMs(1);
            (void)TrySendMouseInputWithRetry(upFlags);
            return false;
        }

        static bool TryDoubleClickMouseButton(const DWORD downFlags, const DWORD upFlags, const int downUpIntervalMs, const int interClickIntervalMs)
        {
            const int normalizedDownUpIntervalMs = ResolveSafeDoubleClickPressDurationMs(downUpIntervalMs);
            const int normalizedInterClickIntervalMs = ResolveHumanLikeDoubleClickIntervalMs(interClickIntervalMs, normalizedDownUpIntervalMs);
            if (!TryClickMouseButton(downFlags, upFlags, normalizedDownUpIntervalMs))
            {
                return false;
            }

            SleepForDelayMs(normalizedInterClickIntervalMs);
            return TryClickMouseButton(downFlags, upFlags, normalizedDownUpIntervalMs);
        }
    }
}

bool GB_Mouse::PressLeftButton()
{
    return internal::TrySendMouseInputWithRetry(MOUSEEVENTF_LEFTDOWN);
}

bool GB_Mouse::ReleaseLeftButton()
{
    return internal::TrySendMouseInputWithRetry(MOUSEEVENTF_LEFTUP);
}

bool GB_Mouse::ClickLeftButton(const int downUpIntervalMs)
{
    return internal::TryClickMouseButton(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, downUpIntervalMs);
}

bool GB_Mouse::DoubleClickLeftButton(const int downUpIntervalMs, const int interClickIntervalMs)
{
    return internal::TryDoubleClickMouseButton(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, downUpIntervalMs, interClickIntervalMs);
}

bool GB_Mouse::PressRightButton()
{
    return internal::TrySendMouseInputWithRetry(MOUSEEVENTF_RIGHTDOWN);
}

bool GB_Mouse::ReleaseRightButton()
{
    return internal::TrySendMouseInputWithRetry(MOUSEEVENTF_RIGHTUP);
}

bool GB_Mouse::ClickRightButton(const int downUpIntervalMs)
{
    return internal::TryClickMouseButton(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, downUpIntervalMs);
}

bool GB_Mouse::PressMiddleButton()
{
    return internal::TrySendMouseInputWithRetry(MOUSEEVENTF_MIDDLEDOWN);
}

bool GB_Mouse::ReleaseMiddleButton()
{
    return internal::TrySendMouseInputWithRetry(MOUSEEVENTF_MIDDLEUP);
}

bool GB_Mouse::ClickMiddleButton(const int downUpIntervalMs)
{
    return internal::TryClickMouseButton(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, downUpIntervalMs);
}

bool GB_Mouse::ScrollVerticalWheel(const int wheelDelta)
{
    if (wheelDelta == 0)
    {
        return true;
    }

    return internal::TrySendMouseInputWithRetry(MOUSEEVENTF_WHEEL, static_cast<DWORD>(wheelDelta));
}

bool GB_Mouse::ScrollHorizontalWheel(const int wheelDelta)
{
    if (wheelDelta == 0)
    {
        return true;
    }

    return internal::TrySendMouseInputWithRetry(MOUSEEVENTF_HWHEEL, static_cast<DWORD>(wheelDelta));
}

#else

bool GB_Mouse::PressLeftButton()
{
    return false;
}

bool GB_Mouse::ReleaseLeftButton()
{
    return false;
}

bool GB_Mouse::ClickLeftButton(const int downUpIntervalMs)
{
    (void)downUpIntervalMs;
    return false;
}

bool GB_Mouse::DoubleClickLeftButton(const int downUpIntervalMs, const int interClickIntervalMs)
{
    (void)downUpIntervalMs;
    (void)interClickIntervalMs;
    return false;
}

bool GB_Mouse::PressRightButton()
{
    return false;
}

bool GB_Mouse::ReleaseRightButton()
{
    return false;
}

bool GB_Mouse::ClickRightButton(const int downUpIntervalMs)
{
    (void)downUpIntervalMs;
    return false;
}

bool GB_Mouse::PressMiddleButton()
{
    return false;
}

bool GB_Mouse::ReleaseMiddleButton()
{
    return false;
}

bool GB_Mouse::ClickMiddleButton(const int downUpIntervalMs)
{
    (void)downUpIntervalMs;
    return false;
}

bool GB_Mouse::ScrollVerticalWheel(const int wheelDelta)
{
    (void)wheelDelta;
    return false;
}

bool GB_Mouse::ScrollHorizontalWheel(const int wheelDelta)
{
    (void)wheelDelta;
    return false;
}

#endif

#if defined(_WIN32)
namespace
{
    namespace internal
    {
        static constexpr size_t globalMouseEventTypeCount = 11;
        static constexpr size_t globalMouseEventQueueCapacity = 256;

        static uint64_t GetSteadyTickCountMs()
        {
            const auto now = std::chrono::steady_clock::now();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        }

        static size_t GetGlobalMouseEventTypeIndex(const GB_GlobalMouseEventType eventType)
        {
            switch (eventType)
            {
            case GB_GlobalMouseEventType::Move:
                return 0;
            case GB_GlobalMouseEventType::LeftButtonDown:
                return 1;
            case GB_GlobalMouseEventType::LeftButtonUp:
                return 2;
            case GB_GlobalMouseEventType::RightButtonDown:
                return 3;
            case GB_GlobalMouseEventType::RightButtonUp:
                return 4;
            case GB_GlobalMouseEventType::MiddleButtonDown:
                return 5;
            case GB_GlobalMouseEventType::MiddleButtonUp:
                return 6;
            case GB_GlobalMouseEventType::XButtonDown:
                return 7;
            case GB_GlobalMouseEventType::XButtonUp:
                return 8;
            case GB_GlobalMouseEventType::VerticalWheel:
                return 9;
            case GB_GlobalMouseEventType::HorizontalWheel:
                return 10;
            }

            return globalMouseEventTypeCount;
        }

        static GB_GlobalMouseEventMask GetGlobalMouseEventMask(const GB_GlobalMouseEventType eventType)
        {
            switch (eventType)
            {
            case GB_GlobalMouseEventType::Move:
                return GB_GlobalMouseEventMask::Move;
            case GB_GlobalMouseEventType::LeftButtonDown:
                return GB_GlobalMouseEventMask::LeftButtonDown;
            case GB_GlobalMouseEventType::LeftButtonUp:
                return GB_GlobalMouseEventMask::LeftButtonUp;
            case GB_GlobalMouseEventType::RightButtonDown:
                return GB_GlobalMouseEventMask::RightButtonDown;
            case GB_GlobalMouseEventType::RightButtonUp:
                return GB_GlobalMouseEventMask::RightButtonUp;
            case GB_GlobalMouseEventType::MiddleButtonDown:
                return GB_GlobalMouseEventMask::MiddleButtonDown;
            case GB_GlobalMouseEventType::MiddleButtonUp:
                return GB_GlobalMouseEventMask::MiddleButtonUp;
            case GB_GlobalMouseEventType::XButtonDown:
                return GB_GlobalMouseEventMask::XButtonDown;
            case GB_GlobalMouseEventType::XButtonUp:
                return GB_GlobalMouseEventMask::XButtonUp;
            case GB_GlobalMouseEventType::VerticalWheel:
                return GB_GlobalMouseEventMask::VerticalWheel;
            case GB_GlobalMouseEventType::HorizontalWheel:
                return GB_GlobalMouseEventMask::HorizontalWheel;
            }

            return GB_GlobalMouseEventMask::None;
        }

        static uint32_t GetGlobalMouseEventMaskBits(const GB_GlobalMouseEventMask eventMask)
        {
            return static_cast<uint32_t>(eventMask);
        }

        static bool IsMoveEventType(const GB_GlobalMouseEventType eventType)
        {
            return eventType == GB_GlobalMouseEventType::Move;
        }


        static void FillGlobalMouseEventCommonFields(GB_GlobalMouseEvent& mouseEvent, const uint32_t messageTimeMs)
        {
            mouseEvent.physicalPixelPoint.Set(0.0, 0.0);
            POINT cursorPoint = {};
            if (TryGetCurrentPhysicalCursorPosition(cursorPoint))
            {
                mouseEvent.physicalPixelPoint.Set(static_cast<double>(cursorPoint.x), static_cast<double>(cursorPoint.y));
            }

            mouseEvent.wheelDelta = 0;
            mouseEvent.xButtonType = GB_GlobalMouseXButtonType::None;
            mouseEvent.isInjected = false;
            mouseEvent.isLowerIntegrityInjected = false;
            mouseEvent.messageTimeMs = messageTimeMs;
            mouseEvent.receiveTickCountMs = GetSteadyTickCountMs();
        }

        struct GlobalMouseCallbackEntry
        {
            GB_GlobalMouseEventCallback callback;
            GB_GlobalMouseCallbackOptions callbackOptions;
        };

        class GlobalMouseListenerState : public std::enable_shared_from_this<GlobalMouseListenerState>
        {
        public:
            void SetInterestedEvents(const GB_GlobalMouseEventMask eventMask)
            {
                interestedEventMaskBits.store(GetGlobalMouseEventMaskBits(eventMask));
            }

            GB_GlobalMouseEventMask GetInterestedEvents() const
            {
                return static_cast<GB_GlobalMouseEventMask>(interestedEventMaskBits.load());
            }

            void AddInterestedEvent(const GB_GlobalMouseEventType eventType)
            {
                interestedEventMaskBits.fetch_or(GetGlobalMouseEventMaskBits(GetGlobalMouseEventMask(eventType)));
            }

            void RemoveInterestedEvent(const GB_GlobalMouseEventType eventType)
            {
                const uint32_t eventMaskBits = GetGlobalMouseEventMaskBits(GetGlobalMouseEventMask(eventType));
                interestedEventMaskBits.fetch_and(~eventMaskBits);
            }

            bool IsInterestedIn(const GB_GlobalMouseEventType eventType) const
            {
                const uint32_t eventMaskBits = GetGlobalMouseEventMaskBits(GetGlobalMouseEventMask(eventType));
                return (interestedEventMaskBits.load() & eventMaskBits) != 0;
            }

            void SetUnifiedCallback(const GB_GlobalMouseEventCallback& callback, const GB_GlobalMouseCallbackOptions& callbackOptions)
            {
                {
                    std::lock_guard<std::mutex> callbackLock(callbackMutex);
                    unifiedCallbackEntry.callback = callback;
                    unifiedCallbackEntry.callbackOptions = callbackOptions;
                    if (!callback)
                    {
                        lastUnifiedMoveCallbackTickMs = 0;
                    }
                }

                hasUnifiedCallback.store(static_cast<bool>(callback));
                UpdateMoveEnqueueMinTriggerIntervalMs();
            }

            void ClearUnifiedCallback()
            {
                SetUnifiedCallback(GB_GlobalMouseEventCallback(), GB_GlobalMouseCallbackOptions());
            }

            void SetEventCallback(const GB_GlobalMouseEventType eventType, const GB_GlobalMouseEventCallback& callback, const GB_GlobalMouseCallbackOptions& callbackOptions)
            {
                const size_t eventTypeIndex = GetGlobalMouseEventTypeIndex(eventType);
                if (eventTypeIndex >= globalMouseEventTypeCount)
                {
                    return;
                }

                {
                    std::lock_guard<std::mutex> callbackLock(callbackMutex);
                    perEventCallbackEntries[eventTypeIndex].callback = callback;
                    perEventCallbackEntries[eventTypeIndex].callbackOptions = callbackOptions;
                    if (!callback)
                    {
                        lastPerEventMoveCallbackTickMs[eventTypeIndex] = 0;
                    }
                }

                const uint32_t eventMaskBits = GetGlobalMouseEventMaskBits(GetGlobalMouseEventMask(eventType));
                if (callback)
                {
                    activePerEventCallbackMaskBits.fetch_or(eventMaskBits);
                    AddInterestedEvent(eventType);
                }
                else
                {
                    activePerEventCallbackMaskBits.fetch_and(~eventMaskBits);
                }

                UpdateMoveEnqueueMinTriggerIntervalMs();
            }

            void ClearEventCallback(const GB_GlobalMouseEventType eventType)
            {
                SetEventCallback(eventType, GB_GlobalMouseEventCallback(), GB_GlobalMouseCallbackOptions());
            }

            void ClearAllCallbacks()
            {
                {
                    std::lock_guard<std::mutex> callbackLock(callbackMutex);
                    unifiedCallbackEntry.callback = GB_GlobalMouseEventCallback();
                    unifiedCallbackEntry.callbackOptions = GB_GlobalMouseCallbackOptions();
                    lastUnifiedMoveCallbackTickMs = 0;
                    for (size_t index = 0; index < globalMouseEventTypeCount; index++)
                    {
                        perEventCallbackEntries[index].callback = GB_GlobalMouseEventCallback();
                        perEventCallbackEntries[index].callbackOptions = GB_GlobalMouseCallbackOptions();
                        lastPerEventMoveCallbackTickMs[index] = 0;
                    }
                }

                hasUnifiedCallback.store(false);
                activePerEventCallbackMaskBits.store(0);
                moveEnqueueMinTriggerIntervalMs.store(0);
                lastMoveEnqueueTickMs.store(0);
            }

            bool StartWorker()
            {
                std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
                if (workerThread.joinable())
                {
                    return true;
                }

                stopWorker.store(false);
                lastMoveEnqueueTickMs.store(0);
                try
                {
                    const auto self = shared_from_this();
                    workerThread = std::thread([self]()
                        {
                            self->WorkerThreadMain();
                        });
                }
                catch (...)
                {
                    stopWorker.store(true);
                    return false;
                }

                return true;
            }

            void StopWorker()
            {
                std::unique_lock<std::mutex> lifecycleLock(lifecycleMutex);
                if (!workerThread.joinable())
                {
                    return;
                }

                stopWorker.store(true);
                {
                    std::lock_guard<std::mutex> queueLock(queueMutex);
                    eventQueue.clear();
                }
                queueConditionVariable.notify_all();

                if (workerThread.get_id() == std::this_thread::get_id())
                {
                    workerThread.detach();
                    return;
                }

                std::thread workerThreadToJoin = std::move(workerThread);
                lifecycleLock.unlock();
                workerThreadToJoin.join();
            }

            bool TryEnqueueEvent(const GB_GlobalMouseEvent& mouseEvent)
            {
                if (!isListening.load())
                {
                    return false;
                }

                const uint32_t eventMaskBits = GetGlobalMouseEventMaskBits(GetGlobalMouseEventMask(mouseEvent.eventType));
                if ((interestedEventMaskBits.load() & eventMaskBits) == 0)
                {
                    return false;
                }

                if (!hasUnifiedCallback.load() && (activePerEventCallbackMaskBits.load() & eventMaskBits) == 0)
                {
                    return false;
                }

                if (IsMoveEventType(mouseEvent.eventType))
                {
                    const uint32_t moveMinTriggerIntervalMs = moveEnqueueMinTriggerIntervalMs.load();
                    if (moveMinTriggerIntervalMs > 0)
                    {
                        const uint64_t lastMoveTickMs = lastMoveEnqueueTickMs.load();
                        if (lastMoveTickMs > 0 && mouseEvent.receiveTickCountMs < lastMoveTickMs + moveMinTriggerIntervalMs)
                        {
                            return false;
                        }
                    }
                }

                {
                    std::lock_guard<std::mutex> queueLock(queueMutex);
                    if (stopWorker.load())
                    {
                        return false;
                    }

                    if (IsMoveEventType(mouseEvent.eventType))
                    {
                        if (!eventQueue.empty() && IsMoveEventType(eventQueue.back().eventType))
                        {
                            eventQueue.back() = mouseEvent;
                            lastMoveEnqueueTickMs.store(mouseEvent.receiveTickCountMs);
                            queueConditionVariable.notify_one();
                            return true;
                        }
                    }

                    if (eventQueue.size() >= globalMouseEventQueueCapacity)
                    {
                        if (IsMoveEventType(mouseEvent.eventType))
                        {
                            return false;
                        }

                        const auto moveEventIter = std::find_if(eventQueue.begin(), eventQueue.end(), [](const GB_GlobalMouseEvent& queuedEvent)
                            {
                                return IsMoveEventType(queuedEvent.eventType);
                            });
                        if (moveEventIter != eventQueue.end())
                        {
                            eventQueue.erase(moveEventIter);
                        }
                        else
                        {
                            eventQueue.pop_front();
                        }
                    }

                    eventQueue.push_back(mouseEvent);
                }

                if (IsMoveEventType(mouseEvent.eventType))
                {
                    lastMoveEnqueueTickMs.store(mouseEvent.receiveTickCountMs);
                }

                queueConditionVariable.notify_one();
                return true;
            }

            void SetListening(const bool listening)
            {
                isListening.store(listening);
                if (!listening)
                {
                    lastMoveEnqueueTickMs.store(0);
                }
            }

            bool IsListening() const
            {
                return isListening.load();
            }

        private:
            static bool ShouldInvokeMoveCallback(const uint64_t currentTickMs, const uint32_t minTriggerIntervalMs, uint64_t& lastInvokeTickMs)
            {
                if (minTriggerIntervalMs == 0)
                {
                    lastInvokeTickMs = currentTickMs;
                    return true;
                }

                if (lastInvokeTickMs > 0 && currentTickMs < lastInvokeTickMs + minTriggerIntervalMs)
                {
                    return false;
                }

                lastInvokeTickMs = currentTickMs;
                return true;
            }

            void UpdateMoveEnqueueMinTriggerIntervalMs()
            {
                uint32_t resolvedIntervalMs = 0;
                bool hasMoveCallback = false;

                std::lock_guard<std::mutex> callbackLock(callbackMutex);
                if (unifiedCallbackEntry.callback)
                {
                    resolvedIntervalMs = unifiedCallbackEntry.callbackOptions.mouseMoveMinTriggerIntervalMs;
                    hasMoveCallback = true;
                }

                const size_t moveEventTypeIndex = GetGlobalMouseEventTypeIndex(GB_GlobalMouseEventType::Move);
                if (moveEventTypeIndex < globalMouseEventTypeCount && perEventCallbackEntries[moveEventTypeIndex].callback)
                {
                    const uint32_t moveCallbackIntervalMs = perEventCallbackEntries[moveEventTypeIndex].callbackOptions.mouseMoveMinTriggerIntervalMs;
                    if (!hasMoveCallback || moveCallbackIntervalMs < resolvedIntervalMs)
                    {
                        resolvedIntervalMs = moveCallbackIntervalMs;
                    }
                    hasMoveCallback = true;
                }

                moveEnqueueMinTriggerIntervalMs.store((hasMoveCallback ? resolvedIntervalMs : 0));
            }

            void DispatchOneEvent(const GB_GlobalMouseEvent& mouseEvent)
            {
                GB_GlobalMouseEventCallback unifiedCallback;
                GB_GlobalMouseEventCallback eventCallback;
                const uint32_t eventMaskBits = GetGlobalMouseEventMaskBits(GetGlobalMouseEventMask(mouseEvent.eventType));
                const size_t eventTypeIndex = GetGlobalMouseEventTypeIndex(mouseEvent.eventType);

                {
                    std::lock_guard<std::mutex> callbackLock(callbackMutex);
                    if ((interestedEventMaskBits.load() & eventMaskBits) == 0)
                    {
                        return;
                    }

                    if (unifiedCallbackEntry.callback)
                    {
                        bool shouldInvokeUnifiedCallback = true;
                        if (IsMoveEventType(mouseEvent.eventType))
                        {
                            shouldInvokeUnifiedCallback = ShouldInvokeMoveCallback(mouseEvent.receiveTickCountMs, unifiedCallbackEntry.callbackOptions.mouseMoveMinTriggerIntervalMs, lastUnifiedMoveCallbackTickMs);
                        }

                        if (shouldInvokeUnifiedCallback)
                        {
                            unifiedCallback = unifiedCallbackEntry.callback;
                        }
                    }

                    if (eventTypeIndex < globalMouseEventTypeCount && perEventCallbackEntries[eventTypeIndex].callback)
                    {
                        bool shouldInvokeEventCallback = true;
                        if (IsMoveEventType(mouseEvent.eventType))
                        {
                            shouldInvokeEventCallback = ShouldInvokeMoveCallback(mouseEvent.receiveTickCountMs, perEventCallbackEntries[eventTypeIndex].callbackOptions.mouseMoveMinTriggerIntervalMs, lastPerEventMoveCallbackTickMs[eventTypeIndex]);
                        }

                        if (shouldInvokeEventCallback)
                        {
                            eventCallback = perEventCallbackEntries[eventTypeIndex].callback;
                        }
                    }
                }

                if (unifiedCallback)
                {
                    unifiedCallback(mouseEvent);
                }

                if (eventCallback)
                {
                    eventCallback(mouseEvent);
                }
            }

            void WorkerThreadMain()
            {
                while (true)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    {
                        std::unique_lock<std::mutex> queueLock(queueMutex);
                        queueConditionVariable.wait(queueLock, [this]()
                            {
                                return stopWorker.load() || !eventQueue.empty();
                            });

                        if (stopWorker.load() && eventQueue.empty())
                        {
                            break;
                        }

                        mouseEvent = eventQueue.front();
                        eventQueue.pop_front();
                    }

                    DispatchOneEvent(mouseEvent);
                }
            }

        private:
            std::atomic<bool> isListening = false;
            std::atomic<bool> stopWorker = false;
            std::atomic<uint32_t> interestedEventMaskBits = 0;
            std::atomic<uint32_t> activePerEventCallbackMaskBits = 0;
            std::atomic<uint32_t> moveEnqueueMinTriggerIntervalMs = 0;
            std::atomic<uint64_t> lastMoveEnqueueTickMs = 0;
            std::atomic<bool> hasUnifiedCallback = false;

            mutable std::mutex callbackMutex;
            GlobalMouseCallbackEntry unifiedCallbackEntry;
            std::array<GlobalMouseCallbackEntry, globalMouseEventTypeCount> perEventCallbackEntries = {};
            uint64_t lastUnifiedMoveCallbackTickMs = 0;
            std::array<uint64_t, globalMouseEventTypeCount> lastPerEventMoveCallbackTickMs = {};

            std::mutex queueMutex;
            std::condition_variable queueConditionVariable;
            std::deque<GB_GlobalMouseEvent> eventQueue;

            std::mutex lifecycleMutex;
            std::thread workerThread;
        };

        class GlobalMouseRawInputManager
        {
        public:
            static GlobalMouseRawInputManager& GetInstance()
            {
                static GlobalMouseRawInputManager* globalMouseRawInputManager = new GlobalMouseRawInputManager();
                return *globalMouseRawInputManager;
            }

            bool RegisterListener(const std::shared_ptr<GlobalMouseListenerState>& listenerState, uint64_t& listenerId)
            {
                if (!listenerState)
                {
                    return false;
                }

                std::unique_lock<std::mutex> managerLock(managerMutex);
                if (shuttingDown)
                {
                    return false;
                }

                if (!EnsureMessageThreadStarted(managerLock))
                {
                    return false;
                }

                listenerId = nextListenerId++;
                listeners[listenerId] = listenerState;
                return true;
            }

            void UnregisterListener(const uint64_t listenerId)
            {
                if (listenerId == 0)
                {
                    return;
                }

                std::unique_lock<std::mutex> managerLock(managerMutex);
                listeners.erase(listenerId);
                RemoveExpiredListenersLocked();
                StopMessageThreadIfIdle(managerLock);
            }

            void DispatchEvent(const GB_GlobalMouseEvent& mouseEvent)
            {
                std::vector<std::shared_ptr<GlobalMouseListenerState>> activeListeners;
                {
                    std::lock_guard<std::mutex> managerLock(managerMutex);
                    if (shuttingDown)
                    {
                        return;
                    }

                    activeListeners.reserve(listeners.size());
                    for (auto listenerIter = listeners.begin(); listenerIter != listeners.end(); )
                    {
                        const auto listenerState = listenerIter->second.lock();
                        if (!listenerState)
                        {
                            listenerIter = listeners.erase(listenerIter);
                            continue;
                        }

                        activeListeners.push_back(listenerState);
                        ++listenerIter;
                    }
                }

                for (const auto& listenerState : activeListeners)
                {
                    listenerState->TryEnqueueEvent(mouseEvent);
                }
            }

            void DispatchRawMousePacket(const RAWMOUSE& rawMouse, const uint32_t messageTimeMs)
            {
                const bool hasMove = ((rawMouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) || rawMouse.lLastX != 0 || rawMouse.lLastY != 0;
                if (hasMove)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::Move;
                    DispatchEvent(mouseEvent);
                }

                const USHORT buttonFlags = rawMouse.usButtonFlags;

                if ((buttonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::LeftButtonDown;
                    DispatchEvent(mouseEvent);
                }

                if ((buttonFlags & RI_MOUSE_LEFT_BUTTON_UP) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::LeftButtonUp;
                    DispatchEvent(mouseEvent);
                }

                if ((buttonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::RightButtonDown;
                    DispatchEvent(mouseEvent);
                }

                if ((buttonFlags & RI_MOUSE_RIGHT_BUTTON_UP) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::RightButtonUp;
                    DispatchEvent(mouseEvent);
                }

                if ((buttonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::MiddleButtonDown;
                    DispatchEvent(mouseEvent);
                }

                if ((buttonFlags & RI_MOUSE_MIDDLE_BUTTON_UP) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::MiddleButtonUp;
                    DispatchEvent(mouseEvent);
                }

                if ((buttonFlags & RI_MOUSE_BUTTON_4_DOWN) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::XButtonDown;
                    mouseEvent.xButtonType = GB_GlobalMouseXButtonType::XButton1;
                    DispatchEvent(mouseEvent);
                }

                if ((buttonFlags & RI_MOUSE_BUTTON_4_UP) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::XButtonUp;
                    mouseEvent.xButtonType = GB_GlobalMouseXButtonType::XButton1;
                    DispatchEvent(mouseEvent);
                }

                if ((buttonFlags & RI_MOUSE_BUTTON_5_DOWN) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::XButtonDown;
                    mouseEvent.xButtonType = GB_GlobalMouseXButtonType::XButton2;
                    DispatchEvent(mouseEvent);
                }

                if ((buttonFlags & RI_MOUSE_BUTTON_5_UP) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::XButtonUp;
                    mouseEvent.xButtonType = GB_GlobalMouseXButtonType::XButton2;
                    DispatchEvent(mouseEvent);
                }

                if ((buttonFlags & RI_MOUSE_WHEEL) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::VerticalWheel;
                    mouseEvent.wheelDelta = static_cast<short>(rawMouse.usButtonData);
                    DispatchEvent(mouseEvent);
                }

                if ((buttonFlags & RI_MOUSE_HWHEEL) != 0)
                {
                    GB_GlobalMouseEvent mouseEvent;
                    FillGlobalMouseEventCommonFields(mouseEvent, messageTimeMs);
                    mouseEvent.eventType = GB_GlobalMouseEventType::HorizontalWheel;
                    mouseEvent.wheelDelta = static_cast<short>(rawMouse.usButtonData);
                    DispatchEvent(mouseEvent);
                }
            }

        private:
            static constexpr UINT shutdownWindowMessage = WM_CLOSE;

            static const wchar_t* GetRawInputWindowClassName()
            {
                return L"GlobalBase_GB_Mouse_RawInputWindow";
            }

            GlobalMouseRawInputManager() = default;
            GlobalMouseRawInputManager(const GlobalMouseRawInputManager&) = delete;
            GlobalMouseRawInputManager& operator=(const GlobalMouseRawInputManager&) = delete;

            static HINSTANCE GetCurrentModuleHandle()
            {
                HMODULE moduleHandle = nullptr;
                const BOOL succeeded = ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&GlobalMouseRawInputManager::StaticWindowProc), &moduleHandle);
                return succeeded != FALSE ? moduleHandle : ::GetModuleHandleW(nullptr);
            }

            static LRESULT CALLBACK StaticWindowProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
            {
                return GetInstance().WindowProc(windowHandle, message, wParam, lParam);
            }

            LRESULT WindowProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
            {
                switch (message)
                {
                case WM_INPUT:
                {
                    HandleRawInputMessage(windowHandle, wParam, lParam);
                    if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT)
                    {
                        (void)::DefWindowProcW(windowHandle, message, wParam, lParam);
                    }
                    return 0;
                }
                case WM_CLOSE:
                    (void)::DestroyWindow(windowHandle);
                    return 0;
                case WM_DESTROY:
                {
                    {
                        std::lock_guard<std::mutex> managerLock(managerMutex);
                        if (rawInputWindowHandle == windowHandle)
                        {
                            rawInputWindowHandle = nullptr;
                        }
                    }
                    ::PostQuitMessage(0);
                    return 0;
                }
                default:
                    return ::DefWindowProcW(windowHandle, message, wParam, lParam);
                }
            }

            void HandleRawInputMessage(HWND windowHandle, WPARAM wParam, LPARAM lParam)
            {
                (void)windowHandle;
                (void)wParam;

                UINT rawInputSize = 0;
                if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &rawInputSize, sizeof(RAWINPUTHEADER)) != 0 || rawInputSize < sizeof(RAWINPUTHEADER))
                {
                    return;
                }

                std::unique_ptr<uint8_t[]> rawInputBuffer(new (std::nothrow) uint8_t[rawInputSize]);
                if (!rawInputBuffer)
                {
                    return;
                }

                UINT copiedByteCount = rawInputSize;
                if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, rawInputBuffer.get(), &copiedByteCount, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) || copiedByteCount < sizeof(RAWINPUTHEADER))
                {
                    return;
                }

                const RAWINPUT* rawInput = reinterpret_cast<const RAWINPUT*>(rawInputBuffer.get());
                if (rawInput == nullptr || rawInput->header.dwType != RIM_TYPEMOUSE)
                {
                    return;
                }

                const uint32_t messageTimeMs = static_cast<uint32_t>(::GetMessageTime());
                DispatchRawMousePacket(rawInput->data.mouse, messageTimeMs);
            }

            static bool RegisterMouseRawInput(const HWND windowHandle)
            {
                RAWINPUTDEVICE rawInputDevice = {};
                rawInputDevice.usUsagePage = 0x01;
                rawInputDevice.usUsage = 0x02;
                rawInputDevice.dwFlags = RIDEV_INPUTSINK;
                rawInputDevice.hwndTarget = windowHandle;
                return ::RegisterRawInputDevices(&rawInputDevice, 1, sizeof(rawInputDevice)) != FALSE;
            }

            bool EnsureMessageThreadStarted(std::unique_lock<std::mutex>& managerLock)
            {
                while (messageThreadStopInProgress)
                {
                    messageThreadReadyConditionVariable.wait(managerLock);
                }

                if (messageThread.joinable())
                {
                    while (!messageThreadStartupResolved)
                    {
                        messageThreadReadyConditionVariable.wait(managerLock);
                    }
                    return rawInputRegisteredSuccessfully;
                }

                messageThreadStartupResolved = false;
                rawInputRegisteredSuccessfully = false;
                rawInputThreadId = 0;
                rawInputWindowHandle = nullptr;
                try
                {
                    messageThread = std::thread(&GlobalMouseRawInputManager::MessageThreadMain, this);
                }
                catch (...)
                {
                    return false;
                }

                while (!messageThreadStartupResolved)
                {
                    messageThreadReadyConditionVariable.wait(managerLock);
                }

                if (!rawInputRegisteredSuccessfully)
                {
                    std::thread messageThreadToJoin = std::move(messageThread);
                    managerLock.unlock();
                    if (messageThreadToJoin.joinable())
                    {
                        messageThreadToJoin.join();
                    }
                    managerLock.lock();
                    rawInputWindowHandle = nullptr;
                    rawInputThreadId = 0;
                    messageThreadStartupResolved = false;
                    rawInputRegisteredSuccessfully = false;
                }

                return rawInputRegisteredSuccessfully;
            }

            void MessageThreadMain()
            {
                MSG message = {};
                (void)::PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

                const DWORD currentThreadId = ::GetCurrentThreadId();
                const HINSTANCE moduleHandle = GetCurrentModuleHandle();

                WNDCLASSEXW windowClass = {};
                windowClass.cbSize = sizeof(windowClass);
                windowClass.lpfnWndProc = &GlobalMouseRawInputManager::StaticWindowProc;
                windowClass.hInstance = moduleHandle;
                windowClass.lpszClassName = GetRawInputWindowClassName();
                const ATOM classAtom = ::RegisterClassExW(&windowClass);
                const DWORD registerClassError = (classAtom == 0 ? ::GetLastError() : ERROR_SUCCESS);

                HWND windowHandle = nullptr;
                bool registerSucceeded = false;
                if (classAtom != 0 || registerClassError == ERROR_CLASS_ALREADY_EXISTS)
                {
                    windowHandle = ::CreateWindowExW(0, GetRawInputWindowClassName(), L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, moduleHandle, nullptr);
                    if (windowHandle != nullptr)
                    {
                        registerSucceeded = RegisterMouseRawInput(windowHandle);
                    }
                }

                {
                    std::lock_guard<std::mutex> managerLock(managerMutex);
                    rawInputThreadId = currentThreadId;
                    rawInputWindowHandle = windowHandle;
                    rawInputRegisteredSuccessfully = registerSucceeded;
                    messageThreadStartupResolved = true;
                }
                messageThreadReadyConditionVariable.notify_all();

                if (!registerSucceeded)
                {
                    if (windowHandle != nullptr)
                    {
                        (void)::DestroyWindow(windowHandle);
                    }
                    if (classAtom != 0)
                    {
                        (void)::UnregisterClassW(GetRawInputWindowClassName(), moduleHandle);
                    }
                    return;
                }

                while (::GetMessageW(&message, nullptr, 0, 0) > 0)
                {
                    (void)::TranslateMessage(&message);
                    (void)::DispatchMessageW(&message);
                }

                if (windowHandle != nullptr && ::IsWindow(windowHandle) != FALSE)
                {
                    (void)::DestroyWindow(windowHandle);
                }
                (void)::UnregisterClassW(GetRawInputWindowClassName(), moduleHandle);

                std::lock_guard<std::mutex> managerLock(managerMutex);
                if (rawInputWindowHandle == windowHandle)
                {
                    rawInputWindowHandle = nullptr;
                }
                if (rawInputThreadId == currentThreadId)
                {
                    rawInputThreadId = 0;
                }
            }

            void RemoveExpiredListenersLocked()
            {
                for (auto listenerIter = listeners.begin(); listenerIter != listeners.end(); )
                {
                    if (listenerIter->second.expired())
                    {
                        listenerIter = listeners.erase(listenerIter);
                    }
                    else
                    {
                        ++listenerIter;
                    }
                }
            }

            void StopMessageThreadIfIdle(std::unique_lock<std::mutex>& managerLock)
            {
                if (!listeners.empty() || !messageThread.joinable())
                {
                    return;
                }

                messageThreadStopInProgress = true;
                const HWND windowHandleToClose = rawInputWindowHandle;
                const DWORD threadIdToQuit = rawInputThreadId;
                std::thread messageThreadToJoin = std::move(messageThread);
                managerLock.unlock();
                if (windowHandleToClose != nullptr)
                {
                    (void)::PostMessageW(windowHandleToClose, shutdownWindowMessage, 0, 0);
                }
                else if (threadIdToQuit != 0)
                {
                    (void)::PostThreadMessageW(threadIdToQuit, WM_QUIT, 0, 0);
                }
                if (messageThreadToJoin.joinable())
                {
                    messageThreadToJoin.join();
                }
                managerLock.lock();
                rawInputWindowHandle = nullptr;
                rawInputThreadId = 0;
                messageThreadStartupResolved = false;
                rawInputRegisteredSuccessfully = false;
                messageThreadStopInProgress = false;
                messageThreadReadyConditionVariable.notify_all();
            }

        private:
            std::mutex managerMutex;
            std::condition_variable messageThreadReadyConditionVariable;
            std::unordered_map<uint64_t, std::weak_ptr<GlobalMouseListenerState>> listeners;
            std::thread messageThread;
            HWND rawInputWindowHandle = nullptr;
            DWORD rawInputThreadId = 0;
            uint64_t nextListenerId = 1;
            bool messageThreadStartupResolved = false;
            bool rawInputRegisteredSuccessfully = false;
            bool messageThreadStopInProgress = false;
            bool shuttingDown = false;
        };
    }
}
#endif

#if defined(_WIN32)
class GB_GlobalMouseListener::Impl
{
public:
    Impl() : listenerState(std::make_shared<internal::GlobalMouseListenerState>())
    {
    }

    void SetInterestedEvents(const GB_GlobalMouseEventMask eventMask)
    {
        listenerState->SetInterestedEvents(eventMask);
    }

    GB_GlobalMouseEventMask GetInterestedEvents() const
    {
        return listenerState->GetInterestedEvents();
    }

    void AddInterestedEvent(const GB_GlobalMouseEventType eventType)
    {
        listenerState->AddInterestedEvent(eventType);
    }

    void RemoveInterestedEvent(const GB_GlobalMouseEventType eventType)
    {
        listenerState->RemoveInterestedEvent(eventType);
    }

    bool IsInterestedIn(const GB_GlobalMouseEventType eventType) const
    {
        return listenerState->IsInterestedIn(eventType);
    }

    void SetUnifiedCallback(const GB_GlobalMouseEventCallback& callback, const GB_GlobalMouseCallbackOptions& callbackOptions)
    {
        listenerState->SetUnifiedCallback(callback, callbackOptions);
    }

    void ClearUnifiedCallback()
    {
        listenerState->ClearUnifiedCallback();
    }

    void SetEventCallback(const GB_GlobalMouseEventType eventType, const GB_GlobalMouseEventCallback& callback, const GB_GlobalMouseCallbackOptions& callbackOptions)
    {
        listenerState->SetEventCallback(eventType, callback, callbackOptions);
    }

    void ClearEventCallback(const GB_GlobalMouseEventType eventType)
    {
        listenerState->ClearEventCallback(eventType);
    }

    void ClearAllCallbacks()
    {
        listenerState->ClearAllCallbacks();
    }

    bool Start()
    {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
        if (listenerState->IsListening())
        {
            return true;
        }

        if (!listenerState->StartWorker())
        {
            return false;
        }

        listenerState->SetListening(true);
        if (internal::GlobalMouseRawInputManager::GetInstance().RegisterListener(listenerState, listenerId))
        {
            return true;
        }

        listenerState->SetListening(false);
        listenerState->StopWorker();
        listenerId = 0;
        return false;
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
        if (!listenerState->IsListening() && listenerId == 0)
        {
            listenerState->StopWorker();
            return;
        }

        listenerState->SetListening(false);
        const uint64_t listenerIdToRemove = listenerId;
        listenerId = 0;
        internal::GlobalMouseRawInputManager::GetInstance().UnregisterListener(listenerIdToRemove);
        listenerState->StopWorker();
    }

    bool IsListening() const
    {
        return listenerState->IsListening();
    }

private:
    std::shared_ptr<internal::GlobalMouseListenerState> listenerState;
    std::mutex lifecycleMutex;
    uint64_t listenerId = 0;
};
#else
class GB_GlobalMouseListener::Impl
{
public:
    void SetInterestedEvents(const GB_GlobalMouseEventMask eventMask)
    {
        (void)eventMask;
    }

    GB_GlobalMouseEventMask GetInterestedEvents() const
    {
        return GB_GlobalMouseEventMask::None;
    }

    void AddInterestedEvent(const GB_GlobalMouseEventType eventType)
    {
        (void)eventType;
    }

    void RemoveInterestedEvent(const GB_GlobalMouseEventType eventType)
    {
        (void)eventType;
    }

    bool IsInterestedIn(const GB_GlobalMouseEventType eventType) const
    {
        (void)eventType;
        return false;
    }

    void SetUnifiedCallback(const GB_GlobalMouseEventCallback& callback, const GB_GlobalMouseCallbackOptions& callbackOptions)
    {
        (void)callback;
        (void)callbackOptions;
    }

    void ClearUnifiedCallback()
    {
    }

    void SetEventCallback(const GB_GlobalMouseEventType eventType, const GB_GlobalMouseEventCallback& callback, const GB_GlobalMouseCallbackOptions& callbackOptions)
    {
        (void)eventType;
        (void)callback;
        (void)callbackOptions;
    }

    void ClearEventCallback(const GB_GlobalMouseEventType eventType)
    {
        (void)eventType;
    }

    void ClearAllCallbacks()
    {
    }

    bool Start()
    {
        return false;
    }

    void Stop()
    {
    }

    bool IsListening() const
    {
        return false;
    }
};
#endif

GB_GlobalMouseListener::GB_GlobalMouseListener() : impl_(new Impl())
{
}

GB_GlobalMouseListener::~GB_GlobalMouseListener()
{
    if (impl_ != nullptr)
    {
        impl_->Stop();
        delete impl_;
        impl_ = nullptr;
    }
}

GB_GlobalMouseListener::GB_GlobalMouseListener(GB_GlobalMouseListener&& other) noexcept : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

GB_GlobalMouseListener& GB_GlobalMouseListener::operator=(GB_GlobalMouseListener&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    if (impl_ != nullptr)
    {
        impl_->Stop();
        delete impl_;
    }

    impl_ = other.impl_;
    other.impl_ = nullptr;
    return *this;
}

bool GB_GlobalMouseListener::IsSupported()
{
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

void GB_GlobalMouseListener::SetInterestedEvents(const GB_GlobalMouseEventMask eventMask)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->SetInterestedEvents(eventMask);
}

GB_GlobalMouseEventMask GB_GlobalMouseListener::GetInterestedEvents() const
{
    if (impl_ == nullptr)
    {
        return GB_GlobalMouseEventMask::None;
    }

    return impl_->GetInterestedEvents();
}

void GB_GlobalMouseListener::ClearInterestedEvents()
{
    SetInterestedEvents(GB_GlobalMouseEventMask::None);
}

void GB_GlobalMouseListener::AddInterestedEvent(const GB_GlobalMouseEventType eventType)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->AddInterestedEvent(eventType);
}

void GB_GlobalMouseListener::RemoveInterestedEvent(const GB_GlobalMouseEventType eventType)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->RemoveInterestedEvent(eventType);
}

bool GB_GlobalMouseListener::IsInterestedIn(const GB_GlobalMouseEventType eventType) const
{
    if (impl_ == nullptr)
    {
        return false;
    }

    return impl_->IsInterestedIn(eventType);
}

void GB_GlobalMouseListener::SetUnifiedCallback(const GB_GlobalMouseEventCallback& callback, const GB_GlobalMouseCallbackOptions& callbackOptions)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->SetUnifiedCallback(callback, callbackOptions);
}

void GB_GlobalMouseListener::ClearUnifiedCallback()
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->ClearUnifiedCallback();
}

void GB_GlobalMouseListener::SetEventCallback(const GB_GlobalMouseEventType eventType, const GB_GlobalMouseEventCallback& callback, const GB_GlobalMouseCallbackOptions& callbackOptions)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->SetEventCallback(eventType, callback, callbackOptions);
}

void GB_GlobalMouseListener::ClearEventCallback(const GB_GlobalMouseEventType eventType)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->ClearEventCallback(eventType);
}

void GB_GlobalMouseListener::ClearAllCallbacks()
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->ClearAllCallbacks();
}

bool GB_GlobalMouseListener::Start()
{
    if (impl_ == nullptr)
    {
        return false;
    }

    return impl_->Start();
}

void GB_GlobalMouseListener::Stop()
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->Stop();
}

bool GB_GlobalMouseListener::IsListening() const
{
    if (impl_ == nullptr)
    {
        return false;
    }

    return impl_->IsListening();
}
