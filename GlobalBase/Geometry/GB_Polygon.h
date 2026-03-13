#ifndef GLOBALBASE_POLYGON_H_H
#define GLOBALBASE_POLYGON_H_H

#include "../GlobalBasePort.h"
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

class GB_Rectangle;

/**
 * @brief 二维闭合折线环。
 *
 * @details
 * 本类表示的是“按给定顶点顺序、隐式首尾相连”的二维闭合折线边界：
 * - 不会自动在末尾补上首点；
 * - 允许输入退化或非简单环，例如相邻重复点、零长度边、自相交、全点共线；
 * - 不会自动做去重、纠正方向、简化或 make-valid。
 *
 * 坐标支持两种存储模式：
 * - Double：直接保存 `GB_Point2d`；
 * - ExactString：保存原始坐标字符串，并按精确有理数解析，用于减少 double 舍入误差带来的判定偏差。
 *
 * 几何语义说明：
 * - 面积相关接口返回的是鞋带公式（shoelace formula）得到的“代数面积”；
 * - 对自相交环，它不等价于填充区域的并集面积；
 * - `PointContainment` 采用 odd-even 规则；
 * - `IsSimple()` 的含义是“简单闭合环”，要求边只允许在相邻边公共端点处接触；
 * - `HasSelfIntersections()` 为兼容现有接口语义，实际表达的是“边界不是 simple ring”，
 *   因此退化边界、重复相邻顶点等情况也会返回 true，而不局限于严格意义上的几何交叉。
 *
 * 有效性约定：
 * - 本类将“至少 2 个顶点的隐式闭合环”视为有效对象；
 * - 这比传统多边形通常要求的“至少 3 个顶点”更宽松，便于表达退化闭合边界；
 * - `SetVertices()` 会写入传入顶点序列，然后以返回值告知该对象是否达到本类的有效性标准。
 *
 * 线程说明：
 * - 只读接口内部带有惰性缓存；
 * - 在“没有外部并发写入同一对象”的前提下，可被多个线程并发读取；
 * - 本类本身不是读写并发安全容器。
 */
class GLOBALBASE_PORT GB_Polygon : public GB_SerializableClass
{
public:
    /**
     * @brief 坐标存储模式。
     */
    enum class CoordinateStorageMode : std::uint8_t
    {
        /** 以 double 顶点直接存储。 */
        Double = 0,

        /** 以精确字符串形式存储原始坐标文本。 */
        ExactString = 1
    };

    /**
     * @brief 闭合环方向。
     */
    enum class Orientation : std::uint8_t
    {
        /** 顺时针。 */
        Clockwise = 0,

        /** 逆时针。 */
        CounterClockwise = 1,

        /** 退化，无法给出稳定方向（例如面积为 0）。 */
        Degenerate = 2
    };

    /**
     * @brief 点相对闭合环的位置。
     */
    enum class PointContainment : std::uint8_t
    {
        /** 在外部。 */
        Outside = 0,

        /** 在边界上。 */
        OnBoundary = 1,

        /** 在内部。 */
        Inside = 2
    };

    /**
     * @brief 精确字符串顶点。
     *
     * first 为 x 坐标文本，second 为 y 坐标文本。
     */
    using ExactStringVertex = std::pair<std::string, std::string>;

    /**
     * @brief 无效多边形常量。
     */
    static const GB_Polygon Invalid;

    /**
     * @brief 构造空对象。
     */
    GB_Polygon();

    /**
     * @brief 以 double 顶点序列构造。
     * @param vertices 顶点序列。
     *
     * @note 顶点会被写入对象；若顶点数小于 2，则对象会处于“已写入但无效”的状态。
     */
    explicit GB_Polygon(const std::vector<GB_Point2d>& vertices);

    /**
     * @brief 以 double 顶点初始化列表构造。
     */
    GB_Polygon(std::initializer_list<GB_Point2d> vertices);

    /**
     * @brief 以精确字符串顶点序列构造。
     * @param exactStringVertices 顶点序列。
     *
     * @note 每个坐标字符串都会先做 ASCII trim，再按精确有理数解析校验。
     *       若顶点数小于 2，则对象会处于“已写入但无效”的状态。
     */
    explicit GB_Polygon(const std::vector<ExactStringVertex>& exactStringVertices);

