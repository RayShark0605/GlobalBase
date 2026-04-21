#ifndef GLOBALBASE_MOUSE_H_H
#define GLOBALBASE_MOUSE_H_H

#include "../GlobalBasePort.h"

#include "../Geometry/GB_Point2d.h"
#include "../Geometry/GB_Vector2d.h"
#include "../CV/GB_Image.h"

/**
 * @brief 鼠标移动方式。
 *
 * 说明：
 * - Teleport 表示直接瞬移到目标位置；
 * - Linear 表示沿直线路径匀速样式移动；
 * - HumanLike 表示以更接近人工操作的方式移动：速度具备缓入缓出特征，并会优先采用轻微弧线而非绝对笔直的路径。
 */
enum class GB_MouseMoveMode
{
    Teleport = 0,
    Linear = 1,
    HumanLike = 2
};

/**
 * @brief MoveTo 单点输入时所采用的坐标解释方式。
 *
 * 说明：
 * - VirtualScreenPhysicalPixel：输入点表示虚拟桌面坐标系中的物理像素坐标；
 * - CurrentScreenPhysicalPixel：输入点表示“当前鼠标所在显示屏”局部坐标系中的物理像素坐标。
 */
enum class GB_MouseMoveCoordinateType
{
    VirtualScreenPhysicalPixel = 0,
    CurrentScreenPhysicalPixel = 1
};

/**
 * @brief 鼠标移动选项。
 *
 * 说明：
 * - 所有速度单位均为“物理像素 / 秒”；
 * - 所有时长单位均为毫秒；
 * - 当 moveMode 为 Teleport 时，耗时 / 速度 / 采样相关参数通常不会生效；
 * - 当同时设置多个约束时，内部会尽量求得一个合理的移动时长；若约束彼此冲突，则会退化为选择一个尽量接近默认值的可执行方案。
 */
struct GB_MouseMoveOptions
{
    /**
     * @brief 移动方式。
     */
    GB_MouseMoveMode moveMode = GB_MouseMoveMode::HumanLike;

    /**
     * @brief 是否指定最小移动耗时。
     */
    bool specifyMinDurationMs = false;

    /**
     * @brief 最小移动耗时，单位为毫秒。
     */
    double minDurationMs = 0;

    /**
     * @brief 是否指定最大移动耗时。
     */
    bool specifyMaxDurationMs = false;

    /**
     * @brief 最大移动耗时，单位为毫秒。
     */
    double maxDurationMs = 0;

    /**
     * @brief 是否指定最小移动速度。
     */
    bool specifyMinSpeedPixelPerSecond = false;

    /**
     * @brief 最小移动速度，单位为物理像素 / 秒。
     */
    double minSpeedPixelPerSecond = 0;

    /**
     * @brief 是否指定最大移动速度。
     */
    bool specifyMaxSpeedPixelPerSecond = false;

    /**
     * @brief 最大移动速度，单位为物理像素 / 秒。
     */
    double maxSpeedPixelPerSecond = 0;

    /**
     * @brief 相邻采样点的目标时间间隔，单位为毫秒。
     *
     * 仅当 moveMode 为 Linear 或 HumanLike 时参与计算。
     * 该值越小，移动轨迹越细腻，但调用开销也会更高。
     */
    double samplingIntervalMs = 4;

    /**
     * @brief 相邻采样点之间允许的最大像素距离，单位为物理像素。
     *
     * 仅当 moveMode 为 Linear 或 HumanLike 时参与计算。
     * 该值越小，路径越平滑。
     */
    double maxStepPixelDistance = 8;
};

/**
 * @brief 与鼠标 / 光标相关的 Windows 工具类。
 */
class GLOBALBASE_PORT GB_Mouse
{
public:
    /**
     * @brief 获取当前鼠标图像。
     *
     * @param cursorImage [out] 输出图像。
     * @param fallbackCaptureRadius 当纯光标图像不可获取或不可可靠还原时，回退截取区域的半径，单位为像素。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 本接口会优先获取“透明背景的纯光标图像”；
     * - 对部分依赖 AND/XOR 掩码的单色系统光标，会返回“规范化后的纯光标图像”：可直接确定的黑/白像素按其原义输出，透明区域保持透明，依赖背景反相显示的像素会优先根据当前背景局部亮度自适应输出为黑或白；若局部背景不可获取，则再退化为根据整体背景亮度输出；
     * - 若当前没有可获取的系统光标，或当前光标确实无法可靠提取为纯光标图像，则会退化为围绕鼠标热点截取一个局部屏幕区域；
     * - 因此，本接口在回退模式下得到的结果不再是“纯光标精灵图”，而是“当前实际显示出来的鼠标附近画面”。
     */
    static bool GetCursorImage(GB_Image& cursorImage, int fallbackCaptureRadius = 32);

