#ifndef GLOBALBASE_INTERVAL_H_H
#define GLOBALBASE_INTERVAL_H_H

#include "GB_Math.h"

#include <cmath>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

// 区间类型（端点开闭）。
enum class GB_IntervalType
{
	Closed = 0,     // 闭区间：[a, b]
	RightOpen,      // 左闭右开：[a, b)
	LeftOpen,       // 左开右闭：(a, b]
	Open,           // 开区间：(a, b)
};

/**
 * @brief 一维区间模板。
 *
 * @tparam T       端点值类型（例如 int / double / 自定义可比较类型）。
 * @tparam Compare 比较器类型（默认 std::less<T>），必须满足 Strict Weak Ordering。
 */
template <typename T, typename Compare = std::less<T>>
class GB_Interval
{
public:
	static_assert(std::is_default_constructible<Compare>::value, "GB_Interval<T, Compare> requires Compare to be default constructible.");

	/**
	 * @brief 下端点。
	 *
	 * @note
	 * - 对浮点类型：若为 NaN/Inf，则 IsValid() 为 false，并在多数运算中被视为空集。
	 */
	T lower = T();

	/**
	 * @brief 上端点。
	 *
	 * @note
	 * - 对浮点类型：若为 NaN/Inf，则 IsValid() 为 false，并在多数运算中被视为空集。
	 */
	T upper = T();

	/**
	 * @brief 是否包含下端点（lower）。
	 *
	 * - true  表示形如 [lower, ... 或 (lower, ... 中的 '['。
	 * - false 表示形如 (lower, ... 中的 '('。
	 */
	bool includeLower = false;

	/**
	 * @brief 是否包含上端点（upper）。
	 *
	 * - true  表示形如 ..., upper] 中的 ']'
	 * - false 表示形如 ..., upper) 中的 ')'
	 */
	bool includeUpper = false;

	/**
	 * @brief 默认构造：Reset() 的效果。
	 *
	 * @details
	 * - lower/upper 置为 T()；
	 * - includeLower/includeUpper 置为 false；
	 * 因而在 lower==upper 的情况下，该区间表示空集。
	 */
	GB_Interval();

	/**
	 * @brief 以端点与开闭标记构造区间，并自动归一化。
	 *
	 * @param lower        下端点。
	 * @param upper        上端点。
	 * @param includeLower 是否包含下端点。
	 * @param includeUpper 是否包含上端点。
	 *
	 * @note 若 lower > upper，会自动交换端点及开闭标记。
	 */
	GB_Interval(const T& lower, const T& upper, bool includeLower = true, bool includeUpper = true);

	/**
	 * @brief 以端点与区间类型构造区间，并自动归一化。
	 *
	 * @param lower 下端点。
	 * @param upper 上端点。
	 * @param type  区间类型（闭/左闭右开/左开右闭/开）。
	 */
	GB_Interval(const T& lower, const T& upper, GB_IntervalType type);

	/**
	 * @brief 严格相等比较。
	 *
	 * @details
	 * - 对非浮点类型：基于 Compare 推导的“等价”关系判断端点是否相同，并比较开闭标记。
	 * - 对浮点类型：若任一端点为 NaN/Inf，则直接返回 false；否则仍使用 Compare 推导“等价”关系（不做容差）。
	 */
	bool operator==(const GB_Interval<T, Compare>& other) const;

	/**
	 * @brief 不等比较（operator== 的否定）。
	 */
	bool operator!=(const GB_Interval<T, Compare>& other) const;

	/**
	 * @brief 重置为“空区间”的默认形态（lower/upper=T()，端点均不包含）。
	 */
	void Reset();

	/**
	 * @brief 直接设置区间（默认两端都包含），并以默认容差 GB_Epsilon 归一化。
	 *
	 * @param lower 下端点。
	 * @param upper 上端点。
	 */
	void Set(const T& lower, const T& upper, bool includeLower = true, bool includeUpper = true);

