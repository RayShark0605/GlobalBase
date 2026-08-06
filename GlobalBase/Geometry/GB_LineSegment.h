#ifndef GLOBALBASE_LINESEGMENT_H_H
#define GLOBALBASE_LINESEGMENT_H_H

#include "../GlobalBasePort.h"
#include "../GB_BaseTypes.h"
#include "../GB_Math.h"
#include "GB_GeometryInterface.h"
#include "GB_Point2d.h"
#include <string>

class GB_Vector2d;
class GB_Rectangle;
class GB_Matrix3x3;

/**
 * @brief 二维线段。
 *
 * @details
 * GB_LineSegment 表示由两个端点确定的二维有限线段。线段在几何语义上通常不区分方向，
 * 即 (A, B) 与 (B, A) 描述的是同一条几何线段；但是本类在存储层面会保留 point1、point2
 * 的端点顺序，便于表达参数化方向、序列化顺序和调用方的原始输入顺序。
 *
 * @note 约定：
 * - 默认构造得到无效线段，两个端点均为 NaN 点。
 * - IsValid() 只要求两个端点坐标均为有限值；允许退化线段，即 point1 与 point2 重合。
 * - PointAt()、ParameterAt()、ToVector()、UnitDirectionVector()、Angle()、SideOfPoint() 等
 *   涉及方向或参数化的接口，均默认采用 point1 -> point2 作为参数化方向。
 * - operator== / operator!= 按存储顺序严格比较；若需要忽略端点顺序，请使用 IsSameLineSegment()。
 */
class GLOBALBASE_PORT GB_LineSegment : public GB_SerializableClass
{
public:
    /** @brief 第一个端点，同时也是默认参数化方向的起点。 */
    GB_Point2d point1 = GB_Point2d(GB_QuietNan, GB_QuietNan);

    /** @brief 第二个端点，同时也是默认参数化方向的终点。 */
    GB_Point2d point2 = GB_Point2d(GB_QuietNan, GB_QuietNan);

    /** @brief 无效线段常量，两个端点均为 NaN。 */
    static const GB_LineSegment Invalid;

    /** @brief 构造无效线段。 */
    GB_LineSegment();

    /**
     * @brief 通过两个端点构造线段。
     * @param point1 第一个端点。
     * @param point2 第二个端点。
     * @note 若任一端点无效，则构造结果为无效线段。
     */
    GB_LineSegment(const GB_Point2d& point1, const GB_Point2d& point2);

    /**
     * @brief 通过两个端点坐标构造线段。
     * @param x1 第一个端点 X 坐标。
     * @param y1 第一个端点 Y 坐标。
     * @param x2 第二个端点 X 坐标。
     * @param y2 第二个端点 Y 坐标。
     */
    GB_LineSegment(double x1, double y1, double x2, double y2);

    /**
     * @brief 通过起点、方向和长度构造线段。
     * @param startPoint 起点，即 point1。
     * @param direction 方向向量，只使用其方向，不要求是单位向量。
     * @param length 线段长度，必须为非负有限值。
     * @note length 为 0 时会构造退化线段；length 大于 0 时 direction 不能为零向量。
     */
    GB_LineSegment(const GB_Point2d& startPoint, const GB_Vector2d& direction, double length);

    virtual ~GB_LineSegment() override;

    /** @brief 获取类类型标识字符串。 */
    virtual const std::string& GetClassType() const override;

    /** @brief 获取类类型标识 Id。 */
    virtual uint64_t GetClassTypeId() const override;

    /** @brief 将线段重置为无效线段。 */
    void Reset();

    /**
     * @brief 设置两个端点。
     * @note 若任一端点无效，则重置为无效线段。
     */
    void Set(const GB_Point2d& point1, const GB_Point2d& point2);

    /**
     * @brief 通过两个端点坐标设置线段。
     * @note 若任一坐标不是有限值，则重置为无效线段。
     */
    void Set(double x1, double y1, double x2, double y2);

    /** @brief 检查线段是否有效。有效线段允许退化为点。 */
    bool IsValid() const;

    /** @brief 判断是否为退化线段，即长度不大于 tolerance。 */
    bool IsDegenerate(double tolerance = GB_Epsilon) const;

    /** @brief 获取线段长度。无效线段返回 NaN。 */
    double Length() const;

    /** @brief 获取线段长度平方。无效线段返回 NaN。 */
    double LengthSquared() const;