    /**
     * @brief 以精确字符串顶点初始化列表构造。
     */
    GB_Polygon(std::initializer_list<ExactStringVertex> exactStringVertices);

    explicit GB_Polygon(const GB_Rectangle& rectangle);

    virtual ~GB_Polygon() override;

    GB_Polygon(const GB_Polygon& other);
    GB_Polygon(GB_Polygon&& other) noexcept;
    GB_Polygon& operator=(const GB_Polygon& other);
    GB_Polygon& operator=(GB_Polygon&& other) noexcept;

    /**
     * @brief 获取固定类名。
     */
    virtual const std::string& GetClassType() const override;

    /**
     * @brief 获取固定类类型 Id。
     */
    virtual uint64_t GetClassTypeId() const override;

    /**
     * @brief 清空对象并恢复到默认空状态。
     */
    void Clear();

    /**
     * @brief 设置 double 顶点序列。
     * @param vertices 顶点序列。
     * @return 当写入后顶点数不少于 2 时返回 true，否则返回 false。
     *
     * @note 只要输入坐标本身合法，即使返回 false，顶点序列仍会被写入对象。
     */
    bool SetVertices(const std::vector<GB_Point2d>& vertices);

    /**
     * @brief 设置 double 顶点序列（移动版本）。
     */
    bool SetVertices(std::vector<GB_Point2d>&& vertices);

    /**
     * @brief 设置精确字符串顶点序列。
     * @param exactStringVertices 顶点序列。
     * @return 当写入后顶点数不少于 2 时返回 true，否则返回 false。
     *
     * @note 只要所有坐标字符串都能成功解析，即使返回 false，顶点序列仍会被写入对象。
     */
    bool SetVertices(const std::vector<ExactStringVertex>& exactStringVertices);

    /**
     * @brief 设置精确字符串顶点序列（移动版本）。
     */
    bool SetVertices(std::vector<ExactStringVertex>&& exactStringVertices);

    /**
     * @brief 获取当前坐标存储模式。
     */
    CoordinateStorageMode GetCoordinateStorageMode() const;

    /**
     * @brief 是否使用精确字符串坐标模式。
     */
    bool UsesExactStringCoordinates() const;

    /**
     * @brief 是否为空对象。
     */
    bool IsEmpty() const;

    /**
     * @brief 是否达到本类的有效性标准。
     * @return 顶点数不少于 2 时返回 true。
     */
    bool IsValid() const;

    /**
     * @brief 是否按本类语义视为闭合。
     * @return 有效对象返回 true。
     *
     * @note 本类始终按“隐式首尾相连”解释顶点序列，因此这里不检查“首点是否显式重复”。
     */
    bool IsClosed() const;

    /**
     * @brief 获取顶点数。
     */
    size_t GetNumVertices() const;

    /**
     * @brief 获取边数。
     * @return 隐式闭合环的边数，数值等于顶点数。
     */
    size_t GetNumEdges() const;

    /**
     * @brief 获取原始 double 顶点存储。
     * @return 仅在 Double 模式下为真实数据；其他模式下通常为空。
     */
    const std::vector<GB_Point2d>& GetDoubleVertices() const;

    /**
     * @brief 获取原始精确字符串顶点存储。
     * @return 仅在 ExactString 模式下为真实数据；其他模式下通常为空。
     */
    const std::vector<ExactStringVertex>& GetExactStringVertices() const;

    /**
     * @brief 获取 double 形式的顶点序列。
     * @return 对任意模式都返回可用于显示或近似计算的顶点。
     *
     * @note 对于 ExactString 模式，若某个坐标无法安全转换为有限 double，则对应点为 NaN 点。
     */
    std::vector<GB_Point2d> GetVerticesAsDouble() const;

    /**
     * @brief 获取指定顶点的 double 表示。
     * @param index 顶点索引。
     * @param outVertex 输出顶点。
     * @return 成功返回 true。
     */
    bool TryGetVertexAsDouble(size_t index, GB_Point2d& outVertex) const;

    /**
     * @brief 获取指定顶点的精确字符串表示。
     * @param index 顶点索引。
     * @param outXText 输出 x 文本。
     * @param outYText 输出 y 文本。
     * @return 成功返回 true。
     *
     * @note 对于 Double 模式，会将当前 double 值按稳定文本格式重新输出。
     */
    bool TryGetVertexExactStrings(size_t index, std::string& outXText, std::string& outYText) const;

