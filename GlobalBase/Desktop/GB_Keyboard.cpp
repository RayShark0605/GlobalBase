#include "GB_Keyboard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <random>
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

    static uint32_t GetGlobalKeyboardEventMaskBits(const GB_GlobalKeyboardEventMask eventMask)
    {
        return static_cast<uint32_t>(eventMask) & static_cast<uint32_t>(GB_GlobalKeyboardEventMask::All);
    }

    static GB_GlobalKeyboardEventMask GetGlobalKeyboardEventMask(const GB_GlobalKeyboardEventType eventType)
    {
        switch (eventType)
        {
        case GB_GlobalKeyboardEventType::KeyDown:
            return GB_GlobalKeyboardEventMask::KeyDown;
        case GB_GlobalKeyboardEventType::KeyUp:
            return GB_GlobalKeyboardEventMask::KeyUp;
        }

        return GB_GlobalKeyboardEventMask::None;
    }

    static size_t GetGlobalKeyboardEventTypeIndex(const GB_GlobalKeyboardEventType eventType)
    {
        switch (eventType)
        {
        case GB_GlobalKeyboardEventType::KeyDown:
            return 0;
        case GB_GlobalKeyboardEventType::KeyUp:
            return 1;
        }

        return 2;
    }
}

#if defined(_WIN32)
namespace
{
    namespace internal
    {
        static constexpr size_t globalKeyboardEventTypeCount = 2;
        static constexpr size_t globalKeyboardEventQueueCapacity = 512;
        static constexpr UINT shutdownWindowMessage = WM_APP + 151;

        static uint64_t GetSteadyTickCountMs()
        {
            const auto now = std::chrono::steady_clock::now();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        }

        static HMODULE GetCurrentModuleHandle()
        {
            HMODULE moduleHandle = nullptr;
            const BOOL getModuleSucceeded = ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&GetCurrentModuleHandle), &moduleHandle);
            if (getModuleSucceeded != FALSE && moduleHandle != nullptr)
            {
                return moduleHandle;
            }

