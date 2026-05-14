#ifndef GLOBALBASE_KEYBOARD_H_H
#define GLOBALBASE_KEYBOARD_H_H

#include "../GlobalBasePort.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * @brief Windows 虚拟键码。
 *
 * 说明：
 * - 该枚举显式封装 Win32 Virtual-Key Codes，避免外部调用方直接依赖 VK_XXX 宏；
 * - 枚举值与 Win32 原始虚拟键码保持一致，可通过 ToVirtualKeyCode / FromVirtualKeyCode 进行数值转换；
 * - 部分枚举项本质上不是“键盘按键”（例如鼠标键），但 Windows 虚拟键码表中包含这些值，因此这里一并保留以保证完整性。
 */
enum class GB_VirtualKey : uint16_t
{
    None = 0x00,

    LeftMouseButton = 0x01,
    RightMouseButton = 0x02,
    Cancel = 0x03,
    MiddleMouseButton = 0x04,
    XMouseButton1 = 0x05,
    XMouseButton2 = 0x06,

    Backspace = 0x08,
    Tab = 0x09,
    Clear = 0x0C,
    Enter = 0x0D,
    Shift = 0x10,
    Control = 0x11,
    Alt = 0x12,
    Pause = 0x13,
    CapsLock = 0x14,
    Kana = 0x15,
    Hangul = 0x15,
    ImeOn = 0x16,
    Junja = 0x17,
    Final = 0x18,
    Hanja = 0x19,
    Kanji = 0x19,
    ImeOff = 0x1A,
    Escape = 0x1B,
    Convert = 0x1C,
    NonConvert = 0x1D,
    Accept = 0x1E,
    ModeChange = 0x1F,

    Space = 0x20,
    PageUp = 0x21,
    PageDown = 0x22,
    End = 0x23,
    Home = 0x24,
    Left = 0x25,
    Up = 0x26,
    Right = 0x27,
    Down = 0x28,
    Select = 0x29,
    Print = 0x2A,
    Execute = 0x2B,
    PrintScreen = 0x2C,
    Insert = 0x2D,
    Delete = 0x2E,
    Help = 0x2F,

    Digit0 = 0x30,
    Digit1 = 0x31,
    Digit2 = 0x32,
    Digit3 = 0x33,
    Digit4 = 0x34,
    Digit5 = 0x35,
    Digit6 = 0x36,
    Digit7 = 0x37,
    Digit8 = 0x38,
    Digit9 = 0x39,

    A = 0x41,
    B = 0x42,
    C = 0x43,
    D = 0x44,
    E = 0x45,
    F = 0x46,
    G = 0x47,
    H = 0x48,
    I = 0x49,
    J = 0x4A,
    K = 0x4B,
    L = 0x4C,
    M = 0x4D,
    N = 0x4E,
    O = 0x4F,
    P = 0x50,
    Q = 0x51,
    R = 0x52,
    S = 0x53,
    T = 0x54,
    U = 0x55,
    V = 0x56,
    W = 0x57,
    X = 0x58,
    Y = 0x59,
    Z = 0x5A,

    LeftWin = 0x5B,
    RightWin = 0x5C,
    Apps = 0x5D,
    Sleep = 0x5F,

    NumPad0 = 0x60,
    NumPad1 = 0x61,
    NumPad2 = 0x62,
    NumPad3 = 0x63,
    NumPad4 = 0x64,
    NumPad5 = 0x65,
    NumPad6 = 0x66,
    NumPad7 = 0x67,
    NumPad8 = 0x68,
    NumPad9 = 0x69,
    NumPadMultiply = 0x6A,
    NumPadAdd = 0x6B,
    NumPadSeparator = 0x6C,
    NumPadSubtract = 0x6D,
    NumPadDecimal = 0x6E,
    NumPadDivide = 0x6F,

