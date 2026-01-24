#include "GB_Matrix3x3.h"
#include "GB_Vector2d.h"
#include "GB_Point2d.h"
#include "../GB_IO.h"
#include <cassert>
#include <iomanip>
#include <locale>
#include <sstream>
#include <algorithm>

namespace
{
    static inline double Sqr(double value)
    {
        return value * value;
    }

    static inline bool IsFinite(double value)
    {
        return std::isfinite(value) != 0;
    }

    struct Linear2x2Info
    {
        double c0Len2 = GB_QuietNan;
        double c1Len2 = GB_QuietNan;
        double dot = GB_QuietNan;
    };

    static inline bool TryGetLinear2x2Info(const GB_Matrix3x3& mat, double absTol, Linear2x2Info& info)
    {
        if (!mat.IsValid())
        {
            return false;
        }

        const double c0x = mat.m[0][0];
        const double c0y = mat.m[1][0];
        const double c1x = mat.m[0][1];
        const double c1y = mat.m[1][1];

        info.c0Len2 = c0x * c0x + c0y * c0y;
        info.c1Len2 = c1x * c1x + c1y * c1y;
        info.dot = c0x * c1x + c0y * c1y;

        const double minLen2 = absTol * absTol;
        if (!IsFinite(info.c0Len2) || !IsFinite(info.c1Len2) || !IsFinite(info.dot))
        {
            return false;
        }

        if (info.c0Len2 <= minLen2 || info.c1Len2 <= minLen2)
        {
            return false;
        }

        return true;
    }

    static inline bool IsScaledOrthogonalByInfo(const Linear2x2Info& info, double absTol)
    {
        // dot^2 <= (tol^2) * |c0|^2 * |c1|^2
        const double lhs = info.dot * info.dot;
        const double rhs = (absTol * absTol) * info.c0Len2 * info.c1Len2;

        if (!IsFinite(lhs) || !IsFinite(rhs))
        {
            return false;
        }

        return lhs <= rhs;
    }

    static inline bool TryGetScaleByLen2(double c0Len2, double c1Len2, double& scaleX, double& scaleY)
    {
        scaleX = std::sqrt(c0Len2);
        scaleY = std::sqrt(c1Len2);
        return IsFinite(scaleX) && IsFinite(scaleY);
    }
}

const GB_Matrix3x3 GB_Matrix3x3::Zero(0, 0, 0, 0, 0, 0, 0, 0, 0);

const GB_Matrix3x3 GB_Matrix3x3::Identity(1, 0, 0, 0, 1, 0, 0, 0, 1);

GB_Matrix3x3::GB_Matrix3x3()
{
    m[0][0] = GB_QuietNan;
    m[0][1] = GB_QuietNan;
    m[0][2] = GB_QuietNan;
    m[1][0] = GB_QuietNan;
    m[1][1] = GB_QuietNan;
    m[1][2] = GB_QuietNan;
    m[2][0] = GB_QuietNan;
    m[2][1] = GB_QuietNan;
    m[2][2] = GB_QuietNan;
}

GB_Matrix3x3::GB_Matrix3x3(double m00, double m01, double m02, double m10, double m11, double m12, double m20, double m21, double m22)
{
    Set(m00, m01, m02, m10, m11, m12, m20, m21, m22);
}

GB_Matrix3x3::~GB_Matrix3x3()
{
}

const std::string& GB_Matrix3x3::GetClassType() const
{
    static const std::string classType = "GB_Matrix3x3";
    return classType;
}

uint64_t GB_Matrix3x3::GetClassTypeId() const
{
    static const uint64_t classTypeId = GB_GenerateClassTypeId(GetClassType()); // 5974923956598778400
    return classTypeId;
}

void GB_Matrix3x3::Set(double m00, double m01, double m02, double m10, double m11, double m12, double m20, double m21, double m22)
{
    m[0][0] = m00;
    m[0][1] = m01;
    m[0][2] = m02;

    m[1][0] = m10;
    m[1][1] = m11;
    m[1][2] = m12;

    m[2][0] = m20;
    m[2][1] = m21;
    m[2][2] = m22;
}

