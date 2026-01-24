#ifndef GLOBALBASE_MATRIX3X3_H_H
#define GLOBALBASE_MATRIX3X3_H_H

#include "../GlobalBasePort.h"
#include "../GB_Math.h"
#include "GB_GeometryInterface.h"
#include <cstddef>
#include <vector>

class GB_Vector2d;
class GB_Point2d;

/*
 * @brief 3×3 双精度矩阵（主用于 2D 齐次坐标/仿射变换，也可作为一般 3×3 矩阵使用）。
 *
 * ## 约定（重要）
 *   - 线性部分（2×2）在左上角：m[0][0..1], m[1][0..1]
 *   - 平移分量在最后一列：m[0][2] = tx, m[1][2] = ty
 *   - 对于标准仿射矩阵，最后一行应接近 (0, 0, 1)
 */
class GLOBALBASE_PORT GB_Matrix3x3 : public GB_SerializableClass
{
public:
	double m[3][3];

	// 全零矩阵。
	static const GB_Matrix3x3 Zero;

	// 单位矩阵。
	static const GB_Matrix3x3 Identity;

	GB_Matrix3x3();
	GB_Matrix3x3(double m00, double m01, double m02, double m10, double m11, double m12, double m20, double m21, double m22);
	virtual ~GB_Matrix3x3() override;

	// 获取类类型标识字符串。
	virtual const std::string& GetClassType() const override;

	// 获取类类型标识 Id。
	virtual uint64_t GetClassTypeId() const override;

	void Set(double m00, double m01, double m02, double m10, double m11, double m12, double m20, double m21, double m22);

	// 设置左上角 2×2 线性部分（不改动平移与最后一行）。
	void SetLinearPart2x2(double m00, double m01, double m10, double m11);

	// 获取左上角 2×2 线性部分。
	void GetLinearPart2x2(double& m00, double& m01, double& m10, double& m11) const;

	void SetToIdentity();

	void SetToZero();

	bool IsValid() const;

	bool IsZero(double tolerance = GB_Epsilon) const;

	bool IsIdentity(double tolerance = GB_Epsilon) const;

	bool IsNearEqual(const GB_Matrix3x3& other, double tolerance = GB_Epsilon) const;

	// 是否是标准 2D 仿射矩阵（最后一行接近 0,0,1）。
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

	// 获取可写行指针，支持 mat[row][col]。
	double* operator[](size_t rowIndex);

	// 获取只读行指针，支持 mat[row][col]。
	const double* operator[](size_t rowIndex) const;

	// 返回首元素地址（按行连续）。
	double* Data();
	const double* Data() const;

	// 左乘：result = left * this。
	GB_Matrix3x3 LeftMultiplied(const GB_Matrix3x3& left) const;
	void LeftMultiply(const GB_Matrix3x3& left);

	// 右乘：result = this * right。
	GB_Matrix3x3 RightMultiplied(const GB_Matrix3x3& right) const;
	void RightMultiply(const GB_Matrix3x3& right);

	// 转置。
	GB_Matrix3x3 Transposed() const;
	void Transpose();

	// 求逆。
	bool CanInvert(double tolerance = GB_Epsilon) const;
	GB_Matrix3x3 Inverted(double tolerance = GB_Epsilon) const;
	bool Invert(double tolerance = GB_Epsilon);

	// 3×3 行列式。
	double Det() const;

	// 2D 仿射线性部分（左上 2×2）的行列式。
	double Det2x2() const;

	// 设置平移分量。
	void SetTranslation(double translateX, double translateY);
	void SetTranslation(const GB_Vector2d& translation);

	// 获取平移分量。
	GB_Vector2d GetTranslation() const;

	// 清空平移分量（把 m[0][2], m[1][2] 置 0）。
	void ClearTranslation();

	// 判断 2×2 线性部分是否“缩放正交”（无剪切）。
	bool IsScaledOrthogonal(double tolerance = GB_Epsilon) const;

	// 判断 2×2 线性部分是否“一致缩放正交”。
	bool IsUniformScaledOrthogonal(double tolerance = GB_Epsilon) const;

	// 判断 2×2 线性部分是否为“正交矩阵”（列正交且列长度为 1）。
	bool IsOrthogonal(double tolerance = GB_Epsilon) const;

	// 判断是否为刚体变换（旋转/反射 + 平移），不含缩放。
	bool IsRigid(double tolerance = GB_Epsilon) const;

	// 判断是否保角（角度保持）。
	bool IsConformal(double tolerance = GB_Epsilon) const;

	// 获取旋转角（弧度，范围 [0, 2π)）。
	double GetRotationAngle(double tolerance = GB_Epsilon) const;

	// 尝试获取 X/Y 缩放因子（基于 2×2 的两列长度）。
	bool TryGetScaleFactors(double& scaleX, double& scaleY, double tolerance = GB_Epsilon) const;

	// 获取一致缩放因子（若非一致缩放正交，则返回 NaN）。
	double GetUniformScaleFactor(double tolerance = GB_Epsilon) const;

	// 使用矩阵变换二维点（包含平移，列向量约定）。
	GB_Point2d TransformPoint(const GB_Point2d& point) const;

	bool TransformPoints(const GB_Point2d* inputPoints, GB_Point2d* outputPoints, size_t numPoints, bool useOpenMP) const;
	bool TransformPoints(const std::vector<GB_Point2d>& inputPoints, std::vector<GB_Point2d>& outputPoints, bool useOpenMP) const;
	bool TransformPoints(GB_Point2d* points, size_t numPoints, bool useOpenMP) const;
	bool TransformPoints(std::vector<GB_Point2d>& points, bool useOpenMP) const;

	// 使用矩阵变换二维向量（不包含平移，只取 2×2 线性部分）。
	GB_Vector2d TransformVector(const GB_Vector2d& vec) const;

	bool TransformVectors(const GB_Vector2d* inputVectors, GB_Vector2d* outputVectors, size_t numVectors, bool useOpenMP) const;
	bool TransformVectors(const std::vector<GB_Vector2d>& inputVectors, std::vector<GB_Vector2d>& outputVectors, bool useOpenMP) const;
	bool TransformVectors(GB_Vector2d* vectors, size_t numVectors, bool useOpenMP) const;
	bool TransformVectors(std::vector<GB_Vector2d>& vectors, bool useOpenMP) const;

	// 创建 2D 平移矩阵（标准仿射）。
	static GB_Matrix3x3 CreateFromTranslation(double translateX, double translateY);
	static GB_Matrix3x3 CreateFromTranslation(const GB_Vector2d& translation);

	// 创建 2D 旋转矩阵（绕原点，逆时针，弧度）。
	static GB_Matrix3x3 CreateFromRotation(double angle);

	// 创建 2D 缩放矩阵（绕原点）。
	static GB_Matrix3x3 CreateFromScaling(double scaleX, double scaleY);
	static GB_Matrix3x3 CreateFromUniformScaling(double scale);

	// 创建 2D 错切矩阵。
	static GB_Matrix3x3 CreateShear(double shearX, double shearY);

	// 序列化。
	virtual std::string SerializeToString() const override;
	virtual GB_ByteBuffer SerializeToBinary() const override;

	// 反序列化。
	virtual bool Deserialize(const std::string& data) override;
	virtual bool Deserialize(const GB_ByteBuffer& data) override;

private:
	bool TryInvertAffine2d(double tolerance);
};






#endif