	/**
	 * @brief 直接设置区间，并以指定容差归一化。
	 *
	 * @param lower        下端点。
	 * @param upper        上端点。
	 * @param includeLower 是否包含下端点。
	 * @param includeUpper 是否包含上端点。
	 * @param tolerance    绝对容差；对浮点取 abs(tolerance) 作为 epsilon；对非浮点忽略。
	 */
	void Set(const T& lower, const T& upper, bool includeLower, bool includeUpper, double tolerance);

	/**
	 * @brief 按区间类型设置（闭/左闭右开/左开右闭/开），并以默认容差 GB_Epsilon 归一化。
	 */
	void Set(const T& lower, const T& upper, GB_IntervalType type);

	/**
	 * @brief 按区间类型设置（闭/左闭右开/左开右闭/开），并以指定容差归一化。
	 */
	void Set(const T& lower, const T& upper, GB_IntervalType type, double tolerance);

	/**
	 * @brief 归一化区间表示。
	 *
	 * @details
	 * - 若 lower > upper（在容差意义下），则交换端点并交换 includeLower/includeUpper；
	 * - 若 lower == upper（在容差意义下），则 upper 被贴合为 lower。
	 *
	 * @param tolerance 绝对容差；对浮点取 abs(tolerance) 作为 epsilon；对非浮点忽略。
	 */
	void Normalize(double tolerance = GB_Epsilon);

	/**
	 * @brief 获取区间类型（四种开闭形式）。
	 */
	GB_IntervalType GetType() const;

	/**
	 * @brief 区间端点是否有效。
	 *
	 * @return
	 * - 对浮点：lower 与 upper 均为有限值（非 NaN/Inf）时返回 true；
	 * - 对非浮点：恒为 true。
	 */
	bool IsValid() const;

	/**
	 * @brief 判断是否为空集。
	 *
	 * @details
	 * 为空集的常见情形：
	 * - 端点无效（浮点 NaN/Inf）；
	 * - lower > upper；
	 * - lower == upper 但至少一个端点不包含（例如 (a,a)、[a,a)、(a,a]）。
	 */
	bool IsEmpty(double tolerance = GB_Epsilon) const;

	/**
	 * @brief 判断是否为单点集合（{x}）。
	 *
	 * @details
	 * 当 lower 与 upper 在容差意义下相等，且两端都包含时，区间表示单点集合。
	 */
	bool IsSingleton(double tolerance = GB_Epsilon) const;

	/**
	 * @brief 判断一个元素是否属于区间（含开闭语义与容差）。
	 *
	 * @param element   待判定元素。
	 * @param tolerance 绝对容差；对浮点取 abs(tolerance) 作为 epsilon；对非浮点忽略。
	 */
	bool IsContains(const T& element, double tolerance = GB_Epsilon) const;

	/**
	 * @brief 判断 other 是否为当前区间的子集（包含判定）。
	 *
	 * @details
	 * - other 为空集（包括端点无效时被视为空集）时，返回 true；
	 * - 当前区间为空集（包括端点无效时被视为空集）且 other 非空时，返回 false；
	 * - 其余情况按端点与开闭语义做包含判定。
	 */
	bool IsContains(const GB_Interval<T, Compare>& other, double tolerance = GB_Epsilon) const;

	/**
	 * @brief 判断两个区间是否有非空交集（“重叠”）。
	 *
	 * @details
	 * - 若交集为空集则返回 false；
	 * - 端点相接但相接点不属于交集时（例如 (a,b) 与 (b,c)），返回 false。
	 */
	bool IsOverlaps(const GB_Interval<T, Compare>& other, double tolerance = GB_Epsilon) const;

	/**
	 * @brief 求交：返回 this ∩ other。
	 *
	 * @return 交集区间；若交集为空集，返回 Reset() 状态（空区间）。
	 */
	GB_Interval<T, Compare> Intersected(const GB_Interval<T, Compare>& other, double tolerance = GB_Epsilon) const;

