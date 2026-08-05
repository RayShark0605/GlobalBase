#include "GB_Matrix3x3.h"
#include "GB_Vector2d.h"
#include "GB_Point2d.h"
#include "../GB_IO.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace
{
    struct Linear2x2Info
    {
        double globalScale = 0.0;
        double normalizedLengthX = 0.0;
        double normalizedLengthY = 0.0;
        double unitX0 = 0.0;
        double unitY0 = 0.0;
        double unitX1 = 0.0;
        double unitY1 = 0.0;
    };

    static inline bool IsFinite(double value)
    {
        return std::isfinite(value) != 0;
    }

    static bool TryGetAbsoluteTolerance(double tolerance, double& absoluteTolerance)
    {
        if (std::isnan(tolerance))
        {
            absoluteTolerance = 0.0;
            return false;
        }

        absoluteTolerance = std::abs(tolerance);
        return true;
    }

    static bool IsExactlyAffine2d(const GB_Matrix3x3& matrix)
    {
        return matrix.m[2][0] == 0.0 && matrix.m[2][1] == 0.0 && matrix.m[2][2] == 1.0;
    }

    static bool TryGetLinear2x2Info(const GB_Matrix3x3& matrix, Linear2x2Info& info)
    {
        if (!matrix.IsValid())
        {
            return false;
        }

        const double globalScale = std::max(std::max(std::abs(matrix.m[0][0]), std::abs(matrix.m[1][0])), std::max(std::abs(matrix.m[0][1]), std::abs(matrix.m[1][1])));
        if (globalScale == 0.0)
        {
            return false;
        }

        const double normalizedX0 = matrix.m[0][0] / globalScale;
        const double normalizedY0 = matrix.m[1][0] / globalScale;
        const double normalizedX1 = matrix.m[0][1] / globalScale;
        const double normalizedY1 = matrix.m[1][1] / globalScale;
        const double normalizedLengthX = std::hypot(normalizedX0, normalizedY0);
        const double normalizedLengthY = std::hypot(normalizedX1, normalizedY1);
        if (!IsFinite(normalizedLengthX) || !IsFinite(normalizedLengthY) || normalizedLengthX == 0.0 || normalizedLengthY == 0.0)
        {
            return false;
        }

        info.globalScale = globalScale;
        info.normalizedLengthX = normalizedLengthX;
        info.normalizedLengthY = normalizedLengthY;
        info.unitX0 = normalizedX0 / normalizedLengthX;
        info.unitY0 = normalizedY0 / normalizedLengthX;
        info.unitX1 = normalizedX1 / normalizedLengthY;
        info.unitY1 = normalizedY1 / normalizedLengthY;
        return true;
    }

    static bool IsScaledOrthogonalByInfo(const Linear2x2Info& info, double absoluteTolerance)
    {
        const double normalizedDot = info.unitX0 * info.unitX1 + info.unitY0 * info.unitY1;
        return std::abs(normalizedDot) <= absoluteTolerance;
    }

    static bool IsUniformScaleByInfo(const Linear2x2Info& info, double absoluteTolerance)
    {
        const double maxNormalizedLength = std::max(info.normalizedLengthX, info.normalizedLengthY);
        return std::abs(info.normalizedLengthX - info.normalizedLengthY) <= absoluteTolerance * maxNormalizedLength;
    }

    static bool TryGetScaleFactorsByInfo(const Linear2x2Info& info, double& scaleX, double& scaleY)
    {
        scaleX = info.globalScale * info.normalizedLengthX;
        scaleY = info.globalScale * info.normalizedLengthY;
        if (!IsFinite(scaleX) || !IsFinite(scaleY))
        {
            scaleX = GB_QuietNan;
            scaleY = GB_QuietNan;
            return false;
        }

        return true;
    }

    static bool TryInvertGeneralMatrix(const GB_Matrix3x3& sourceMatrix, double tolerance, GB_Matrix3x3& inverseMatrix)
    {
        double absoluteTolerance = 0.0;
        if (!sourceMatrix.IsValid() || !TryGetAbsoluteTolerance(tolerance, absoluteTolerance))
        {
            return false;
        }

        double augmentedMatrix[3][6];
        double rowScales[3];
        for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
        {
            rowScales[rowIndex] = 0.0;
            for (size_t colIndex = 0; colIndex < 3; colIndex++)
            {
                augmentedMatrix[rowIndex][colIndex] = sourceMatrix.m[rowIndex][colIndex];
                rowScales[rowIndex] = std::max(rowScales[rowIndex], std::abs(sourceMatrix.m[rowIndex][colIndex]));
                augmentedMatrix[rowIndex][colIndex + 3] = rowIndex == colIndex ? 1.0 : 0.0;
            }

            if (rowScales[rowIndex] == 0.0)
            {
                return false;
            }
        }

        for (size_t pivotColumn = 0; pivotColumn < 3; pivotColumn++)
        {
            size_t pivotRow = pivotColumn;
            double bestScaledPivot = 0.0;
            for (size_t candidateRow = pivotColumn; candidateRow < 3; candidateRow++)
            {
                const double scaledPivot = std::abs(augmentedMatrix[candidateRow][pivotColumn]) / rowScales[candidateRow];
                if (scaledPivot > bestScaledPivot)
                {
                    bestScaledPivot = scaledPivot;
                    pivotRow = candidateRow;
                }
            }

            if (!IsFinite(bestScaledPivot) || bestScaledPivot <= absoluteTolerance)
            {
                return false;
            }

            if (pivotRow != pivotColumn)
            {
                for (size_t colIndex = 0; colIndex < 6; colIndex++)
                {
                    std::swap(augmentedMatrix[pivotRow][colIndex], augmentedMatrix[pivotColumn][colIndex]);
                }
                std::swap(rowScales[pivotRow], rowScales[pivotColumn]);
            }

            const double pivotValue = augmentedMatrix[pivotColumn][pivotColumn];
            if (!IsFinite(pivotValue) || pivotValue == 0.0)
            {
                return false;
            }

            for (size_t colIndex = 0; colIndex < 6; colIndex++)
            {
                augmentedMatrix[pivotColumn][colIndex] /= pivotValue;
                if (!IsFinite(augmentedMatrix[pivotColumn][colIndex]))
                {
                    return false;
                }
            }

            for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
            {
                if (rowIndex == pivotColumn)
                {
                    continue;
                }

                const double eliminationFactor = augmentedMatrix[rowIndex][pivotColumn];
                if (eliminationFactor == 0.0)
                {
                    continue;
                }

                for (size_t colIndex = 0; colIndex < 6; colIndex++)
                {
                    augmentedMatrix[rowIndex][colIndex] -= eliminationFactor * augmentedMatrix[pivotColumn][colIndex];
                    if (!IsFinite(augmentedMatrix[rowIndex][colIndex]))
                    {
                        return false;
                    }
                }
            }
        }

        GB_Matrix3x3 result;
        for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
        {
            for (size_t colIndex = 0; colIndex < 3; colIndex++)
            {
                result.m[rowIndex][colIndex] = augmentedMatrix[rowIndex][colIndex + 3];
            }
        }

        if (!result.IsValid())
        {
            return false;
        }

        inverseMatrix = result;
        return true;
    }

    template<typename ElementType>
    static bool HasUnsupportedPartialOverlap(const ElementType* inputElements, ElementType* outputElements, size_t numElements)
    {
        if (numElements == 0 || inputElements == outputElements)
        {
            return false;
        }

        if (numElements > std::numeric_limits<size_t>::max() / sizeof(ElementType))
        {
            return true;
        }

        const size_t byteSize = numElements * sizeof(ElementType);
        const uintptr_t inputBegin = reinterpret_cast<uintptr_t>(inputElements);
        const uintptr_t outputBegin = reinterpret_cast<uintptr_t>(outputElements);
        if (inputBegin > std::numeric_limits<uintptr_t>::max() - byteSize || outputBegin > std::numeric_limits<uintptr_t>::max() - byteSize)
        {
            return true;
        }

        const uintptr_t inputEnd = inputBegin + byteSize;
        const uintptr_t outputEnd = outputBegin + byteSize;
        return inputBegin < outputEnd && outputBegin < inputEnd;
    }

    static GB_Point2d TransformPointAffineUnchecked(const GB_Matrix3x3& matrix, const GB_Point2d& point)
    {
        if (!point.IsValid())
        {
            return GB_Point2d();
        }

        const double transformedX = std::fma(matrix.m[0][0], point.x, std::fma(matrix.m[0][1], point.y, matrix.m[0][2]));
        const double transformedY = std::fma(matrix.m[1][0], point.x, std::fma(matrix.m[1][1], point.y, matrix.m[1][2]));
        const GB_Point2d result(transformedX, transformedY);
        return result.IsValid() ? result : GB_Point2d();
    }

    static GB_Vector2d TransformVectorUnchecked(const GB_Matrix3x3& matrix, const GB_Vector2d& vector)
    {
        if (!vector.IsValid())
        {
            return GB_Vector2d();
        }

        const double transformedX = std::fma(matrix.m[0][0], vector.x, matrix.m[0][1] * vector.y);
        const double transformedY = std::fma(matrix.m[1][0], vector.x, matrix.m[1][1] * vector.y);
        const GB_Vector2d result(transformedX, transformedY);
        return result.IsValid() ? result : GB_Vector2d();
    }
}

