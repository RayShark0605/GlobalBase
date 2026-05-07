#ifndef GLOBALBASE_POLYLINE_H_H
#define GLOBALBASE_POLYLINE_H_H

#include "../GlobalBasePort.h"
#include "../GB_BaseTypes.h"
#include "../GB_Math.h"
#include "GB_GeometryInterface.h"
#include "GB_Point2d.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

class GB_LineSegment;
class GB_Matrix3x3;
class GB_Rectangle;
class GB_Vector2d;
class GB_Polygon;

/**
 * @brief 二维开放多段线。
 *
 * @details
 * GB_Polyline 表示由一组按顺序连接的二维顶点构成的有限折线：
 * - 不会自动对顶点去重；
 * - 允许相邻重复顶点和零长度线段；
 * - 允许首尾顶点相同，此时 IsClosed() 返回 true，但本类仍按“显式开放折线”存储和处理；
 * - 不会自动做简化、纠偏、打断、拓扑修复或方向调整。
 *
 * 有效性约定：
 * - 至少 2 个有效顶点时，对象有效；
 * - 顶点坐标必须均为有限值；
 * - 有效对象允许退化，例如所有顶点完全重合导致 Length() 为 0。
 * - 在“没有外部并发写入同一对象”的前提下，可被多个线程并发只读访问。
 */
class GLOBALBASE_PORT GB_Polyline : public GB_SerializableClass
{
public:
    /** @brief 无效多段线常量。 */
    static const GB_Polyline Invalid;

    /** @brief 构造空对象。 */
    GB_Polyline();

    /**
     * @brief 以顶点序列构造。
     * @param vertices 顶点序列。
     * @note 不会自动移除重复顶点；若顶点数不足 2 或任一顶点无效，则构造为空对象。
     */
    explicit GB_Polyline(const std::vector<GB_Point2d>& vertices);

    /**
     * @brief 以顶点序列构造（移动版本）。
     * @param vertices 顶点序列。
     * @note 不会自动移除重复顶点；若顶点数不足 2 或任一顶点无效，则构造为空对象。
     */
    explicit GB_Polyline(std::vector<GB_Point2d>&& vertices);

    /** @brief 以初始化列表构造。 */
    GB_Polyline(std::initializer_list<GB_Point2d> vertices);

    /** @brief 以线段构造两点多段线。 */
    explicit GB_Polyline(const GB_LineSegment& segment);

    virtual ~GB_Polyline() override;

    GB_Polyline(const GB_Polyline& other);
    GB_Polyline(GB_Polyline&& other) noexcept;
    GB_Polyline& operator=(const GB_Polyline& other);
    GB_Polyline& operator=(GB_Polyline&& other) noexcept;

    /** @brief 获取固定类名。 */
    virtual const std::string& GetClassType() const override;

    /** @brief 获取固定类类型 Id。 */
    virtual uint64_t GetClassTypeId() const override;

    /** @brief 清空顶点并使对象变为无效。 */
    void Clear();

    /** @brief 重置为无效空对象。 */
    void Reset();

    /**
     * @brief 设置顶点序列。
     * @return 设置后对象有效返回 true；否则对象被清空并返回 false。
     * @note 不会自动对相邻重复点或全局重复点去重。
     */
    bool SetVertices(const std::vector<GB_Point2d>& vertices);

    /** @brief 设置顶点序列（移动版本）。 */
    bool SetVertices(std::vector<GB_Point2d>&& vertices);

    /** @brief 为顶点数组预留容量，不改变当前顶点。 */
    void Reserve(size_t count);

    /** @brief 追加一个顶点。顶点无效时返回 false。 */
    bool AddVertex(const GB_Point2d& vertex);

    /** @brief 在指定位置插入顶点。index 可等于 GetNumVertices() 表示追加。 */
    bool InsertVertex(size_t index, const GB_Point2d& vertex);

    /** @brief 修改指定顶点。 */
    bool SetVertex(size_t index, const GB_Point2d& vertex);

    /** @brief 删除指定顶点。删除后顶点数可能不足 2，此时对象变为无效但保留剩余顶点。 */
    bool RemoveVertex(size_t index);

    /** @brief 是否没有任何顶点。 */
    bool IsEmpty() const;

    /** @brief 是否至少包含 2 个有效顶点。 */
    bool IsValid() const;