	/**
	 * @brief 求并：返回 this ∪ other。
	 *
	 * @details
	 * - 若可合并为单一区间，返回 size=1 的 vector；
	 * - 若不相交（或仅端点相接但该点不在并集中），返回 size=2 的 vector，按 lower 升序；
	 * - 若两者均为空集，返回空 vector。
	 */
	std::vector<GB_Interval<T, Compare>> United(const GB_Interval<T, Compare>& other, double tolerance = GB_Epsilon) const;

	/**
	 * @brief 求差：返回 this \ other（集合差）。
	 *
	 * @details
	 * 结果最多为两个不相交区间：
	 * - size=0：other 覆盖了 this；
	 * - size=1：一般情形；
	 * - size=2：other 从中间切开了 this。
	 *
	 * @return 结果区间集合，按 lower 升序。
	 */
	std::vector<GB_Interval<T, Compare>> Differenced(const GB_Interval<T, Compare>& other, double tolerance = GB_Epsilon) const;

	/**
	 * @brief operator+：并集（等价于 United(other, GB_Epsilon)）。
	 */
	std::vector<GB_Interval<T, Compare>> operator+(const GB_Interval<T, Compare>& other) const;

	/**
	 * @brief operator-：差集（等价于 Differenced(other, GB_Epsilon)）。
	 */
	std::vector<GB_Interval<T, Compare>> operator-(const GB_Interval<T, Compare>& other) const;

	/**
	 * @brief 求补集：返回 wholeInterval \ this。
	 *
	 * @param wholeInterval “全集”区间（补集相对于它来定义）。
	 */
	std::vector<GB_Interval<T, Compare>> Complemented(const GB_Interval<T, Compare>& wholeInterval, double tolerance = GB_Epsilon) const;

	/**
	 * @brief 容差近似相等比较。
	 *
	 * @details
	 * - 对浮点：端点在容差意义下相等，且 includeLower/includeUpper 完全相同，才返回 true；
	 * - 对非浮点：等价于 operator==（因为 epsilon 恒为 0）。
	 */
	bool IsNearEqual(const GB_Interval<T, Compare>& other, double tolerance = GB_Epsilon) const;

private:
	static const Compare& GetComparator()
	{
		static const Compare compare = Compare();
		return compare;
	}

	/**
	 * @brief 基于 Compare 推导“等价”关系：equiv(a,b) := !comp(a,b) && !comp(b,a)。
	 */
	static bool IsEquivalent(const T& a, const T& b);

	/**
	 * @brief 将区间类型转换为端点包含标记。
	 */
	static void TypeToFlags(GB_IntervalType type, bool& includeLower, bool& includeUpper);

	/**
	 * @brief 判断 value 是否为有限值。
	 *
	 * @details
	 * - 对浮点：使用 std::isfinite；
	 * - 对非浮点：恒返回 true。
	 */
	static bool IsFiniteValue(const T& value);
	static bool IsFiniteValueImpl(const T& value, std::true_type);
	static bool IsFiniteValueImpl(const T& value, std::false_type);

	/**
	 * @brief 将 tolerance 归一为用于比较的 epsilon。
	 *
	 * @details
	 * - 对浮点：epsilon = abs(tolerance)，且若 tolerance 非有限则视为 0；
	 * - 对非浮点：epsilon 恒为 0。
	 */
	static double GetCompareEpsilon(double tolerance);
	static double GetCompareEpsilonImpl(double tolerance, std::true_type);
	static double GetCompareEpsilonImpl(double tolerance, std::false_type);

	/**
	 * @brief 带 epsilon 的三态比较：返回 1/0/-1 分别表示 a>b / a==b / a<b。
	 *
	 * @details
	 * - 对浮点：基于 diff 与 epsilon 的绝对比较；
	 * - 对非浮点：基于 Compare 的顺序关系。
	 */
	static int CompareValueWithEpsilon(const T& a, const T& b, double epsilon);
	static int CompareValueWithEpsilonImpl(const T& a, const T& b, double epsilon, std::true_type);
	static int CompareValueWithEpsilonImpl(const T& a, const T& b, double /*epsilon*/, std::false_type);

	/**
	 * @brief IsEmpty 的内部实现（epsilon 已预先计算）。
	 */
	bool IsEmptyInternal(double epsilon) const;