    F1 = 0x70,
    F2 = 0x71,
    F3 = 0x72,
    F4 = 0x73,
    F5 = 0x74,
    F6 = 0x75,
    F7 = 0x76,
    F8 = 0x77,
    F9 = 0x78,
    F10 = 0x79,
    F11 = 0x7A,
    F12 = 0x7B,
    F13 = 0x7C,
    F14 = 0x7D,
    F15 = 0x7E,
    F16 = 0x7F,
    F17 = 0x80,
    F18 = 0x81,
    F19 = 0x82,
    F20 = 0x83,
    F21 = 0x84,
    F22 = 0x85,
    F23 = 0x86,
    F24 = 0x87,

    NumLock = 0x90,
    ScrollLock = 0x91,

    LeftShift = 0xA0,
    RightShift = 0xA1,
    LeftControl = 0xA2,
    RightControl = 0xA3,
    LeftAlt = 0xA4,
    RightAlt = 0xA5,

    BrowserBack = 0xA6,
    BrowserForward = 0xA7,
    BrowserRefresh = 0xA8,
    BrowserStop = 0xA9,
    BrowserSearch = 0xAA,
    BrowserFavorites = 0xAB,
    BrowserHome = 0xAC,

    VolumeMute = 0xAD,
    VolumeDown = 0xAE,
    VolumeUp = 0xAF,
    MediaNextTrack = 0xB0,
    MediaPreviousTrack = 0xB1,
    MediaStop = 0xB2,
    MediaPlayPause = 0xB3,
    LaunchMail = 0xB4,
    LaunchMediaSelect = 0xB5,
    LaunchApp1 = 0xB6,
    LaunchApp2 = 0xB7,

    OemSemicolon = 0xBA,
    OemPlus = 0xBB,
    OemComma = 0xBC,
    OemMinus = 0xBD,
    OemPeriod = 0xBE,
    OemSlash = 0xBF,
    OemTilde = 0xC0,
    OemOpenBracket = 0xDB,
    OemBackslash = 0xDC,
    OemCloseBracket = 0xDD,
    OemQuote = 0xDE,
    Oem8 = 0xDF,
    Oem102 = 0xE2,

    ImeProcess = 0xE5,
    Packet = 0xE7,

    Attn = 0xF6,
    CrSel = 0xF7,
    ExSel = 0xF8,
    EraseEof = 0xF9,
    Play = 0xFA,
    Zoom = 0xFB,
    NoName = 0xFC,
    Pa1 = 0xFD,
    OemClear = 0xFE
};

/**
 * @brief 键盘模拟输入模式。
 *
 * 说明：
 * - VirtualKey：按 Win32 虚拟键码生成输入，最适合快捷键、功能键和普通控制键；
 * - ScanCode：先将虚拟键码转换为硬件扫描码，再按扫描码生成输入，更接近物理键盘位置；
 * - Unicode：按 Unicode 字符生成文本输入，通常绕过当前键盘布局和输入法候选过程，更适合直接输入文本。
 */
enum class GB_KeyboardInputMode
{
    VirtualKey = 0,
    ScanCode = 1,
    Unicode = 2
};

/**
 * @brief 文本输入模拟选项。
 */
struct GB_TextInputOptions
{
    /**
     * @brief 文本输入模式。
     *
     * 说明：
     * - Unicode 模式适合中文、日文、特殊符号等无法稳定用当前键盘布局直接敲出的字符；
     * - VirtualKey / ScanCode 模式会尝试基于当前前台窗口的键盘布局把字符翻译为按键组合，无法翻译的字符会导致接口返回 false。
     */
    GB_KeyboardInputMode inputMode = GB_KeyboardInputMode::Unicode;

    /**
     * @brief 每分钟字数。
     *
     * 说明：
     * - 按英文输入领域常用近似值 1 个 word = 5 个字符计算字符间隔；
     * - 小于等于 0 表示不额外等待。
     */
    double wordsPerMinute = 180.0;

    /**
     * @brief 字符间隔随机扰动比例。
     *
     * 说明：
     * - 0 表示固定间隔；
     * - 0.35 表示在基础间隔的 ±35% 范围内随机扰动。
     */
    double randomIntervalRatio = 0.35;