    /**
     * @brief 获取 double 轴对齐包围盒。
     * @return 成功时返回有效矩形；失败时返回无效矩形。
     *
     * @note 对于 ExactString 模式，若任一坐标无法安全转换为有限 double，则返回无效矩形。
     */
    GB_Rectangle GetBoundingBox() const;

    /**
     * @brief 获取精确字符串形式的包围盒坐标。
     * @param outMinXText 输出最小 x 文本。
     * @param outMinYText 输出最小 y 文本。
     * @param outMaxXText 输出最大 x 文本。
     * @param outMaxYText 输出最大 y 文本。
     * @return 成功返回 true。
     *
     * @note 当前对象无效时返回 false。
     */
    bool TryGetBoundingBoxExactStrings(std::string& outMinXText, std::string& outMinYText, std::string& outMaxXText, std::string& outMaxYText) const;

    /**
     * @brief 若当前闭合环恰好表示横平竖直矩形边界，则输出对应的轴对齐矩形。
     * @param outRectangle 输出矩形。
     * @return 成功返回 true；否则 outRectangle 会被置为无效矩形并返回 false。
     */
    bool TryGetAxisAlignedRectangle(GB_Rectangle& outRectangle) const;

    /**
     * @brief 是否存在相邻重复顶点。
     *
     * @note 这里也会检查“最后一个顶点与第一个顶点”这一隐式闭合连接。
     */
    bool HasDuplicateAdjacentVertices() const;

    /**
     * @brief 是否存在零长度边。
     *
     * @note 当前实现与 `HasDuplicateAdjacentVertices()` 等价。
     */
    bool HasZeroLengthEdges() const;

    /**
     * @brief 是否存在相邻三点共线。
     */
    bool HasCollinearAdjacentTriples() const;

    /**
     * @brief 是否所有顶点都共线。
     */
    bool AreAllVerticesCollinear() const;

    /**
     * @brief 是否为简单闭合环。
     * @return 满足 simple ring 语义时返回 true。
     */
    bool IsSimple() const;

    /**
     * @brief 是否存在“非 simple ring”问题。
     * @return 当对象有效且边界非 simple 时返回 true。
     *
     * @warning 该接口名沿用旧语义，返回 true 的情况不仅包含严格的边界自交，
     *          还包括退化边界、相邻重复顶点等导致 non-simple 的情况。
     */
    bool HasSelfIntersections() const;

    /**
     * @brief 获取闭合环方向。
     */
    Orientation GetOrientation() const;

    /**
     * @brief 是否为顺时针方向。
     */
    bool IsClockwise() const;

    /**
     * @brief 是否为逆时针方向。
     */
    bool IsCounterClockwise() const;

    /**
     * @brief 获取代数面积的 double 值。
     * @return 成功时返回有限值；失败时返回 NaN。
     */
    double GetSignedArea() const;

    /**
     * @brief 获取代数面积绝对值的 double 值。
     */
    double GetUnsignedArea() const;

    /**
     * @brief 获取精确字符串形式的代数面积。
     * @return 有效对象返回精确文本；无效对象返回空串。
     */
    std::string GetSignedAreaExactString() const;

    /**
     * @brief 获取周长的 double 近似值。
     * @return 成功时返回有限值；失败时返回 NaN。
     */
    double GetPerimeter() const;

    /**
     * @brief 使用 odd-even 规则进行点包含判定。
     * @param point 查询点。
     */
    PointContainment ClassifyPointOddEven(const GB_Point2d& point) const;

    /**
     * @brief 使用 odd-even 规则进行点包含判定（精确字符串输入）。
     * @param xText 查询点 x 坐标文本。
     * @param yText 查询点 y 坐标文本。
     */
    PointContainment ClassifyPointOddEven(const std::string& xText, const std::string& yText) const;

    /**
     * @brief 返回反向顶点顺序的新对象。
     */
    GB_Polygon Reversed() const;

    /**
     * @brief 原地反转顶点顺序。
     */
    void Reverse();

