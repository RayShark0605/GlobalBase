#ifndef GLOBALBASE_POINT2D_H_H
#define GLOBALBASE_POINT2D_H_H

#include "../GlobalBasePort.h"
#include "../GB_Math.h"
#include "GB_GeometryInterface.h"

class GB_Matrix3x3;
class GB_Vector2d;

class GLOBALBASE_PORT GB_Point2d : public GB_SerializableClass
{
public:
	double x = GB_QuietNan;
	double y = GB_QuietNan;

	// 原点 (0, 0)
	static const GB_Point2d Origin; 

	GB_Point2d();
	GB_Point2d(double x, double y);
	GB_Point2d(const GB_Vector2d& vec);
	virtual ~GB_Point2d() override;

	virtual const std::string& GetClassType() const override;

	// 获取类类型标识 Id
	virtual uint64_t GetClassTypeId() const override;

	void Set(double x, double y);

	bool IsValid() const;

	// 判断是否为原点（基于容差）。
	bool IsOrigin(double tolerance = GB_Epsilon) const;

	GB_Point2d operator*(double scalar) const;
	GB_Point2d& operator*=(double scalar);
	GB_Point2d operator/(double scalar) const;
	GB_Point2d& operator/=(double scalar);
	GB_Point2d operator+(const GB_Vector2d& vec) const;
	GB_Point2d& operator+=(const GB_Vector2d& vec);
	GB_Point2d operator-(const GB_Vector2d& vec) const;
	GB_Point2d& operator-=(const GB_Vector2d& vec);

	GB_Vector2d operator-(const GB_Point2d& other) const;
	bool operator==(const GB_Point2d& other) const;
	bool operator!=(const GB_Point2d& other) const;

	double& operator[](size_t index);
	const double& operator[](size_t index) const;

	GB_Vector2d ToVector2d() const;

	double DistanceTo(const GB_Point2d& other) const;
	double DistanceToSquared(const GB_Point2d& other) const;

	double DistanceToOrigin() const;
	double DistanceToOriginSquared() const;

	bool IsNearEqual(const GB_Point2d& other, double tolerance = GB_Epsilon) const;

	// 矩阵变换（包含平移）。
	GB_Point2d Transformed(const GB_Matrix3x3& mat) const;
	void Transform(const GB_Matrix3x3& mat);

	// 绕 center 旋转（逆时针，弧度）。
	GB_Point2d Rotated(double angle, const GB_Point2d& center = GB_Point2d::Origin) const;
	void Rotate(double angle, const GB_Point2d& center = GB_Point2d::Origin);

	// 平移。
	GB_Point2d Offsetted(double deltaX, double deltaY) const;
	void Offset(double deltaX, double deltaY);

	// 中点。
	static GB_Point2d MidPoint(const GB_Point2d& a, const GB_Point2d& b);
	GB_Point2d MidPointTo(const GB_Point2d& other) const;

	// 线性插值：a + (b - a) * t。
	static GB_Point2d Lerp(const GB_Point2d& a, const GB_Point2d& b, double t);
	GB_Point2d LerpTo(const GB_Point2d& other, double t) const;

	// 序列化。
	virtual std::string SerializeToString() const override;
	virtual GB_ByteBuffer SerializeToBinary() const override;

	// 反序列化。
	virtual bool Deserialize(const std::string& data) override;
	virtual bool Deserialize(const GB_ByteBuffer& data) override;
};

GB_Point2d operator*(double scalar, const GB_Point2d& point);



#endif