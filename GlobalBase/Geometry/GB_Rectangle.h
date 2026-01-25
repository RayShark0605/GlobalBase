#ifndef GLOBALBASE_RECTANGLE_H_H
#define GLOBALBASE_RECTANGLE_H_H

#include "../GlobalBasePort.h"
#include "../GB_Math.h"
#include "GB_GeometryInterface.h"
#include "GB_Point2d.h"

#include <vector>

class GB_Vector2d;
class GB_Matrix3x3;

/**
 * @brief 二维轴对齐矩形（Axis-Aligned Rectangle / AABB）。
 *
 * - 数据采用 (minX, minY, maxX, maxY) 表示。
 * - 默认构造为无效矩形（四个值均为 NaN）。
 * - 允许退化：宽或高为 0（线段/点）。
 */
class GLOBALBASE_PORT GB_Rectangle : public GB_SerializableClass
{
public:
    double minX = GB_QuietNan;
    double minY = GB_QuietNan;
    double maxX = GB_QuietNan;
    double maxY = GB_QuietNan;

    // 无效矩形（min/max 均为 NaN）。
    static const GB_Rectangle Invalid;

    GB_Rectangle();
    explicit GB_Rectangle(const GB_Point2d& point);
    GB_Rectangle(const GB_Point2d& corner1, const GB_Point2d& corner2);
    GB_Rectangle(const GB_Point2d& center, double width, double height);
    GB_Rectangle(double minX, double minY, double maxX, double maxY);
    virtual ~GB_Rectangle() override;

    virtual const std::string& GetClassType() const override;
    virtual uint64_t GetClassTypeId() const override;

    // 设为无效（min/max 均为 NaN）。
    void Reset();

    // 直接设置并自动归一化（保证 min<=max）。
    void Set(double minX, double minY, double maxX, double maxY);

    // 以单点构造退化矩形（min=max=该点）。
    void SetFromPoint(const GB_Point2d& point);

    // 以两个角点构造矩形（自动归一化）。
    void SetFromCorners(const GB_Point2d& corner1, const GB_Point2d& corner2);

    // 通过中心点与宽高构造矩形。width/height 必须为非负且有限。
    // 成功返回 true；失败时矩形变为无效并返回 false。
    bool SetFromCenter(const GB_Point2d& center, double width, double height);

    bool IsValid() const;

    // 归一化：若 min>max 则交换（要求四个值 finite）。
    void Normalize();

    // 宽 / 高。
    double Width() const;
    double Height() const;

    // 周长 / 面积 / 对角线长度。
    double Perimeter() const;
    double Area() const;
    double DiagLength() const;
    double DiagLengthSquared() const;

    GB_Point2d MinPoint() const;
    GB_Point2d MaxPoint() const;
    GB_Point2d Center() const;

    // 四个角点：(minX,minY) -> (maxX,minY) -> (maxX,maxY) -> (minX,maxY)
    std::vector<GB_Point2d> GetCorners() const;

    // 平移。
    GB_Rectangle Offsetted(double deltaX, double deltaY) const;
    void Offset(double deltaX, double deltaY);
    GB_Rectangle Offsetted(const GB_Vector2d& translation) const;
    void Offset(const GB_Vector2d& translation);

    // 缩放（默认绕矩形中心缩放）。
    GB_Rectangle Scaled(double scaleFactor) const;
    GB_Rectangle Scaled(double scaleFactor, const GB_Point2d& center) const;
    GB_Rectangle Scaled(double scaleX, double scaleY, const GB_Point2d& center) const;
    void Scale(double scaleFactor);
    void Scale(double scaleFactor, const GB_Point2d& center);
    void Scale(double scaleX, double scaleY, const GB_Point2d& center);

    // Buffer / Inflate：正数外扩，负数内缩。
    GB_Rectangle Buffered(double delta) const;
    GB_Rectangle Buffered(double deltaX, double deltaY) const;
    void Buffer(double delta);
    void Buffer(double deltaX, double deltaY);

    // 通过点/点列/矩形/矩形列扩充自身（无效矩形会被直接“初始化”为输入范围）。
    void Expand(const GB_Point2d& point);
    void Expand(const std::vector<GB_Point2d>& points);
    void Expand(const GB_Rectangle& rect);
    void Expand(const std::vector<GB_Rectangle>& rectangles);

    // 求交 / 判断相交（含边界）。tolerance 用于放宽判定（例如 1e-9）。
    bool IsIntersects(const GB_Rectangle& other, double tolerance = GB_Epsilon) const;
    GB_Rectangle Intersected(const GB_Rectangle& other, double tolerance = GB_Epsilon) const;

    // 包含判定（含边界）。
    bool IsContains(const GB_Point2d& point, double tolerance = GB_Epsilon) const;
    bool IsContains(const GB_Rectangle& other, double tolerance = GB_Epsilon) const;

    // 点到矩形的最近距离（矩形内部为 0）。
    double DistanceTo(const GB_Point2d& point) const;
    double DistanceToSquared(const GB_Point2d& point) const;

    // 将点 clamp 到矩形内（若无效则返回 NaN 点）。
    GB_Point2d ClampPoint(const GB_Point2d& point) const;

    // 仿射变换后，返回四角点变换结果的轴对齐包围盒。
    GB_Rectangle Transformed(const GB_Matrix3x3& mat) const;
    void Transform(const GB_Matrix3x3& mat);

    // 逐分量严格相等（不做容差比较）。
    bool operator==(const GB_Rectangle& other) const;
    bool operator!=(const GB_Rectangle& other) const;

    // 容差比较（四个分量分别近似相等）。
    bool IsNearEqual(const GB_Rectangle& other, double tolerance = GB_Epsilon) const;

    // 序列化。
    virtual std::string SerializeToString() const override;
    virtual GB_ByteBuffer SerializeToBinary() const override;

    // 反序列化。
    virtual bool Deserialize(const std::string& data) override;
    virtual bool Deserialize(const GB_ByteBuffer& data) override;
};


#endif