    /** @brief 是否退化为长度不大于 tolerance 的多段线。 */
    bool IsDegenerate(double tolerance = GB_Epsilon) const;

    /** @brief 首尾顶点是否在容差内相等。 */
    bool IsClosed(double tolerance = GB_Epsilon) const;

    /** @brief 获取顶点数。 */
    size_t GetNumVertices() const;

    /** @brief 获取线段数，等于有效顶点数减 1。 */
    size_t GetNumSegments() const;

    /** @brief 获取内部顶点数组的只读引用。 */
    const std::vector<GB_Point2d>& GetVertices() const;

    /** @brief 获取指定顶点；索引越界时返回 NaN 点。 */
    GB_Point2d GetVertex(size_t index) const;

    /** @brief 尝试获取指定顶点。 */
    bool TryGetVertex(size_t index, GB_Point2d& outVertex) const;

    /** @brief 获取指定线段；索引越界时返回无效线段。 */
    GB_LineSegment GetSegment(size_t index) const;

    /** @brief 尝试获取指定线段。 */
    bool TryGetSegment(size_t index, GB_LineSegment& outSegment) const;

    /** @brief 获取全部线段。 */
    std::vector<GB_LineSegment> GetSegments() const;

    /** @brief 获取总长度。无效对象返回 NaN。 */
    double Length() const;

    /** @brief 获取轴对齐包围矩形。无效对象返回无效矩形。 */
    GB_Rectangle BoundingRectangle() const;

    /** @brief 判断是否存在相邻重复顶点。 */
    bool HasDuplicateAdjacentVertices(double tolerance = GB_Epsilon) const;

    /** @brief 判断是否存在零长度线段。 */
    bool HasZeroLengthSegments(double tolerance = GB_Epsilon) const;

    /** @brief 判断点是否位于多段线上。 */
    bool IsContains(const GB_Point2d& point, double tolerance = GB_Epsilon) const;

    /** @brief 获取多段线上距离 point 最近的点。失败返回 NaN 点。 */
    GB_Point2d ClosestPointTo(const GB_Point2d& point) const;

    /** @brief 获取点到多段线的最短距离。 */
    double DistanceTo(const GB_Point2d& point) const;

    /** @brief 获取点到多段线的最短距离平方。 */
    double DistanceToSquared(const GB_Point2d& point) const;

    /**
     * @brief 最近点查询结果。
     */
    struct ClosestPointResult
    {
        /** @brief 查询是否成功。 */
        bool succeeded = false;

        /** @brief 最近点在整条多段线上的归一化弧长参数，范围 [0, 1]；退化多段线返回 0。 */
        double parameter = GB_QuietNan;

        /** @brief 最近点所在的线段索引。 */
        size_t segmentIndex = 0;

        /** @brief 最近点在线段上的局部参数，范围 [0, 1]。 */
        double segmentParameter = GB_QuietNan;

        /** @brief 最短距离。 */
        double distance = GB_QuietNan;

        /** @brief 最近点坐标。 */
        GB_Point2d closestPoint;
    };

    /** @brief 获取点到多段线的最近点详细结果。 */
    ClosestPointResult GetClosestPointResult(const GB_Point2d& point) const;

    /** @brief 按弧长距离取点。clampToRange 为 true 时会限制到 [0, Length()]。 */
    GB_Point2d PointAtDistance(double distance, bool clampToRange = true) const;

    /** @brief 按归一化弧长参数取点。t=0 表示起点，t=1 表示终点。 */
    GB_Point2d PointAtNormalizedLength(double t, bool clampToRange = true) const;

    /** @brief 返回反向顶点顺序的新对象。 */
    GB_Polyline Reversed() const;

    /** @brief 原地反转顶点顺序。 */
    void Reverse();

    /** @brief 返回平移后的新对象。 */
    GB_Polyline Offsetted(double deltaX, double deltaY) const;

    /** @brief 返回按向量平移后的新对象。 */
    GB_Polyline Offsetted(const GB_Vector2d& translation) const;

    /** @brief 原地平移。平移量无效时对象被重置。 */
    void Offset(double deltaX, double deltaY);

    /** @brief 原地按向量平移。向量无效时对象被重置。 */
    void Offset(const GB_Vector2d& translation);

