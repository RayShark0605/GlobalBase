#ifndef GLOBALBASE_MOUSE_H_H
#define GLOBALBASE_MOUSE_H_H

#include "../GlobalBasePort.h"
#include "../Geometry/GB_Point2d.h"
#include "../CV/GB_Image.h"

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
     * - 若当前没有可获取的系统光标，或当前光标无法可靠还原为普通 RGBA 图像，则会退化为围绕鼠标热点截取一个局部屏幕区域；
     * - 因此，本接口在回退模式下得到的结果不再是“纯光标精灵图”，而是“当前实际显示出来的鼠标附近画面”；
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
     * - 若主路径成功，输出的是透明背景的纯光标图像，hotSpot 为系统光标热点；
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
};

#endif
