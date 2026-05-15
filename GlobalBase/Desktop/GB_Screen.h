#ifndef GLOBALBASE_SCREEN_H_H
#define GLOBALBASE_SCREEN_H_H

#include "../GlobalBasePort.h"
#include "../Geometry/GB_Point2d.h"
#include "../Geometry/GB_Rectangle.h"
#include "../Geometry/GB_Polygon.h"
#include "../CV/GB_Image.h"
#include "../CV/GB_ColorRGBA.h"

#include <cstdint>
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
     * @brief 根据虚拟桌面坐标系中的点，获取包含该点的显示屏信息。
     *
     * @param point 虚拟桌面坐标系中的点，单位为物理像素。
     * @return 命中时返回对应显示屏信息；未命中时返回默认构造的 GB_ScreenInfo。
     *
     * 说明：
     * - point 使用虚拟桌面坐标系；
     * - 判定边界采用左闭右开、上闭下开的半开区间语义；
     * - 在复制显示等多个物理显示屏共享同一区域的场景下，返回当前枚举结果中的首个命中项。
     */
    static GB_ScreenInfo GetScreenFromPoint(const GB_Point2d& point);

    /**
     * @brief 获取当前主显示屏信息。
     *
     * 若当前能够识别到主显示屏，则返回其信息；否则返回默认构造的 GB_ScreenInfo。
     */
    static GB_ScreenInfo GetPrimaryScreen();

    /**
     * @brief 截取整个虚拟桌面。
     *
     * 截图成功时，输出图像为 8 位 4 通道 BGRA 图像，Alpha 会被统一置为 255。
     *
     * @param screenImage [out] 截得的图像。
     * @param withCursor  是否将当前系统光标叠加到截图结果中。true=带光标；false=不带光标。
     */
    static bool CaptureVirtualScreen(GB_Image& screenImage, bool withCursor = false);

    /**
     * @brief 截取虚拟桌面中的指定矩形区域。
     *
     * @param virtualScreenRectangle 虚拟桌面坐标系中的矩形，单位为物理像素。
     * @param screenImage            [out] 截得的图像。
     * @param withCursor             是否将当前系统光标叠加到截图结果中。true=带光标；false=不带光标。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 当输入矩形超出虚拟桌面边界时，会自动与虚拟桌面求交；
     * - 输入无效矩形时直接返回 false；
     * - 若求交后为空，则返回 false。
     */
    static bool CaptureVirtualScreen(const GB_Rectangle& virtualScreenRectangle, GB_Image& screenImage, bool withCursor = false);

    /**
     * @brief 截取第 screenIndex 个显示屏的完整画面。
     *
     * @param screenIndex 显示屏编号，0 基，对应 GetAllScreens() 返回顺序。
     * @param screenImage [out] 截得的图像。
     * @param withCursor  是否将当前系统光标叠加到截图结果中。true=带光标；false=不带光标。
     * @return true=成功；false=失败。
     */
    static bool CaptureScreen(int screenIndex, GB_Image& screenImage, bool withCursor = false);

    /**
     * @brief 截取第 screenIndex 个显示屏局部区域的画面。
     *
     * @param screenIndex          显示屏编号，0 基，对应 GetAllScreens() 返回顺序。
     * @param screenLocalRectangle 该显示屏局部坐标系中的矩形，左上角为 (0, 0)，单位为物理像素。
     * @param screenImage          [out] 截得的图像。
     * @param withCursor           是否将当前系统光标叠加到截图结果中。true=带光标；false=不带光标。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 输入矩形允许越界，内部会自动与该显示屏范围求交；
     * - 输入无效矩形时直接返回 false；
     * - 若求交后为空，则返回 false。
     */
    static bool CaptureScreen(int screenIndex, const GB_Rectangle& screenLocalRectangle, GB_Image& screenImage, bool withCursor = false);

    /**
     * @brief 根据 GDI 设备名截取指定显示屏的完整画面。
     *
     * @param gdiDeviceName 显示屏设备名，典型值如 "\\.\DISPLAY1"。
     * @param screenImage   [out] 截得的图像。
     * @param withCursor    是否将当前系统光标叠加到截图结果中。true=带光标；false=不带光标。
     * @return true=成功；false=失败。
     */
    static bool CaptureScreen(const std::string& gdiDeviceName, GB_Image& screenImage, bool withCursor = false);

    /**
     * @brief 根据 GDI 设备名截取指定显示屏局部区域的画面。
     *
     * @param gdiDeviceName       显示屏设备名，典型值如 "\\.\DISPLAY1"。
     * @param screenLocalRectangle 该显示屏局部坐标系中的矩形，左上角为 (0, 0)，单位为物理像素。
     * @param screenImage          [out] 截得的图像。
     * @param withCursor           是否将当前系统光标叠加到截图结果中。true=带光标；false=不带光标。
     * @return true=成功；false=失败。
     */
    static bool CaptureScreen(const std::string& gdiDeviceName, const GB_Rectangle& screenLocalRectangle, GB_Image& screenImage, bool withCursor = false);

    /**
     * @brief 将系统逻辑像素坐标转换为所在显示屏编号及该屏上的物理像素坐标。
     *
     * @param logicalPixelPoint         系统逻辑像素坐标。
     * @param screenIndex               [out] 命中的显示屏编号，0 基，对应 GetAllScreens() 返回顺序。
     * @param physicalPixelPointOnScreen[out] 该点在命中显示屏局部坐标系中的物理像素坐标，左上角为 (0, 0)。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 这里的“逻辑像素”统一采用系统 DPI（通常等于主显示屏 DPI）下的虚拟桌面坐标；
     * - 该接口用于在一个稳定、可复现的坐标定义下完成逻辑 / 物理坐标换算；
     * - 若输入点不落在任何显示屏范围内，则返回 false。
     */
    static bool LogicalPixelToPhysicalPixel(const GB_Point2d& logicalPixelPoint, int& screenIndex, GB_Point2d& physicalPixelPointOnScreen);

    /**
     * @brief 将指定显示屏局部坐标系中的物理像素坐标转换为系统逻辑像素坐标。
     *
     * @param screenIndex                显示屏编号，0 基，对应 GetAllScreens() 返回顺序。
     * @param physicalPixelPointOnScreen 该显示屏局部坐标系中的物理像素坐标，左上角为 (0, 0)。
     * @param logicalPixelPoint          [out] 转换得到的系统逻辑像素坐标。
     * @return true=成功；false=失败。
     *
     * 说明：
     * - 这里的“逻辑像素”定义与 LogicalPixelToPhysicalPixel() 完全一致；
     * - 若 screenIndex 非法，或输入点不在该显示屏有效范围内，则返回 false。
     */
    static bool PhysicalPixelToLogicalPixel(int screenIndex, const GB_Point2d& physicalPixelPointOnScreen, GB_Point2d& logicalPixelPoint);
};