    /**
     * @brief 获取当前鼠标图像，并同时返回热点坐标。
     *
     * @param cursorImage [out] 输出图像。
     * @param hotSpot [out] 热点在输出图像局部坐标系中的位置。
     * @param fallbackCaptureRadius 当纯光标图像不可获取或不可可靠还原时，回退截取区域的半径，单位为像素。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 若主路径成功，输出的是透明背景的纯光标图像，hotSpot 为该输出图像局部坐标系中的热点位置；
     * - 对部分单色系统光标，主路径返回的是“规范化后的纯光标图像”，其中依赖背景反相显示的像素会优先根据当前背景局部亮度自适应输出为黑或白；若局部背景不可获取，则再退化为根据整体背景亮度输出；
     * - 若进入回退路径，则 hotSpot 对应当前鼠标物理位置在截取结果中的局部坐标；
     * - 回退路径的输出图像 Alpha 通常为 255，因为它本质上是屏幕截图局部块。
     */
    static bool GetCursorImage(GB_Image& cursorImage, GB_Point2d& hotSpot, int fallbackCaptureRadius = 32);

    /**
     * @brief 获取当前鼠标在系统逻辑像素坐标系中的位置。
     *
     * @param logicalPixelPoint [out] 当前鼠标位置。
     * @return true=成功；false=失败。
     */
    static bool GetMousePosition(GB_Point2d& logicalPixelPoint);

    /**
     * @brief 获取当前鼠标在虚拟桌面物理像素坐标系中的位置。
     *
     * @param physicalPixelPoint [out] 当前鼠标位置，单位为物理像素。
     * @return true=成功；false=失败。
     */
    static bool GetMousePhysicalPosition(GB_Point2d& physicalPixelPoint);

    /**
     * @brief 获取当前鼠标所在显示屏编号，以及该点在该显示屏局部坐标系中的物理像素坐标。
     *
     * @param screenIndex [out] 命中的显示屏编号，0 基，对应 GB_Screen::GetAllScreens() 返回顺序。
     * @param physicalPixelPointOnScreen [out] 该点在命中显示屏局部坐标系中的物理像素坐标，左上角为 (0, 0)。
     * @return true=成功；false=失败。
     */
    static bool GetMousePhysicalPosition(int& screenIndex, GB_Point2d& physicalPixelPointOnScreen);

    /**
     * @brief 将鼠标移动到目标位置。
     *
     * @param physicalPixelPoint 目标点。
     * @param coordinateType 输入点的坐标解释方式。
     * @param moveOptions 移动选项。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 当 coordinateType 为 VirtualScreenPhysicalPixel 时，physicalPixelPoint 表示虚拟桌面坐标系中的物理像素坐标；
     * - 当 coordinateType 为 CurrentScreenPhysicalPixel 时，physicalPixelPoint 表示当前鼠标所在显示屏局部坐标系中的物理像素坐标；
     * - 若目标点越出可达范围，内部会自动裁剪到最近的可达像素位置。
     */
    static bool MoveTo(const GB_Point2d& physicalPixelPoint, GB_MouseMoveCoordinateType coordinateType = GB_MouseMoveCoordinateType::CurrentScreenPhysicalPixel, const GB_MouseMoveOptions& moveOptions = GB_MouseMoveOptions());

    /**
     * @brief 将鼠标移动到指定显示屏局部坐标系中的目标位置。
     *
     * @param screenIndex 显示屏编号，0 基，对应 GB_Screen::GetAllScreens() 返回顺序。
     * @param physicalPixelPointOnScreen 目标点在该显示屏局部坐标系中的物理像素坐标，左上角为 (0, 0)。
     * @param moveOptions 移动选项。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 若输入点越出该显示屏范围，内部会自动裁剪到该显示屏内最近的可达像素位置；
     * - 本接口不会把目标解释为虚拟桌面坐标。
     */
    static bool MoveTo(int screenIndex, const GB_Point2d& physicalPixelPointOnScreen, const GB_MouseMoveOptions& moveOptions = GB_MouseMoveOptions());