    /**
     * @brief 最小字符间隔，单位为毫秒。
     */
    int minCharacterIntervalMs = 8;

    /**
     * @brief 最大字符间隔，单位为毫秒。
     *
     * 说明：
     * - 小于 0 表示不限制最大值。
     */
    int maxCharacterIntervalMs = 250;

    /**
     * @brief 单字符按下与抬起之间的间隔，单位为毫秒。
     */
    int keyDownUpIntervalMs = 0;
};

/**
 * @brief 组合键模拟选项。
 */
struct GB_KeyCombinationOptions
{
    /**
     * @brief 组合键输入模式。
     *
     * 说明：
     * - 组合键通常应使用 VirtualKey 或 ScanCode；
     * - Unicode 不适合组合键，内部会按 VirtualKey 处理。
     */
    GB_KeyboardInputMode inputMode = GB_KeyboardInputMode::VirtualKey;

    /**
     * @brief 相邻按键按下之间的间隔，单位为毫秒。
     */
    int keyDownIntervalMs = 10;

    /**
     * @brief 所有按键按下后，到开始反向抬起之间的保持时间，单位为毫秒。
     */
    int holdIntervalMs = 80;

    /**
     * @brief 相邻按键抬起之间的间隔，单位为毫秒。
     */
    int keyUpIntervalMs = 10;

    /**
     * @brief 是否在模拟组合键前临时释放当前已按下的无关修饰键。
     *
     * 说明：
     * - 例如模拟 Ctrl+V 时，如果用户正在按 Alt，系统可能识别为 Ctrl+Alt+V；
     * - 开启后，内部会临时释放 Shift / Ctrl / Alt / Win 中未参与本次组合的按键，并在组合键结束后尽量恢复这些键的逻辑按下状态；
     * - 这不能阻止用户在模拟过程中继续按下新按键，它只处理调用瞬间已经处于按下状态的修饰键。
     */
    bool temporarilyReleaseActiveModifierKeys = true;

    /**
     * @brief 临时释放修饰键后，组合键结束时是否尝试恢复这些修饰键的逻辑按下状态。
     */
    bool restoreReleasedModifierKeys = true;
};

/**
 * @brief 全局键盘事件类型。
 */
enum class GB_GlobalKeyboardEventType
{
    KeyDown = 0,
    KeyUp = 1
};

/**
 * @brief 全局键盘事件掩码。
 */
enum class GB_GlobalKeyboardEventMask : uint32_t
{
    None = 0,
    KeyDown = 1u << 0,
    KeyUp = 1u << 1,
    All = (1u << 0) | (1u << 1)
};

inline GB_GlobalKeyboardEventMask operator|(const GB_GlobalKeyboardEventMask leftMask, const GB_GlobalKeyboardEventMask rightMask)
{
    return static_cast<GB_GlobalKeyboardEventMask>(static_cast<uint32_t>(leftMask) | static_cast<uint32_t>(rightMask));
}

inline GB_GlobalKeyboardEventMask operator&(const GB_GlobalKeyboardEventMask leftMask, const GB_GlobalKeyboardEventMask rightMask)
{
    return static_cast<GB_GlobalKeyboardEventMask>(static_cast<uint32_t>(leftMask) & static_cast<uint32_t>(rightMask));
}

inline GB_GlobalKeyboardEventMask operator~(const GB_GlobalKeyboardEventMask mask)
{
    return static_cast<GB_GlobalKeyboardEventMask>(~static_cast<uint32_t>(mask));
}

inline GB_GlobalKeyboardEventMask& operator|=(GB_GlobalKeyboardEventMask& leftMask, const GB_GlobalKeyboardEventMask rightMask)
{
    leftMask = (leftMask | rightMask);
    return leftMask;
}

inline GB_GlobalKeyboardEventMask& operator&=(GB_GlobalKeyboardEventMask& leftMask, const GB_GlobalKeyboardEventMask rightMask)
{
    leftMask = (leftMask & rightMask);
    return leftMask;
}

