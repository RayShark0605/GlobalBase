#ifndef GLOBALBASE_VECTOR2D_H_H
#define GLOBALBASE_VECTOR2D_H_H

#include "../GlobalBasePort.h"
#include "../GB_Math.h"
#include "GB_GeometryInterface.h"

class GB_Matrix3x3;

class GLOBALBASE_PORT GB_Vector2d : public GB_SerializableClass
{
public:
	double x = GB_QuietNan;
	double y = GB_QuietNan;

	// 二维零向量
	static const GB_Vector2d Zero;

	// X轴单位向量 (1, 0)
	static const GB_Vector2d UnitX;

	// Y轴单位向量 (0, 1)
	static const GB_Vector2d UnitY;

	GB_Vector2d();
	GB_Vector2d(double x, double y);
	virtual ~GB_Vector2d() override;

	// 获取类类型标识字符串。
	virtual const std::string& GetClassType() const override;

	// 获取类类型标识 Id。
	virtual uint64_t GetClassTypeId() const override;

	// 设置向量坐标。
	void Set(double x, double y);

	// 检查向量是否有效（非 NaN / 非 Inf）。
	bool IsValid() const;

	// 检查向量是否为零向量（基于容差）。
	bool IsZero(double tolerance = GB_Epsilon) const;

	// 检查向量是否为单位向量（长度等于 1，基于容差）。
	bool IsUnit(double tolerance = GB_Epsilon) const;

	// 检查两个向量是否“近似相等”（距离 <= tolerance）。
	bool IsNearEqual(const GB_Vector2d& other, double tolerance = GB_Epsilon) const;

	GB_Vector2d operator+(const GB_Vector2d& other) const;
	GB_Vector2d operator-(const GB_Vector2d& other) const;
	GB_Vector2d operator*(double scalar) const;
	GB_Vector2d operator/(double scalar) const;
	GB_Vector2d& operator+=(const GB_Vector2d& other);
	GB_Vector2d& operator-=(const GB_Vector2d& other);
	GB_Vector2d& operator*=(double scalar);
	GB_Vector2d& operator/=(double scalar);

	// 取反：(-x, -y)。
	GB_Vector2d operator-() const;

	// 严格相等（逐分量 ==），不做容差比较。
	bool operator==(const GB_Vector2d& other) const;

	// 严格不等（逐分量 !=），不做容差比较。
	bool operator!=(const GB_Vector2d& other) const;

	// 向量长度。
	double Length() const;

	// 长度的平方。
	double LengthSquared() const;

	// 向量与 X 轴正方向的夹角（弧度），范围 [0, 2π)。
	double Angle() const;

	// 由角度创建单位向量： (cos(angle), sin(angle))。
	static GB_Vector2d FromAngle(double angle);

	// 向量归一化。
	GB_Vector2d Normalized() const;
	void Normalize();

	// 计算两个向量的点积。
	static double DotProduct(const GB_Vector2d& a, const GB_Vector2d& b);
	double DotProduct(const GB_Vector2d& other) const;

	// 计算两个向量的叉积（二维标量形式）。
	static double CrossProduct(const GB_Vector2d& a, const GB_Vector2d& b);
	double CrossProduct(const GB_Vector2d& other) const;

	// 应用矩阵变换（不包含平移的线性变换）。
	static GB_Vector2d Transform(const GB_Vector2d& vec, const GB_Matrix3x3& mat);
	void Transform(const GB_Matrix3x3& mat);
	GB_Vector2d Transformed(const GB_Matrix3x3& mat) const;

	// 计算两个向量的夹角（弧度），范围 [0, π]。
	static double AngleBetween(const GB_Vector2d& a, const GB_Vector2d& b);
	double AngleBetween(const GB_Vector2d& other) const;

	// 计算从本向量旋转到 other 的有符号夹角（弧度），范围 (-π, π]。
	double SignedAngleTo(const GB_Vector2d& other) const;

	/**
	 * @brief 判断是否平行（方向相同或相反）。
	 * @note 使用 cross 的相对判定：|cross| <= tolerance * |a| * |b|。
	 */
	bool IsParallelTo(const GB_Vector2d& other, double tolerance = GB_Epsilon) const;

	/**
	 * @brief 判断是否垂直。
	 * @note 使用 dot 的相对判定：|dot| <= tolerance * |a| * |b|。
	 */
	bool IsPerpendicularTo(const GB_Vector2d& other, double tolerance = GB_Epsilon) const;

	/**
	* @brief 判断是否同向（平行且点积为正）。
	* @note 若任一向量为零向量或无效，返回 false。
	*/
	bool IsCodirectionalTo(const GB_Vector2d& other, double tolerance = GB_Epsilon) const;

	// 旋转向量（逆时针，弧度）。
	GB_Vector2d Rotated(double angle) const;
	void Rotate(double angle);

	// 向量投影：将本向量投影到 onto 方向上。
	GB_Vector2d ProjectOn(const GB_Vector2d& onto) const;

	// 序列化。
	virtual std::string SerializeToString() const override;
	virtual GB_ByteBuffer SerializeToBinary() const override;

	// 反序列化。
	virtual bool Deserialize(const std::string& data) override;
	virtual bool Deserialize(const GB_ByteBuffer& data) override;
};




#endif