#ifndef GLOBALBASE_POLYGON_H_H
#define GLOBALBASE_POLYGON_H_H

#include "../GlobalBasePort.h"
#include "../GB_Math.h"
#include "GB_GeometryInterface.h"
#include "GB_Point2d.h"
#include "GB_Rectangle.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief 二维多边形（闭合折线环）。
 *
 * @remarks
 * - 本类存储的是“顶点序列 + 隐式首尾闭合”的二维闭合折线环；不会自动在尾部补第一个点。
 * - 本类不会自动修复或简化输入；相邻重复点、零长度边、自相交、全点共线、部分点共线等都会被原样保留。
 * - 面积相关接口返回的是该闭合折线环的“代数面积 / shoelace area”，
 *   对自相交多边形并不等同于“填充后的并集面积”。
 * - 当以 std::vector<GB_Point2d> 构造时，内部仍会尽量使用精确有理数方式进行判定与面积计算；
 *   当以字符串坐标构造时，会保留原始字符串并按精确有理数解析，从而避免 double 舍入误差。
 * - 只读成员函数不会修改对象内部状态，不使用惰性缓存，因此同一个 GB_Polygon 在“无外部并发写入”的前提下，
 *   可被多个线程无锁并发读取。
 */
class GLOBALBASE_PORT GB_Polygon : public GB_SerializableClass
{
public:
    enum class CoordinateStorageMode : std::uint8_t
    {
        Double = 0,
        ExactString = 1
    };

    enum class Orientation : std::uint8_t
    {
        Clockwise = 0,
        CounterClockwise = 1,
        Degenerate = 2
    };

    enum class PointContainment : std::uint8_t
    {
        Outside = 0,
        OnBoundary = 1,
        Inside = 2
    };

    using ExactStringVertex = std::pair<std::string, std::string>;

    static const GB_Polygon Invalid;

    GB_Polygon();
    explicit GB_Polygon(const std::vector<GB_Point2d>& vertices);
    GB_Polygon(std::initializer_list<GB_Point2d> vertices);
    explicit GB_Polygon(const std::vector<ExactStringVertex>& exactStringVertices);
    GB_Polygon(std::initializer_list<ExactStringVertex> exactStringVertices);
    virtual ~GB_Polygon() override;

    GB_Polygon(const GB_Polygon& other);
    GB_Polygon(GB_Polygon&& other) noexcept;
    GB_Polygon& operator=(const GB_Polygon& other);
    GB_Polygon& operator=(GB_Polygon&& other) noexcept;

    virtual const std::string& GetClassType() const override;
    virtual uint64_t GetClassTypeId() const override;

    void Clear();

    bool SetVertices(const std::vector<GB_Point2d>& vertices);
    bool SetVertices(const std::vector<ExactStringVertex>& exactStringVertices);

    CoordinateStorageMode GetCoordinateStorageMode() const;
    bool UsesExactStringCoordinates() const;

    bool IsEmpty() const;
    bool IsValid() const;

    // 本类总是按“隐式首尾相连”解释顶点序列；当对象有效且至少有 2 个点时，视为有闭合边界。
    bool IsClosed() const;

    size_t GetNumVertices() const;
    size_t GetNumEdges() const;

    // 仅在 Double 模式下返回原始存储的顶点；ExactString 模式下通常为空。
    const std::vector<GB_Point2d>& GetDoubleVertices() const;

    // 仅在 ExactString 模式下返回原始存储的精确字符串坐标；Double 模式下通常为空。
    const std::vector<ExactStringVertex>& GetExactStringVertices() const;

    // 对任意模式都返回“可用于显示/近似计算”的 double 顶点序列；若某个点无法安全转换为有限 double，则该点为 NaN。
    std::vector<GB_Point2d> GetVerticesAsDouble() const;

    bool TryGetVertexAsDouble(size_t index, GB_Point2d& outVertex) const;
    bool TryGetVertexExactStrings(size_t index, std::string& outXText, std::string& outYText) const;

    GB_Rectangle GetBoundingBox() const;
    bool TryGetBoundingBoxExactStrings(std::string& outMinXText, std::string& outMinYText, std::string& outMaxXText, std::string& outMaxYText) const;

    bool HasDuplicateAdjacentVertices() const;
    bool HasZeroLengthEdges() const;
    bool HasCollinearAdjacentTriples() const;
    bool AreAllVerticesCollinear() const;

    // 简单多边形判定：不允许非相邻边相交，也不允许边重叠/退化为破坏简单性的情况。
    bool IsSimple() const;

    // 这里的“自相交/非简单”按广义边界非简单理解；
    // 因此相邻重复点、重叠边等导致的 non-simple 情况也会返回 true。
    bool HasSelfIntersections() const;

    Orientation GetOrientation() const;
    bool IsClockwise() const;
    bool IsCounterClockwise() const;

    // 代数面积（shoelace / signed area）。
    double GetSignedArea() const;
    double GetUnsignedArea() const;

    // 精确字符串形式的代数面积；成功时通常为整数、小数对应的有理数形式（例如 "3/2"、"-7"）。
    // 若对象无效则返回空串。
    std::string GetSignedAreaExactString() const;

    // 周长是欧氏长度和，通常不再是有理数；因此这里只提供 double 近似值。
    double GetPerimeter() const;

    // 按 odd-even 规则进行点包含判定；对自相交闭合折线也可使用。
    PointContainment ClassifyPointOddEven(const GB_Point2d& point) const;
    PointContainment ClassifyPointOddEven(const std::string& xText, const std::string& yText) const;

    GB_Polygon Reversed() const;
    void Reverse();

    bool operator==(const GB_Polygon& other) const;
    bool operator!=(const GB_Polygon& other) const;

    // 顶点数量、顶点顺序必须一致；比较时统一转为 double，并按点间距离 <= tolerance 判定。
    bool IsNearEqual(const GB_Polygon& other, double tolerance = GB_Epsilon) const;

    virtual std::string SerializeToString() const override;
    virtual GB_ByteBuffer SerializeToBinary() const override;

    virtual bool Deserialize(const std::string& data) override;
    virtual bool Deserialize(const GB_ByteBuffer& data) override;

private:
    CoordinateStorageMode storageMode = CoordinateStorageMode::Double;
    std::vector<GB_Point2d> vertices;
    std::vector<ExactStringVertex> exactStringVertices;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