    /** @brief 获取线段中点。无效线段返回 NaN 点。 */
    GB_Point2d MidPoint() const;

    /** @brief 获取 point1 -> point2 的向量。无效线段返回 NaN 向量。 */
    GB_Vector2d ToVector() const;

    /** @brief 获取 point1 -> point2 的单位方向向量。无效或退化线段返回 NaN 向量。 */
    GB_Vector2d UnitDirectionVector(double tolerance = GB_Epsilon) const;

    /** @brief 获取 point1 -> point2 向量与 X 轴正方向的夹角，范围 [0, 2PI)。无效或退化线段返回 NaN。 */
    double Angle() const;

    /** @brief 获取线段的轴对齐包围矩形。无效线段返回无效矩形。 */
    GB_Rectangle BoundingRectangle() const;

    /** @brief 按存储顺序严格比较两个端点，不做容差比较。 */
    bool operator==(const GB_LineSegment& other) const;

    /** @brief 按存储顺序严格比较两个端点，不做容差比较。 */
    bool operator!=(const GB_LineSegment& other) const;

    /** @brief 判断是否按相同端点顺序近似相等。 */
    bool IsNearEqual(const GB_LineSegment& other, double tolerance = GB_Epsilon) const;

    /** @brief 判断是否为同一条几何线段，忽略 point1 / point2 的存储顺序。 */
    bool IsSameLineSegment(const GB_LineSegment& other, double tolerance = GB_Epsilon) const;

    /**
     * @brief 取参数 t 对应的线段参数化点：point1 + (point2 - point1) * t。
     * @note t=0 返回 point1，t=1 返回 point2；t 超出 [0,1] 时返回线段所在直线上的外推点。
     */
    GB_Point2d PointAt(double t) const;

    /**
     * @brief 计算点在线段 point1 -> point2 参数化方向上的投影参数。
     * @return 投影参数 t；无效或退化线段返回 NaN。
     * @note 本函数不要求 point 在线段上。t 位于 [0,1] 表示投影落在线段范围内。
     */
    double ParameterAt(const GB_Point2d& point) const;

    /** @brief 将点正交投影到线段所在直线上。退化线段返回 point1。 */
    GB_Point2d ProjectPointOnLine(const GB_Point2d& point) const;

    /** @brief 获取线段上距离指定点最近的点。退化线段返回 point1。 */
    GB_Point2d ClosestPointTo(const GB_Point2d& point) const;

    /** @brief 获取点到线段的最短距离。 */
    double DistanceTo(const GB_Point2d& point) const;

    /** @brief 获取点到线段最短距离的平方。 */
    double DistanceToSquared(const GB_Point2d& point) const;

    /** @brief 获取点到线段所在无限直线的距离。退化线段按点到 point1 的距离处理。 */
    double DistanceToLine(const GB_Point2d& point) const;

    /** @brief 获取两条线段之间的最短距离。相交时返回 0。 */
    double DistanceTo(const GB_LineSegment& other) const;

    /** @brief 返回端点顺序反转后的线段。 */
    GB_LineSegment Reversed() const;

    /** @brief 原地反转端点顺序。 */
    void Reverse();

    /** @brief 返回按字典序归一化端点顺序后的线段，用于稳定存储或作为无方向线段键值。 */
    GB_LineSegment NormalizedEndpointOrder() const;

    /** @brief 原地按字典序归一化端点顺序。 */
    void NormalizeEndpointOrder();

    /** @brief 返回平移后的线段。 */
    GB_LineSegment Offsetted(double deltaX, double deltaY) const;

    /** @brief 返回按向量平移后的线段。 */
    GB_LineSegment Offsetted(const GB_Vector2d& translation) const;

    /** @brief 原地平移线段。若平移量无效，则线段变为无效。 */
    void Offset(double deltaX, double deltaY);

    /** @brief 原地按向量平移线段。若向量无效，则线段变为无效。 */
    void Offset(const GB_Vector2d& translation);

    /** @brief 返回绕 center 逆时针旋转 angle 弧度后的线段。 */
    GB_LineSegment Rotated(double angle, const GB_Point2d& center = GB_Point2d::Origin) const;

    /** @brief 原地绕 center 逆时针旋转 angle 弧度。 */
    void Rotate(double angle, const GB_Point2d& center = GB_Point2d::Origin);