    /**
     * @brief 以当前鼠标位置为基础，按物理像素偏移量进行移动。
     *
     * @param physicalPixelOffset 偏移量，单位为物理像素。
     * @param allowMoveToOtherScreens 是否允许移动到其它显示屏。true=允许；false=限制在当前显示屏内。
     * @param moveOptions 移动选项。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 当 allowMoveToOtherScreens=false 时，本次移动的最终位置不会移出当前显示屏；
     * - 当 allowMoveToOtherScreens=true 时，目标点按虚拟桌面物理像素坐标解释，若偏移后落在不可达区域，则会自动裁剪到最近的可达像素位置。
     */
    static bool Move(const GB_Vector2d& physicalPixelOffset, bool allowMoveToOtherScreens = true, const GB_MouseMoveOptions& moveOptions = GB_MouseMoveOptions());

    /**
     * @brief 在当前鼠标位置按下左键。
     *
     * @return true=成功；false=失败。
     */
    static bool PressLeftButton();

    /**
     * @brief 在当前鼠标位置抬起左键。
     *
     * @return true=成功；false=失败。
     */
    static bool ReleaseLeftButton();

    /**
     * @brief 在当前鼠标位置单击左键。
     *
     * @param downUpIntervalMs 按下与抬起之间的间隔耗时，单位为毫秒。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 该间隔越短，越接近“极快点击”；越长，则越接近有意停顿后再抬起；
     * - 若 downUpIntervalMs 小于 0，则按 0 处理；
     */
    static bool ClickLeftButton(int downUpIntervalMs = 80);

    /**
     * @brief 在当前鼠标位置尽最大可能模拟人工双击左键。
     *
     * @param downUpIntervalMs 每一次单击内部“按下与抬起之间”的间隔耗时，单位为毫秒。
     * @param interClickIntervalMs 第一次抬起到第二次按下之间的间隔耗时，单位为毫秒。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 若 downUpIntervalMs 小于 0，则按 0 处理；
     * - 若 interClickIntervalMs 小于 0，则内部会结合当前系统双击时间阈值自动选择一个更稳妥的默认间隔；
     * - 若调用方给出的按下/抬起间隔与两次点击间隔之和过大，内部会尽量自动压缩到更容易被系统识别为双击的范围；
     * - 默认参数分别为 80ms 与 -1。
     */
    static bool DoubleClickLeftButton(int downUpIntervalMs = 80, int interClickIntervalMs = -1);

    /**
     * @brief 在当前鼠标位置按下右键。
     *
     * @return true=成功；false=失败。
     */
    static bool PressRightButton();

    /**
     * @brief 在当前鼠标位置抬起右键。
     *
     * @return true=成功；false=失败。
     */
    static bool ReleaseRightButton();

    /**
     * @brief 在当前鼠标位置单击右键。
     *
     * @param downUpIntervalMs 按下与抬起之间的间隔耗时，单位为毫秒。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 若 downUpIntervalMs 小于 0，则按 0 处理；
     * - 默认值取 80ms。
     */
    static bool ClickRightButton(int downUpIntervalMs = 80);

    /**
     * @brief 在当前鼠标位置按下滚轮键。
     *
     * @return true=成功；false=失败。
     */
    static bool PressMiddleButton();

    /**
     * @brief 在当前鼠标位置抬起滚轮键。
     *
     * @return true=成功；false=失败。
     */
    static bool ReleaseMiddleButton();

    /**
     * @brief 在当前鼠标位置单击滚轮键。
     *
     * @param downUpIntervalMs 按下与抬起之间的间隔耗时，单位为毫秒。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 若 downUpIntervalMs 小于 0，则按 0 处理；
     * - 默认值取 80ms。
     */
    static bool ClickMiddleButton(int downUpIntervalMs = 80);

    /**
     * @brief 在当前鼠标位置滚动垂直滚轮。
     *
     * @param wheelDelta 滚动增量。正值表示向前滚动，负值表示向后滚动。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - Windows 约定一个标准滚轮刻度通常为 120；
     * - 若 wheelDelta 为 0，则视为无操作并直接返回 true。
     */
    static bool ScrollVerticalWheel(int wheelDelta);

    /**
     * @brief 在当前鼠标位置滚动水平滚轮。
     *
     * @param wheelDelta 滚动增量。正值表示向右滚动，负值表示向左滚动。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - Windows 约定一个标准滚轮刻度通常为 120；
     * - 若 wheelDelta 为 0，则视为无操作并直接返回 true。
     */
    static bool ScrollHorizontalWheel(int wheelDelta);
};

#endif
