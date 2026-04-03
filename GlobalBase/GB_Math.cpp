#include "GB_Math.h"
#include <cerrno>
#include <cstdlib>
#include <locale.h>

#if !defined(_WIN32) && !defined(LC_NUMERIC_MASK)
#define LC_NUMERIC_MASK (1 << LC_NUMERIC)
#endif

std::string GB_RandomString(size_t length, const std::vector<char32_t>& characterPool)
{
	if (length == 0 || characterPool.empty())
	{
		return std::string();
	}

	auto AppendUtf8FromCodePoint = [](std::string& output, char32_t codePoint) {
		// 过滤无效码点：代理项范围 / 超出 Unicode 上限
		if ((codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF)
		{
			codePoint = U'?';
		}

		if (codePoint <= 0x7F)
		{
			output.push_back(static_cast<char>(codePoint));
		}
		else if (codePoint <= 0x7FF)
		{
			output.push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		}
		else if (codePoint <= 0xFFFF)
		{
			output.push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		}
		else
		{
			output.push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		}
		};

	static thread_local std::mt19937 generator(std::random_device{}());
	std::uniform_int_distribution<size_t> distribution(0, characterPool.size() - 1);

	std::string result;
	result.reserve(length * 4);

	for (size_t i = 0; i < length; i++)
	{
		const char32_t character = characterPool[distribution(generator)];
		AppendUtf8FromCodePoint(result, character);
	}

	return result;
}

std::string GB_RandomString(size_t length)
{
	static const std::vector<char32_t> defaultCharacterPool = []() {
		const std::u32string poolString = U"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
		return std::vector<char32_t>(poolString.begin(), poolString.end());
		}();

	return GB_RandomString(length, defaultCharacterPool);
}

namespace
{
	inline bool IsAsciiSpace(char character)
	{
		return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\v' || character == '\f';
	}

	inline void SkipAsciiSpaces(const char*& cursor, const char* end)
	{
		while (cursor < end && IsAsciiSpace(*cursor))
		{
			cursor++;
		}
	}

	template<typename UnsignedT>
	bool TryParseUnsignedDecimal(const std::string& text, UnsignedT& outValue)
	{
		const char* const begin = text.c_str();
		const char* const end = begin + text.size();
		const char* cursor = begin;

		SkipAsciiSpaces(cursor, end);
		if (cursor == end)
		{
			return false;
		}

		if (*cursor == '+')
		{
			cursor++;
		}
		else if (*cursor == '-')
		{
			return false;
		}

		if (cursor == end)
		{
			return false;
		}

		const UnsignedT maxValue = std::numeric_limits<UnsignedT>::max();
		UnsignedT value = 0;
		bool hasDigits = false;

		while (cursor < end)
		{
			const char character = *cursor;
			if (character < '0' || character > '9')
			{
				break;
			}

			hasDigits = true;
			const UnsignedT digit = static_cast<UnsignedT>(character - '0');

			if (value > (maxValue - digit) / 10)
			{
				return false;
			}

			value = static_cast<UnsignedT>(value * 10 + digit);
			cursor++;
		}

		if (!hasDigits)
		{
			return false;
		}

		SkipAsciiSpaces(cursor, end);
		if (cursor != end)
		{
			return false;
		}

		outValue = value;
		return true;
	}

	template<typename SignedT>
	bool TryParseSignedDecimal(const std::string& text, SignedT& outValue)
	{
		const char* const begin = text.c_str();
		const char* const end = begin + text.size();
		const char* cursor = begin;

		SkipAsciiSpaces(cursor, end);
		if (cursor == end)
		{
			return false;
		}

		bool isNegative = false;
		if (*cursor == '+')
		{
			cursor++;
		}
		else if (*cursor == '-')
		{
			isNegative = true;
			cursor++;
		}

		if (cursor == end)
		{
			return false;
		}

		constexpr unsigned long long positiveLimit = static_cast<unsigned long long>(std::numeric_limits<SignedT>::max());
		constexpr unsigned long long negativeLimit = positiveLimit + 1ULL;
		const unsigned long long limit = isNegative ? negativeLimit : positiveLimit;

		unsigned long long value = 0;
		bool hasDigits = false;

		while (cursor < end)
		{
			const char character = *cursor;
			if (character < '0' || character > '9')
			{
				break;
			}

			hasDigits = true;
			const unsigned long long digit = static_cast<unsigned long long>(character - '0');

			if (value > (limit - digit) / 10ULL)
			{
				return false;
			}

			value = value * 10ULL + digit;
			cursor++;
		}

		if (!hasDigits)
		{
			return false;
		}

		SkipAsciiSpaces(cursor, end);
		if (cursor != end)
		{
			return false;
		}

		if (!isNegative)
		{
			outValue = static_cast<SignedT>(value);
			return true;
		}

		if (value == negativeLimit)
		{
			outValue = std::numeric_limits<SignedT>::min();
			return true;
		}

		outValue = static_cast<SignedT>(-static_cast<long long>(value));
		return true;
	}

	inline double StrToDoubleCLocale(const char* text, char** parseEnd)
	{
#if defined(_WIN32)
		struct CLocaleHolder
		{
			_locale_t locale;

			CLocaleHolder()
				: locale(_create_locale(LC_NUMERIC, "C"))
			{
			}

			~CLocaleHolder()
			{
				if (locale)
				{
					_free_locale(locale);
					locale = nullptr;
				}
			}
		};

		static thread_local CLocaleHolder cLocaleHolder;
		if (cLocaleHolder.locale)
		{
			return _strtod_l(text, parseEnd, cLocaleHolder.locale);
		}

		return std::strtod(text, parseEnd);
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
		// POSIX 线程安全 locale 版本：避免全局 setlocale 的副作用
		struct CLocaleHolder
		{
			locale_t locale;

			CLocaleHolder()
				: locale(newlocale(LC_NUMERIC_MASK, "C", (locale_t)0))
			{
			}

			~CLocaleHolder()
			{
				if (locale)
				{
					freelocale(locale);
					locale = (locale_t)0;
				}
			}
		};

		static thread_local CLocaleHolder cLocaleHolder;
		if (cLocaleHolder.locale)
		{
			return ::strtod_l(text, parseEnd, cLocaleHolder.locale);
		}

		return std::strtod(text, parseEnd);
#else
		return std::strtod(text, parseEnd);
#endif
	}

	inline bool TryParseDouble(const std::string& text, double& outValue)
	{
		const char* const begin = text.c_str();
		const char* const end = begin + text.size();
		const char* cursor = begin;

		SkipAsciiSpaces(cursor, end);
		if (cursor == end)
		{
			return false;
		}

		errno = 0;
		char* parseEnd = nullptr;
		const double value = StrToDoubleCLocale(cursor, &parseEnd);

		if (parseEnd == cursor)
		{
			return false;
		}

		const char* after = parseEnd;
		SkipAsciiSpaces(after, end);
		if (after != end)
		{
			return false;
		}

		// 溢出：errno=ERANGE 且返回值为 HUGE_VAL/-HUGE_VAL（部分实现 HUGE_VAL 可能为无穷大）
		if (errno == ERANGE && (value == HUGE_VAL || value == -HUGE_VAL))
		{
			return false;
		}

		// 不接受非有限值（NaN/Inf）
		if (!std::isfinite(value))
		{
			return false;
		}

		outValue = value;
		return true;
	}
}

int GB_ToInt(const std::string& str, int defaultValue, bool* isOk)
{
	int parsedValue = 0;
	const bool success = TryParseSignedDecimal<int>(str, parsedValue);

	if (isOk)
	{
		*isOk = success;
	}

	return success ? parsedValue : defaultValue;
}

unsigned int GB_ToUInt(const std::string& str, unsigned int defaultValue, bool* isOk)
{
	unsigned int parsedValue = 0;
	const bool success = TryParseUnsignedDecimal<unsigned int>(str, parsedValue);

	if (isOk)
	{
		*isOk = success;
	}

	return success ? parsedValue : defaultValue;
}

long long GB_ToLongLong(const std::string& str, long long defaultValue, bool* isOk)
{
	long long parsedValue = 0;
	const bool success = TryParseSignedDecimal<long long>(str, parsedValue);

	if (isOk)
	{
		*isOk = success;
	}

	return success ? parsedValue : defaultValue;
}

unsigned long long GB_ToULongLong(const std::string& str, unsigned long long defaultValue, bool* isOk)
{
	unsigned long long parsedValue = 0;
	const bool success = TryParseUnsignedDecimal<unsigned long long>(str, parsedValue);

	if (isOk)
	{
		*isOk = success;
	}

	return success ? parsedValue : defaultValue;
}

double GB_ToDouble(const std::string& str, double defaultValue, bool* isOk)
{
	double parsedValue = 0.0;
	const bool success = TryParseDouble(str, parsedValue);

	if (isOk)
	{
		*isOk = success;
	}

	return success ? parsedValue : defaultValue;
}