/**
 * @brief 单个全局键盘事件的描述信息。
 */
struct GB_GlobalKeyboardEvent
{
    /**
     * @brief 事件类型。
     */
    GB_GlobalKeyboardEventType eventType = GB_GlobalKeyboardEventType::KeyDown;

    /**
     * @brief 虚拟键枚举值。
     */
    GB_VirtualKey virtualKey = GB_VirtualKey::None;

    /**
     * @brief 原始虚拟键码数值。
     */
    uint16_t virtualKeyCode = 0;

    /**
     * @brief 原始硬件扫描码低字节。
     */
    uint16_t scanCode = 0;

    /**
     * @brief 带 E0 / E1 前缀信息的扫描码。
     *
     * 说明：
     * - 低字节为扫描码；
     * - 高字节为 0xE0 或 0xE1 时表示扩展前缀；
     * - 高字节为 0 表示普通扫描码。
     */
    uint16_t fullScanCode = 0;

    /**
     * @brief 是否为 E0 扩展键。
     */
    bool isExtendedKey = false;

    /**
     * @brief 是否为 E1 前缀键。
     */
    bool isE1Key = false;

    /**
     * @brief 是否来自 WM_SYSKEYDOWN / WM_SYSKEYUP 这类系统键消息。
     */
    bool isSystemKey = false;

    /**
     * @brief 是否为按键自动重复产生的 KeyDown。
     */
    bool isAutoRepeat = false;

    /**
     * @brief 触发事件时 Shift 是否处于按下状态。
     */
    bool shiftPressed = false;

    /**
     * @brief 触发事件时 Ctrl 是否处于按下状态。
     */
    bool ctrlPressed = false;

    /**
     * @brief 触发事件时 Alt 是否处于按下状态。
     */
    bool altPressed = false;

    /**
     * @brief 触发事件时 Win 是否处于按下状态。
     */
    bool winPressed = false;

    /**
     * @brief 事件对应的原始 Windows 键盘消息值。
     */
    uint32_t rawMessage = 0;

    /**
     * @brief 系统原始消息时间戳，单位为毫秒。
     */
    uint32_t messageTimeMs = 0;

    /**
     * @brief 当前库接收到该事件时的单调时钟时间戳，单位为毫秒。
     */
    uint64_t receiveTickCountMs = 0;
};

/**
 * @brief 全局键盘回调选项。
 */
struct GB_GlobalKeyboardCallbackOptions
{
    /**
     * @brief 是否在该回调层面忽略自动重复产生的 KeyDown 事件。
     */
    bool ignoreAutoRepeatKeyDown = false;
};

/**
 * @brief 全局键盘事件回调函数类型。
 */
using GB_GlobalKeyboardEventCallback = std::function<void(const GB_GlobalKeyboardEvent& keyboardEvent)>;

/**
 * @brief 与键盘输入模拟和键盘状态查询相关的 Windows 工具类。
 */
class GLOBALBASE_PORT GB_Keyboard
{
public:
    /**
     * @brief 将枚举形式的虚拟键转换为原始虚拟键码。
     */
    static uint16_t ToVirtualKeyCode(GB_VirtualKey virtualKey);

    /**
     * @brief 将原始虚拟键码转换为枚举形式。
     */
    static GB_VirtualKey FromVirtualKeyCode(uint16_t virtualKeyCode);

    /**
     * @brief 按下指定按键。
     *
     * @param virtualKey 虚拟键。
     * @param inputMode 输入模式。Unicode 模式不适合单个虚拟键，内部会按 VirtualKey 处理。
     * @return true=成功；false=失败。
     */
    static bool PressKey(GB_VirtualKey virtualKey, GB_KeyboardInputMode inputMode = GB_KeyboardInputMode::VirtualKey);

