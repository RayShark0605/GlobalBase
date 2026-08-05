#ifndef GLOBALBASE_MATRIX3X3_H_H
#define GLOBALBASE_MATRIX3X3_H_H

#include "../GlobalBasePort.h"
#include "../GB_Math.h"
#include "GB_GeometryInterface.h"

#include <cstddef>
#include <vector>

class GB_Vector2d;
class GB_Point2d;

/**
 * @brief 3×3 双精度矩阵，主要用于 2D 齐次坐标与仿射变换。
 *
 * 约定：
 * - 线性部分位于左上角 2×2；
 * - 平移分量位于最后一列；
 * - 标准仿射矩阵的最后一行必须精确为 (0, 0, 1)，近似仿射判断只用于查询，不能用于跳过齐次除法。
 */
class GLOBALBASE_PORT GB_Matrix3x3 : public GB_SerializableClass
{
public:
    double m[3][3];

    static const GB_Matrix3x3 Zero;
    static const GB_Matrix3x3 Identity;

    GB_Matrix3x3();
    GB_Matrix3x3(double m00, double m01, double m02, double m10, double m11, double m12, double m20, double m21, double m22);
    virtual ~GB_Matrix3x3() override;

    virtual const std::string& GetClassType() const override;
    virtual uint64_t GetClassTypeId() const override;

    void Set(double m00, double m01, double m02, double m10, double m11, double m12, double m20, double m21, double m22);
    void SetLinearPart2x2(double m00, double m01, double m10, double m11);
    void GetLinearPart2x2(double& m00, double& m01, double& m10, double& m11) const;

    void SetToIdentity();
    void SetToZero();

    bool IsValid() const;
    bool IsZero(double tolerance = GB_Epsilon) const;
    bool IsIdentity(double tolerance = GB_Epsilon) const;
    bool IsNearEqual(const GB_Matrix3x3& other, double tolerance = GB_Epsilon) const;
    bool IsAffine2d(double tolerance = GB_Epsilon) const;

    GB_Matrix3x3 operator+(const GB_Matrix3x3& other) const;
    GB_Matrix3x3 operator-(const GB_Matrix3x3& other) const;
    GB_Matrix3x3 operator*(const GB_Matrix3x3& other) const;
    GB_Matrix3x3& operator+=(const GB_Matrix3x3& other);
    GB_Matrix3x3& operator-=(const GB_Matrix3x3& other);
    GB_Matrix3x3& operator*=(const GB_Matrix3x3& other);
    GB_Matrix3x3 operator-() const;
    bool operator==(const GB_Matrix3x3& other) const;
    bool operator!=(const GB_Matrix3x3& other) const;

    double* operator[](size_t rowIndex);
    const double* operator[](size_t rowIndex) const;
    double* Data();
    const double* Data() const;

    GB_Matrix3x3 LeftMultiplied(const GB_Matrix3x3& left) const;
    void LeftMultiply(const GB_Matrix3x3& left);
    GB_Matrix3x3 RightMultiplied(const GB_Matrix3x3& right) const;
    void RightMultiply(const GB_Matrix3x3& right);

    GB_Matrix3x3 Transposed() const;
    void Transpose();

    // 使用缩放部分主元 Gauss-Jordan 消元，避免绝对行列式阈值误判小尺度可逆矩阵。
    bool CanInvert(double tolerance = GB_Epsilon) const;
    GB_Matrix3x3 Inverted(double tolerance = GB_Epsilon) const;
    bool Invert(double tolerance = GB_Epsilon);

    double Det() const;
    double Det2x2() const;

    void SetTranslation(double translateX, double translateY);
    void SetTranslation(const GB_Vector2d& translation);
    GB_Vector2d GetTranslation() const;
    void ClearTranslation();

    bool IsScaledOrthogonal(double tolerance = GB_Epsilon) const;
    bool IsUniformScaledOrthogonal(double tolerance = GB_Epsilon) const;
    bool IsOrthogonal(double tolerance = GB_Epsilon) const;
    bool IsRigid(double tolerance = GB_Epsilon) const;
    bool IsConformal(double tolerance = GB_Epsilon) const;
    double GetRotationAngle(double tolerance = GB_Epsilon) const;
    bool TryGetScaleFactors(double& scaleX, double& scaleY, double tolerance = GB_Epsilon) const;
    double GetUniformScaleFactor(double tolerance = GB_Epsilon) const;

    GB_Point2d TransformPoint(const GB_Point2d& point) const;

    bool TransformPoints(const GB_Point2d* inputPoints, GB_Point2d* outputPoints, size_t numPoints, bool useOpenMP) const;
    bool TransformPoints(const std::vector<GB_Point2d>& inputPoints, std::vector<GB_Point2d>& outputPoints, bool useOpenMP) const;
    bool TransformPoints(GB_Point2d* points, size_t numPoints, bool useOpenMP) const;
    bool TransformPoints(std::vector<GB_Point2d>& points, bool useOpenMP) const;

    GB_Vector2d TransformVector(const GB_Vector2d& vec) const;

    bool TransformVectors(const GB_Vector2d* inputVectors, GB_Vector2d* outputVectors, size_t numVectors, bool useOpenMP) const;
    bool TransformVectors(const std::vector<GB_Vector2d>& inputVectors, std::vector<GB_Vector2d>& outputVectors, bool useOpenMP) const;
    bool TransformVectors(GB_Vector2d* vectors, size_t numVectors, bool useOpenMP) const;
    bool TransformVectors(std::vector<GB_Vector2d>& vectors, bool useOpenMP) const;

    static GB_Matrix3x3 CreateFromTranslation(double translateX, double translateY);
    static GB_Matrix3x3 CreateFromTranslation(const GB_Vector2d& translation);
    static GB_Matrix3x3 CreateFromRotation(double angle);
    static GB_Matrix3x3 CreateFromScaling(double scaleX, double scaleY);
    static GB_Matrix3x3 CreateFromUniformScaling(double scale);
    static GB_Matrix3x3 CreateShear(double shearX, double shearY);

    virtual std::string SerializeToString() const override;
    virtual GB_ByteBuffer SerializeToBinary() const override;

    // 失败时保持当前对象原值不变。
    virtual bool Deserialize(const std::string& data) override;
    virtual bool Deserialize(const GB_ByteBuffer& data) override;

private:
    bool TryInvertAffine2d(double tolerance);
};

#endif
