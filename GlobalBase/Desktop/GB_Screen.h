#ifndef GLOBALBASE_SCREEN_H_H
#define GLOBALBASE_SCREEN_H_H

#include "../GlobalBasePort.h"
#include "../Geometry/GB_Rectangle.h"
#include "../CV/GB_Image.h"

#include <string>
#include <vector>

/**
 * @brief 单个物理显示屏的信息。
 *
 * 说明：
 * - 该结构体尽量同时给出“桌面当前使用中的显示参数”和“显示器本体参数”；
 * - virtualScreenRectangle 使用虚拟桌面坐标系，单位为物理像素；
 * - DPI / 缩放比例优先反映当前系统对该显示屏的实际桌面缩放；
 * - physicalWidthMm / physicalHeightMm 等物理尺寸信息通常来自 EDID，若系统或设备未提供则为 0。
 */
struct GB_ScreenInfo
{
    /**
     * @brief 该显示目标对应的 GDI 设备名。
     *
     * 典型值类似于 "\\.\\DISPLAY1"。
     * 该名称通常可用于与 EnumDisplaySettings、EnumDisplayDevices 等传统 GDI 显示接口进行关联。
     */
    std::string gdiDeviceName = "";

    /**
     * @brief 显示器设备路径。
     *
     * 该值通常来自 CCD / DisplayConfig 接口返回的 monitorDevicePath，
     * 一般形如系统设备实例路径，可用于进一步关联 EDID、PnP 设备信息等。
     */
    std::string monitorDevicePath = "";

    /**
     * @brief 显卡适配器设备路径。
     *
     * 该值通常来自 DisplayConfig 的 adapterDevicePath，
     * 用于标识当前显示器所连接的显示适配器设备。
     */
    std::string adapterDevicePath = "";

    /**
     * @brief 显示器友好名称。
     *
     * 优先取系统当前识别到的显示器名称，通常来源于 EDID 或系统显示配置，
     * 例如 "DELL U2720Q"、"Generic PnP Monitor"。
     */
    std::string monitorFriendlyName = "";

    /**
     * @brief 显示器厂商三字符编码。
     *
     * 一般来自 EDID Manufacturer ID，例如 "DEL"、"AOC"。
     * 若无法获取，则为空字符串。
     */
    std::string manufacturerCode = "";

    /**
     * @brief 厂商品牌名。
     *
     * 这是在 manufacturerCode 基础上尽量解析得到的人类可读品牌名，
     * 例如 "Dell"、"AOC"、"LG"。若无法可靠推断，则可能为空。
     */
    std::string brandName = "";

    /**
     * @brief 显示器产品型号名。
     *
     * 尽量从 EDID 的显示器描述信息中提取，用于表示更具体的型号标识。
     * 若设备未提供，则可能为空。
     */
    std::string productName = "";

    /**
     * @brief 显示器序列号。
     *
     * 尽量从 EDID 中提取；不同厂商设备的填写质量可能存在差异。
     * 若系统或设备未提供，则可能为空。
     */
    std::string serialNumber = "";

    /**
     * @brief 该显示屏在当前虚拟桌面坐标系中的包围矩形。
     *
     * 坐标单位为物理像素，而不是逻辑像素。
     * 在多显示屏场景下，该矩形可能位于虚拟桌面的任意位置，左上角坐标也可能为负值。
     */
    GB_Rectangle virtualScreenRectangle;

    /**
     * @brief 当前桌面模式下该显示屏的实际输出宽度，单位为物理像素。
     *
     * 该值反映当前正在使用的显示模式宽度，而非面板理论最大分辨率。
     */
    int currentPixelWidth = 0;

    /**
     * @brief 当前桌面模式下该显示屏的实际输出高度，单位为物理像素。
     *
     * 该值反映当前正在使用的显示模式高度，而非面板理论最大分辨率。
     */
    int currentPixelHeight = 0;

    /**
     * @brief 系统报告的该显示屏首选宽度，单位为物理像素。
     *
     * 通常来源于显示器首选时序 / 首选模式；若无法获取，则为 0。
     */
    int preferredPixelWidth = 0;

    /**
     * @brief 系统报告的该显示屏首选高度，单位为物理像素。
     *
     * 通常来源于显示器首选时序 / 首选模式；若无法获取，则为 0。
     */
    int preferredPixelHeight = 0;

    /**
     * @brief 当前刷新率，单位为 Hz。
     *
     * 该值优先表示当前实际桌面模式对应的刷新率。
     * 若系统未能提供有效值，则可能为 0。
     */
    double refreshRateHz = 0;