    /** @brief 返回以 center 为中心缩放后的线段。scaleFactor 可为负值。 */
    GB_LineSegment Scaled(double scaleFactor, const GB_Point2d& center) const;

    /** @brief 原地以 center 为中心缩放线段。scaleFactor 可为负值。 */
    void Scale(double scaleFactor, const GB_Point2d& center);

    /** @brief 返回矩阵变换后的线段，端点按点变换处理。 */
    GB_LineSegment Transformed(const GB_Matrix3x3& mat) const;

    /** @brief 原地执行矩阵变换，端点按点变换处理。 */
    void Transform(const GB_Matrix3x3& mat);

    /** @brief 在两个端点方向上各延伸 delta。delta 为负数时表示从两端缩短。 */
    GB_LineSegment Extended(double delta) const;

    /**
     * @brief 分别在 point1 端和 point2 端延伸指定长度。
     * @param deltaAtPoint1 point1 端的延伸长度；正数向 point1 外侧延伸，负数向内部缩短。
     * @param deltaAtPoint2 point2 端的延伸长度；正数向 point2 外侧延伸，负数向内部缩短。
     * @note 若缩短量超过线段长度导致端点交叉，则返回无效线段；恰好缩短为 0 长度时返回退化线段。
     */
    GB_LineSegment Extended(double deltaAtPoint1, double deltaAtPoint2) const;

    /** @brief 原地在两个端点方向上各延伸 delta。 */
    void Extend(double delta);

    /** @brief 原地分别在 point1 端和 point2 端延伸指定长度。 */
    void Extend(double deltaAtPoint1, double deltaAtPoint2);

    /** @brief 判断点是否在线段上（含端点，使用点到线段的最短距离容差）。 */
    bool IsContains(const GB_Point2d& point, double tolerance = GB_Epsilon) const;

    /**
     * @brief 判断点位于线段参数化方向 point1 -> point2 的哪一侧。
     * @return +1 表示左侧，0 表示共线，-1 表示右侧。
     */
    int SideOfPoint(const GB_Point2d& point, double tolerance = GB_Epsilon) const;

    /** @brief 判断两条线段方向是否平行。退化线段返回 false。 */
    bool IsParallelTo(const GB_LineSegment& other, double tolerance = GB_Epsilon) const;

    /** @brief 判断两条线段方向是否垂直。退化线段返回 false。 */
    bool IsPerpendicularTo(const GB_LineSegment& other, double tolerance = GB_Epsilon) const;

    /** @brief 判断两条线段是否位于同一直线上；退化线段按端点到另一条线段所在直线的距离处理。 */
    bool IsCollinearWith(const GB_LineSegment& other, double tolerance = GB_Epsilon) const;

    /** @brief 获取两条线段的无方向夹角，范围 [0, PI/2]。无效或退化线段返回 NaN。 */
    double AngleBetween(const GB_LineSegment& other) const;

    /** @brief 判断两条线段是否相交（含端点接触和共线重叠）。 */
    bool IsIntersects(const GB_LineSegment& other, double tolerance = GB_Epsilon) const;

    /**
     * @brief 计算两条线段的相交关系。
     * @param other 另一条线段。
     * @param outIntersection 输出交点；仅返回 1 时有效。
     * @param outOverlap 输出重叠线段；仅返回 2 时有效。
     * @param tolerance 几何容差。
     * @return 0 表示不相交；1 表示交于一个点；2 表示共线重叠。
     * @note 对共线重叠，outOverlap 的端点顺序按当前线段 point1 -> point2 的方向输出。
     */
    int Intersect(const GB_LineSegment& other, GB_Point2d& outIntersection, GB_LineSegment& outOverlap, double tolerance = GB_Epsilon) const;

    /** @brief 序列化为可读字符串，格式为：(GB_LineSegment x1,y1,x2,y2)。 */
    virtual std::string SerializeToString() const override;

    /** @brief 序列化为二进制字节缓冲区。 */
    virtual GB_ByteBuffer SerializeToBinary() const override;

    /** @brief 从 SerializeToString() 生成的字符串反序列化。失败时保持当前对象原值不变。 */
    virtual bool Deserialize(const std::string& data) override;

    /** @brief 从 SerializeToBinary() 生成的字节缓冲区反序列化。失败时保持当前对象原值不变。 */
    virtual bool Deserialize(const GB_ByteBuffer& data) override;
};

#endif