    /**
     * @brief 抬起指定按键。
     *
     * @param virtualKey 虚拟键。
     * @param inputMode 输入模式。Unicode 模式不适合单个虚拟键，内部会按 VirtualKey 处理。
     * @return true=成功；false=失败。
     */
    static bool ReleaseKey(GB_VirtualKey virtualKey, GB_KeyboardInputMode inputMode = GB_KeyboardInputMode::VirtualKey);

    /**
     * @brief 单击指定按键。
     *
     * @param virtualKey 虚拟键。
     * @param downUpIntervalMs 按下与抬起之间的间隔耗时，单位为毫秒。
     * @param inputMode 输入模式。Unicode 模式不适合单个虚拟键，内部会按 VirtualKey 处理。
     * @return true=成功；false=失败。
     */
    static bool ClickKey(GB_VirtualKey virtualKey, int downUpIntervalMs = 80, GB_KeyboardInputMode inputMode = GB_KeyboardInputMode::VirtualKey);

    /**
     * @brief 按下指定硬件扫描码。
     */
    static bool PressScanCode(uint16_t scanCode, bool isExtendedKey = false);

    /**
     * @brief 抬起指定硬件扫描码。
     */
    static bool ReleaseScanCode(uint16_t scanCode, bool isExtendedKey = false);

    /**
     * @brief 单击指定硬件扫描码。
     */
    static bool ClickScanCode(uint16_t scanCode, bool isExtendedKey = false, int downUpIntervalMs = 80);

    /**
     * @brief 输入一个 Unicode 字符。
     *
     * @param unicodeChar Unicode 字符。Windows 下按 UTF-16 code unit 发送。
     * @param downUpIntervalMs 按下与抬起之间的间隔耗时，单位为毫秒。
     * @return true=成功；false=失败。
     */
    static bool InputUnicodeCharacter(wchar_t unicodeChar, int downUpIntervalMs = 0);