    /** @brief 返回绕 center 逆时针旋转 angle 弧度后的新对象。 */
    GB_Polyline Rotated(double angle, const GB_Point2d& center = GB_Point2d::Origin) const;

    /** @brief 原地绕 center 逆时针旋转 angle 弧度。 */
    void Rotate(double angle, const GB_Point2d& center = GB_Point2d::Origin);

    /** @brief 返回以 center 为中心等比缩放后的新对象。 */
    GB_Polyline Scaled(double scaleFactor, const GB_Point2d& center) const;

    /** @brief 返回以 center 为中心非等比缩放后的新对象。 */
    GB_Polyline Scaled(double scaleX, double scaleY, const GB_Point2d& center) const;

    /** @brief 原地以 center 为中心等比缩放。 */
    void Scale(double scaleFactor, const GB_Point2d& center);

    /** @brief 原地以 center 为中心非等比缩放。 */
    void Scale(double scaleX, double scaleY, const GB_Point2d& center);

    /** @brief 返回矩阵变换后的新对象。 */
    GB_Polyline Transformed(const GB_Matrix3x3& mat) const;

    /** @brief 原地矩阵变换。 */
    void Transform(const GB_Matrix3x3& mat);

    /** @brief 返回移除相邻重复顶点后的新对象。 */
    GB_Polyline RemovedDuplicateAdjacentVertices(double tolerance = GB_Epsilon) const;

    /** @brief 原地移除相邻重复顶点。 */
    void RemoveDuplicateAdjacentVertices(double tolerance = GB_Epsilon);

    /**
     * @brief 使用 Ramer-Douglas-Peucker 算法简化顶点。
     * @param tolerance 简化容差；会按绝对值使用。
     * @return 简化后的多段线。无效对象返回无效多段线。
     * @note 本函数只简化返回结果，不修改当前对象。
     */
    GB_Polyline Simplified(double tolerance) const;

    /**
     * @brief 按归一化弧长参数 t 将多段线断开为两条多段线。
     * @param t 归一化弧长参数，0 为起点，1 为终点；超出范围时会 clamp 到 [0, 1]。
     * @return first 为 [0, t] 段，second 为 [t, 1] 段；断点会同时出现在两条结果中。
     */
    std::pair<GB_Polyline, GB_Polyline> SplitAt(double t) const;

    /**
     * @brief 按归一化弧长参数截取一段多段线。
     * @param t1 起始归一化弧长参数。
     * @param t2 终止归一化弧长参数。
     * @return 截取结果。若 t1 > t2，则结果按 t1 -> t2 方向返回。
     */
    GB_Polyline SubPolyline(double t1, double t2) const;

    /** @brief 尝试转换为 GB_Polygon。只有首尾闭合且结果顶点数足够时返回 true。 */
    bool TryToPolygon(GB_Polygon& outPolygon, bool removeDuplicatedClosingVertex = true) const;

    /** @brief 转换为 GB_Polygon。失败时返回无效多边形。 */
    GB_Polygon ToPolygon(bool removeDuplicatedClosingVertex = true) const;

    /** @brief 按顶点顺序严格相等比较，不做容差比较。 */
    bool operator==(const GB_Polyline& other) const;

    /** @brief 按顶点顺序严格不等比较，不做容差比较。 */
    bool operator!=(const GB_Polyline& other) const;

    /** @brief 按顶点顺序进行容差比较。 */
    bool IsNearEqual(const GB_Polyline& other, double tolerance = GB_Epsilon) const;

    /** @brief 序列化为可读字符串。 */
    virtual std::string SerializeToString() const override;

    /** @brief 序列化为二进制字节流。 */
    virtual GB_ByteBuffer SerializeToBinary() const override;

    /** @brief 从可读字符串反序列化。 */
    virtual bool Deserialize(const std::string& data) override;

    /** @brief 从二进制字节流反序列化。 */
    virtual bool Deserialize(const GB_ByteBuffer& data) override;

private:
    struct CacheData;

    std::vector<GB_Point2d> vertices;

    mutable std::atomic<std::uint64_t> cacheVersion{ 1 };
    mutable std::shared_ptr<const CacheData> cache;

    void InvalidateCaches();
    std::uint64_t GetCurrentCacheVersion() const;
    std::shared_ptr<const CacheData> GetOrBuildCache() const;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
