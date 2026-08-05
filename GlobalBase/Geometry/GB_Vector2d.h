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

    // 二维零向量。
    static const GB_Vector2d Zero;

    // X 轴单位向量 (1, 0)。
    static const GB_Vector2d UnitX;

    // Y 轴单位向量 (0, 1)。
    static const GB_Vector2d UnitY;

    GB_Vector2d();
    GB_Vector2d(double x, double y);
    virtual ~GB_Vector2d() override;

    virtual const std::string& GetClassType() const override;
    virtual uint64_t GetClassTypeId() const override;

    void Set(double x, double y);
    bool IsValid() const;
    bool IsZero(double tolerance = GB_Epsilon) const;
    bool IsUnit(double tolerance = GB_Epsilon) const;
    bool IsNearEqual(const GB_Vector2d& other, double tolerance = GB_Epsilon) const;

    GB_Vector2d operator+(const GB_Vector2d& other) const;
    GB_Vector2d operator-(const GB_Vector2d& other) const;
    GB_Vector2d operator*(double scalar) const;
    GB_Vector2d operator/(double scalar) const;
    GB_Vector2d& operator+=(const GB_Vector2d& other);
    GB_Vector2d& operator-=(const GB_Vector2d& other);
    GB_Vector2d& operator*=(double scalar);
    GB_Vector2d& operator/=(double scalar);
    GB_Vector2d operator-() const;
    bool operator==(const GB_Vector2d& other) const;
    bool operator!=(const GB_Vector2d& other) const;

    // 向量长度。使用抗溢出的 hypot 计算。
    double Length() const;

    // 长度平方。超过 double 可表示范围时返回正无穷。
    double LengthSquared() const;

    double Angle() const;
    static GB_Vector2d FromAngle(double angle);
    GB_Vector2d Normalized() const;
    void Normalize();

    static double DotProduct(const GB_Vector2d& a, const GB_Vector2d& b);
    double DotProduct(const GB_Vector2d& other) const;
    static double CrossProduct(const GB_Vector2d& a, const GB_Vector2d& b);
    double CrossProduct(const GB_Vector2d& other) const;

    static GB_Vector2d Transform(const GB_Vector2d& vec, const GB_Matrix3x3& mat);
    void Transform(const GB_Matrix3x3& mat);
    GB_Vector2d Transformed(const GB_Matrix3x3& mat) const;

    static double AngleBetween(const GB_Vector2d& a, const GB_Vector2d& b);
    double AngleBetween(const GB_Vector2d& other) const;
    double SignedAngleTo(const GB_Vector2d& other) const;

    bool IsParallelTo(const GB_Vector2d& other, double tolerance = GB_Epsilon) const;
    bool IsPerpendicularTo(const GB_Vector2d& other, double tolerance = GB_Epsilon) const;
    bool IsCodirectionalTo(const GB_Vector2d& other, double tolerance = GB_Epsilon) const;

    GB_Vector2d Rotated(double angle) const;
    void Rotate(double angle);

    GB_Vector2d ProjectOn(const GB_Vector2d& onto) const;

    virtual std::string SerializeToString() const override;
    virtual GB_ByteBuffer SerializeToBinary() const override;

    // 失败时保持当前对象原值不变。
    virtual bool Deserialize(const std::string& data) override;
    virtual bool Deserialize(const GB_ByteBuffer& data) override;
};

#endif