/**
 * @brief 屏幕绘制对象类型。
 */
enum class GB_ScreenPaintObjectType
{
    /** @brief 无有效绘制对象。 */
    None = 0,

    /** @brief 多边形绘制对象。 */
    Polygon = 1,

    /** @brief 影像绘制对象。 */
    Image = 2
};

/**
 * @brief 屏幕多边形绘制参数。
 */
struct GB_ScreenPaintPolygonOptions
{
    /** @brief 多边形边界颜色，支持 Alpha 半透明。 */
    GB_ColorRGBA boundaryColor = GB_ColorRGBA::Red;

    /** @brief 多边形边界线宽，单位为屏幕物理像素。小于等于 0 表示不绘制边界。 */
    int boundaryThickness = 2;

    /** @brief 是否填充多边形内部。 */
    bool fill = false;

    /** @brief 多边形填充颜色，支持 Alpha 半透明。 */
    GB_ColorRGBA fillColor = GB_ColorRGBA(255, 0, 0, 64);

    /** @brief 是否启用抗锯齿。 */
    bool antialias = true;
};

/**
 * @brief 屏幕影像绘制参数。
 */
struct GB_ScreenPaintImageOptions
{
    /** @brief 影像绘制到屏幕虚拟桌面坐标系中的目标区域，单位为物理像素。 */
    GB_Rectangle screenRectangle;

    /** @brief 是否启用线性插值缩放。 */
    bool smoothResize = true;
};

/**
 * @brief 屏幕绘制对象快照。
 *
 * 说明：
 * - 该结构体由查询接口返回，是当前绘制对象的只读语义副本；
 * - 修改返回副本不会影响屏幕上已经绘制的对象；
 * - 当 objectType 为 Polygon 时 polygon / polygonOptions 有意义；
 * - 当 objectType 为 Image 时 image / imageOptions 有意义。
 */
struct GB_ScreenPaintObject
{
    /** @brief 绘制对象唯一标识，0 表示无效。 */
    uint64_t uid = 0;