void GB_Matrix3x3::SetLinearPart2x2(double m00, double m01, double m10, double m11)
{
    m[0][0] = m00;
    m[0][1] = m01;
    m[1][0] = m10;
    m[1][1] = m11;
}

void GB_Matrix3x3::GetLinearPart2x2(double& m00, double& m01, double& m10, double& m11) const
{
    m00 = m[0][0];
    m01 = m[0][1];
    m10 = m[1][0];
    m11 = m[1][1];
}

void GB_Matrix3x3::SetToIdentity()
{
    *this = Identity;
}

void GB_Matrix3x3::SetToZero()
{
    *this = Zero;
}

bool GB_Matrix3x3::IsValid() const
{
    return IsFinite(m[0][0]) && IsFinite(m[0][1]) && IsFinite(m[0][2]) &&
        IsFinite(m[1][0]) && IsFinite(m[1][1]) && IsFinite(m[1][2]) &&
        IsFinite(m[2][0]) && IsFinite(m[2][1]) && IsFinite(m[2][2]);
}

bool GB_Matrix3x3::IsZero(double tolerance) const
{
    if (!IsValid())
    {
        return false;
    }

    const double absTol = std::abs(tolerance);
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            if (std::abs(m[rowIndex][colIndex]) > absTol)
            {
                return false;
            }
        }
    }
    return true;
}

bool GB_Matrix3x3::IsIdentity(double tolerance) const
{
    if (!IsValid())
    {
        return false;
    }

    const double absTol = std::abs(tolerance);
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            const double expected = (rowIndex == colIndex) ? 1 : 0;
            if (std::abs(m[rowIndex][colIndex] - expected) > absTol)
            {
                return false;
            }
        }
    }
    return true;
}

bool GB_Matrix3x3::IsNearEqual(const GB_Matrix3x3& other, double tolerance) const
{
    if (!IsValid() || !other.IsValid())
    {
        return false;
    }

    const double absTol = std::abs(tolerance);
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            if (std::abs(m[rowIndex][colIndex] - other.m[rowIndex][colIndex]) > absTol)
            {
                return false;
            }
        }
    }
    return true;
}

bool GB_Matrix3x3::IsAffine2d(double tolerance) const
{
    if (!IsValid())
    {
        return false;
    }

    const double absTol = std::abs(tolerance);
    return std::abs(m[2][0]) <= absTol && std::abs(m[2][1]) <= absTol && std::abs(m[2][2] - 1) <= absTol;
}

GB_Matrix3x3 GB_Matrix3x3::operator+(const GB_Matrix3x3& other) const
{
    GB_Matrix3x3 result;
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            result.m[rowIndex][colIndex] = m[rowIndex][colIndex] + other.m[rowIndex][colIndex];
        }
    }
    return result;
}

GB_Matrix3x3 GB_Matrix3x3::operator-(const GB_Matrix3x3& other) const
{
    GB_Matrix3x3 result;
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            result.m[rowIndex][colIndex] = m[rowIndex][colIndex] - other.m[rowIndex][colIndex];
        }
    }
    return result;
}

GB_Matrix3x3 GB_Matrix3x3::operator*(const GB_Matrix3x3& other) const
{
    GB_Matrix3x3 result = Zero;

    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            double sum = 0;
            for (size_t k = 0; k < 3; k++)
            {
                sum += m[rowIndex][k] * other.m[k][colIndex];
            }
            result.m[rowIndex][colIndex] = sum;
        }
    }

    return result;
}

GB_Matrix3x3& GB_Matrix3x3::operator+=(const GB_Matrix3x3& other)
{
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            m[rowIndex][colIndex] += other.m[rowIndex][colIndex];
        }
    }
    return *this;
}

GB_Matrix3x3& GB_Matrix3x3::operator-=(const GB_Matrix3x3& other)
{
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            m[rowIndex][colIndex] -= other.m[rowIndex][colIndex];
        }
    }
    return *this;
}

GB_Matrix3x3& GB_Matrix3x3::operator*=(const GB_Matrix3x3& other)
{
    *this = (*this) * other;
    return *this;
}