	/**
	 * @brief IsOverlaps 的内部实现（epsilon 已预先计算）。
	 */
	bool IsOverlapsInternal(const GB_Interval<T, Compare>& other, double epsilon) const;
};

using GB_DoubleInterval = GB_Interval<double>;

using GB_IntInterval = GB_Interval<int>;

// ============================
// Template Implementations
// ============================

template <typename T, typename Compare>
GB_Interval<T, Compare>::GB_Interval()
{
	Reset();
}

template <typename T, typename Compare>
GB_Interval<T, Compare>::GB_Interval(const T& lower, const T& upper, bool includeLower, bool includeUpper)
{
	Set(lower, upper, includeLower, includeUpper);
}

template <typename T, typename Compare>
GB_Interval<T, Compare>::GB_Interval(const T& lower, const T& upper, GB_IntervalType type)
{
	Set(lower, upper, type);
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::operator==(const GB_Interval<T, Compare>& other) const
{
	// 对浮点：这里先排除 NaN/Inf，避免 Compare 在 NaN 上破坏严格弱序，从而导致错误“等价”。
	if (!IsFiniteValue(lower) || !IsFiniteValue(upper) || !IsFiniteValue(other.lower) || !IsFiniteValue(other.upper))
	{
		return false;
	}

	return IsEquivalent(lower, other.lower) && IsEquivalent(upper, other.upper)
		&& includeLower == other.includeLower
		&& includeUpper == other.includeUpper;
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::operator!=(const GB_Interval<T, Compare>& other) const
{
	return !(*this == other);
}

template <typename T, typename Compare>
void GB_Interval<T, Compare>::Reset()
{
	lower = T();
	upper = T();
	includeLower = false;
	includeUpper = false;
}

template <typename T, typename Compare>
void GB_Interval<T, Compare>::Set(const T& lower, const T& upper, bool includeLower, bool includeUpper)
{
	Set(lower, upper, includeLower, includeUpper, GB_Epsilon);
}

template <typename T, typename Compare>
void GB_Interval<T, Compare>::Set(const T& lower, const T& upper, bool includeLower, bool includeUpper, double tolerance)
{
	this->lower = lower;
	this->upper = upper;
	this->includeLower = includeLower;
	this->includeUpper = includeUpper;
	Normalize(tolerance);
}

template <typename T, typename Compare>
void GB_Interval<T, Compare>::Set(const T& lower, const T& upper, GB_IntervalType type)
{
	Set(lower, upper, type, GB_Epsilon);
}

template <typename T, typename Compare>
void GB_Interval<T, Compare>::Set(const T& lower, const T& upper, GB_IntervalType type, double tolerance)
{
	bool includeLower = false;
	bool includeUpper = false;
	TypeToFlags(type, includeLower, includeUpper);
	Set(lower, upper, includeLower, includeUpper, tolerance);
}

template <typename T, typename Compare>
void GB_Interval<T, Compare>::Normalize(double tolerance)
{
	if (!IsFiniteValue(lower) || !IsFiniteValue(upper))
	{
		// 对于 NaN/Inf，保持原样；IsEmpty 会将其视为空/无效。
		return;
	}

	const double epsilon = GetCompareEpsilon(tolerance);
	const int compareResult = CompareValueWithEpsilon(lower, upper, epsilon);
	if (compareResult > 0)
	{
		std::swap(lower, upper);
		std::swap(includeLower, includeUpper);
	}
	else if (compareResult == 0)
	{
		// 将 upper 贴合到 lower。
		upper = lower;
	}
}

template <typename T, typename Compare>
GB_IntervalType GB_Interval<T, Compare>::GetType() const
{
	if (includeLower && includeUpper)
	{
		return GB_IntervalType::Closed;
	}
	if (includeLower && !includeUpper)
	{
		return GB_IntervalType::RightOpen;
	}
	if (!includeLower && includeUpper)
	{
		return GB_IntervalType::LeftOpen;
	}
	return GB_IntervalType::Open;
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsValid() const
{
	return IsFiniteValue(lower) && IsFiniteValue(upper);
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsEmpty(double tolerance) const
{
	const double epsilon = GetCompareEpsilon(tolerance);
	return IsEmptyInternal(epsilon);
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsEmptyInternal(double epsilon) const
{
	if (!IsFiniteValue(lower) || !IsFiniteValue(upper))
	{
		return true;
	}

	const int compareResult = CompareValueWithEpsilon(lower, upper, epsilon);
	if (compareResult > 0)
	{
		return true;
	}
	if (compareResult == 0)
	{
		return !(includeLower && includeUpper);
	}
	return false;
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsSingleton(double tolerance) const
{
	if (!IsFiniteValue(lower) || !IsFiniteValue(upper))
	{
		return false;
	}

	const double epsilon = GetCompareEpsilon(tolerance);
	const int compareResult = CompareValueWithEpsilon(lower, upper, epsilon);
	if (compareResult != 0)
	{
		return false;
	}
	return includeLower && includeUpper;
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsContains(const T& element, double tolerance) const
{
	const double epsilon = GetCompareEpsilon(tolerance);
	if (IsEmptyInternal(epsilon))
	{
		return false;
	}
	if (!IsFiniteValue(element))
	{
		return false;
	}

	const int lowerCompare = CompareValueWithEpsilon(element, lower, epsilon);
	if (lowerCompare < 0)
	{
		return false;
	}
	if (lowerCompare == 0 && !includeLower)
	{
		return false;
	}

	const int upperCompare = CompareValueWithEpsilon(element, upper, epsilon);
	if (upperCompare > 0)
	{
		return false;
	}
	if (upperCompare == 0 && !includeUpper)
	{
		return false;
	}

	return true;
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsContains(const GB_Interval<T, Compare>& other, double tolerance) const
{
	const double epsilon = GetCompareEpsilon(tolerance);

	// 先处理空集：空集永远是任意集合的子集（包括端点无效时被视为空集的情形）。
	if (other.IsEmptyInternal(epsilon))
	{
		return true;
	}

	// 若当前区间为空集（包含端点无效 NaN/Inf 的情形），则无法包含非空 other。
	if (IsEmptyInternal(epsilon))
	{
		return false;
	}

	// 走到这里，两者都为非空区间，因此端点均为有限值（对浮点而言）。


	// lower 端
	const int lowerCompare = CompareValueWithEpsilon(other.lower, lower, epsilon);
	if (lowerCompare < 0)
	{
		return false;
	}
	if (lowerCompare == 0)
	{
		if (other.includeLower && !includeLower)
		{
			return false;
		}
	}

	// upper 端
	const int upperCompare = CompareValueWithEpsilon(other.upper, upper, epsilon);
	if (upperCompare > 0)
	{
		return false;
	}
	if (upperCompare == 0)
	{
		if (other.includeUpper && !includeUpper)
		{
			return false;
		}
	}

	return true;
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsOverlaps(const GB_Interval<T, Compare>& other, double tolerance) const
{
	const double epsilon = GetCompareEpsilon(tolerance);
	return IsOverlapsInternal(other, epsilon);
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsOverlapsInternal(const GB_Interval<T, Compare>& other, double epsilon) const
{
	if (IsEmptyInternal(epsilon) || other.IsEmptyInternal(epsilon))
	{
		return false;
	}

	// this.upper < other.lower  -> disjoint
	const int leftGap = CompareValueWithEpsilon(upper, other.lower, epsilon);
	if (leftGap < 0)
	{
		return false;
	}
	if (leftGap == 0 && !(includeUpper && other.includeLower))
	{
		return false;
	}

	// other.upper < this.lower -> disjoint
	const int rightGap = CompareValueWithEpsilon(other.upper, lower, epsilon);
	if (rightGap < 0)
	{
		return false;
	}
	if (rightGap == 0 && !(other.includeUpper && includeLower))
	{
		return false;
	}

	return true;
}

template <typename T, typename Compare>
GB_Interval<T, Compare> GB_Interval<T, Compare>::Intersected(const GB_Interval<T, Compare>& other, double tolerance) const
{
	GB_Interval<T, Compare> emptyResult;
	emptyResult.Reset();
	const double epsilon = GetCompareEpsilon(tolerance);

	if (IsEmptyInternal(epsilon) || other.IsEmptyInternal(epsilon))
	{
		return emptyResult;
	}

	// newLower = max(lower)
	T newLower = lower;
	bool newIncludeLower = includeLower;
	{
		const int compareLower = CompareValueWithEpsilon(lower, other.lower, epsilon);
		if (compareLower < 0)
		{
			newLower = other.lower;
			newIncludeLower = other.includeLower;
		}
		else if (compareLower == 0)
		{
			newLower = lower;
			newIncludeLower = includeLower && other.includeLower;
		}
	}

	// newUpper = min(upper)
	T newUpper = upper;
	bool newIncludeUpper = includeUpper;
	{
		const int compareUpper = CompareValueWithEpsilon(upper, other.upper, epsilon);
		if (compareUpper > 0)
		{
			newUpper = other.upper;
			newIncludeUpper = other.includeUpper;
		}
		else if (compareUpper == 0)
		{
			newUpper = upper;
			newIncludeUpper = includeUpper && other.includeUpper;
		}
	}

	// 关键：交集计算可能出现 newLower > newUpper（表示无交集）。
	// 不能调用 Set/Normalize（其会交换端点），否则会把“空集”错误地变成一个“合法区间”。
	const int orderCompare = CompareValueWithEpsilon(newLower, newUpper, epsilon);
	if (orderCompare > 0)
	{
		return emptyResult;
	}

	GB_Interval<T, Compare> result;
	result.lower = newLower;
	result.upper = (orderCompare == 0) ? newLower : newUpper;
	result.includeLower = newIncludeLower;
	result.includeUpper = newIncludeUpper;

	if (result.IsEmptyInternal(epsilon))
	{
		result.Reset();
	}
	return result;
}

template <typename T, typename Compare>
std::vector<GB_Interval<T, Compare>> GB_Interval<T, Compare>::United(const GB_Interval<T, Compare>& other, double tolerance) const
{
	std::vector<GB_Interval<T, Compare>> result;
	result.reserve(2);
	const double epsilon = GetCompareEpsilon(tolerance);

	if (IsEmptyInternal(epsilon))
	{
		if (!other.IsEmptyInternal(epsilon))
		{
			result.push_back(other);
		}
		return result;
	}
	if (other.IsEmptyInternal(epsilon))
	{
		result.push_back(*this);
		return result;
	}

	// 先按 lower 排序，避免对 T 做不必要的临时拷贝。
	const GB_Interval<T, Compare>* leftPtr = this;
	const GB_Interval<T, Compare>* rightPtr = &other;

	int lowerOrderCompare = CompareValueWithEpsilon(leftPtr->lower, rightPtr->lower, epsilon);
	if (lowerOrderCompare > 0)
	{
		std::swap(leftPtr, rightPtr);
		lowerOrderCompare = -lowerOrderCompare;
	}

	const bool mergedIncludeLower =
		(lowerOrderCompare == 0)
		? (leftPtr->includeLower || rightPtr->includeLower)
		: leftPtr->includeLower;

	// 检查 left 与 right 是否存在严格缺口。
	const int gapCompare = CompareValueWithEpsilon(leftPtr->upper, rightPtr->lower, epsilon);
	if (gapCompare < 0)
	{
		result.push_back(*leftPtr);
		result.push_back(*rightPtr);
		return result;
	}
	if (gapCompare == 0 && !(leftPtr->includeUpper || rightPtr->includeLower))
	{
		// (a,b) \cup (b,c) 仍然不是单一区间，因为 b 不属于并集。
		result.push_back(*leftPtr);
		result.push_back(*rightPtr);
		return result;
	}

	// 可合并为单一区间。
	const T newLower = leftPtr->lower;
	const bool newIncludeLower = mergedIncludeLower;

	T newUpper = leftPtr->upper;
	bool newIncludeUpper = leftPtr->includeUpper;
	{
		const int upperCompare = CompareValueWithEpsilon(leftPtr->upper, rightPtr->upper, epsilon);
		if (upperCompare < 0)
		{
			newUpper = rightPtr->upper;
			newIncludeUpper = rightPtr->includeUpper;
		}
		else if (upperCompare == 0)
		{
			newUpper = leftPtr->upper;
			newIncludeUpper = leftPtr->includeUpper || rightPtr->includeUpper;
		}
	}

	GB_Interval<T, Compare> merged;
	merged.Set(newLower, newUpper, newIncludeLower, newIncludeUpper, tolerance);
	result.push_back(merged);
	return result;
}

template <typename T, typename Compare>
std::vector<GB_Interval<T, Compare>> GB_Interval<T, Compare>::operator+(const GB_Interval<T, Compare>& other) const
{
	return United(other, GB_Epsilon);
}

template <typename T, typename Compare>
std::vector<GB_Interval<T, Compare>> GB_Interval<T, Compare>::operator-(const GB_Interval<T, Compare>& other) const
{
	return Differenced(other, GB_Epsilon);
}

template <typename T, typename Compare>
std::vector<GB_Interval<T, Compare>> GB_Interval<T, Compare>::Differenced(const GB_Interval<T, Compare>& other, double tolerance) const
{
	std::vector<GB_Interval<T, Compare>> result;
	result.reserve(2);
	const double epsilon = GetCompareEpsilon(tolerance);

	if (IsEmptyInternal(epsilon))
	{
		return result;
	}
	if (other.IsEmptyInternal(epsilon))
	{
		result.push_back(*this);
		return result;
	}

	if (!IsOverlapsInternal(other, epsilon))
	{
		result.push_back(*this);
		return result;
	}
	if (other.IsContains(*this, tolerance))
	{
		return result;
	}

	// 左段：[this.lower, other.lower)；当端点相等时，若 this 包含 lower 且 other 不包含 lower，则剩余为单点。
	{
		const int cutCompare = CompareValueWithEpsilon(other.lower, lower, epsilon);
		const bool hasLeftPart = (cutCompare > 0) || (cutCompare == 0 && includeLower && !other.includeLower);
		if (hasLeftPart)
		{
			GB_Interval<T, Compare> leftPart;
			leftPart.lower = lower;
			leftPart.includeLower = includeLower;
			leftPart.upper = other.lower;
			leftPart.includeUpper = !other.includeLower;
			// 避免 Normalize 交换端点导致语义错误：这里端点顺序由 hasLeftPart 保证。
			const int orderCompare = CompareValueWithEpsilon(leftPart.lower, leftPart.upper, epsilon);
			if (orderCompare == 0)
			{
				leftPart.upper = leftPart.lower;
			}
			if (!leftPart.IsEmptyInternal(epsilon))
			{
				result.push_back(leftPart);
			}
		}
	}

	// 右段：(other.upper, this.upper]；当端点相等时，若 this 包含 upper 且 other 不包含 upper，则剩余为单点。
	{
		const int cutCompare = CompareValueWithEpsilon(other.upper, upper, epsilon);
		const bool hasRightPart = (cutCompare < 0) || (cutCompare == 0 && includeUpper && !other.includeUpper);
		if (hasRightPart)
		{
			GB_Interval<T, Compare> rightPart;
			rightPart.lower = other.upper;
			rightPart.includeLower = !other.includeUpper;
			rightPart.upper = upper;
			rightPart.includeUpper = includeUpper;
			// 避免 Normalize 交换端点导致语义错误：这里端点顺序由 hasRightPart 保证。
			const int orderCompare = CompareValueWithEpsilon(rightPart.lower, rightPart.upper, epsilon);
			if (orderCompare == 0)
			{
				rightPart.upper = rightPart.lower;
			}
			if (!rightPart.IsEmptyInternal(epsilon))
			{
				result.push_back(rightPart);
			}
		}
	}

	// 保证 lower 升序
	if (result.size() == 2)
	{
		if (CompareValueWithEpsilon(result[0].lower, result[1].lower, epsilon) > 0)
		{
			std::swap(result[0], result[1]);
		}
	}
	return result;
}

template <typename T, typename Compare>
std::vector<GB_Interval<T, Compare>> GB_Interval<T, Compare>::Complemented(const GB_Interval<T, Compare>& wholeInterval, double tolerance) const
{
	return wholeInterval.Differenced(*this, tolerance);
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsNearEqual(const GB_Interval<T, Compare>& other, double tolerance) const
{
	if (!IsFiniteValue(lower) || !IsFiniteValue(upper) || !IsFiniteValue(other.lower) || !IsFiniteValue(other.upper))
	{
		return false;
	}

	const double epsilon = GetCompareEpsilon(tolerance);

	return CompareValueWithEpsilon(lower, other.lower, epsilon) == 0
		&& CompareValueWithEpsilon(upper, other.upper, epsilon) == 0
		&& includeLower == other.includeLower
		&& includeUpper == other.includeUpper;
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsEquivalent(const T& a, const T& b)
{
	const Compare& compare = GetComparator();
	return !compare(a, b) && !compare(b, a);
}

template <typename T, typename Compare>
void GB_Interval<T, Compare>::TypeToFlags(GB_IntervalType type, bool& includeLower, bool& includeUpper)
{
	switch (type)
	{
	case GB_IntervalType::Closed:
		includeLower = true;
		includeUpper = true;
		break;
	case GB_IntervalType::RightOpen:
		includeLower = true;
		includeUpper = false;
		break;
	case GB_IntervalType::LeftOpen:
		includeLower = false;
		includeUpper = true;
		break;
	case GB_IntervalType::Open:
	default:
		includeLower = false;
		includeUpper = false;
		break;
	}
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsFiniteValue(const T& value)
{
	return IsFiniteValueImpl(value, typename std::is_floating_point<T>::type());
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsFiniteValueImpl(const T& value, std::true_type)
{
	const long double longDoubleValue = static_cast<long double>(value);
	return std::isfinite(longDoubleValue);
}

template <typename T, typename Compare>
bool GB_Interval<T, Compare>::IsFiniteValueImpl(const T& /*value*/, std::false_type)
{
	return true;
}

template <typename T, typename Compare>
double GB_Interval<T, Compare>::GetCompareEpsilon(double tolerance)
{
	return GetCompareEpsilonImpl(tolerance, typename std::is_floating_point<T>::type());
}

template <typename T, typename Compare>
double GB_Interval<T, Compare>::GetCompareEpsilonImpl(double tolerance, std::true_type)
{
	if (!std::isfinite(tolerance))
	{
		return 0;
	}
	return (tolerance >= 0) ? tolerance : -tolerance;
}

template <typename T, typename Compare>
double GB_Interval<T, Compare>::GetCompareEpsilonImpl(double /*tolerance*/, std::false_type)
{
	return 0;
}

template <typename T, typename Compare>
int GB_Interval<T, Compare>::CompareValueWithEpsilon(const T& a, const T& b, double epsilon)
{
	return CompareValueWithEpsilonImpl(a, b, epsilon, typename std::is_floating_point<T>::type());
}

template <typename T, typename Compare>
int GB_Interval<T, Compare>::CompareValueWithEpsilonImpl(const T& a, const T& b, double epsilon, std::true_type)
{
	const long double safeEpsilon = (epsilon >= 0 && std::isfinite(epsilon)) ? static_cast<long double>(epsilon) : 0.0L;
	const long double diff = static_cast<long double>(a) - static_cast<long double>(b);
	if (diff > safeEpsilon)
	{
		return 1;
	}
	if (diff < -safeEpsilon)
	{
		return -1;
	}
	return 0;
}

template <typename T, typename Compare>
int GB_Interval<T, Compare>::CompareValueWithEpsilonImpl(const T& a, const T& b, double /*epsilon*/, std::false_type)
{
	const Compare& compare = GetComparator();
	if (compare(a, b))
	{
		return -1;
	}
	if (compare(b, a))
	{
		return 1;
	}
	return 0;
}


#endif