    /**
     * @brief 当前有效 DPI 的 X 方向值。
     *
     * 该值通常用于反映桌面缩放后的有效 DPI，
     * 更接近应用在当前显示屏上的实际 UI 缩放基准。
     */
    double effectiveDpiX = 0;

    /**
     * @brief 当前有效 DPI 的 Y 方向值。
     *
     * 通常与 effectiveDpiX 相同；在少数特殊环境下也应分别保留。
     */
    double effectiveDpiY = 0;

    /**
     * @brief 当前原始 DPI 的 X 方向值。
     *
     * 该值更接近显示器物理像素密度，不考虑桌面缩放设置；
     * 若系统无法获取，则可能为 0。
     */
    double rawDpiX = 0;

    /**
     * @brief 当前原始 DPI 的 Y 方向值。
     *
     * 该值更接近显示器物理像素密度，不考虑桌面缩放设置；
     * 若系统无法获取，则可能为 0。
     */
    double rawDpiY = 0;

    /**
     * @brief X 方向缩放比例。
     *
     * 通常按 effectiveDpiX / 96.0 计算，
     * 例如 1.0 表示 100%，1.25 表示 125%，1.5 表示 150%。
     */
    double scaleFactorX = 1;

    /**
     * @brief Y 方向缩放比例。
     *
     * 通常按 effectiveDpiY / 96.0 计算。
     * 在常见桌面环境下通常与 scaleFactorX 相同。
     */
    double scaleFactorY = 1;

    /**
     * @brief 显示器可视物理宽度，单位为毫米。
     *
     * 该值通常来自 EDID；若设备未提供、读取失败，或系统无法匹配到对应 EDID，则为 0。
     */
    double physicalWidthMm = 0;

    /**
     * @brief 显示器可视物理高度，单位为毫米。
     *
     * 该值通常来自 EDID；若设备未提供、读取失败，或系统无法匹配到对应 EDID，则为 0。
     */
    double physicalHeightMm = 0;

    /**
     * @brief 显示器对角线尺寸，单位为英寸。
     *
     * 该值通常由 physicalWidthMm 与 physicalHeightMm 计算得到；
     * 若缺少物理尺寸信息，则为 0。
     */
    double diagonalInches = 0;

    /**
     * @brief 是否为主显示屏。
     *
     * true 表示该显示屏当前被系统标记为主显示屏。
     */
    bool isPrimary = false;

    /**
     * @brief 是否为内置显示屏。
     *
     * true 通常表示该显示器被系统识别为内置面板，例如笔记本电脑自带屏幕；
     * false 则通常表示外接显示器，或系统无法确认其是否为内置屏。
     */
    bool isInternal = false;
};

/**
 * @brief 与屏幕相关的 Windows 工具类。
 *
 * 说明：
 * - 该模块仅在 Windows 下有实际功能；
 * - 非 Windows 平台下，获取显示屏信息会返回空数组，截图接口会返回 false / 空图像；
 * - 截图坐标统一使用虚拟桌面坐标系，且尽量按物理像素工作，以适配多显示屏和不同缩放比例。
 */
class GLOBALBASE_PORT GB_Screen
{
public:
    /**
     * @brief 获取当前所有物理显示屏的尽可能完整的信息。
     *
     * 返回结果中，每个元素尽量对应一个“当前已连接且参与当前显示配置的物理显示目标”。
     * 在复制显示等场景下，不同物理显示屏可能共享同一块虚拟桌面区域。
     */
    static std::vector<GB_ScreenInfo> GetAllScreens();

    /**
     * @brief 获取当前整个虚拟桌面的包围矩形。
     *
     * 返回值单位为物理像素；若当前无法获取，则返回无效矩形。
     */
    static GB_Rectangle GetVirtualScreenRectangle();

    /**
     * @brief 截取整个虚拟桌面。
     *
     * 截图成功时，输出图像为 8 位 4 通道 BGRA 图像，Alpha 会被统一置为 255。
     */
    static bool CaptureVirtualScreen(GB_Image& screenImage);

    /**
     * @brief 截取虚拟桌面中的指定矩形区域。
     *
     * @param virtualScreenRectangle 虚拟桌面坐标系中的矩形，单位为物理像素。
     * @param screenImage            [out] 截得的图像。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 当输入矩形超出虚拟桌面边界时，会自动与虚拟桌面求交；
     * - 输入无效矩形时直接返回 false；
     * - 若求交后为空，则返回 false。
     */
    static bool CaptureVirtualScreen(const GB_Rectangle& virtualScreenRectangle, GB_Image& screenImage);
};

#endif