    /** @brief 绘制对象类型。 */
    GB_ScreenPaintObjectType objectType = GB_ScreenPaintObjectType::None;

    /** @brief 多边形对象。 */
    GB_Polygon polygon;

    /** @brief 多边形绘制参数。 */
    GB_ScreenPaintPolygonOptions polygonOptions;

    /** @brief 影像对象。 */
    GB_Image image;

    /** @brief 影像绘制参数。 */
    GB_ScreenPaintImageOptions imageOptions;

    /** @brief 原始持久化显示时长，单位为毫秒。 */
    long long displayDurationMilliseconds = 0;

    /** @brief 查询时剩余显示时长，单位为毫秒。 */
    long long remainingMilliseconds = 0;
};

/**
 * @brief 屏幕顶层绘制工具类。
 *
 * 说明：
 * - 该类用于在屏幕虚拟桌面坐标系中临时绘制多边形或影像；
 * - 绘制采用进程内共享的透明置顶覆盖窗口，Paint 接口只提交绘制对象并立即返回 uid；
 * - 坐标统一使用物理像素，不使用 Qt / Win32 逻辑像素；
 * - 非 Windows 平台下接口不会产生实际绘制，Paint 接口返回 0。
 */
class GLOBALBASE_PORT GB_ScreenPainter
{
public:
    /**
     * @brief 绘制多边形。
     *
     * @param polygon                     要绘制的多边形，顶点坐标为虚拟桌面物理像素坐标。
     * @param paintOptions                绘制参数。
     * @param displayDurationMilliseconds 持久化显示时长，单位为毫秒；小于等于 0 时不绘制并返回 0。
     * @return 绘制对象 uid；失败返回 0。
     */
    static uint64_t PaintPolygon(const GB_Polygon& polygon, const GB_ScreenPaintPolygonOptions& paintOptions, long long displayDurationMilliseconds);

    /**
     * @brief 绘制多边形。
     */
    static uint64_t PaintPolygon(const GB_Polygon& polygon, const GB_ColorRGBA& boundaryColor, int boundaryThickness, bool fill, const GB_ColorRGBA& fillColor, long long displayDurationMilliseconds);

    /**
     * @brief 绘制影像。
     *
     * @param image                       要绘制的影像。
     * @param paintOptions                绘制参数。
     * @param displayDurationMilliseconds 持久化显示时长，单位为毫秒；小于等于 0 时不绘制并返回 0。
     * @return 绘制对象 uid；失败返回 0。
     */
    static uint64_t PaintImage(const GB_Image& image, const GB_ScreenPaintImageOptions& paintOptions, long long displayDurationMilliseconds);

    /**
     * @brief 绘制影像。
     */
    static uint64_t PaintImage(const GB_Image& image, const GB_Rectangle& screenRectangle, long long displayDurationMilliseconds);

    /**
     * @brief 获取当前仍处于显示期内的所有绘制对象 uid。
     */
    static std::vector<uint64_t> GetPaintedObjectUids();

    /**
     * @brief 获取指定 uid 对应的绘制对象快照。
     */
    static bool GetPaintedObject(uint64_t uid, GB_ScreenPaintObject& paintObject);

    /**
     * @brief 获取指定 uid 列表对应的绘制对象快照。
     */
    static std::vector<GB_ScreenPaintObject> GetPaintedObjects(const std::vector<uint64_t>& uids);

    /**
     * @brief 获取当前仍处于显示期内的全部绘制对象快照。
     */
    static std::vector<GB_ScreenPaintObject> GetAllPaintedObjects();

    /**
     * @brief 判断指定 uid 是否仍处于显示期内。
     */
    static bool IsPainting(uint64_t uid);

    /**
     * @brief 删除指定 uid 对应的绘制对象，并立即从屏幕上移除。
     */
    static bool RemovePaintedObject(uint64_t uid);

    /**
     * @brief 批量删除指定 uid 对应的绘制对象，并立即从屏幕上移除。
     */
    static size_t RemovePaintedObjects(const std::vector<uint64_t>& uids);

    /**
     * @brief 清空当前所有绘制对象，并立即从屏幕上移除。
     */
    static void Clear();

private:
    GB_ScreenPainter() = delete;
    ~GB_ScreenPainter() = delete;
    GB_ScreenPainter(const GB_ScreenPainter&) = delete;
    GB_ScreenPainter& operator=(const GB_ScreenPainter&) = delete;
};


#endif
