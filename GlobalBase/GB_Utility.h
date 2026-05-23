#ifndef GLOBALBASE_UTILITY_H_H
#define GLOBALBASE_UTILITY_H_H

#include <vector>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <iterator>
#include <type_traits>
#include <chrono>
#include <thread>

/**
 * @brief 删除单个由 new 创建的对象，并将指针置空。
 */
template <typename T>
void GB_Delete(T*& ptr)
{
	delete ptr;
	ptr = nullptr;
}

/**
 * @brief 删除单个由 new[] 创建的数组，并将指针置空。
 */
template <typename T>
void GB_DeleteArray(T*& ptr)
{
	delete[] ptr;
	ptr = nullptr;
}

/**
 * @brief 删除 vector 中所有裸指针指向的对象，并将每个元素置空。
 */
template <typename T>
void GB_DeleteElements(std::vector<T*>& ptrVec)
{
	for (T*& element : ptrVec)
	{
		GB_Delete(element);
	}
}

/**
 * @brief 删除 vector 中所有裸指针指向的对象，并清空 vector。
 */
template <typename T>
void GB_DeleteElementsAndClear(std::vector<T*>& ptrVec)
{
	GB_DeleteElements(ptrVec);
	ptrVec.clear();
}

namespace GB_UtilityDetail
{
	inline std::size_t GB_CheckedAddSize(std::size_t leftSize, std::size_t rightSize, std::size_t maxSize)
	{
		if (rightSize > maxSize || leftSize > maxSize - rightSize)
		{
			throw std::length_error("GB_Utility vector size is too large.");
		}
		return leftSize + rightSize;
	}

	template <typename T>
	std::size_t GB_GetFlattenedVectorSize(const std::vector<std::vector<T>>& nestedVec, std::size_t maxSize)
	{
		std::size_t totalSize = 0;
		for (const std::vector<T>& innerVec : nestedVec)
		{
			totalSize = GB_CheckedAddSize(totalSize, innerVec.size(), maxSize);
		}
		return totalSize;
	}
}

/**
 * @brief 将二维 vector 展平成一维 vector。
 */
template <typename T>
std::vector<T> GB_FlattenVector(const std::vector<std::vector<T>>& nestedVec)
{
	std::vector<T> flatVec;
	const std::size_t totalSize = GB_UtilityDetail::GB_GetFlattenedVectorSize(nestedVec, flatVec.max_size());
	flatVec.reserve(totalSize);
	for (const std::vector<T>& innerVec : nestedVec)
	{
		flatVec.insert(flatVec.end(), innerVec.begin(), innerVec.end());
	}
	return flatVec;
}

/**
 * @brief 将二维 vector 展平成一维 vector，元素从输入容器中移动出来。
 */
template <typename T>
std::vector<T> GB_FlattenVector(std::vector<std::vector<T>>&& nestedVec)
{
	std::vector<T> flatVec;
	const std::size_t totalSize = GB_UtilityDetail::GB_GetFlattenedVectorSize(nestedVec, flatVec.max_size());
	flatVec.reserve(totalSize);
	for (std::vector<T>& innerVec : nestedVec)
	{
		flatVec.insert(flatVec.end(), std::make_move_iterator(innerVec.begin()), std::make_move_iterator(innerVec.end()));
	}
	return flatVec;
}

/**
 * @brief 提取 shared_ptr vector 中的裸指针。返回的裸指针不拥有对象生命周期。
 */
template <typename T>
std::vector<T*> GB_ExtractRawPtr(const std::vector<std::shared_ptr<T>>& sharedPtrVec)
{
	const std::size_t size = sharedPtrVec.size();
	std::vector<T*> rawPtrVec(size, nullptr);
	for (std::size_t i = 0; i < size; i++)
	{
		rawPtrVec[i] = sharedPtrVec[i].get();
	}
	return rawPtrVec;
}

/**
 * @brief 提取 unique_ptr vector 中的裸指针。返回的裸指针不拥有对象生命周期。
 */
template <typename T>
std::vector<T*> GB_ExtractRawPtr(const std::vector<std::unique_ptr<T>>& uniquePtrVec)
{
	const std::size_t size = uniquePtrVec.size();
	std::vector<T*> rawPtrVec(size, nullptr);
	for (std::size_t i = 0; i < size; i++)
	{
		rawPtrVec[i] = uniquePtrVec[i].get();
	}
	return rawPtrVec;
}

/**
 * @brief 判断裸指针 vector 中是否存在 nullptr。
 */
template <typename T>
bool GB_ContainsNullptr(const std::vector<T*>& ptrVec)
{
	for (const T* element : ptrVec)
	{
		if (element == nullptr)
		{
			return true;
		}
	}
	return false;
}