const GB_Matrix3x3 GB_Matrix3x3::Zero(0, 0, 0, 0, 0, 0, 0, 0, 0);
const GB_Matrix3x3 GB_Matrix3x3::Identity(1, 0, 0, 0, 1, 0, 0, 0, 1);

GB_Matrix3x3::GB_Matrix3x3()
{
    Set(GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan);
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
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            if (!IsFinite(m[rowIndex][colIndex]))
            {
                return false;
            }
        }
    }
    return true;
}

bool GB_Matrix3x3::IsZero(double tolerance) const
{
    double absoluteTolerance = 0.0;
    if (!IsValid() || !TryGetAbsoluteTolerance(tolerance, absoluteTolerance))
    {
        return false;
    }

    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            if (std::abs(m[rowIndex][colIndex]) > absoluteTolerance)
            {
                return false;
            }
        }
    }
    return true;
}

bool GB_Matrix3x3::IsIdentity(double tolerance) const
{
    double absoluteTolerance = 0.0;
    if (!IsValid() || !TryGetAbsoluteTolerance(tolerance, absoluteTolerance))
    {
        return false;
    }

    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            const double expectedValue = rowIndex == colIndex ? 1.0 : 0.0;
            if (std::abs(m[rowIndex][colIndex] - expectedValue) > absoluteTolerance)
            {
                return false;
            }
        }
    }
    return true;
}

bool GB_Matrix3x3::IsNearEqual(const GB_Matrix3x3& other, double tolerance) const
{
    double absoluteTolerance = 0.0;
    if (!IsValid() || !other.IsValid() || !TryGetAbsoluteTolerance(tolerance, absoluteTolerance))
    {
        return false;
    }

    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            if (std::abs(m[rowIndex][colIndex] - other.m[rowIndex][colIndex]) > absoluteTolerance)
            {
                return false;
            }
        }
    }
    return true;
}

bool GB_Matrix3x3::IsAffine2d(double tolerance) const
{
    double absoluteTolerance = 0.0;
    if (!IsValid() || !TryGetAbsoluteTolerance(tolerance, absoluteTolerance))
    {
        return false;
    }

    return std::abs(m[2][0]) <= absoluteTolerance && std::abs(m[2][1]) <= absoluteTolerance && std::abs(m[2][2] - 1.0) <= absoluteTolerance;
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
    GB_Matrix3x3 result;
    for (size_t rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (size_t colIndex = 0; colIndex < 3; colIndex++)
        {
            result.m[rowIndex][colIndex] = std::fma(m[rowIndex][0], other.m[0][colIndex], std::fma(m[rowIndex][1], other.m[1][colIndex], m[rowIndex][2] * other.m[2][colIndex]));
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
    *this = *this * other;
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
    return left * *this;
}

void GB_Matrix3x3::LeftMultiply(const GB_Matrix3x3& left)
{
    *this = left * *this;
}

GB_Matrix3x3 GB_Matrix3x3::RightMultiplied(const GB_Matrix3x3& right) const
{
    return *this * right;
}

void GB_Matrix3x3::RightMultiply(const GB_Matrix3x3& right)
{
    *this = *this * right;
}

GB_Matrix3x3 GB_Matrix3x3::Transposed() const
{
    return GB_Matrix3x3(m[0][0], m[1][0], m[2][0], m[0][1], m[1][1], m[2][1], m[0][2], m[1][2], m[2][2]);
}

void GB_Matrix3x3::Transpose()
{
    std::swap(m[0][1], m[1][0]);
    std::swap(m[0][2], m[2][0]);
    std::swap(m[1][2], m[2][1]);
}

bool GB_Matrix3x3::CanInvert(double tolerance) const
{
    GB_Matrix3x3 inverseMatrix;
    return TryInvertGeneralMatrix(*this, tolerance, inverseMatrix);
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

    if (IsExactlyAffine2d(*this) && TryInvertAffine2d(tolerance))
    {
        return true;
    }

    GB_Matrix3x3 inverseMatrix;
    if (!TryInvertGeneralMatrix(*this, tolerance, inverseMatrix))
    {
        *this = GB_Matrix3x3();
        return false;
    }

    *this = inverseMatrix;
    return true;
}

double GB_Matrix3x3::Det() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    const double firstMinor = std::fma(m[1][1], m[2][2], -m[1][2] * m[2][1]);
    const double secondMinor = std::fma(m[1][0], m[2][2], -m[1][2] * m[2][0]);
    const double thirdMinor = std::fma(m[1][0], m[2][1], -m[1][1] * m[2][0]);
    return std::fma(m[0][0], firstMinor, std::fma(-m[0][1], secondMinor, m[0][2] * thirdMinor));
}

double GB_Matrix3x3::Det2x2() const
{
    if (!IsValid())
    {
        return GB_QuietNan;
    }

    return std::fma(m[0][0], m[1][1], -m[0][1] * m[1][0]);
}

void GB_Matrix3x3::SetTranslation(double translateX, double translateY)
{
    m[0][2] = translateX;
    m[1][2] = translateY;
}

void GB_Matrix3x3::SetTranslation(const GB_Vector2d& translation)
{
    SetTranslation(translation.x, translation.y);
}

GB_Vector2d GB_Matrix3x3::GetTranslation() const
{
    return GB_Vector2d(m[0][2], m[1][2]);
}

void GB_Matrix3x3::ClearTranslation()
{
    m[0][2] = 0.0;
    m[1][2] = 0.0;
}

bool GB_Matrix3x3::IsScaledOrthogonal(double tolerance) const
{
    double absoluteTolerance = 0.0;
    Linear2x2Info info;
    return TryGetAbsoluteTolerance(tolerance, absoluteTolerance) && TryGetLinear2x2Info(*this, info) && IsScaledOrthogonalByInfo(info, absoluteTolerance);
}

bool GB_Matrix3x3::IsUniformScaledOrthogonal(double tolerance) const
{
    double absoluteTolerance = 0.0;
    Linear2x2Info info;
    return TryGetAbsoluteTolerance(tolerance, absoluteTolerance) && TryGetLinear2x2Info(*this, info) && IsScaledOrthogonalByInfo(info, absoluteTolerance) && IsUniformScaleByInfo(info, absoluteTolerance);
}

bool GB_Matrix3x3::IsOrthogonal(double tolerance) const
{
    double absoluteTolerance = 0.0;
    Linear2x2Info info;
    if (!TryGetAbsoluteTolerance(tolerance, absoluteTolerance) || !TryGetLinear2x2Info(*this, info) || !IsScaledOrthogonalByInfo(info, absoluteTolerance))
    {
        return false;
    }

    double scaleX = GB_QuietNan;
    double scaleY = GB_QuietNan;
    if (!TryGetScaleFactorsByInfo(info, scaleX, scaleY))
    {
        return false;
    }

    return std::abs(scaleX - 1.0) <= absoluteTolerance && std::abs(scaleY - 1.0) <= absoluteTolerance;
}

bool GB_Matrix3x3::IsRigid(double tolerance) const
{
    return IsAffine2d(tolerance) && IsOrthogonal(tolerance);
}

bool GB_Matrix3x3::IsConformal(double tolerance) const
{
    return IsAffine2d(tolerance) && IsUniformScaledOrthogonal(tolerance);
}

double GB_Matrix3x3::GetRotationAngle(double tolerance) const
{
    if (!IsUniformScaledOrthogonal(tolerance))
    {
        return GB_QuietNan;
    }

    Linear2x2Info info;
    if (!TryGetLinear2x2Info(*this, info))
    {
        return GB_QuietNan;
    }

    double angle = std::atan2(info.unitY0, info.unitX0);
    if (angle < 0.0)
    {
        angle += GB_2Pi;
    }
    return angle;
}

bool GB_Matrix3x3::TryGetScaleFactors(double& scaleX, double& scaleY, double tolerance) const
{
    double absoluteTolerance = 0.0;
    Linear2x2Info info;
    if (!TryGetAbsoluteTolerance(tolerance, absoluteTolerance) || !TryGetLinear2x2Info(*this, info))
    {
        scaleX = GB_QuietNan;
        scaleY = GB_QuietNan;
        return false;
    }

    return TryGetScaleFactorsByInfo(info, scaleX, scaleY);
}

double GB_Matrix3x3::GetUniformScaleFactor(double tolerance) const
{
    double absoluteTolerance = 0.0;
    Linear2x2Info info;
    if (!TryGetAbsoluteTolerance(tolerance, absoluteTolerance) || !TryGetLinear2x2Info(*this, info) || !IsScaledOrthogonalByInfo(info, absoluteTolerance) || !IsUniformScaleByInfo(info, absoluteTolerance))
    {
        return GB_QuietNan;
    }

    double scaleX = GB_QuietNan;
    double scaleY = GB_QuietNan;
    if (!TryGetScaleFactorsByInfo(info, scaleX, scaleY))
    {
        return GB_QuietNan;
    }

    return scaleX * 0.5 + scaleY * 0.5;
}

GB_Point2d GB_Matrix3x3::TransformPoint(const GB_Point2d& point) const
{
    if (!IsValid() || !point.IsValid())
    {
        return GB_Point2d();
    }

    if (IsExactlyAffine2d(*this))
    {
        return TransformPointAffineUnchecked(*this, point);
    }

    const double numeratorX = std::fma(m[0][0], point.x, std::fma(m[0][1], point.y, m[0][2]));
    const double numeratorY = std::fma(m[1][0], point.x, std::fma(m[1][1], point.y, m[1][2]));
    const double homogeneousW = std::fma(m[2][0], point.x, std::fma(m[2][1], point.y, m[2][2]));
    if (!IsFinite(numeratorX) || !IsFinite(numeratorY) || !IsFinite(homogeneousW) || homogeneousW == 0.0)
    {
        return GB_Point2d();
    }

    const GB_Point2d result(numeratorX / homogeneousW, numeratorY / homogeneousW);
    return result.IsValid() ? result : GB_Point2d();
}

bool GB_Matrix3x3::TransformPoints(const GB_Point2d* inputPoints, GB_Point2d* outputPoints, size_t numPoints, bool useOpenMP) const
{
    if (numPoints == 0)
    {
        return true;
    }

    if (!IsValid() || inputPoints == nullptr || outputPoints == nullptr || HasUnsupportedPartialOverlap(inputPoints, outputPoints, numPoints))
    {
        return false;
    }

    const bool useParallelLoop = useOpenMP && numPoints <= static_cast<size_t>(std::numeric_limits<long long>::max());
    if (IsExactlyAffine2d(*this))
    {
        if (useParallelLoop)
        {
#pragma omp parallel for schedule(static)
            for (long long pointIndex = 0; pointIndex < static_cast<long long>(numPoints); pointIndex++)
            {
                outputPoints[pointIndex] = TransformPointAffineUnchecked(*this, inputPoints[pointIndex]);
            }
        }
        else
        {
            for (size_t pointIndex = 0; pointIndex < numPoints; pointIndex++)
            {
                outputPoints[pointIndex] = TransformPointAffineUnchecked(*this, inputPoints[pointIndex]);
            }
        }
        return true;
    }

    if (useParallelLoop)
    {
#pragma omp parallel for schedule(static)
        for (long long pointIndex = 0; pointIndex < static_cast<long long>(numPoints); pointIndex++)
        {
            outputPoints[pointIndex] = TransformPoint(inputPoints[pointIndex]);
        }
    }
    else
    {
        for (size_t pointIndex = 0; pointIndex < numPoints; pointIndex++)
        {
            outputPoints[pointIndex] = TransformPoint(inputPoints[pointIndex]);
        }
    }
    return true;
}

bool GB_Matrix3x3::TransformPoints(const std::vector<GB_Point2d>& inputPoints, std::vector<GB_Point2d>& outputPoints, bool useOpenMP) const
{
    if (&inputPoints == &outputPoints)
    {
        return TransformPoints(outputPoints, useOpenMP);
    }

    if (inputPoints.empty())
    {
        outputPoints.clear();
        return true;
    }

    try
    {
        std::vector<GB_Point2d> transformedPoints(inputPoints.size());
        if (!TransformPoints(inputPoints.data(), transformedPoints.data(), inputPoints.size(), useOpenMP))
        {
            return false;
        }
        outputPoints.swap(transformedPoints);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool GB_Matrix3x3::TransformPoints(GB_Point2d* points, size_t numPoints, bool useOpenMP) const
{
    return TransformPoints(points, points, numPoints, useOpenMP);
}

bool GB_Matrix3x3::TransformPoints(std::vector<GB_Point2d>& points, bool useOpenMP) const
{
    return points.empty() || TransformPoints(points.data(), points.data(), points.size(), useOpenMP);
}

GB_Vector2d GB_Matrix3x3::TransformVector(const GB_Vector2d& vec) const
{
    if (!IsValid())
    {
        return GB_Vector2d();
    }

    return TransformVectorUnchecked(*this, vec);
}

bool GB_Matrix3x3::TransformVectors(const GB_Vector2d* inputVectors, GB_Vector2d* outputVectors, size_t numVectors, bool useOpenMP) const
{
    if (numVectors == 0)
    {
        return true;
    }

    if (!IsValid() || inputVectors == nullptr || outputVectors == nullptr || HasUnsupportedPartialOverlap(inputVectors, outputVectors, numVectors))
    {
        return false;
    }

    const bool useParallelLoop = useOpenMP && numVectors <= static_cast<size_t>(std::numeric_limits<long long>::max());
    if (useParallelLoop)
    {
#pragma omp parallel for schedule(static)
        for (long long vectorIndex = 0; vectorIndex < static_cast<long long>(numVectors); vectorIndex++)
        {
            outputVectors[vectorIndex] = TransformVectorUnchecked(*this, inputVectors[vectorIndex]);
        }
    }
    else
    {
        for (size_t vectorIndex = 0; vectorIndex < numVectors; vectorIndex++)
        {
            outputVectors[vectorIndex] = TransformVectorUnchecked(*this, inputVectors[vectorIndex]);
        }
    }
    return true;
}

bool GB_Matrix3x3::TransformVectors(const std::vector<GB_Vector2d>& inputVectors, std::vector<GB_Vector2d>& outputVectors, bool useOpenMP) const
{
    if (&inputVectors == &outputVectors)
    {
        return TransformVectors(outputVectors, useOpenMP);
    }

    if (inputVectors.empty())
    {
        outputVectors.clear();
        return true;
    }

    try
    {
        std::vector<GB_Vector2d> transformedVectors(inputVectors.size());
        if (!TransformVectors(inputVectors.data(), transformedVectors.data(), inputVectors.size(), useOpenMP))
        {
            return false;
        }
        outputVectors.swap(transformedVectors);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool GB_Matrix3x3::TransformVectors(GB_Vector2d* vectors, size_t numVectors, bool useOpenMP) const
{
    return TransformVectors(vectors, vectors, numVectors, useOpenMP);
}

bool GB_Matrix3x3::TransformVectors(std::vector<GB_Vector2d>& vectors, bool useOpenMP) const
{
    return vectors.empty() || TransformVectors(vectors.data(), vectors.data(), vectors.size(), useOpenMP);
}

GB_Matrix3x3 GB_Matrix3x3::CreateFromTranslation(double translateX, double translateY)
{
    if (!IsFinite(translateX) || !IsFinite(translateY))
    {
        return GB_Matrix3x3();
    }

    return GB_Matrix3x3(1, 0, translateX, 0, 1, translateY, 0, 0, 1);
}

GB_Matrix3x3 GB_Matrix3x3::CreateFromTranslation(const GB_Vector2d& translation)
{
    return CreateFromTranslation(translation.x, translation.y);
}

GB_Matrix3x3 GB_Matrix3x3::CreateFromRotation(double angle)
{
    if (!IsFinite(angle))
    {
        return GB_Matrix3x3();
    }

    const double cosAngle = std::cos(angle);
    const double sinAngle = std::sin(angle);
    return GB_Matrix3x3(cosAngle, -sinAngle, 0, sinAngle, cosAngle, 0, 0, 0, 1);
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
    return CreateFromScaling(scale, scale);
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
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "(" << GetClassType() << " " << std::setprecision(17)
        << m[0][0] << "," << m[0][1] << "," << m[0][2] << ","
        << m[1][0] << "," << m[1][1] << "," << m[1][2] << ","
        << m[2][0] << "," << m[2][1] << "," << m[2][2] << ")";
    return stream.str();
}

GB_ByteBuffer GB_Matrix3x3::SerializeToBinary() const
{
    constexpr static uint16_t payloadVersion = 1;

    GB_ByteBuffer buffer;
    buffer.reserve(88);
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
    std::istringstream stream(data);
    stream.imbue(std::locale::classic());

    char leftParenthesis = 0;
    std::string type;
    double values[9] = { GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan };
    if (!(stream >> leftParenthesis >> type) || leftParenthesis != '(' || type != GetClassType())
    {
        return false;
    }

    for (size_t valueIndex = 0; valueIndex < 9; valueIndex++)
    {
        if (!(stream >> values[valueIndex]))
        {
            return false;
        }

        char separator = 0;
        if (!(stream >> separator) || separator != (valueIndex < 8 ? ',' : ')'))
        {
            return false;
        }
    }

    stream >> std::ws;
    if (!stream.eof())
    {
        return false;
    }

    Set(values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8]);
    return true;
}

bool GB_Matrix3x3::Deserialize(const GB_ByteBuffer& data)
{
    constexpr static uint16_t expectedPayloadVersion = 1;
    constexpr static size_t expectedSize = 88;

    if (data.size() != expectedSize)
    {
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
        return false;
    }

    if (magic != GB_ClassMagicNumber || typeId != GetClassTypeId() || payloadVersion != expectedPayloadVersion || reserved != 0)
    {
        return false;
    }

    double values[9] = { GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan, GB_QuietNan };
    for (size_t valueIndex = 0; valueIndex < 9; valueIndex++)
    {
        if (!GB_ByteBufferIO::ReadDoubleLE(data, offset, values[valueIndex]))
        {
            return false;
        }
    }

    if (offset != data.size())
    {
        return false;
    }

    Set(values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8]);
    return true;
}

bool GB_Matrix3x3::TryInvertAffine2d(double tolerance)
{
    double absoluteTolerance = 0.0;
    if (!IsExactlyAffine2d(*this) || !TryGetAbsoluteTolerance(tolerance, absoluteTolerance))
    {
        return false;
    }

    const double linearScale = std::max(std::max(std::abs(m[0][0]), std::abs(m[0][1])), std::max(std::abs(m[1][0]), std::abs(m[1][1])));
    if (linearScale == 0.0)
    {
        return false;
    }

    const double normalized00 = m[0][0] / linearScale;
    const double normalized01 = m[0][1] / linearScale;
    const double normalized10 = m[1][0] / linearScale;
    const double normalized11 = m[1][1] / linearScale;
    const double normalizedDeterminant = std::fma(normalized00, normalized11, -normalized01 * normalized10);
    if (!IsFinite(normalizedDeterminant) || std::abs(normalizedDeterminant) <= absoluteTolerance)
    {
        return false;
    }

    const double inverseScaledDeterminant = 1.0 / (linearScale * normalizedDeterminant);
    if (!IsFinite(inverseScaledDeterminant))
    {
        return false;
    }

    const double inverse00 = normalized11 * inverseScaledDeterminant;
    const double inverse01 = -normalized01 * inverseScaledDeterminant;
    const double inverse10 = -normalized10 * inverseScaledDeterminant;
    const double inverse11 = normalized00 * inverseScaledDeterminant;
    const double inverseTranslateX = -std::fma(inverse00, m[0][2], inverse01 * m[1][2]);
    const double inverseTranslateY = -std::fma(inverse10, m[0][2], inverse11 * m[1][2]);

    const GB_Matrix3x3 result(inverse00, inverse01, inverseTranslateX, inverse10, inverse11, inverseTranslateY, 0, 0, 1);
    if (!result.IsValid())
    {
        return false;
    }

    *this = result;
    return true;
}