            return ::GetModuleHandleW(nullptr);
        }

        static uint16_t ToVirtualKeyCode(const GB_VirtualKey virtualKey)
        {
            return static_cast<uint16_t>(virtualKey);
        }

        static bool IsValidVirtualKeyCode(const uint16_t virtualKeyCode)
        {
            return virtualKeyCode > 0 && virtualKeyCode < 256;
        }

        static bool IsKeyPressedByCode(const uint16_t virtualKeyCode)
        {
            if (!IsValidVirtualKeyCode(virtualKeyCode))
            {
                return false;
            }

            return (::GetAsyncKeyState(static_cast<int>(virtualKeyCode)) & 0x8000) != 0;
        }

        static bool IsAnyKeyPressed(const GB_VirtualKey firstKey, const GB_VirtualKey secondKey)
        {
            return IsKeyPressedByCode(ToVirtualKeyCode(firstKey)) || IsKeyPressedByCode(ToVirtualKeyCode(secondKey));
        }

        static bool IsShiftPressed()
        {
            return IsKeyPressedByCode(ToVirtualKeyCode(GB_VirtualKey::Shift)) || IsAnyKeyPressed(GB_VirtualKey::LeftShift, GB_VirtualKey::RightShift);
        }

        static bool IsCtrlPressed()
        {
            return IsKeyPressedByCode(ToVirtualKeyCode(GB_VirtualKey::Control)) || IsAnyKeyPressed(GB_VirtualKey::LeftControl, GB_VirtualKey::RightControl);
        }

        static bool IsAltPressed()
        {
            return IsKeyPressedByCode(ToVirtualKeyCode(GB_VirtualKey::Alt)) || IsAnyKeyPressed(GB_VirtualKey::LeftAlt, GB_VirtualKey::RightAlt);
        }

        static bool IsWinPressed()
        {
            return IsAnyKeyPressed(GB_VirtualKey::LeftWin, GB_VirtualKey::RightWin);
        }

        static HKL GetForegroundKeyboardLayout()
        {
            const HWND foregroundWindowHandle = ::GetForegroundWindow();
            if (foregroundWindowHandle != nullptr)
            {
                const DWORD foregroundThreadId = ::GetWindowThreadProcessId(foregroundWindowHandle, nullptr);
                if (foregroundThreadId != 0)
                {
                    const HKL keyboardLayout = ::GetKeyboardLayout(foregroundThreadId);
                    if (keyboardLayout != nullptr)
                    {
                        return keyboardLayout;
                    }
                }
            }

            return ::GetKeyboardLayout(0);
        }

        static UINT MapVirtualKeyToScanCodeEx(const uint16_t virtualKeyCode)
        {
            return ::MapVirtualKeyExW(static_cast<UINT>(virtualKeyCode), MAPVK_VK_TO_VSC_EX, GetForegroundKeyboardLayout());
        }

        static bool IsKnownExtendedVirtualKey(const uint16_t virtualKeyCode)
        {
            switch (static_cast<GB_VirtualKey>(virtualKeyCode))
            {
            case GB_VirtualKey::RightControl:
            case GB_VirtualKey::RightAlt:
            case GB_VirtualKey::LeftWin:
            case GB_VirtualKey::RightWin:
            case GB_VirtualKey::Apps:
            case GB_VirtualKey::Insert:
            case GB_VirtualKey::Delete:
            case GB_VirtualKey::Home:
            case GB_VirtualKey::End:
            case GB_VirtualKey::PageUp:
            case GB_VirtualKey::PageDown:
            case GB_VirtualKey::Left:
            case GB_VirtualKey::Up:
            case GB_VirtualKey::Right:
            case GB_VirtualKey::Down:
            case GB_VirtualKey::NumPadDivide:
            case GB_VirtualKey::NumLock:
            case GB_VirtualKey::PrintScreen:
            case GB_VirtualKey::Pause:
            case GB_VirtualKey::BrowserBack:
            case GB_VirtualKey::BrowserForward:
            case GB_VirtualKey::BrowserRefresh:
            case GB_VirtualKey::BrowserStop:
            case GB_VirtualKey::BrowserSearch:
            case GB_VirtualKey::BrowserFavorites:
            case GB_VirtualKey::BrowserHome:
            case GB_VirtualKey::VolumeMute:
            case GB_VirtualKey::VolumeDown:
            case GB_VirtualKey::VolumeUp:
            case GB_VirtualKey::MediaNextTrack:
            case GB_VirtualKey::MediaPreviousTrack:
            case GB_VirtualKey::MediaStop:
            case GB_VirtualKey::MediaPlayPause:
            case GB_VirtualKey::LaunchMail:
            case GB_VirtualKey::LaunchMediaSelect:
            case GB_VirtualKey::LaunchApp1:
            case GB_VirtualKey::LaunchApp2:
                return true;
            default:
                return false;
            }
        }

        static bool IsExtendedScanCodeEx(const UINT scanCodeEx)
        {
            const UINT scanCodePrefix = scanCodeEx & 0xFF00u;
            return scanCodePrefix == 0xE000u || scanCodePrefix == 0xE100u;
        }

        static bool IsExtendedVirtualKey(const uint16_t virtualKeyCode)
        {
            const UINT scanCodeEx = MapVirtualKeyToScanCodeEx(virtualKeyCode);
            return IsExtendedScanCodeEx(scanCodeEx) || IsKnownExtendedVirtualKey(virtualKeyCode);
        }

        static INPUT MakeKeyboardInputByVirtualKey(const uint16_t virtualKeyCode, const bool isKeyUp)
        {
            INPUT input = {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = static_cast<WORD>(virtualKeyCode);
            input.ki.wScan = static_cast<WORD>(MapVirtualKeyToScanCodeEx(virtualKeyCode) & 0xFFu);
            input.ki.dwFlags = 0;
            if (isKeyUp)
            {
                input.ki.dwFlags |= KEYEVENTF_KEYUP;
            }
            if (IsExtendedVirtualKey(virtualKeyCode))
            {
                input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
            return input;
        }

        static INPUT MakeKeyboardInputByScanCode(const uint16_t scanCode, const bool isExtendedKey, const bool isKeyUp)
        {
            INPUT input = {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = 0;
            input.ki.wScan = static_cast<WORD>(scanCode & 0xFFu);
            input.ki.dwFlags = KEYEVENTF_SCANCODE;
            if (isExtendedKey)
            {
                input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
            if (isKeyUp)
            {
                input.ki.dwFlags |= KEYEVENTF_KEYUP;
            }
            return input;
        }

        static INPUT MakeKeyboardInputByVirtualKeyScanCode(const uint16_t virtualKeyCode, const bool isKeyUp)
        {
            const UINT scanCodeEx = MapVirtualKeyToScanCodeEx(virtualKeyCode);
            if (scanCodeEx == 0)
            {
                return MakeKeyboardInputByVirtualKey(virtualKeyCode, isKeyUp);
            }

            const bool isExtendedKey = IsExtendedScanCodeEx(scanCodeEx) || IsKnownExtendedVirtualKey(virtualKeyCode);
            return MakeKeyboardInputByScanCode(static_cast<uint16_t>(scanCodeEx & 0xFFu), isExtendedKey, isKeyUp);
        }

        static INPUT MakeKeyboardInputByUnicode(const wchar_t unicodeChar, const bool isKeyUp)
        {
            INPUT input = {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = 0;
            input.ki.wScan = static_cast<WORD>(unicodeChar);
            input.ki.dwFlags = KEYEVENTF_UNICODE;
            if (isKeyUp)
            {
                input.ki.dwFlags |= KEYEVENTF_KEYUP;
            }
            return input;
        }

        static bool TrySendInputEvents(const std::vector<INPUT>& inputs)
        {
            if (inputs.empty())
            {
                return true;
            }

            const UINT inputCount = static_cast<UINT>(inputs.size());
            const UINT sentInputCount = ::SendInput(inputCount, const_cast<INPUT*>(inputs.data()), sizeof(INPUT));
            return sentInputCount == inputCount;
        }

        static bool TrySendInputEventsWithRetry(const std::vector<INPUT>& inputs, const int maxAttemptCount = 2, const int retryDelayMs = 1)
        {
            const int attemptCount = std::max(maxAttemptCount, 1);
            for (int attemptIndex = 0; attemptIndex < attemptCount; attemptIndex++)
            {
                if (TrySendInputEvents(inputs))
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

        static INPUT MakeKeyboardInputByMode(const GB_VirtualKey virtualKey, GB_KeyboardInputMode inputMode, const bool isKeyUp)
        {
            if (inputMode == GB_KeyboardInputMode::Unicode)
            {
                inputMode = GB_KeyboardInputMode::VirtualKey;
            }

            const uint16_t virtualKeyCode = ToVirtualKeyCode(virtualKey);
            if (inputMode == GB_KeyboardInputMode::ScanCode)
            {
                return MakeKeyboardInputByVirtualKeyScanCode(virtualKeyCode, isKeyUp);
            }

            return MakeKeyboardInputByVirtualKey(virtualKeyCode, isKeyUp);
        }

        static bool TrySendKeyByMode(const GB_VirtualKey virtualKey, const GB_KeyboardInputMode inputMode, const bool isKeyUp)
        {
            const uint16_t virtualKeyCode = ToVirtualKeyCode(virtualKey);
            if (!IsValidVirtualKeyCode(virtualKeyCode))
            {
                return false;
            }

            std::vector<INPUT> inputs;
            inputs.push_back(MakeKeyboardInputByMode(virtualKey, inputMode, isKeyUp));
            return TrySendInputEventsWithRetry(inputs);
        }

        static bool IsSameModifierFamily(const GB_VirtualKey leftKey, const GB_VirtualKey rightKey)
        {
            const auto getModifierFamily = [](const GB_VirtualKey virtualKey) -> int
                {
                    switch (virtualKey)
                    {
                    case GB_VirtualKey::Shift:
                    case GB_VirtualKey::LeftShift:
                    case GB_VirtualKey::RightShift:
                        return 1;
                    case GB_VirtualKey::Control:
                    case GB_VirtualKey::LeftControl:
                    case GB_VirtualKey::RightControl:
                        return 2;
                    case GB_VirtualKey::Alt:
                    case GB_VirtualKey::LeftAlt:
                    case GB_VirtualKey::RightAlt:
                        return 3;
                    case GB_VirtualKey::LeftWin:
                    case GB_VirtualKey::RightWin:
                        return 4;
                    default:
                        return 0;
                    }
                };

            const int leftFamily = getModifierFamily(leftKey);
            const int rightFamily = getModifierFamily(rightKey);
            return leftFamily != 0 && leftFamily == rightFamily;
        }

        static bool ContainsModifierFamily(const std::vector<GB_VirtualKey>& virtualKeys, const GB_VirtualKey modifierKey)
        {
            for (const GB_VirtualKey virtualKey : virtualKeys)
            {
                if (IsSameModifierFamily(virtualKey, modifierKey))
                {
                    return true;
                }
            }

            return false;
        }

        static std::vector<GB_VirtualKey> GetCurrentlyPressedModifierKeysNotInCombination(const std::vector<GB_VirtualKey>& virtualKeys)
        {
            const std::array<GB_VirtualKey, 8> modifierKeys =
            {
                GB_VirtualKey::LeftShift,
                GB_VirtualKey::RightShift,
                GB_VirtualKey::LeftControl,
                GB_VirtualKey::RightControl,
                GB_VirtualKey::LeftAlt,
                GB_VirtualKey::RightAlt,
                GB_VirtualKey::LeftWin,
                GB_VirtualKey::RightWin
            };

            std::vector<GB_VirtualKey> modifierKeysToRelease;
            for (const GB_VirtualKey modifierKey : modifierKeys)
            {
                if (!IsKeyPressedByCode(ToVirtualKeyCode(modifierKey)))
                {
                    continue;
                }

                if (ContainsModifierFamily(virtualKeys, modifierKey))
                {
                    continue;
                }

                modifierKeysToRelease.push_back(modifierKey);
            }

            return modifierKeysToRelease;
        }

        static void AppendKeyInputs(std::vector<INPUT>& inputs, const GB_VirtualKey virtualKey, const GB_KeyboardInputMode inputMode, const bool isKeyUp)
        {
            inputs.push_back(MakeKeyboardInputByMode(virtualKey, inputMode, isKeyUp));
        }

        static bool TryInputUnicodeCharacter(const wchar_t unicodeChar, const int downUpIntervalMs)
        {
            std::vector<INPUT> downInputs;
            downInputs.push_back(MakeKeyboardInputByUnicode(unicodeChar, false));
            if (!TrySendInputEventsWithRetry(downInputs))
            {
                return false;
            }

            SleepForDelayMs(downUpIntervalMs);

            std::vector<INPUT> upInputs;
            upInputs.push_back(MakeKeyboardInputByUnicode(unicodeChar, true));
            return TrySendInputEventsWithRetry(upInputs);
        }

        static int ResolveCharacterIntervalMs(const GB_TextInputOptions& options)
        {
            if (options.wordsPerMinute <= 0.0)
            {
                return 0;
            }

            const double baseIntervalMs = 60000.0 / (options.wordsPerMinute * 5.0);
            const double safeRandomRatio = std::max(0.0, options.randomIntervalRatio);
            double randomFactor = 1.0;
            if (safeRandomRatio > 0.0)
            {
                static thread_local std::mt19937 randomGenerator(static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
                std::uniform_real_distribution<double> randomDistribution(1.0 - safeRandomRatio, 1.0 + safeRandomRatio);
                randomFactor = randomDistribution(randomGenerator);
            }

            int intervalMs = static_cast<int>(baseIntervalMs * randomFactor + 0.5);
            intervalMs = std::max(intervalMs, std::max(0, options.minCharacterIntervalMs));
            if (options.maxCharacterIntervalMs >= 0)
            {
                intervalMs = std::min(intervalMs, options.maxCharacterIntervalMs);
            }
            return intervalMs;
        }

        static bool TryPressCharacterByKeyboardLayout(const wchar_t unicodeChar, const GB_TextInputOptions& options)
        {
            if (unicodeChar == L'\r' || unicodeChar == L'\n')
            {
                return GB_Keyboard::ClickKey(GB_VirtualKey::Enter, options.keyDownUpIntervalMs, options.inputMode == GB_KeyboardInputMode::ScanCode ? GB_KeyboardInputMode::ScanCode : GB_KeyboardInputMode::VirtualKey);
            }
            if (unicodeChar == L'\t')
            {
                return GB_Keyboard::ClickKey(GB_VirtualKey::Tab, options.keyDownUpIntervalMs, options.inputMode == GB_KeyboardInputMode::ScanCode ? GB_KeyboardInputMode::ScanCode : GB_KeyboardInputMode::VirtualKey);
            }

            const HKL keyboardLayout = GetForegroundKeyboardLayout();
            const SHORT virtualKeyAndShiftState = ::VkKeyScanExW(unicodeChar, keyboardLayout);
            if (virtualKeyAndShiftState == -1)
            {
                return false;
            }

            const uint8_t virtualKeyCode = static_cast<uint8_t>(virtualKeyAndShiftState & 0xFF);
            const uint8_t shiftState = static_cast<uint8_t>((virtualKeyAndShiftState >> 8) & 0xFF);
            if (!IsValidVirtualKeyCode(virtualKeyCode))
            {
                return false;
            }

            std::vector<GB_VirtualKey> virtualKeys;
            if ((shiftState & 1u) != 0)
            {
                virtualKeys.push_back(GB_VirtualKey::Shift);
            }
            if ((shiftState & 2u) != 0)
            {
                virtualKeys.push_back(GB_VirtualKey::Control);
            }
            if ((shiftState & 4u) != 0)
            {
                virtualKeys.push_back(GB_VirtualKey::Alt);
            }
            virtualKeys.push_back(static_cast<GB_VirtualKey>(virtualKeyCode));

            GB_KeyCombinationOptions combinationOptions;
            combinationOptions.inputMode = options.inputMode == GB_KeyboardInputMode::ScanCode ? GB_KeyboardInputMode::ScanCode : GB_KeyboardInputMode::VirtualKey;
            combinationOptions.keyDownIntervalMs = 0;
            combinationOptions.holdIntervalMs = options.keyDownUpIntervalMs;
            combinationOptions.keyUpIntervalMs = 0;
            combinationOptions.temporarilyReleaseActiveModifierKeys = false;
            combinationOptions.restoreReleasedModifierKeys = false;
            return GB_Keyboard::PressKeyCombination(virtualKeys, combinationOptions);
        }

        static bool TryInputTextByMode(const std::wstring& text, const GB_TextInputOptions& options)
        {
            const int downUpIntervalMs = ClampNonNegativeDelayMs(options.keyDownUpIntervalMs);
            for (size_t charIndex = 0; charIndex < text.size(); charIndex++)
            {
                const wchar_t unicodeChar = text[charIndex];
                bool succeeded = false;
                if (options.inputMode == GB_KeyboardInputMode::Unicode)
                {
                    succeeded = TryInputUnicodeCharacter(unicodeChar, downUpIntervalMs);
                }
                else
                {
                    succeeded = TryPressCharacterByKeyboardLayout(unicodeChar, options);
                }

                if (!succeeded)
                {
                    return false;
                }

                if (charIndex + 1 < text.size())
                {
                    SleepForDelayMs(ResolveCharacterIntervalMs(options));
                }
            }

            return true;
        }

        static bool ConvertUtf8ToWideString(const std::string& utf8Text, std::wstring& wideText)
        {
            wideText.clear();
            if (utf8Text.empty())
            {
                return true;
            }

            const int requiredLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.data(), static_cast<int>(utf8Text.size()), nullptr, 0);
            if (requiredLength <= 0)
            {
                return false;
            }

            wideText.resize(static_cast<size_t>(requiredLength));
            const int convertedLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.data(), static_cast<int>(utf8Text.size()), &wideText[0], requiredLength);
            return convertedLength == requiredLength;
        }

        struct GlobalKeyboardCallbackEntry
        {
            GB_GlobalKeyboardEventCallback callback;
            GB_GlobalKeyboardCallbackOptions callbackOptions;
        };

        class GlobalKeyboardListenerState : public std::enable_shared_from_this<GlobalKeyboardListenerState>
        {
        public:
            GlobalKeyboardListenerState()
            {
                interestedEventMaskBits.store(GetGlobalKeyboardEventMaskBits(GB_GlobalKeyboardEventMask::All));
                interestedAllKeys.store(true);
                for (auto& interestedKey : interestedKeyTable)
                {
                    interestedKey.store(false);
                }
            }

            void SetInterestedEvents(const GB_GlobalKeyboardEventMask eventMask)
            {
                interestedEventMaskBits.store(GetGlobalKeyboardEventMaskBits(eventMask));
            }

            GB_GlobalKeyboardEventMask GetInterestedEvents() const
            {
                return static_cast<GB_GlobalKeyboardEventMask>(interestedEventMaskBits.load());
            }

            void AddInterestedEvent(const GB_GlobalKeyboardEventType eventType)
            {
                interestedEventMaskBits.fetch_or(GetGlobalKeyboardEventMaskBits(GetGlobalKeyboardEventMask(eventType)));
            }

            void RemoveInterestedEvent(const GB_GlobalKeyboardEventType eventType)
            {
                const uint32_t eventMaskBits = GetGlobalKeyboardEventMaskBits(GetGlobalKeyboardEventMask(eventType));
                interestedEventMaskBits.fetch_and(~eventMaskBits);
            }

            bool IsInterestedIn(const GB_GlobalKeyboardEventType eventType) const
            {
                const uint32_t eventMaskBits = GetGlobalKeyboardEventMaskBits(GetGlobalKeyboardEventMask(eventType));
                return (interestedEventMaskBits.load() & eventMaskBits) != 0;
            }

            void SetInterestedVirtualKeyCodes(const std::vector<uint16_t>& virtualKeyCodes)
            {
                interestedAllKeys.store(false);
                for (auto& interestedKey : interestedKeyTable)
                {
                    interestedKey.store(false);
                }

                for (const uint16_t virtualKeyCode : virtualKeyCodes)
                {
                    if (virtualKeyCode < interestedKeyTable.size())
                    {
                        interestedKeyTable[virtualKeyCode].store(true);
                    }
                }
            }

            void SetInterestedAllKeys()
            {
                interestedAllKeys.store(true);
            }

            void ClearInterestedKeys()
            {
                interestedAllKeys.store(false);
                for (auto& interestedKey : interestedKeyTable)
                {
                    interestedKey.store(false);
                }
            }

            bool IsInterestedInKeyCode(const uint16_t virtualKeyCode) const
            {
                if (interestedAllKeys.load())
                {
                    return true;
                }

                if (virtualKeyCode >= interestedKeyTable.size())
                {
                    return false;
                }

                return interestedKeyTable[virtualKeyCode].load();
            }

            void SetUnifiedCallback(const GB_GlobalKeyboardEventCallback& callback, const GB_GlobalKeyboardCallbackOptions& callbackOptions)
            {
                {
                    std::lock_guard<std::mutex> callbackLock(callbackMutex);
                    unifiedCallbackEntry.callback = callback;
                    unifiedCallbackEntry.callbackOptions = callbackOptions;
                }

                hasUnifiedCallback.store(static_cast<bool>(callback));
            }

            void ClearUnifiedCallback()
            {
                SetUnifiedCallback(GB_GlobalKeyboardEventCallback(), GB_GlobalKeyboardCallbackOptions());
            }

            void SetEventCallback(const GB_GlobalKeyboardEventType eventType, const GB_GlobalKeyboardEventCallback& callback, const GB_GlobalKeyboardCallbackOptions& callbackOptions)
            {
                const size_t eventTypeIndex = GetGlobalKeyboardEventTypeIndex(eventType);
                if (eventTypeIndex >= globalKeyboardEventTypeCount)
                {
                    return;
                }

                {
                    std::lock_guard<std::mutex> callbackLock(callbackMutex);
                    perEventCallbackEntries[eventTypeIndex].callback = callback;
                    perEventCallbackEntries[eventTypeIndex].callbackOptions = callbackOptions;
                }

                const uint32_t eventMaskBits = GetGlobalKeyboardEventMaskBits(GetGlobalKeyboardEventMask(eventType));
                if (callback)
                {
                    activePerEventCallbackMaskBits.fetch_or(eventMaskBits);
                    AddInterestedEvent(eventType);
                }
                else
                {
                    activePerEventCallbackMaskBits.fetch_and(~eventMaskBits);
                }
            }

            void ClearEventCallback(const GB_GlobalKeyboardEventType eventType)
            {
                SetEventCallback(eventType, GB_GlobalKeyboardEventCallback(), GB_GlobalKeyboardCallbackOptions());
            }

            void ClearAllCallbacks()
            {
                {
                    std::lock_guard<std::mutex> callbackLock(callbackMutex);
                    unifiedCallbackEntry.callback = GB_GlobalKeyboardEventCallback();
                    unifiedCallbackEntry.callbackOptions = GB_GlobalKeyboardCallbackOptions();
                    for (size_t index = 0; index < globalKeyboardEventTypeCount; index++)
                    {
                        perEventCallbackEntries[index].callback = GB_GlobalKeyboardEventCallback();
                        perEventCallbackEntries[index].callbackOptions = GB_GlobalKeyboardCallbackOptions();
                    }
                }

                hasUnifiedCallback.store(false);
                activePerEventCallbackMaskBits.store(0);
            }

            bool StartWorker()
            {
                std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
                if (workerThread.joinable())
                {
                    return true;
                }

                stopWorker.store(false);
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

            bool TryEnqueueEvent(const GB_GlobalKeyboardEvent& keyboardEvent)
            {
                if (!isListening.load())
                {
                    return false;
                }

                const uint32_t eventMaskBits = GetGlobalKeyboardEventMaskBits(GetGlobalKeyboardEventMask(keyboardEvent.eventType));
                if ((interestedEventMaskBits.load() & eventMaskBits) == 0)
                {
                    return false;
                }

                if (!IsInterestedInKeyCode(keyboardEvent.virtualKeyCode))
                {
                    return false;
                }

                if (!hasUnifiedCallback.load() && (activePerEventCallbackMaskBits.load() & eventMaskBits) == 0)
                {
                    return false;
                }

                {
                    std::lock_guard<std::mutex> queueLock(queueMutex);
                    if (stopWorker.load())
                    {
                        return false;
                    }

                    if (eventQueue.size() >= globalKeyboardEventQueueCapacity)
                    {
                        eventQueue.pop_front();
                    }

                    eventQueue.push_back(keyboardEvent);
                }

                queueConditionVariable.notify_one();
                return true;
            }

            void SetListening(const bool listening)
            {
                isListening.store(listening);
            }

            bool IsListening() const
            {
                return isListening.load();
            }

        private:
            void WorkerThreadMain()
            {
                while (!stopWorker.load())
                {
                    GB_GlobalKeyboardEvent keyboardEvent;
                    {
                        std::unique_lock<std::mutex> queueLock(queueMutex);
                        queueConditionVariable.wait(queueLock, [this]()
                            {
                                return stopWorker.load() || !eventQueue.empty();
                            });

                        if (stopWorker.load())
                        {
                            break;
                        }

                        keyboardEvent = eventQueue.front();
                        eventQueue.pop_front();
                    }

                    DispatchOneEvent(keyboardEvent);
                }
            }

            void DispatchOneEvent(const GB_GlobalKeyboardEvent& keyboardEvent)
            {
                GB_GlobalKeyboardEventCallback unifiedCallback;
                GB_GlobalKeyboardCallbackOptions unifiedCallbackOptions;
                GB_GlobalKeyboardEventCallback eventCallback;
                GB_GlobalKeyboardCallbackOptions eventCallbackOptions;

                {
                    std::lock_guard<std::mutex> callbackLock(callbackMutex);
                    unifiedCallback = unifiedCallbackEntry.callback;
                    unifiedCallbackOptions = unifiedCallbackEntry.callbackOptions;
                    const size_t eventTypeIndex = GetGlobalKeyboardEventTypeIndex(keyboardEvent.eventType);
                    if (eventTypeIndex < globalKeyboardEventTypeCount)
                    {
                        eventCallback = perEventCallbackEntries[eventTypeIndex].callback;
                        eventCallbackOptions = perEventCallbackEntries[eventTypeIndex].callbackOptions;
                    }
                }

                if (unifiedCallback && (!keyboardEvent.isAutoRepeat || !unifiedCallbackOptions.ignoreAutoRepeatKeyDown))
                {
                    unifiedCallback(keyboardEvent);
                }

                if (eventCallback && (!keyboardEvent.isAutoRepeat || !eventCallbackOptions.ignoreAutoRepeatKeyDown))
                {
                    eventCallback(keyboardEvent);
                }
            }

        private:
            std::atomic<bool> isListening = false;
            std::atomic<bool> stopWorker = false;
            std::atomic<uint32_t> interestedEventMaskBits = 0;
            std::atomic<bool> interestedAllKeys = true;
            std::array<std::atomic<bool>, 256> interestedKeyTable;
            std::atomic<bool> hasUnifiedCallback = false;
            std::atomic<uint32_t> activePerEventCallbackMaskBits = 0;
            std::mutex callbackMutex;
            GlobalKeyboardCallbackEntry unifiedCallbackEntry;
            std::array<GlobalKeyboardCallbackEntry, globalKeyboardEventTypeCount> perEventCallbackEntries;
            std::mutex queueMutex;
            std::condition_variable queueConditionVariable;
            std::deque<GB_GlobalKeyboardEvent> eventQueue;
            std::mutex lifecycleMutex;
            std::thread workerThread;
        };

        class GlobalKeyboardRawInputManager
        {
        public:
            static GlobalKeyboardRawInputManager& GetInstance()
            {
                static GlobalKeyboardRawInputManager instance;
                return instance;
            }

            bool RegisterListener(const std::shared_ptr<GlobalKeyboardListenerState>& listenerState, uint64_t& listenerId)
            {
                if (!listenerState)
                {
                    return false;
                }

                std::unique_lock<std::mutex> managerLock(managerMutex);
                RemoveExpiredListenersLocked();
                if (!EnsureMessageThreadStarted(managerLock))
                {
                    return false;
                }

                listenerId = nextListenerId++;
                if (listenerId == 0)
                {
                    listenerId = nextListenerId++;
                }
                listeners[listenerId] = listenerState;
                return true;
            }

            void UnregisterListener(const uint64_t listenerId)
            {
                std::unique_lock<std::mutex> managerLock(managerMutex);
                if (listenerId != 0)
                {
                    listeners.erase(listenerId);
                }
                RemoveExpiredListenersLocked();
                StopMessageThreadIfIdle(managerLock);
            }

        private:
            GlobalKeyboardRawInputManager()
            {
                for (auto& keyDownStateValue : keyDownState)
                {
                    keyDownStateValue.store(false);
                }
            }

            ~GlobalKeyboardRawInputManager()
            {
                std::unique_lock<std::mutex> managerLock(managerMutex);
                listeners.clear();
                shuttingDown = true;
                StopMessageThreadIfIdle(managerLock);
            }

            GlobalKeyboardRawInputManager(const GlobalKeyboardRawInputManager&) = delete;
            GlobalKeyboardRawInputManager& operator=(const GlobalKeyboardRawInputManager&) = delete;

            static const wchar_t* GetRawInputWindowClassName()
            {
                return L"GlobalBase_GB_Keyboard_RawInputWindow";
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
                    HandleRawInputMessage(windowHandle, wParam, lParam);
                    return ::DefWindowProcW(windowHandle, message, wParam, lParam);
                case shutdownWindowMessage:
                    if (windowHandle != nullptr && ::IsWindow(windowHandle) != FALSE)
                    {
                        (void)::DestroyWindow(windowHandle);
                    }
                    return 0;
                case WM_DESTROY:
                {
                    std::lock_guard<std::mutex> managerLock(managerMutex);
                    if (rawInputWindowHandle == windowHandle)
                    {
                        rawInputWindowHandle = nullptr;
                    }
                }
                ::PostQuitMessage(0);
                return 0;
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
                if (rawInput == nullptr || rawInput->header.dwType != RIM_TYPEKEYBOARD)
                {
                    return;
                }

                const uint32_t messageTimeMs = static_cast<uint32_t>(::GetMessageTime());
                DispatchRawKeyboardPacket(rawInput->data.keyboard, messageTimeMs);
            }

            static bool RegisterKeyboardRawInput(const HWND windowHandle)
            {
                RAWINPUTDEVICE rawInputDevice = {};
                rawInputDevice.usUsagePage = 0x01;
                rawInputDevice.usUsage = 0x06;
                rawInputDevice.dwFlags = RIDEV_INPUTSINK;
                rawInputDevice.hwndTarget = windowHandle;
                return ::RegisterRawInputDevices(&rawInputDevice, 1, sizeof(rawInputDevice)) != FALSE;
            }

            void DispatchRawKeyboardPacket(const RAWKEYBOARD& rawKeyboard, const uint32_t messageTimeMs)
            {
                if (rawKeyboard.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE)
                {
                    return;
                }

                const uint16_t virtualKeyCode = static_cast<uint16_t>(rawKeyboard.VKey);
                if (!IsValidVirtualKeyCode(virtualKeyCode))
                {
                    return;
                }

                const bool isKeyUp = (rawKeyboard.Flags & RI_KEY_BREAK) != 0;
                const bool isExtendedKey = (rawKeyboard.Flags & RI_KEY_E0) != 0;
                const bool isE1Key = (rawKeyboard.Flags & RI_KEY_E1) != 0;
                uint16_t scanCode = static_cast<uint16_t>(rawKeyboard.MakeCode & 0xFFu);
                uint16_t fullScanCode = scanCode;

                if (scanCode == 0)
                {
                    const UINT scanCodeEx = MapVirtualKeyToScanCodeEx(virtualKeyCode);
                    scanCode = static_cast<uint16_t>(scanCodeEx & 0xFFu);
                    fullScanCode = static_cast<uint16_t>(scanCodeEx & 0xFFFFu);
                }
                else if (isExtendedKey)
                {
                    fullScanCode = static_cast<uint16_t>(0xE000u | scanCode);
                }
                else if (isE1Key)
                {
                    fullScanCode = static_cast<uint16_t>(0xE100u | scanCode);
                }

                const bool isAutoRepeat = !isKeyUp && keyDownState[virtualKeyCode].load();
                if (isKeyUp)
                {
                    keyDownState[virtualKeyCode].store(false);
                }
                else
                {
                    keyDownState[virtualKeyCode].store(true);
                }

                GB_GlobalKeyboardEvent keyboardEvent;
                keyboardEvent.eventType = isKeyUp ? GB_GlobalKeyboardEventType::KeyUp : GB_GlobalKeyboardEventType::KeyDown;
                keyboardEvent.virtualKey = static_cast<GB_VirtualKey>(virtualKeyCode);
                keyboardEvent.virtualKeyCode = virtualKeyCode;
                keyboardEvent.scanCode = scanCode;
                keyboardEvent.fullScanCode = fullScanCode;
                keyboardEvent.isExtendedKey = isExtendedKey;
                keyboardEvent.isE1Key = isE1Key;
                keyboardEvent.isSystemKey = rawKeyboard.Message == WM_SYSKEYDOWN || rawKeyboard.Message == WM_SYSKEYUP;
                keyboardEvent.isAutoRepeat = isAutoRepeat;
                keyboardEvent.shiftPressed = IsShiftPressed();
                keyboardEvent.ctrlPressed = IsCtrlPressed();
                keyboardEvent.altPressed = IsAltPressed();
                keyboardEvent.winPressed = IsWinPressed();
                keyboardEvent.rawMessage = static_cast<uint32_t>(rawKeyboard.Message);
                keyboardEvent.messageTimeMs = messageTimeMs;
                keyboardEvent.receiveTickCountMs = GetSteadyTickCountMs();
                DispatchEvent(keyboardEvent);
            }

            void DispatchEvent(const GB_GlobalKeyboardEvent& keyboardEvent)
            {
                std::vector<std::shared_ptr<GlobalKeyboardListenerState>> listenerStates;
                {
                    std::lock_guard<std::mutex> managerLock(managerMutex);
                    RemoveExpiredListenersLocked();
                    listenerStates.reserve(listeners.size());
                    for (const auto& listenerPair : listeners)
                    {
                        const std::shared_ptr<GlobalKeyboardListenerState> listenerState = listenerPair.second.lock();
                        if (listenerState)
                        {
                            listenerStates.push_back(listenerState);
                        }
                    }
                }

                for (const std::shared_ptr<GlobalKeyboardListenerState>& listenerState : listenerStates)
                {
                    listenerState->TryEnqueueEvent(keyboardEvent);
                }
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
                for (auto& keyDownStateValue : keyDownState)
                {
                    keyDownStateValue.store(false);
                }

                try
                {
                    messageThread = std::thread(&GlobalKeyboardRawInputManager::MessageThreadMain, this);
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
                windowClass.lpfnWndProc = &GlobalKeyboardRawInputManager::StaticWindowProc;
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
                        registerSucceeded = RegisterKeyboardRawInput(windowHandle);
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
            std::unordered_map<uint64_t, std::weak_ptr<GlobalKeyboardListenerState>> listeners;
            std::thread messageThread;
            HWND rawInputWindowHandle = nullptr;
            DWORD rawInputThreadId = 0;
            uint64_t nextListenerId = 1;
            bool messageThreadStartupResolved = false;
            bool rawInputRegisteredSuccessfully = false;
            bool messageThreadStopInProgress = false;
            bool shuttingDown = false;
            std::array<std::atomic<bool>, 256> keyDownState;
        };
    }
}
#endif

uint16_t GB_Keyboard::ToVirtualKeyCode(const GB_VirtualKey virtualKey)
{
    return static_cast<uint16_t>(virtualKey);
}

GB_VirtualKey GB_Keyboard::FromVirtualKeyCode(const uint16_t virtualKeyCode)
{
    return static_cast<GB_VirtualKey>(virtualKeyCode);
}

bool GB_Keyboard::PressKey(const GB_VirtualKey virtualKey, const GB_KeyboardInputMode inputMode)
{
#if defined(_WIN32)
    return internal::TrySendKeyByMode(virtualKey, inputMode, false);
#else
    (void)virtualKey;
    (void)inputMode;
    return false;
#endif
}

bool GB_Keyboard::ReleaseKey(const GB_VirtualKey virtualKey, const GB_KeyboardInputMode inputMode)
{
#if defined(_WIN32)
    return internal::TrySendKeyByMode(virtualKey, inputMode, true);
#else
    (void)virtualKey;
    (void)inputMode;
    return false;
#endif
}

bool GB_Keyboard::ClickKey(const GB_VirtualKey virtualKey, const int downUpIntervalMs, const GB_KeyboardInputMode inputMode)
{
#if defined(_WIN32)
    if (!PressKey(virtualKey, inputMode))
    {
        return false;
    }

    SleepForDelayMs(downUpIntervalMs);

    if (ReleaseKey(virtualKey, inputMode))
    {
        return true;
    }

    SleepForDelayMs(1);
    (void)ReleaseKey(virtualKey, inputMode);
    return false;
#else
    (void)virtualKey;
    (void)downUpIntervalMs;
    (void)inputMode;
    return false;
#endif
}

bool GB_Keyboard::PressScanCode(const uint16_t scanCode, const bool isExtendedKey)
{
#if defined(_WIN32)
    std::vector<INPUT> inputs;
    inputs.push_back(internal::MakeKeyboardInputByScanCode(scanCode, isExtendedKey, false));
    return internal::TrySendInputEventsWithRetry(inputs);
#else
    (void)scanCode;
    (void)isExtendedKey;
    return false;
#endif
}

bool GB_Keyboard::ReleaseScanCode(const uint16_t scanCode, const bool isExtendedKey)
{
#if defined(_WIN32)
    std::vector<INPUT> inputs;
    inputs.push_back(internal::MakeKeyboardInputByScanCode(scanCode, isExtendedKey, true));
    return internal::TrySendInputEventsWithRetry(inputs);
#else
    (void)scanCode;
    (void)isExtendedKey;
    return false;
#endif
}

bool GB_Keyboard::ClickScanCode(const uint16_t scanCode, const bool isExtendedKey, const int downUpIntervalMs)
{
#if defined(_WIN32)
    if (!PressScanCode(scanCode, isExtendedKey))
    {
        return false;
    }

    SleepForDelayMs(downUpIntervalMs);

    if (ReleaseScanCode(scanCode, isExtendedKey))
    {
        return true;
    }

    SleepForDelayMs(1);
    (void)ReleaseScanCode(scanCode, isExtendedKey);
    return false;
#else
    (void)scanCode;
    (void)isExtendedKey;
    (void)downUpIntervalMs;
    return false;
#endif
}

bool GB_Keyboard::InputUnicodeCharacter(const wchar_t unicodeChar, const int downUpIntervalMs)
{
#if defined(_WIN32)
    return internal::TryInputUnicodeCharacter(unicodeChar, downUpIntervalMs);
#else
    (void)unicodeChar;
    (void)downUpIntervalMs;
    return false;
#endif
}

bool GB_Keyboard::PressKeyCombination(const std::vector<GB_VirtualKey>& virtualKeys, const GB_KeyCombinationOptions& options)
{
#if defined(_WIN32)
    if (virtualKeys.empty())
    {
        return true;
    }

    GB_KeyboardInputMode inputMode = options.inputMode;
    if (inputMode == GB_KeyboardInputMode::Unicode)
    {
        inputMode = GB_KeyboardInputMode::VirtualKey;
    }

    for (const GB_VirtualKey virtualKey : virtualKeys)
    {
        if (!internal::IsValidVirtualKeyCode(internal::ToVirtualKeyCode(virtualKey)))
        {
            return false;
        }
    }

    std::vector<GB_VirtualKey> temporarilyReleasedModifierKeys;
    if (options.temporarilyReleaseActiveModifierKeys)
    {
        temporarilyReleasedModifierKeys = internal::GetCurrentlyPressedModifierKeysNotInCombination(virtualKeys);
        if (!temporarilyReleasedModifierKeys.empty())
        {
            std::vector<INPUT> releaseModifierInputs;
            releaseModifierInputs.reserve(temporarilyReleasedModifierKeys.size());
            for (const GB_VirtualKey modifierKey : temporarilyReleasedModifierKeys)
            {
                internal::AppendKeyInputs(releaseModifierInputs, modifierKey, inputMode, true);
            }
            (void)internal::TrySendInputEventsWithRetry(releaseModifierInputs);
            SleepForDelayMs(1);
        }
    }

    std::vector<GB_VirtualKey> pressedKeys;
    pressedKeys.reserve(virtualKeys.size());
    bool succeeded = true;
    for (const GB_VirtualKey virtualKey : virtualKeys)
    {
        if (!PressKey(virtualKey, inputMode))
        {
            succeeded = false;
            break;
        }

        pressedKeys.push_back(virtualKey);
        SleepForDelayMs(options.keyDownIntervalMs);
    }

    if (succeeded)
    {
        SleepForDelayMs(options.holdIntervalMs);
    }

    for (auto reverseIter = pressedKeys.rbegin(); reverseIter != pressedKeys.rend(); ++reverseIter)
    {
        if (!ReleaseKey(*reverseIter, inputMode))
        {
            succeeded = false;
        }

        if (reverseIter + 1 != pressedKeys.rend())
        {
            SleepForDelayMs(options.keyUpIntervalMs);
        }
    }

    if (options.temporarilyReleaseActiveModifierKeys && options.restoreReleasedModifierKeys && !temporarilyReleasedModifierKeys.empty())
    {
        std::vector<INPUT> restoreModifierInputs;
        restoreModifierInputs.reserve(temporarilyReleasedModifierKeys.size());
        for (const GB_VirtualKey modifierKey : temporarilyReleasedModifierKeys)
        {
            internal::AppendKeyInputs(restoreModifierInputs, modifierKey, inputMode, false);
        }
        (void)internal::TrySendInputEventsWithRetry(restoreModifierInputs);
    }

    return succeeded;
#else
    (void)virtualKeys;
    (void)options;
    return false;
#endif
}

bool GB_Keyboard::InputText(const std::wstring& text, const GB_TextInputOptions& options)
{
#if defined(_WIN32)
    if (text.empty())
    {
        return true;
    }

    return internal::TryInputTextByMode(text, options);
#else
    (void)text;
    (void)options;
    return false;
#endif
}

bool GB_Keyboard::InputUtf8Text(const std::string& utf8Text, const GB_TextInputOptions& options)
{
#if defined(_WIN32)
    std::wstring wideText;
    if (!internal::ConvertUtf8ToWideString(utf8Text, wideText))
    {
        return false;
    }

    return InputText(wideText, options);
#else
    (void)utf8Text;
    (void)options;
    return false;
#endif
}

bool GB_Keyboard::IsKeyPressed(const GB_VirtualKey virtualKey)
{
#if defined(_WIN32)
    return internal::IsKeyPressedByCode(internal::ToVirtualKeyCode(virtualKey));
#else
    (void)virtualKey;
    return false;
#endif
}

bool GB_Keyboard::IsToggleKeyOn(const GB_VirtualKey virtualKey)
{
#if defined(_WIN32)
    const uint16_t virtualKeyCode = internal::ToVirtualKeyCode(virtualKey);
    if (!internal::IsValidVirtualKeyCode(virtualKeyCode))
    {
        return false;
    }

    return (::GetKeyState(static_cast<int>(virtualKeyCode)) & 0x0001) != 0;
#else
    (void)virtualKey;
    return false;
#endif
}

bool GB_Keyboard::IsCapsLockOn()
{
    return IsToggleKeyOn(GB_VirtualKey::CapsLock);
}

bool GB_Keyboard::IsNumLockOn()
{
    return IsToggleKeyOn(GB_VirtualKey::NumLock);
}

bool GB_Keyboard::IsScrollLockOn()
{
    return IsToggleKeyOn(GB_VirtualKey::ScrollLock);
}

#if defined(_WIN32)
class GB_GlobalKeyboardListener::Impl
{
public:
    Impl() : listenerState(std::make_shared<internal::GlobalKeyboardListenerState>())
    {
    }

    void SetInterestedEvents(const GB_GlobalKeyboardEventMask eventMask)
    {
        listenerState->SetInterestedEvents(eventMask);
    }

    GB_GlobalKeyboardEventMask GetInterestedEvents() const
    {
        return listenerState->GetInterestedEvents();
    }

    void AddInterestedEvent(const GB_GlobalKeyboardEventType eventType)
    {
        listenerState->AddInterestedEvent(eventType);
    }

    void RemoveInterestedEvent(const GB_GlobalKeyboardEventType eventType)
    {
        listenerState->RemoveInterestedEvent(eventType);
    }

    bool IsInterestedIn(const GB_GlobalKeyboardEventType eventType) const
    {
        return listenerState->IsInterestedIn(eventType);
    }

    void SetInterestedVirtualKeyCodes(const std::vector<uint16_t>& virtualKeyCodes)
    {
        listenerState->SetInterestedVirtualKeyCodes(virtualKeyCodes);
    }

    void SetInterestedAllKeys()
    {
        listenerState->SetInterestedAllKeys();
    }

    void ClearInterestedKeys()
    {
        listenerState->ClearInterestedKeys();
    }

    bool IsInterestedInKeyCode(const uint16_t virtualKeyCode) const
    {
        return listenerState->IsInterestedInKeyCode(virtualKeyCode);
    }

    void SetUnifiedCallback(const GB_GlobalKeyboardEventCallback& callback, const GB_GlobalKeyboardCallbackOptions& callbackOptions)
    {
        listenerState->SetUnifiedCallback(callback, callbackOptions);
    }

    void ClearUnifiedCallback()
    {
        listenerState->ClearUnifiedCallback();
    }

    void SetEventCallback(const GB_GlobalKeyboardEventType eventType, const GB_GlobalKeyboardEventCallback& callback, const GB_GlobalKeyboardCallbackOptions& callbackOptions)
    {
        listenerState->SetEventCallback(eventType, callback, callbackOptions);
    }

    void ClearEventCallback(const GB_GlobalKeyboardEventType eventType)
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
        if (internal::GlobalKeyboardRawInputManager::GetInstance().RegisterListener(listenerState, listenerId))
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
        internal::GlobalKeyboardRawInputManager::GetInstance().UnregisterListener(listenerIdToRemove);
        listenerState->StopWorker();
    }

    bool IsListening() const
    {
        return listenerState->IsListening();
    }

private:
    std::shared_ptr<internal::GlobalKeyboardListenerState> listenerState;
    std::mutex lifecycleMutex;
    uint64_t listenerId = 0;
};
#else
class GB_GlobalKeyboardListener::Impl
{
public:
    void SetInterestedEvents(const GB_GlobalKeyboardEventMask eventMask)
    {
        (void)eventMask;
    }

    GB_GlobalKeyboardEventMask GetInterestedEvents() const
    {
        return GB_GlobalKeyboardEventMask::None;
    }

    void AddInterestedEvent(const GB_GlobalKeyboardEventType eventType)
    {
        (void)eventType;
    }

    void RemoveInterestedEvent(const GB_GlobalKeyboardEventType eventType)
    {
        (void)eventType;
    }

    bool IsInterestedIn(const GB_GlobalKeyboardEventType eventType) const
    {
        (void)eventType;
        return false;
    }

    void SetInterestedVirtualKeyCodes(const std::vector<uint16_t>& virtualKeyCodes)
    {
        (void)virtualKeyCodes;
    }

    void SetInterestedAllKeys()
    {
    }

    void ClearInterestedKeys()
    {
    }

    bool IsInterestedInKeyCode(const uint16_t virtualKeyCode) const
    {
        (void)virtualKeyCode;
        return false;
    }

    void SetUnifiedCallback(const GB_GlobalKeyboardEventCallback& callback, const GB_GlobalKeyboardCallbackOptions& callbackOptions)
    {
        (void)callback;
        (void)callbackOptions;
    }

    void ClearUnifiedCallback()
    {
    }

    void SetEventCallback(const GB_GlobalKeyboardEventType eventType, const GB_GlobalKeyboardEventCallback& callback, const GB_GlobalKeyboardCallbackOptions& callbackOptions)
    {
        (void)eventType;
        (void)callback;
        (void)callbackOptions;
    }

    void ClearEventCallback(const GB_GlobalKeyboardEventType eventType)
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

GB_GlobalKeyboardListener::GB_GlobalKeyboardListener() : impl_(new Impl())
{
}

GB_GlobalKeyboardListener::~GB_GlobalKeyboardListener()
{
    if (impl_ != nullptr)
    {
        impl_->Stop();
        delete impl_;
        impl_ = nullptr;
    }
}

GB_GlobalKeyboardListener::GB_GlobalKeyboardListener(GB_GlobalKeyboardListener&& other) noexcept : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

GB_GlobalKeyboardListener& GB_GlobalKeyboardListener::operator=(GB_GlobalKeyboardListener&& other) noexcept
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

bool GB_GlobalKeyboardListener::IsSupported()
{
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

void GB_GlobalKeyboardListener::SetInterestedEvents(const GB_GlobalKeyboardEventMask eventMask)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->SetInterestedEvents(eventMask);
}

GB_GlobalKeyboardEventMask GB_GlobalKeyboardListener::GetInterestedEvents() const
{
    if (impl_ == nullptr)
    {
        return GB_GlobalKeyboardEventMask::None;
    }

    return impl_->GetInterestedEvents();
}

void GB_GlobalKeyboardListener::ClearInterestedEvents()
{
    SetInterestedEvents(GB_GlobalKeyboardEventMask::None);
}

void GB_GlobalKeyboardListener::AddInterestedEvent(const GB_GlobalKeyboardEventType eventType)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->AddInterestedEvent(eventType);
}

void GB_GlobalKeyboardListener::RemoveInterestedEvent(const GB_GlobalKeyboardEventType eventType)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->RemoveInterestedEvent(eventType);
}

bool GB_GlobalKeyboardListener::IsInterestedIn(const GB_GlobalKeyboardEventType eventType) const
{
    if (impl_ == nullptr)
    {
        return false;
    }

    return impl_->IsInterestedIn(eventType);
}

void GB_GlobalKeyboardListener::SetInterestedKeys(const std::vector<GB_VirtualKey>& virtualKeys)
{
    std::vector<uint16_t> virtualKeyCodes;
    virtualKeyCodes.reserve(virtualKeys.size());
    for (const GB_VirtualKey virtualKey : virtualKeys)
    {
        virtualKeyCodes.push_back(GB_Keyboard::ToVirtualKeyCode(virtualKey));
    }

    SetInterestedVirtualKeyCodes(virtualKeyCodes);
}

void GB_GlobalKeyboardListener::SetInterestedVirtualKeyCodes(const std::vector<uint16_t>& virtualKeyCodes)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->SetInterestedVirtualKeyCodes(virtualKeyCodes);
}

void GB_GlobalKeyboardListener::SetInterestedAllKeys()
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->SetInterestedAllKeys();
}

void GB_GlobalKeyboardListener::ClearInterestedKeys()
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->ClearInterestedKeys();
}

bool GB_GlobalKeyboardListener::IsInterestedInKey(const GB_VirtualKey virtualKey) const
{
    if (impl_ == nullptr)
    {
        return false;
    }

    return impl_->IsInterestedInKeyCode(GB_Keyboard::ToVirtualKeyCode(virtualKey));
}

void GB_GlobalKeyboardListener::SetUnifiedCallback(const GB_GlobalKeyboardEventCallback& callback, const GB_GlobalKeyboardCallbackOptions& callbackOptions)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->SetUnifiedCallback(callback, callbackOptions);
}

void GB_GlobalKeyboardListener::ClearUnifiedCallback()
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->ClearUnifiedCallback();
}

void GB_GlobalKeyboardListener::SetEventCallback(const GB_GlobalKeyboardEventType eventType, const GB_GlobalKeyboardEventCallback& callback, const GB_GlobalKeyboardCallbackOptions& callbackOptions)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->SetEventCallback(eventType, callback, callbackOptions);
}

void GB_GlobalKeyboardListener::ClearEventCallback(const GB_GlobalKeyboardEventType eventType)
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->ClearEventCallback(eventType);
}

void GB_GlobalKeyboardListener::ClearAllCallbacks()
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->ClearAllCallbacks();
}

bool GB_GlobalKeyboardListener::Start()
{
    if (impl_ == nullptr)
    {
        return false;
    }

    return impl_->Start();
}

void GB_GlobalKeyboardListener::Stop()
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->Stop();
}

bool GB_GlobalKeyboardListener::IsListening() const
{
    if (impl_ == nullptr)
    {
        return false;
    }

    return impl_->IsListening();
}