/**
 * @brief 判断 shared_ptr vector 中是否存在空指针。
 */
template <typename T>
bool GB_ContainsNullptr(const std::vector<std::shared_ptr<T>>& ptrVec)
{
	for (const std::shared_ptr<T>& element : ptrVec)
	{
		if (!element)
		{
			return true;
		}
	}
	return false;
}

/**
 * @brief 判断 unique_ptr vector 中是否存在空指针。
 */
template <typename T>
bool GB_ContainsNullptr(const std::vector<std::unique_ptr<T>>& ptrVec)
{
	for (const std::unique_ptr<T>& element : ptrVec)
	{
		if (!element)
		{
			return true;
		}
	}
	return false;
}

/**
 * @brief 判断二维裸指针 vector 中是否存在 nullptr。
 */
template <typename T>
bool GB_ContainsNullptr(const std::vector<std::vector<T*>>& nestedPtrVec)
{
	for (const std::vector<T*>& innerVec : nestedPtrVec)
	{
		if (GB_ContainsNullptr(innerVec))
		{
			return true;
		}
	}
	return false;
}

/**
 * @brief 将裸指针 vector 中的非空指针解引用为值 vector，自动跳过 nullptr。
 */
template <typename T>
std::vector<T> GB_DereferencePtrVector(const std::vector<T*>& ptrVec)
{
	std::vector<T> valueVec;
	valueVec.reserve(ptrVec.size());
	for (const T* ptr : ptrVec)
	{
		if (ptr != nullptr)
		{
			valueVec.push_back(*ptr);
		}
	}
	return valueVec;
}

/**
 * @brief 将裸指针 vector 转换为另一个裸指针 vector。转换失败的位置保留为 nullptr。
 */
template <typename OriginalType, typename TargetType>
std::vector<TargetType*> GB_DynamicCastVector(const std::vector<OriginalType*>& originalVec)
{
	const std::size_t size = originalVec.size();
	std::vector<TargetType*> targetVec(size, nullptr);
	for (std::size_t i = 0; i < size; i++)
	{
		targetVec[i] = dynamic_cast<TargetType*>(originalVec[i]);
	}
	return targetVec;
}

/**
 * @brief 将裸指针 vector 通过 static_cast 转换为另一个裸指针 vector。
 */
template <typename OriginalType, typename TargetType>
std::vector<TargetType*> GB_StaticCastVector(const std::vector<OriginalType*>& originalVec)
{
	const std::size_t size = originalVec.size();
	std::vector<TargetType*> targetVec(size, nullptr);
	for (std::size_t i = 0; i < size; i++)
	{
		targetVec[i] = static_cast<TargetType*>(originalVec[i]);
	}
	return targetVec;
}

/**
 * @brief 将裸指针转移到 unique_ptr，并将原裸指针置空。
 */
template <typename T>
std::unique_ptr<T> GB_TakeUniquePtr(T*& ptr)
{
	std::unique_ptr<T> resultPtr(ptr);
	ptr = nullptr;
	return resultPtr;
}

/**
 * @brief 判断 vector 中是否存在指定值。
 */
template <typename T>
bool GB_Contains(const std::vector<T>& vec, const T& value)
{
	return std::find(vec.begin(), vec.end(), value) != vec.end();
}

constexpr static std::size_t GB_InvalidIndex = static_cast<std::size_t>(-1);

/**
 * @brief 查找指定值第一次出现的位置；不存在则返回 GB_InvalidIndex。
 */
template <typename T>
std::size_t GB_IndexOf(const std::vector<T>& vec, const T& value)
{
	const typename std::vector<T>::const_iterator it = std::find(vec.begin(), vec.end(), value);
	if (it == vec.end())
	{
		return GB_InvalidIndex;
	}
	return static_cast<std::size_t>(std::distance(vec.begin(), it));
}

/**
 * @brief 取得枚举的底层整数值。
 */
template <typename EnumType>
typename std::underlying_type<EnumType>::type GB_ToUnderlying(EnumType value)
{
	static_assert(std::is_enum<EnumType>::value, "GB_ToUnderlying only supports enum types.");
	return static_cast<typename std::underlying_type<EnumType>::type>(value);
}

/**
 * @brief 判断枚举位标志中是否包含指定标志。
 */
template <typename EnumType>
bool GB_HasFlag(EnumType value, EnumType flag)
{
	static_assert(std::is_enum<EnumType>::value, "GB_HasFlag only supports enum types.");
	using UnderlyingType = typename std::underlying_type<EnumType>::type;
	return (static_cast<UnderlyingType>(value) & static_cast<UnderlyingType>(flag)) == static_cast<UnderlyingType>(flag);
}

inline void GB_SleepFor(unsigned int milliseconds)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}


#endif // !GLOBALBASE_UTILITY_H_H