GB_Matrix3x3 GB_Matrix3x3::operator-() const
{
    GB_Matrix3x3 result;
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            result.m[rowIndex][colIndex] = -m[rowIndex][colIndex];
        }
    }
    return result;
}

bool GB_Matrix3x3::operator==(const GB_Matrix3x3& other) const
{
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            if (m[rowIndex][colIndex] != other.m[rowIndex][colIndex])
            {
                return false;
            }
        }
    }
    return true;
}

bool GB_Matrix3x3::operator!=(const GB_Matrix3x3& other) const
{
    return !(*this == other);
}

double* GB_Matrix3x3::operator[](size_t rowIndex)
{
    assert(rowIndex < 3);
    return m[rowIndex];
}

const double* GB_Matrix3x3::operator[](size_t rowIndex) const
{
    assert(rowIndex < 3);
    return m[rowIndex];
}

double* GB_Matrix3x3::Data()
{
    return &m[0][0];
}

const double* GB_Matrix3x3::Data() const
{
    return &m[0][0];
}

GB_Matrix3x3 GB_Matrix3x3::LeftMultiplied(const GB_Matrix3x3& left) const
{
    return left * (*this);
}

void GB_Matrix3x3::LeftMultiply(const GB_Matrix3x3& left)
{
    *this = left * (*this);
}

GB_Matrix3x3 GB_Matrix3x3::RightMultiplied(const GB_Matrix3x3& right) const
{
    return (*this) * right;
}

void GB_Matrix3x3::RightMultiply(const GB_Matrix3x3& right)
{
    *this = (*this) * right;
}

GB_Matrix3x3 GB_Matrix3x3::Transposed() const
{
    GB_Matrix3x3 result;
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            result.m[rowIndex][colIndex] = m[colIndex][rowIndex];
        }
    }
    return result;
}

void GB_Matrix3x3::Transpose()
{
    *this = Transposed();
}

bool GB_Matrix3x3::CanInvert(double tolerance) const
{
    if (!IsValid())
    {
        return false;
    }

    const double absTol = std::abs(tolerance);
    const double det = IsAffine2d(absTol) ? Det2x2() : Det();
    if (!std::isfinite(det))
    {
        return false;
    }
    return std::abs(det) > absTol;
}

GB_Matrix3x3 GB_Matrix3x3::Inverted(double tolerance) const
{
    GB_Matrix3x3 result = *this;
    if (!result.Invert(tolerance))
    {
        return GB_Matrix3x3();
    }
    return result;
}

bool GB_Matrix3x3::Invert(double tolerance)
{
    if (!IsValid())
    {
        *this = GB_Matrix3x3();
        return false;
    }

    if (TryInvertAffine2d(tolerance))
    {
        return true;
    }

    const double det = Det();
    if (!IsFinite(det) || std::abs(det) <= std::abs(tolerance))
    {
        *this = GB_Matrix3x3();
        return false;
    }

    const double invDet = 1.0 / det;

    // 伴随矩阵（余子式矩阵转置）
    GB_Matrix3x3 adj;

    adj.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]);
    adj.m[0][1] = -(m[0][1] * m[2][2] - m[0][2] * m[2][1]);
    adj.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]);

    adj.m[1][0] = -(m[1][0] * m[2][2] - m[1][2] * m[2][0]);
    adj.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]);
    adj.m[1][2] = -(m[0][0] * m[1][2] - m[0][2] * m[1][0]);

    adj.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    adj.m[2][1] = -(m[0][0] * m[2][1] - m[0][1] * m[2][0]);
    adj.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]);

    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            m[rowIndex][colIndex] = adj.m[rowIndex][colIndex] * invDet;
        }
    }

    return true;
}

double GB_Matrix3x3::Det() const
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) 
        - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) 
        + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

double GB_Matrix3x3::Det2x2() const
{
    return m[0][0] * m[1][1] - m[0][1] * m[1][0];
}

void GB_Matrix3x3::SetTranslation(double translateX, double translateY)
{
    m[0][2] = translateX;
    m[1][2] = translateY;
}