    /**
     * @brief 计算与 other 的规则化交集。
     * @param other 另一闭合环。
     * @param outOuterBoundaries 输出结果中的外环集合，每个外环均为 CCW。
     * @param outHoleBoundaries 输出结果中的洞环集合；其大小与 outOuterBoundaries 相同，
     *        outHoleBoundaries[i] 对应 outOuterBoundaries[i] 的所有洞，每个洞环均为 CW。
     * @return 成功返回 true。若输入不满足 CGAL 规则化布尔运算要求（例如非 simple、面积为 0、存在相邻重复顶点/显式闭合重复点、CGAL 不可用），返回 false。
     */
    bool ComputeIntersection(const GB_Polygon& other, std::vector<GB_Polygon>& outOuterBoundaries, std::vector<std::vector<GB_Polygon>>& outHoleBoundaries) const;

    /**
     * @brief 计算与 other 的规则化并集。
     */
    bool ComputeUnion(const GB_Polygon& other, std::vector<GB_Polygon>& outOuterBoundaries, std::vector<std::vector<GB_Polygon>>& outHoleBoundaries) const;

    /**
     * @brief 计算与 other 的规则化差集（*this \ other）。
     */
    bool ComputeDifference(const GB_Polygon& other, std::vector<GB_Polygon>& outOuterBoundaries, std::vector<std::vector<GB_Polygon>>& outHoleBoundaries) const;

    /**
     * @brief 对当前闭合环做规则化/规范化，输出简单外环与洞环。
     * @param outOuterBoundaries 输出结果中的外环集合，每个外环均为 CCW。
     * @param outHoleBoundaries 输出结果中的洞环集合；其大小与 outOuterBoundaries 相同，
     *        outHoleBoundaries[i] 对应 outOuterBoundaries[i] 的所有洞，每个洞环均为 CW。
     * @return 成功返回 true。
     *
     * @details
     * - 若当前对象本身已经是 simple ring，则会输出与其表示区域等价的简单结果；
     * - 若当前对象存在自交、自接触等 non-simple 情况，则会尽量使用 CGAL 2D Polygon Repair 的 even-odd 规则做规则化；
     * - 例如“8”字形自交环会被拆分为两个简单多边形；
     * - 若 CGAL Polygon Repair 不可用、输入退化到无法形成二维区域，或内部转换失败，则返回 false。
     */
    bool ComputeNormalizedPolygons(std::vector<GB_Polygon>& outOuterBoundaries, std::vector<std::vector<GB_Polygon>>& outHoleBoundaries) const;

    bool operator==(const GB_Polygon& other) const;
    bool operator!=(const GB_Polygon& other) const;

    /**
     * @brief 按顶点顺序逐点做近似比较。
     * @param other 另一个对象。
     * @param tolerance 比较容差。
     * @return 顶点数量一致、顶点顺序一致且各对应点距离不超过容差时返回 true。
     */
    bool IsNearEqual(const GB_Polygon& other, double tolerance = GB_Epsilon) const;

    /**
     * @brief 序列化为可读字符串。
     */
    virtual std::string SerializeToString() const override;

    /**
     * @brief 序列化为二进制字节流。
     */
    virtual GB_ByteBuffer SerializeToBinary() const override;

    /**
     * @brief 从字符串反序列化。
     * @param data 输入字符串。
     * @return 成功返回 true，失败返回 false。
     *
     * @note 失败时对象会被清空，不保留半写入状态。
     */
    virtual bool Deserialize(const std::string& data) override;

    /**
     * @brief 从二进制字节流反序列化。
     * @param data 输入字节流。
     * @return 成功返回 true，失败返回 false。
     *
     * @note 失败时对象会被清空，不保留半写入状态。
     */
    virtual bool Deserialize(const GB_ByteBuffer& data) override;

private:
    struct ExactCacheData;
    CoordinateStorageMode storageMode = CoordinateStorageMode::Double;
    std::vector<GB_Point2d> vertices;
    std::vector<ExactStringVertex> exactStringVertices;
    mutable std::atomic<std::uint64_t> cacheVersion{ 1 };
    mutable std::shared_ptr<const ExactCacheData> exactCache;

    void InvalidateCaches();
    std::uint64_t GetCurrentCacheVersion() const;
    std::shared_ptr<const ExactCacheData> GetOrBuildExactCache() const;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif