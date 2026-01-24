#ifndef GLOBALBASE_MATH_H_H
#define GLOBALBASE_MATH_H_H

#include "GlobalBasePort.h"
#include <cmath>
#include <numeric>
#include <limits>
#include <random>

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

constexpr static int GB_IntMin = std::numeric_limits<int>::min();
constexpr static int GB_IntMax = std::numeric_limits<int>::max();
constexpr static unsigned int GB_UIntMax = std::numeric_limits<unsigned int>::max();
constexpr static unsigned long long GB_ULongLongMax = std::numeric_limits<unsigned long long>::max();
constexpr static double GB_DoubleMin = std::numeric_limits<double>::lowest();
constexpr static double GB_DoubleMax = std::numeric_limits<double>::max();

constexpr static double GB_Pi = 3.14159265358979323846;
constexpr static double GB_2Pi = 2 * GB_Pi;
constexpr static double GB_3Pi = 3 * GB_Pi;
constexpr static double GB_4Pi = 4 * GB_Pi;
constexpr static double GB_HalfPi = GB_Pi / 2;
constexpr static double GB_ThreeHalfPi = 3 * GB_HalfPi;
constexpr static double GB_QuarterPi = GB_Pi / 4;
constexpr static double GB_ThreeQuarterPi = 3 * GB_QuarterPi;
constexpr static double GB_FiveQuarterPi = 5 * GB_QuarterPi;
constexpr static double GB_SevenQuarterPi = 7 * GB_QuarterPi;

constexpr static double GB_DegToRad = GB_Pi / 180;
constexpr static double GB_RadToDeg = 180 / GB_Pi;

constexpr static double GB_Epsilon = 1e-10;

constexpr static double GB_QuietNan = std::numeric_limits<double>::quiet_NaN();

/**
 * @brief 将 value 限制在 [minValue, maxValue] 内。
 * @note 前置条件：minValue <= maxValue（调用方保证；否则返回值语义不可靠）。
 */
template<class T>
inline T GB_Clamp(const T& value, const T& minValue, const T& maxValue)
{
	return (value < minValue) ? minValue : ((value > maxValue) ? maxValue : value);
}

/**
 * @brief 线性插值：a + (b - a) * t
 * @note 一般 t 取 [0, 1] 表示在 a->b 之间插值；
 *       若 t 超出 [0,1]，则含义为“外推/延拓”，本函数不会自动 clamp t。
 */
inline double GB_Lerp(double a, double b, double t)
{
	return a + (b - a) * t;
}

// value > 0
inline bool GB_IsPositive(double value, double epsilon = GB_Epsilon)
{
	return value > epsilon;
}

// value <= 0
inline bool GB_IsNonPositive(double value, double epsilon = GB_Epsilon)
{
	return value <= epsilon;
}

// value < 0
inline bool GB_IsNegative(double value, double epsilon = GB_Epsilon)
{
	return value < -epsilon;
}

// value >= 0
inline bool GB_IsNonNegative(double value, double epsilon = GB_Epsilon)
{
	return value >= -epsilon;
}

// value == 0
inline bool GB_IsZero(double value, double epsilon = GB_Epsilon)
{
	return value >= -epsilon && value <= epsilon;
}

// value != 0
inline bool GB_IsNonZero(double value, double epsilon = GB_Epsilon)
{
	return value < -epsilon || value > epsilon;
}

// a == b
inline bool GB_DoubleEquals(double a, double b, double epsilon = GB_Epsilon)
{
	return GB_IsZero(a - b, epsilon);
}

// a != b
inline bool GB_DoubleNotEquals(double a, double b, double epsilon = GB_Epsilon)
{
	return GB_IsNonZero(a - b, epsilon);
}

// a > b
inline bool GB_DoubleLarger(double a, double b, double epsilon = GB_Epsilon)
{
	return GB_IsPositive(a - b, epsilon);
}

// a >= b
inline bool GB_DoubleLargerOrEquals(double a, double b, double epsilon = GB_Epsilon)
{
	return GB_IsNonNegative(a - b, epsilon);
}

// a < b
inline bool GB_DoubleSmaller(double a, double b, double epsilon = GB_Epsilon)
{
	return GB_IsNegative(a - b, epsilon);
}

// a <= b
inline bool GB_DoubleSmallerOrEquals(double a, double b, double epsilon = GB_Epsilon)
{
	return GB_IsNonPositive(a - b, epsilon);
}

/**
 * @brief 双精度比较：返回 1/0/-1，分别表示 a>b / a==b / a<b（基于绝对容差 epsilon）。
 * @note 若 a 或 b 为 NaN，则所有大小比较结果都为 false，可能导致返回 0；
 *       调用方若关心 NaN，请在外部先做 std::isfinite/std::isnan 判断。
 */
inline int GB_DoubleCompare(double a, double b, double epsilon = GB_Epsilon)
{
	if (GB_DoubleLarger(a, b, epsilon))
	{
		return 1;
	}
	else if (GB_DoubleSmaller(a, b, epsilon))
	{
		return -1;
	}
	return 0;
}

// 角度值归一化到 [-180, 180]
inline double GB_DegNormalize(double degrees)
{
	double mod = std::fmod(degrees, 360.0);
	if (mod < -180)
	{
		mod += 360;
	}
	if (mod > 180)
	{
		mod -= 360;
	}
	return mod;
}

// 弧度值归一化到 [-π, π]
inline double GB_RadNormalize(double rad)
{
	double mod = std::fmod(rad, GB_2Pi);
	if (mod < -GB_Pi)
	{
		mod += GB_2Pi;
	}
	if (mod > GB_Pi)
	{
		mod -= GB_2Pi;
	}
	return mod;
}

// 生成 [minValue, maxValue] 范围内的随机整数
inline int GB_RandomInt(int minValue, int maxValue)
{
	static thread_local std::mt19937 generator(std::random_device{}());
	std::uniform_int_distribution<int> distribution(minValue, maxValue);
	return distribution(generator);
}

// 生成 [minValue, maxValue) 范围内的随机 double 浮点数
inline double GB_RandomDouble(double minValue, double maxValue)
{
	static thread_local std::mt19937 generator(std::random_device{}());
	std::uniform_real_distribution<double> distribution(minValue, maxValue);
	return distribution(generator);
}




#endif