void GB_Matrix3x3::SetTranslation(const GB_Vector2d& translation)
{
    m[0][2] = translation.x;
    m[1][2] = translation.y;
}

GB_Vector2d GB_Matrix3x3::GetTranslation() const
{
    return GB_Vector2d(m[0][2], m[1][2]);
}

void GB_Matrix3x3::ClearTranslation()
{
    m[0][2] = 0;
    m[1][2] = 0;
}

bool GB_Matrix3x3::IsScaledOrthogonal(double tolerance) const
{
    const double absTol = std::abs(tolerance);

    Linear2x2Info info;
    if (!TryGetLinear2x2Info(*this, absTol, info))
    {
        return false;
    }

    return IsScaledOrthogonalByInfo(info, absTol);
}

bool GB_Matrix3x3::IsUniformScaledOrthogonal(double tolerance) const
{
    const double absTol = std::abs(tolerance);

    Linear2x2Info info;
    if (!TryGetLinear2x2Info(*this, absTol, info))
    {
        return false;
    }

    if (!IsScaledOrthogonalByInfo(info, absTol))
    {
        return false;
    }

    double scaleX = GB_QuietNan;
    double scaleY = GB_QuietNan;
    if (!TryGetScaleByLen2(info.c0Len2, info.c1Len2, scaleX, scaleY))
    {
        return false;
    }

    const double maxLen = std::max(scaleX, scaleY);
    return std::abs(scaleX - scaleY) <= absTol * maxLen;
}

bool GB_Matrix3x3::IsOrthogonal(double tolerance) const
{
    const double absTol = std::abs(tolerance);

    Linear2x2Info info;
    if (!TryGetLinear2x2Info(*this, absTol, info))
    {
        return false;
    }

    if (!IsScaledOrthogonalByInfo(info, absTol))
    {
        return false;
    }

    double scaleX = GB_QuietNan;
    double scaleY = GB_QuietNan;
    if (!TryGetScaleByLen2(info.c0Len2, info.c1Len2, scaleX, scaleY))
    {
        return false;
    }

    const double maxLen = std::max(scaleX, scaleY);
    if (std::abs(scaleX - scaleY) > absTol * maxLen)
    {
        return false;
    }

    const double scale = 0.5 * (scaleX + scaleY);
    if (!IsFinite(scale))
    {
        return false;
    }

    return std::abs(scale - 1.0) <= absTol;
}

bool GB_Matrix3x3::IsRigid(double tolerance) const
{
    return IsAffine2d(tolerance) && IsOrthogonal(tolerance);
}

bool GB_Matrix3x3::IsConformal(double tolerance) const
{
    return IsUniformScaledOrthogonal(tolerance);
}

double GB_Matrix3x3::GetRotationAngle(double tolerance) const
{
    if (!IsUniformScaledOrthogonal(tolerance))
    {
        return GB_QuietNan;
    }

    const double scale = GetUniformScaleFactor(tolerance);
    if (!IsFinite(scale) || GB_IsZero(scale, std::abs(tolerance)))
    {
        return GB_QuietNan;
    }

    const double c0x = m[0][0] / scale;
    const double c0y = m[1][0] / scale;
    if (!IsFinite(c0x) || !IsFinite(c0y))
    {
        return GB_QuietNan;
    }

    double angle = std::atan2(c0y, c0x);
    if (angle < 0)
    {
        angle += GB_2Pi;
    }
    return angle;
}

bool GB_Matrix3x3::TryGetScaleFactors(double& scaleX, double& scaleY, double tolerance) const
{
    if (!IsValid())
    {
        scaleX = GB_QuietNan;
        scaleY = GB_QuietNan;
        return false;
    }

    const double c0x = m[0][0];
    const double c0y = m[1][0];
    const double c1x = m[0][1];
    const double c1y = m[1][1];

    const double c0Len2 = c0x * c0x + c0y * c0y;
    const double c1Len2 = c1x * c1x + c1y * c1y;

    const double absTol = std::abs(tolerance);
    const double minLen2 = absTol * absTol;

    if (!IsFinite(c0Len2) || !IsFinite(c1Len2) || c0Len2 <= minLen2 || c1Len2 <= minLen2)
    {
        scaleX = GB_QuietNan;
        scaleY = GB_QuietNan;
        return false;
    }

    scaleX = std::sqrt(c0Len2);
    scaleY = std::sqrt(c1Len2);
    return IsFinite(scaleX) && IsFinite(scaleY);
}

double GB_Matrix3x3::GetUniformScaleFactor(double tolerance) const
{
    const double absTol = std::abs(tolerance);

    Linear2x2Info info;
    if (!TryGetLinear2x2Info(*this, absTol, info))
    {
        return GB_QuietNan;
    }

    if (!IsScaledOrthogonalByInfo(info, absTol))
    {
        return GB_QuietNan;
    }

    double scaleX = GB_QuietNan;
    double scaleY = GB_QuietNan;
    if (!TryGetScaleByLen2(info.c0Len2, info.c1Len2, scaleX, scaleY))
    {
        return GB_QuietNan;
    }

    const double maxLen = std::max(scaleX, scaleY);
    if (std::abs(scaleX - scaleY) > absTol * maxLen)
    {
        return GB_QuietNan;
    }

    return 0.5 * (scaleX + scaleY);
}

GB_Point2d GB_Matrix3x3::TransformPoint(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return GB_Point2d(GB_QuietNan, GB_QuietNan);
    }

    const double x = point.x;
    const double y = point.y;

    // 仿射快路径：w 恒为 1，无需算 wPrime/除法
    if (std::abs(m[2][0]) <= GB_Epsilon &&
        std::abs(m[2][1]) <= GB_Epsilon &&
        std::abs(m[2][2] - 1) <= GB_Epsilon)
    {
        const double xPrime = m[0][0] * x + m[0][1] * y + m[0][2];
        const double yPrime = m[1][0] * x + m[1][1] * y + m[1][2];
        return GB_Point2d(xPrime, yPrime);
    }

    // 通用路径（透视/投影）
    const double xPrime = m[0][0] * x + m[0][1] * y + m[0][2];
    const double yPrime = m[1][0] * x + m[1][1] * y + m[1][2];
    const double wPrime = m[2][0] * x + m[2][1] * y + m[2][2];

    if (!IsFinite(wPrime) || std::abs(wPrime) <= GB_Epsilon)
    {
        return GB_Point2d(GB_QuietNan, GB_QuietNan);
    }

    const double invW = 1.0 / wPrime;
    return GB_Point2d(xPrime * invW, yPrime * invW);
}

bool GB_Matrix3x3::TransformPoints(const GB_Point2d* inputPoints, GB_Point2d* outputPoints, size_t numPoints, bool useOpenMP) const
{
    if (numPoints == 0)
    {
        return true;
    }

    if (!IsValid() || inputPoints == nullptr || outputPoints == nullptr)
    {
        return false;
    }

    const double m00 = m[0][0];
    const double m01 = m[0][1];
    const double m02 = m[0][2];
    const double m10 = m[1][0];
    const double m11 = m[1][1];
    const double m12 = m[1][2];

    const bool isAffine = IsAffine2d();

    if (isAffine)
    {
        if (useOpenMP)
        {
#pragma omp parallel for schedule(static)
            for (long long i = 0; i < static_cast<long long>(numPoints); i++)
            {
                const double x = inputPoints[i].x;
                const double y = inputPoints[i].y;

                const double xPrime = m00 * x + m01 * y + m02;
                const double yPrime = m10 * x + m11 * y + m12;

                outputPoints[i] = GB_Point2d(xPrime, yPrime);
            }
            return true;
        }
        else
        {
            for (size_t i = 0; i < numPoints; i++)
            {
                const double x = inputPoints[i].x;
                const double y = inputPoints[i].y;

                const double xPrime = m00 * x + m01 * y + m02;
                const double yPrime = m10 * x + m11 * y + m12;

                outputPoints[i] = GB_Point2d(xPrime, yPrime);
            }
            return true;
        }
    }

    // 一般 3×3（可能含透视）：逐点计算 w，并进行齐次除法
    const double m20 = m[2][0];
    const double m21 = m[2][1];
    const double m22 = m[2][2];
    if (useOpenMP)
    {
#pragma omp parallel for schedule(static)
        for (long long i = 0; i < static_cast<long long>(numPoints); i++)
        {
            const double x = inputPoints[i].x;
            const double y = inputPoints[i].y;

            const double xPrime = m00 * x + m01 * y + m02;
            const double yPrime = m10 * x + m11 * y + m12;
            const double wPrime = m20 * x + m21 * y + m22;

            if (!IsFinite(wPrime) || std::abs(wPrime) <= GB_Epsilon)
            {
                outputPoints[i] = GB_Point2d(GB_QuietNan, GB_QuietNan);
                continue;
            }

            if (std::abs(wPrime - 1) <= GB_Epsilon)
            {
                outputPoints[i] = GB_Point2d(xPrime, yPrime);
                continue;
            }

            const double invW = 1.0 / wPrime;
            outputPoints[i] = GB_Point2d(xPrime * invW, yPrime * invW);
        }
        return true;
    }
    else
    {
        for (size_t i = 0; i < numPoints; i++)
        {
            const double x = inputPoints[i].x;
            const double y = inputPoints[i].y;

            const double xPrime = m00 * x + m01 * y + m02;
            const double yPrime = m10 * x + m11 * y + m12;
            const double wPrime = m20 * x + m21 * y + m22;

            if (!IsFinite(wPrime) || std::abs(wPrime) <= GB_Epsilon)
            {
                outputPoints[i] = GB_Point2d(GB_QuietNan, GB_QuietNan);
                continue;
            }

            if (std::abs(wPrime - 1) <= GB_Epsilon)
            {
                outputPoints[i] = GB_Point2d(xPrime, yPrime);
                continue;
            }

            const double invW = 1.0 / wPrime;
            outputPoints[i] = GB_Point2d(xPrime * invW, yPrime * invW);
        }

        return true;
    }
}

bool GB_Matrix3x3::TransformPoints(const std::vector<GB_Point2d>& inputPoints, std::vector<GB_Point2d>& outputPoints, bool useOpenMP) const
{
    if (&inputPoints == &outputPoints)
    {
        return TransformPoints(outputPoints, useOpenMP);
    }

    const size_t numPoints = inputPoints.size();
    if (numPoints == 0)
    {
        outputPoints.clear();
        return true;
    }

    outputPoints.resize(numPoints);
    return TransformPoints(inputPoints.data(), outputPoints.data(), numPoints, useOpenMP);
}

bool GB_Matrix3x3::TransformPoints(GB_Point2d* points, size_t numPoints, bool useOpenMP) const
{
    return TransformPoints(points, points, numPoints, useOpenMP);
}

bool GB_Matrix3x3::TransformPoints(std::vector<GB_Point2d>& points, bool useOpenMP) const
{
    const size_t numPoints = points.size();
    if (numPoints == 0)
    {
        return true;
    }

    return TransformPoints(points.data(), points.data(), numPoints, useOpenMP);
}

GB_Vector2d GB_Matrix3x3::TransformVector(const GB_Vector2d& vec) const
{
    if (!IsValid() || !vec.IsValid())
    {
        return GB_Vector2d(GB_QuietNan, GB_QuietNan);
    }

    const double x = vec.x;
    const double y = vec.y;

    return GB_Vector2d(m[0][0] * x + m[0][1] * y, m[1][0] * x + m[1][1] * y);
}

bool GB_Matrix3x3::TransformVectors(const GB_Vector2d* inputVectors, GB_Vector2d* outputVectors, size_t numVectors, bool useOpenMP) const
{
    if (numVectors == 0)
    {
        return true;
    }

    if (!IsValid() || inputVectors == nullptr || outputVectors == nullptr)
    {
        return false;
    }

    const double m00 = m[0][0];
    const double m01 = m[0][1];
    const double m10 = m[1][0];
    const double m11 = m[1][1];

    if (useOpenMP)
    {
#pragma omp parallel for schedule(static)
        for (long long i = 0; i < static_cast<long long>(numVectors); i++)
        {
            const double x = inputVectors[i].x;
            const double y = inputVectors[i].y;
            outputVectors[i] = GB_Vector2d(m00 * x + m01 * y, m10 * x + m11 * y);
        }
        return true;
    }
    else
    {
        for (size_t i = 0; i < numVectors; i++)
        {
            const double x = inputVectors[i].x;
            const double y = inputVectors[i].y;
            outputVectors[i] = GB_Vector2d(m00 * x + m01 * y, m10 * x + m11 * y);
        }

        return true;
    }
}

bool GB_Matrix3x3::TransformVectors(const std::vector<GB_Vector2d>& inputVectors, std::vector<GB_Vector2d>& outputVectors, bool useOpenMP) const
{
    if (&inputVectors == &outputVectors)
    {
        return TransformVectors(outputVectors, useOpenMP);
    }

    const size_t numVectors = inputVectors.size();
    if (numVectors == 0)
    {
        outputVectors.clear();
        return true;
    }

    outputVectors.resize(numVectors);
    return TransformVectors(inputVectors.data(), outputVectors.data(), numVectors, useOpenMP);
}

bool GB_Matrix3x3::TransformVectors(GB_Vector2d* vectors, size_t numVectors, bool useOpenMP) const
{
    return TransformVectors(vectors, vectors, numVectors, useOpenMP);
}

bool GB_Matrix3x3::TransformVectors(std::vector<GB_Vector2d>& vectors, bool useOpenMP) const
{
    const size_t numVectors = vectors.size();
    if (numVectors == 0)
    {
        return true;
    }

    return TransformVectors(vectors.data(), vectors.data(), numVectors, useOpenMP);
}

GB_Matrix3x3 GB_Matrix3x3::CreateFromTranslation(double translateX, double translateY)
{
    GB_Matrix3x3 mat = Identity;
    mat.m[0][2] = translateX;
    mat.m[1][2] = translateY;
    return mat;
}

GB_Matrix3x3 GB_Matrix3x3::CreateFromTranslation(const GB_Vector2d& translation)
{
    GB_Matrix3x3 mat = Identity;
    mat.m[0][2] = translation.x;
    mat.m[1][2] = translation.y;
    return mat;
}

GB_Matrix3x3 GB_Matrix3x3::CreateFromRotation(double angle)
{
    if (!IsFinite(angle))
    {
        return GB_Matrix3x3();
    }

    const double c = std::cos(angle);
    const double s = std::sin(angle);

    return GB_Matrix3x3(c, -s, 0, s, c, 0, 0, 0, 1);
}

GB_Matrix3x3 GB_Matrix3x3::CreateFromScaling(double scaleX, double scaleY)
{
    if (!IsFinite(scaleX) || !IsFinite(scaleY))
    {
        return GB_Matrix3x3();
    }

    return GB_Matrix3x3(scaleX, 0, 0, 0, scaleY, 0, 0, 0, 1);
}

GB_Matrix3x3 GB_Matrix3x3::CreateFromUniformScaling(double scale)
{
    if (!IsFinite(scale))
    {
        return GB_Matrix3x3();
    }

    return GB_Matrix3x3(scale, 0, 0, 0, scale, 0, 0, 0, 1);
}

GB_Matrix3x3 GB_Matrix3x3::CreateShear(double shearX, double shearY)
{
    if (!IsFinite(shearX) || !IsFinite(shearY))
    {
        return GB_Matrix3x3();
    }

    return GB_Matrix3x3(1, shearX, 0, shearY, 1, 0, 0, 0, 1);
}

std::string GB_Matrix3x3::SerializeToString() const
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << "(" << GetClassType() << " " << std::setprecision(17)
        << m[0][0] << "," << m[0][1] << "," << m[0][2] << ","
        << m[1][0] << "," << m[1][1] << "," << m[1][2] << ","
        << m[2][0] << "," << m[2][1] << "," << m[2][2]
        << ")";
    return oss.str();
}