    /**
     * @brief 按组合键顺序模拟一组按键。
     *
     * @param virtualKeys 组合键序列。例如 Ctrl+C 可传入 { GB_VirtualKey::Control, GB_VirtualKey::C }。
     * @param options 组合键模拟选项。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 内部会按 virtualKeys 顺序依次按下，再按反向顺序依次抬起；
     * - 若中途失败，会尽量释放已经按下的键，避免按键逻辑状态残留。
     */
    static bool PressKeyCombination(const std::vector<GB_VirtualKey>& virtualKeys, const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+C 复制快捷键。
     */
    static bool PressCtrlC(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+V 粘贴快捷键。
     */
    static bool PressCtrlV(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+X 剪切快捷键。
     */
    static bool PressCtrlX(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+A 全选快捷键。
     */
    static bool PressCtrlA(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+S 保存快捷键。
     */
    static bool PressCtrlS(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+Z 撤销快捷键。
     */
    static bool PressCtrlZ(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+Y 重做快捷键。
     */
    static bool PressCtrlY(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+F 查找快捷键。
     */
    static bool PressCtrlF(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+P 打印快捷键。
     */
    static bool PressCtrlP(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+N 新建快捷键。
     */
    static bool PressCtrlN(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+O 打开快捷键。
     */
    static bool PressCtrlO(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+W 关闭当前标签页或当前文档快捷键。
     */
    static bool PressCtrlW(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Win+D 显示桌面快捷键。
     */
    static bool PressWinD(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行 Win+L 对应的锁定工作站操作。
     *
     * 说明：
     * - Windows 下内部直接调用 LockWorkStation，以避免锁屏过程中 Win 键状态残留；
     * - 非 Windows 平台返回 false。
     */
    static bool PressWinL();

    /**
     * @brief 模拟 Win+E 打开文件资源管理器快捷键。
     */
    static bool PressWinE(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Win+R 打开运行对话框快捷键。
     */
    static bool PressWinR(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Alt+F4 关闭当前活动窗口快捷键。
     */
    static bool PressAltF4(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 模拟 Ctrl+Shift+Esc 打开任务管理器快捷键。
     */
    static bool PressCtrlShiftEsc(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行复制命令。
     */
    static bool Copy(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行粘贴命令。
     */
    static bool Paste(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行剪切命令。
     */
    static bool Cut(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行全选命令。
     */
    static bool SelectAll(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行保存命令。
     */
    static bool Save(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行撤销命令。
     */
    static bool Undo(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行重做命令。
     */
    static bool Redo(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行查找命令。
     */
    static bool Find(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行打印命令。
     */
    static bool Print(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行新建命令。
     */
    static bool NewDocument(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行打开命令。
     */
    static bool OpenDocument(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行关闭当前标签页或当前文档命令。
     */
    static bool CloseTabOrDocument(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行显示桌面命令。
     */
    static bool ShowDesktop(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 锁定当前工作站。
     */
    static bool LockWorkstation();

    /**
     * @brief 执行打开文件资源管理器命令。
     */
    static bool OpenFileExplorer(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行打开运行对话框命令。
     */
    static bool OpenRunDialog(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 执行关闭当前活动窗口命令。
     */
    static bool CloseActiveWindow(const GB_KeyCombinationOptions& options = GB_KeyCombinationOptions());

    /**
     * @brief 打开任务管理器。
     *
     * 说明：
     * - Windows 下优先直接启动 taskmgr.exe，失败后回退到 Ctrl+Shift+Esc；
     * - 非 Windows 平台返回 false。
     */
    static bool OpenTaskManager(const GB_KeyCombinationOptions& fallbackShortcutOptions = GB_KeyCombinationOptions());

    /**
     * @brief 输入一段宽字符文本。
     *
     * @param text 文本内容。
     * @param options 文本输入模拟选项。
     * @return true=成功；false=失败。
     */
    static bool InputText(const std::wstring& text, const GB_TextInputOptions& options = GB_TextInputOptions());

    /**
     * @brief 输入一段 UTF-8 文本。
     *
     * @param utf8Text UTF-8 文本内容。
     * @param options 文本输入模拟选项。
     * @return true=成功；false=失败。
     */
    static bool InputUtf8Text(const std::string& utf8Text, const GB_TextInputOptions& options = GB_TextInputOptions());

    /**
     * @brief 获取当前物理按键状态。
     *
     * @return true=当前处于按下状态；false=当前没有按下或查询失败。
     */
    static bool IsKeyPressed(GB_VirtualKey virtualKey);

    /**
     * @brief 获取指定锁定键的开关状态。
     */
    static bool IsToggleKeyOn(GB_VirtualKey virtualKey);

    /**
     * @brief 获取 CapsLock 是否处于开启状态。
     */
    static bool IsCapsLockOn();

    /**
     * @brief 获取 NumLock 是否处于开启状态。
     */
    static bool IsNumLockOn();

    /**
     * @brief 获取 ScrollLock 是否处于开启状态。
     */
    static bool IsScrollLockOn();
};

/**
 * @brief 系统级全局键盘监听器。
 *
 * 说明：
 * - Windows 下内部基于 Raw Input + 隐藏消息窗口实现，监听范围不局限于某个窗口；
 * - 监听器只采集事件，不拦截、不吞掉、不修改系统键盘输入；
 * - 对外回调采用异步触发：底层消息线程只负责快速采集与入队，真正的用户回调在监听器自己的工作线程中执行；
 * - 该类本身不是单例，但底层 Raw Input 监听源会在进程内自动共享。
 */
class GLOBALBASE_PORT GB_GlobalKeyboardListener
{
public:
    GB_GlobalKeyboardListener();
    ~GB_GlobalKeyboardListener();

    GB_GlobalKeyboardListener(const GB_GlobalKeyboardListener&) = delete;
    GB_GlobalKeyboardListener& operator=(const GB_GlobalKeyboardListener&) = delete;

    GB_GlobalKeyboardListener(GB_GlobalKeyboardListener&& other) noexcept;
    GB_GlobalKeyboardListener& operator=(GB_GlobalKeyboardListener&& other) noexcept;

    /**
     * @brief 当前平台是否支持该全局监听器。
     */
    static bool IsSupported();

    /**
     * @brief 设置当前关心的事件集合。
     */
    void SetInterestedEvents(GB_GlobalKeyboardEventMask eventMask);

    /**
     * @brief 获取当前关心的事件集合。
     */
    GB_GlobalKeyboardEventMask GetInterestedEvents() const;

    /**
     * @brief 清空当前关心的事件集合。
     */
    void ClearInterestedEvents();

    /**
     * @brief 新增一个关心的事件。
     */
    void AddInterestedEvent(GB_GlobalKeyboardEventType eventType);

    /**
     * @brief 移除一个关心的事件。
     */
    void RemoveInterestedEvent(GB_GlobalKeyboardEventType eventType);

    /**
     * @brief 判断当前是否关心指定事件。
     */
    bool IsInterestedIn(GB_GlobalKeyboardEventType eventType) const;

    /**
     * @brief 设置当前关心的按键集合。
     *
     * 说明：
     * - 默认关心全部按键；
     * - 传入空数组表示不关心任何按键；
     * - 若希望恢复为关心全部按键，请调用 SetInterestedAllKeys。
     */
    void SetInterestedKeys(const std::vector<GB_VirtualKey>& virtualKeys);

    /**
     * @brief 设置当前关心的原始虚拟键码集合。
     */
    void SetInterestedVirtualKeyCodes(const std::vector<uint16_t>& virtualKeyCodes);

    /**
     * @brief 恢复为关心全部按键。
     */
    void SetInterestedAllKeys();

    /**
     * @brief 清空当前关心的按键集合，使监听器暂时不关心任何按键。
     */
    void ClearInterestedKeys();

    /**
     * @brief 判断当前是否关心指定按键。
     */
    bool IsInterestedInKey(GB_VirtualKey virtualKey) const;

    /**
     * @brief 设置统一回调函数。
     *
     * @param callback 统一回调函数。传入空回调可视为清空。
     * @param callbackOptions 回调选项。
     */
    void SetUnifiedCallback(const GB_GlobalKeyboardEventCallback& callback, const GB_GlobalKeyboardCallbackOptions& callbackOptions = GB_GlobalKeyboardCallbackOptions());

    /**
     * @brief 清空统一回调函数。
     */
    void ClearUnifiedCallback();

    /**
     * @brief 为指定事件单独设置回调函数。
     *
     * @param eventType 事件类型。
     * @param callback 单独回调函数。传入空回调可视为清空。
     * @param callbackOptions 回调选项。
     *
     * 说明：
     * - 设置单独回调时，会自动把该事件加入“当前关心的事件集合”。
     */
    void SetEventCallback(GB_GlobalKeyboardEventType eventType, const GB_GlobalKeyboardEventCallback& callback, const GB_GlobalKeyboardCallbackOptions& callbackOptions = GB_GlobalKeyboardCallbackOptions());

    /**
     * @brief 清空指定事件的单独回调函数。
     */
    void ClearEventCallback(GB_GlobalKeyboardEventType eventType);

    /**
     * @brief 清空所有单独回调函数与统一回调函数。
     */
    void ClearAllCallbacks();

    /**
     * @brief 启动监听。
     *
     * @return true=启动成功；false=启动失败。
     *
     * 说明：
     * - 本接口不会阻塞调用方；
     * - 若已经处于监听状态，则直接返回 true；
     * - 启动后，后续对“关心事件集合 / 关心按键集合 / 回调函数”的修改会立即生效。
     */
    bool Start();

    /**
     * @brief 停止监听。
     *
     * 说明：
     * - 停止后不会再接收新的系统事件；
     * - 已经开始执行的回调不会被强行中断；
     * - 尚未执行的排队事件会被丢弃。
     */
    void Stop();

    /**
     * @brief 当前是否处于监听状态。
     */
    bool IsListening() const;

private:
    class Impl;
    Impl* impl_ = nullptr;
};

#endif