GB_ByteBuffer GB_Matrix3x3::SerializeToBinary() const
{
    constexpr static uint16_t payloadVersion = 1;

    GB_ByteBuffer buffer;
    buffer.reserve(96);

    GB_ByteBufferIO::AppendUInt32LE(buffer, GB_ClassMagicNumber);
    GB_ByteBufferIO::AppendUInt64LE(buffer, GetClassTypeId());
    GB_ByteBufferIO::AppendUInt16LE(buffer, payloadVersion);
    GB_ByteBufferIO::AppendUInt16LE(buffer, 0);

    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            GB_ByteBufferIO::AppendDoubleLE(buffer, m[rowIndex][colIndex]);
        }
    }

    return buffer;
}

bool GB_Matrix3x3::Deserialize(const std::string& data)
{
    std::istringstream iss(data);
    iss.imbue(std::locale::classic());

    char leftParen = 0;
    std::string type;
    double values[9];
    for (int i = 0; i < 9; i++)
    {
        values[i] = GB_QuietNan;
    }

    if (!(iss >> leftParen >> type))
    {
        *this = GB_Matrix3x3();
        return false;
    }

    if (leftParen != '(' || type != GetClassType())
    {
        *this = GB_Matrix3x3();
        return false;
    }

    for (int i = 0; i < 9; i++)
    {
        if (!(iss >> values[i]))
        {
            *this = GB_Matrix3x3();
            return false;
        }

        if (i < 8)
        {
            char comma = 0;
            if (!(iss >> comma) || comma != ',')
            {
                *this = GB_Matrix3x3();
                return false;
            }
        }
        else
        {
            char rightParen = 0;
            if (!(iss >> rightParen) || rightParen != ')')
            {
                *this = GB_Matrix3x3();
                return false;
            }
        }
    }

    Set(values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8]);
    return true;
}

bool GB_Matrix3x3::Deserialize(const GB_ByteBuffer& data)
{
    constexpr static uint16_t expectedPayloadVersion = 1;
    constexpr static size_t minSize = 4 + 8 + 2 + 2 + 9 * 8;

    if (data.size() < minSize)
    {
        *this = GB_Matrix3x3();
        return false;
    }

    size_t offset = 0;
    uint32_t magic = 0;
    uint64_t typeId = 0;
    uint16_t payloadVersion = 0;
    uint16_t reserved = 0;

    if (!GB_ByteBufferIO::ReadUInt32LE(data, offset, magic)
        || !GB_ByteBufferIO::ReadUInt64LE(data, offset, typeId)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, payloadVersion)
        || !GB_ByteBufferIO::ReadUInt16LE(data, offset, reserved))
    {
        *this = GB_Matrix3x3();
        return false;
    }

    if (magic != GB_ClassMagicNumber || typeId != GetClassTypeId() || payloadVersion != expectedPayloadVersion)
    {
        *this = GB_Matrix3x3();
        return false;
    }

    double values[9] = { GB_QuietNan };
    for (int i = 0; i < 9; i++)
    {
        if (!GB_ByteBufferIO::ReadDoubleLE(data, offset, values[i]))
        {
            *this = GB_Matrix3x3();
            return false;
        }
    }

    Set(values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8]);
    return true;
}

bool GB_Matrix3x3::TryInvertAffine2d(double tolerance)
{
    if (!IsAffine2d(tolerance))
    {
        return false;
    }

    const double det = Det2x2();
    if (!IsFinite(det) || std::abs(det) <= std::abs(tolerance))
    {
        *this = GB_Matrix3x3();
        return false;
    }

    const double invDet = 1.0 / det;

    const double a00 = m[0][0];
    const double a01 = m[0][1];
    const double a10 = m[1][0];
    const double a11 = m[1][1];

    const double tx = m[0][2];
    const double ty = m[1][2];

    const double inv00 = a11 * invDet;
    const double inv01 = -a01 * invDet;
    const double inv10 = -a10 * invDet;
    const double inv11 = a00 * invDet;

    const double invTx = -(inv00 * tx + inv01 * ty);
    const double invTy = -(inv10 * tx + inv11 * ty);

    Set(inv00, inv01, invTx, inv10, inv11, invTy, 0, 0, 1);
    return true;
}













































