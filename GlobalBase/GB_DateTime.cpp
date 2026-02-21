#include "GB_DateTime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <sstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
	constexpr long long kUnixEpochJdn = 2440588LL; // 1970-01-01
	constexpr uint32_t kDateBinaryVersion = 1;
	constexpr size_t kIsoDateStringLength = 10; // YYYY-MM-DD

#if !defined(_WIN32)
	extern "C" std::time_t timegm(::tm* timeptr);
#endif


	inline bool IsAsciiWhitespace(unsigned char ch)
	{
		return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
	}

	inline bool IsUtf8BomAt(const char* data, size_t length, size_t offset)
	{
		if (data == nullptr)
		{
			return false;
		}
		if (offset + 3 > length)
		{
			return false;
		}
		return static_cast<unsigned char>(data[offset + 0]) == 0xEF
			&& static_cast<unsigned char>(data[offset + 1]) == 0xBB
			&& static_cast<unsigned char>(data[offset + 2]) == 0xBF;
	}

	inline void GetTrimmedAsciiRange(const char* data, size_t length, size_t& start, size_t& end)
	{
		start = 0;
		end = length;
		while (start < end && IsAsciiWhitespace(static_cast<unsigned char>(data[start])))
		{
			start++;
		}

		while (end > start && IsAsciiWhitespace(static_cast<unsigned char>(data[end - 1])))
		{
			end--;
		}

		// Tolerate UTF-8 BOM (EF BB BF) after trimming ASCII whitespace.
		if (IsUtf8BomAt(data, length, start))
		{
			start += 3;
			while (start < end && IsAsciiWhitespace(static_cast<unsigned char>(data[start])))
			{
				start++;
			}
		}
	}

	inline void GetTrimmedAsciiRange(const std::string& text, size_t& start, size_t& end)
	{
		GetTrimmedAsciiRange(text.data(), text.size(), start, end);
	}

	inline bool StartsWith(const char* data, size_t length, const char* prefix, size_t prefixLength)
	{
		if (data == nullptr || prefix == nullptr)
		{
			return false;
		}
		if (length < prefixLength)
		{
			return false;
		}
		return std::memcmp(data, prefix, prefixLength) == 0;
	}

	inline bool IsAllDigits(const char* data, size_t length)
	{
		if (data == nullptr)
		{
			return false;
		}
		for (size_t i = 0; i < length; i++)
		{
			const char ch = data[i];
			if (ch < '0' || ch > '9')
			{
				return false;
			}
		}
		return true;
	}

	inline bool ParseFixedDigits(const char* data, size_t length, size_t offset, size_t count, int& outValue)
	{
		if (data == nullptr)
		{
			return false;
		}
		if (offset + count > length)
		{
			return false;
		}

		int value = 0;
		for (size_t i = 0; i < count; i++)
		{
			const char ch = data[offset + i];
			if (ch < '0' || ch > '9')
			{
				return false;
			}
			value = value * 10 + (ch - '0');
		}

		outValue = value;
		return true;
	}

	inline bool ParseIsoDateSpan(const char* data, size_t length, GB_Date& outDate)
	{
		if (data == nullptr)
		{
			return false;
		}

		int parsedYear = 0;
		int parsedMonth = 0;
		int parsedDay = 0;

		if (length == 10)
		{
			// YYYY-MM-DD
			if (data[4] != '-' || data[7] != '-')
			{
				return false;
			}
			if (!ParseFixedDigits(data, length, 0, 4, parsedYear)
				|| !ParseFixedDigits(data, length, 5, 2, parsedMonth)
				|| !ParseFixedDigits(data, length, 8, 2, parsedDay))
			{
				return false;
			}
		}
		else if (length == 8)
		{
			// YYYYMMDD
			if (!IsAllDigits(data, length))
			{
				return false;
			}
			if (!ParseFixedDigits(data, length, 0, 4, parsedYear)
				|| !ParseFixedDigits(data, length, 4, 2, parsedMonth)
				|| !ParseFixedDigits(data, length, 6, 2, parsedDay))
			{
				return false;
			}
		}
		else
		{
			return false;
		}

		GB_Date date;
		if (!date.Set(parsedYear, parsedMonth, parsedDay))
		{
			return false;
		}

		outDate = date;
		return true;
	}

	inline bool TryParseInt32Span(const char* data, size_t length, int& outValue)
	{
		if (data == nullptr || length == 0)
		{
			return false;
		}

		size_t i = 0;
		bool negative = false;
		if (data[0] == '+' || data[0] == '-')
		{
			negative = (data[0] == '-');
			i++;
			if (i >= length)
			{
				return false;
			}
		}

		const long long positiveLimit = static_cast<long long>(std::numeric_limits<int>::max());
		const long long negativeLimitAbs = -static_cast<long long>(std::numeric_limits<int>::min());
		const long long limitAbs = negative ? negativeLimitAbs : positiveLimit;

		long long value = 0;
		for (; i < length; i++)
		{
			const char ch = data[i];
			if (ch < '0' || ch > '9')
			{
				return false;
			}

			const int digit = (ch - '0');
			if (value > (limitAbs - digit) / 10LL)
			{
				return false;
			}
			value = value * 10LL + digit;
		}

		if (negative)
		{
			outValue = static_cast<int>(-value);
			return true;
		}

		outValue = static_cast<int>(value);
		return true;
	}

	inline bool ReadDelimitedField(const char* data, size_t length, size_t& cursor, char delimiter,
		size_t& fieldOffset, size_t& fieldLength, bool isLast)
	{
		if (data == nullptr)
		{
			return false;
		}
		if (cursor > length)
		{
			return false;
		}

		fieldOffset = cursor;
		if (isLast)
		{
			fieldLength = length - cursor;
			cursor = length;
			return fieldLength > 0;
		}

		size_t i = cursor;
		while (i < length&& data[i] != delimiter)
		{
			i++;
		}
		if (i >= length)
		{
			return false;
		}

		fieldLength = i - cursor;
		cursor = i + 1;
		return fieldLength > 0;
	}

	inline bool DeserializeDateFromSpan(GB_Date& date, const char* data, size_t length)
	{
		if (data == nullptr)
		{
			date.Reset();
			return false;
		}

		size_t start = 0;
		size_t end = 0;
		GetTrimmedAsciiRange(data, length, start, end);
		if (end <= start)
		{
			date.Reset();
			return false;
		}

		const char* textData = data + start;
		const size_t textLength = end - start;

		// 1) Prefer our explicit serialization format.
		//    GB_Date|version|year|month|day
		if (StartsWith(textData, textLength, "GB_Date|", 8))
		{
			constexpr size_t prefixLength = 8;
			size_t cursor = prefixLength;
			size_t versionOffset = 0;
			size_t versionLength = 0;
			size_t yearOffset = 0;
			size_t yearLength = 0;
			size_t monthOffset = 0;
			size_t monthLength = 0;
			size_t dayOffset = 0;
			size_t dayLength = 0;

			if (!ReadDelimitedField(textData, textLength, cursor, '|', versionOffset, versionLength, false)
				|| !ReadDelimitedField(textData, textLength, cursor, '|', yearOffset, yearLength, false)
				|| !ReadDelimitedField(textData, textLength, cursor, '|', monthOffset, monthLength, false)
				|| !ReadDelimitedField(textData, textLength, cursor, '|', dayOffset, dayLength, true))
			{
				date.Reset();
				return false;
			}
			if (cursor != textLength)
			{
				date.Reset();
				return false;
			}
			for (size_t i = dayOffset; i < dayOffset + dayLength; i++)
			{
				if (textData[i] == '|')
				{
					date.Reset();
					return false;
				}
			}

			int version = 0;
			int parsedYear = 0;
			int parsedMonth = 0;
			int parsedDay = 0;
			if (!TryParseInt32Span(textData + versionOffset, versionLength, version)
				|| !TryParseInt32Span(textData + yearOffset, yearLength, parsedYear)
				|| !TryParseInt32Span(textData + monthOffset, monthLength, parsedMonth)
				|| !TryParseInt32Span(textData + dayOffset, dayLength, parsedDay))
			{
				date.Reset();
				return false;
			}

			if (version != static_cast<int>(kDateBinaryVersion))
			{
				date.Reset();
				return false;
			}

			if (parsedYear == 0 && parsedMonth == 0 && parsedDay == 0)
			{
				date.Reset();
				return true;
			}

			if (!date.Set(parsedYear, parsedMonth, parsedDay))
			{
				date.Reset();
				return false;
			}

			return true;
		}

		// 2) Fallback: accept ISO strings.
		GB_Date parsed;
		if (ParseIsoDateSpan(textData, textLength, parsed))
		{
			date = parsed;
			return true;
		}

		date.Reset();
		return false;
	}

	// Floor division for possibly negative values.
	inline long long DivFloor(long long a, long long b)
	{
		// b > 0 assumed
		long long q = a / b;
		const long long r = a % b;
		// In C/C++, remainder has the sign of the dividend. For floor division (with b>0),
		// we need to decrement the truncated quotient when the remainder is negative.
		if (r < 0)
		{
			q--;
		}
		return q;
	}

	inline long long ModFloor(long long a, long long b)
	{
		// b > 0 assumed
		long long r = a % b;
		if (r != 0 && r < 0)
		{
			r += b;
		}
		return r;
	}

	inline void AppendUInt32LE(GB_ByteBuffer& buffer, uint32_t value)
	{
		buffer.push_back(static_cast<unsigned char>(value & 0xFF));
		buffer.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
		buffer.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
		buffer.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
	}

	inline void AppendInt32LE(GB_ByteBuffer& buffer, int32_t value)
	{
		AppendUInt32LE(buffer, static_cast<uint32_t>(value));
	}

	inline bool ReadUInt32LE(const GB_ByteBuffer& buffer, size_t& offset, uint32_t& value)
	{
		if (offset + 4 > buffer.size())
		{
			return false;
		}

		value = 0;
		value |= static_cast<uint32_t>(buffer[offset + 0]);
		value |= static_cast<uint32_t>(buffer[offset + 1]) << 8;
		value |= static_cast<uint32_t>(buffer[offset + 2]) << 16;
		value |= static_cast<uint32_t>(buffer[offset + 3]) << 24;
		offset += 4;
		return true;
	}

	inline bool ReadInt32LE(const GB_ByteBuffer& buffer, size_t& offset, int32_t& value)
	{
		uint32_t temp = 0;
		if (!ReadUInt32LE(buffer, offset, temp))
		{
			return false;
		}
		value = static_cast<int32_t>(temp);
		return true;
	}


	inline bool DeserializeDateFromBinary(GB_Date& date, const GB_ByteBuffer& buffer)
	{
		// Binary layout (little-endian):
		// [uint32 magic][uint32 version][int32 year][int32 month][int32 day]
		// 说明：
		// - 如果 magic 匹配，则认为这是二进制格式；此时不再回退按文本解析（避免误解析）。
		// - 如果版本不支持，直接失败。
		constexpr size_t kExpectedBinarySize = 20;

		if (buffer.empty())
		{
			date.Reset();
			return false;
		}

		// If the magic matches, treat it as binary and be strict.
		if (buffer.size() >= 4)
		{
			const uint32_t magic =
				(static_cast<uint32_t>(buffer[0]) << 0)
				| (static_cast<uint32_t>(buffer[1]) << 8)
				| (static_cast<uint32_t>(buffer[2]) << 16)
				| (static_cast<uint32_t>(buffer[3]) << 24);

			if (magic == GB_ClassMagicNumber)
			{
				if (buffer.size() < kExpectedBinarySize)
				{
					date.Reset();
					return false;
				}

				size_t offset = 0;
				uint32_t readMagic = 0;
				uint32_t version = 0;
				int32_t parsedYear = 0;
				int32_t parsedMonth = 0;
				int32_t parsedDay = 0;

				if (!ReadUInt32LE(buffer, offset, readMagic)
					|| !ReadUInt32LE(buffer, offset, version)
					|| !ReadInt32LE(buffer, offset, parsedYear)
					|| !ReadInt32LE(buffer, offset, parsedMonth)
					|| !ReadInt32LE(buffer, offset, parsedDay))
				{
					date.Reset();
					return false;
				}

				if (readMagic != GB_ClassMagicNumber)
				{
					date.Reset();
					return false;
				}

				if (version != kDateBinaryVersion)
				{
					date.Reset();
					return false;
				}

				if (parsedYear == 0 && parsedMonth == 0 && parsedDay == 0)
				{
					date.Reset();
					return true;
				}

				if (!date.Set(static_cast<int>(parsedYear), static_cast<int>(parsedMonth), static_cast<int>(parsedDay)))
				{
					date.Reset();
					return false;
				}

				return true;
			}
		}

		// Fallback: treat it as UTF-8 text (ASCII subset) and reuse textual deserializer.
		const char* dataPtr = reinterpret_cast<const char*>(buffer.data());
		return DeserializeDateFromSpan(date, dataPtr, buffer.size());
	}


	inline long long DateToJdnGregorian(int year, int month, int day)
	{
		// Fliegel & Van Flandern / standard proleptic Gregorian conversion.
		const int a = (14 - month) / 12;
		const int y = year + 4800 - a;
		const int m = month + 12 * a - 3;
		return static_cast<long long>(day)
			+ (153LL * m + 2) / 5
			+ 365LL * y
			+ y / 4
			- y / 100
			+ y / 400
			- 32045LL;
	}

	inline GB_Date JdnToDateGregorian(long long jdn)
	{
		// Inverse of the above conversion (proleptic Gregorian).
		const long long a = jdn + 32044LL;
		const long long b = (4LL * a + 3) / 146097LL;
		const long long c = a - (146097LL * b) / 4LL;
		const long long d = (4LL * c + 3) / 1461LL;
		const long long e = c - (1461LL * d) / 4LL;
		const long long m = (5LL * e + 2) / 153LL;

		const int day = static_cast<int>(e - (153LL * m + 2) / 5LL + 1);
		const int month = static_cast<int>(m + 3 - 12 * (m / 10));
		const int year = static_cast<int>(100LL * b + d - 4800LL + (m / 10));

		return GB_Date(year, month, day);
	}

	inline bool GetLocalTm(std::time_t timeValue, std::tm& outTm)
	{
		std::tm temp = {};
#if defined(_WIN32)
		if (localtime_s(&temp, &timeValue) != 0)
		{
			return false;
		}
#else
		if (localtime_r(&timeValue, &temp) == nullptr)
		{
			return false;
		}
#endif
		outTm = temp;
		return true;
	}

	inline bool GetGmTm(std::time_t timeValue, std::tm& outTm)
	{
		std::tm temp = {};
#if defined(_WIN32)
		if (gmtime_s(&temp, &timeValue) != 0)
		{
			return false;
		}
#else
		if (gmtime_r(&timeValue, &temp) == nullptr)
		{
			return false;
		}
#endif
		outTm = temp;
		return true;
	}

	constexpr uint32_t kTimeBinaryVersion = 1;
	constexpr size_t kIsoTimeStringLengthNoMs = 8;   // HH:MM:SS
	constexpr size_t kIsoTimeStringLengthWithMs = 12; // HH:MM:SS.mmm
	constexpr int kMillisecondsPerDay = 24 * 60 * 60 * 1000;
	constexpr int kNullTimeEncodedValue = -2;

	inline bool ParseFractionalMillisecondsSpan(const char* data, size_t length, int& outMillisecond)
	{
		if (data == nullptr)
		{
			return false;
		}
		if (length == 0 || length > 3)
		{
			return false;
		}

		int value = 0;
		for (size_t i = 0; i < length; i++)
		{
			const char ch = data[i];
			if (ch < '0' || ch > '9')
			{
				return false;
			}
			value = value * 10 + (ch - '0');
		}

		// ".1"  = 100ms, ".12" = 120ms, ".123" = 123ms
		if (length == 1)
		{
			value *= 100;
		}
		else if (length == 2)
		{
			value *= 10;
		}

		outMillisecond = value;
		return true;
	}

	inline bool ParseIsoTimeSpan(const char* data, size_t length, GB_Time& outTime)
	{
		if (data == nullptr)
		{
			return false;
		}

		// Support:
		// - "HH:MM"
		// - "HH:MM:SS"
		// - "HH:MM:SS.mmm" / "HH:MM:SS,mmm"
		size_t fractionalPos = length;
		for (size_t i = 0; i < length; i++)
		{
			if (data[i] == '.' || data[i] == ',')
			{
				fractionalPos = i;
				break;
			}
		}

		const size_t mainLength = (fractionalPos < length) ? fractionalPos : length;
		const bool hasFractional = (fractionalPos < length);

		int hour = 0;
		int minute = 0;
		int second = 0;
		int millisecond = 0;

		if (mainLength == 5)
		{
			// HH:MM
			// NOTE: We do NOT accept "HH:MM.xxx" because the fractional part is defined on seconds.
			if (hasFractional)
			{
				return false;
			}

			if (data[2] != ':')
			{
				return false;
			}
			if (!ParseFixedDigits(data, mainLength, 0, 2, hour)
				|| !ParseFixedDigits(data, mainLength, 3, 2, minute))
			{
				return false;
			}
		}
		else if (mainLength == 8)
		{
			// HH:MM:SS
			if (data[2] != ':' || data[5] != ':')
			{
				return false;
			}
			if (!ParseFixedDigits(data, mainLength, 0, 2, hour)
				|| !ParseFixedDigits(data, mainLength, 3, 2, minute)
				|| !ParseFixedDigits(data, mainLength, 6, 2, second))
			{
				return false;
			}
		}
		else
		{
			return false;
		}

		if (hasFractional)
		{
			// Only "HH:MM:SS.xxx" is allowed.
			if (mainLength != 8)
			{
				return false;
			}

			const size_t fractionalStart = fractionalPos + 1;
			if (fractionalStart >= length)
			{
				return false;
			}
			const size_t fractionalLength = length - fractionalStart;
			if (!ParseFractionalMillisecondsSpan(data + fractionalStart, fractionalLength, millisecond))
			{
				return false;
			}
		}

		GB_Time time;
		if (!time.Set(hour, minute, second, millisecond))
		{
			return false;
		}

		outTime = time;
		return true;
	}


	inline bool DeserializeTimeFromSpan(GB_Time& time, const char* data, size_t length)
	{
		if (data == nullptr)
		{
			time = GB_Time::Invalid;
			return false;
		}

		size_t start = 0;
		size_t end = 0;
		GetTrimmedAsciiRange(data, length, start, end);
		if (end <= start)
		{
			time = GB_Time::Invalid;
			return false;
		}

		const char* textData = data + start;
		const size_t textLength = end - start;

		// 1) Prefer our explicit serialization format.
		//    GB_Time|version|encodedMsecs
		if (StartsWith(textData, textLength, "GB_Time|", 8))
		{
			constexpr size_t prefixLength = 8;
			size_t cursor = prefixLength;
			size_t versionOffset = 0;
			size_t versionLength = 0;
			size_t valueOffset = 0;
			size_t valueLength = 0;

			if (!ReadDelimitedField(textData, textLength, cursor, '|', versionOffset, versionLength, false)
				|| !ReadDelimitedField(textData, textLength, cursor, '|', valueOffset, valueLength, true))
			{
				time = GB_Time::Invalid;
				return false;
			}
			if (cursor != textLength)
			{
				time = GB_Time::Invalid;
				return false;
			}
			for (size_t i = valueOffset; i < valueOffset + valueLength; i++)
			{
				if (textData[i] == '|')
				{
					time = GB_Time::Invalid;
					return false;
				}
			}

			int version = 0;
			int encoded = 0;
			if (!TryParseInt32Span(textData + versionOffset, versionLength, version)
				|| !TryParseInt32Span(textData + valueOffset, valueLength, encoded))
			{
				time = GB_Time::Invalid;
				return false;
			}

			if (version != static_cast<int>(kTimeBinaryVersion))
			{
				time = GB_Time::Invalid;
				return false;
			}

			if (encoded == kNullTimeEncodedValue)
			{
				time.Reset();
				return true;
			}

			if (encoded == -1)
			{
				time = GB_Time::Invalid;
				return true;
			}

			time = GB_Time::CreateFromMillisecondsSinceStartOfDay(encoded);
			return time.IsValid();
		}

		// 2) Fallback: accept ISO strings.
		GB_Time parsed;
		if (ParseIsoTimeSpan(textData, textLength, parsed))
		{
			time = parsed;
			return true;
		}

		time = GB_Time::Invalid;
		return false;
	}

	inline bool DeserializeTimeFromBinary(GB_Time& time, const GB_ByteBuffer& buffer)
	{
		// Binary layout (little-endian):
		// [uint32 magic][uint32 version][int32 encodedMsecs]
		// 说明：
		// - 如果 magic 匹配，则认为这是二进制格式；此时不再回退按文本解析（避免误解析）。
		// - 如果版本不支持，直接失败。
		constexpr size_t kExpectedBinarySize = 12;

		if (buffer.empty())
		{
			time = GB_Time::Invalid;
			return false;
		}

		// If the magic matches, treat it as binary and be strict.
		if (buffer.size() >= 4)
		{
			const uint32_t magic =
				(static_cast<uint32_t>(buffer[0]) << 0)
				| (static_cast<uint32_t>(buffer[1]) << 8)
				| (static_cast<uint32_t>(buffer[2]) << 16)
				| (static_cast<uint32_t>(buffer[3]) << 24);

			if (magic == GB_ClassMagicNumber)
			{
				if (buffer.size() < kExpectedBinarySize)
				{
					time = GB_Time::Invalid;
					return false;
				}

				size_t offset = 0;
				uint32_t readMagic = 0;
				uint32_t version = 0;
				int32_t encoded = 0;

				if (!ReadUInt32LE(buffer, offset, readMagic)
					|| !ReadUInt32LE(buffer, offset, version)
					|| !ReadInt32LE(buffer, offset, encoded))
				{
					time = GB_Time::Invalid;
					return false;
				}

				if (readMagic != GB_ClassMagicNumber)
				{
					time = GB_Time::Invalid;
					return false;
				}

				if (version != kTimeBinaryVersion)
				{
					time = GB_Time::Invalid;
					return false;
				}

				if (encoded == kNullTimeEncodedValue)
				{
					time.Reset();
					return true;
				}

				if (encoded == -1)
				{
					time = GB_Time::Invalid;
					return true;
				}

				time = GB_Time::CreateFromMillisecondsSinceStartOfDay(static_cast<int>(encoded));
				return time.IsValid();
			}
		}

		// Fallback: treat it as UTF-8 text (ASCII subset) and reuse textual deserializer.
		const char* dataPtr = reinterpret_cast<const char*>(buffer.data());
		return DeserializeTimeFromSpan(time, dataPtr, buffer.size());
	}

}

const GB_Date GB_Date::Invalid = GB_Date();
const GB_Date GB_Date::MinValue = GB_Date(1, 1, 1);
const GB_Date GB_Date::MaxValue = GB_Date(9999, 12, 31);

GB_Date::GB_Date() = default;

GB_Date::GB_Date(int year, int month, int day)
{
	Set(year, month, day);
}

bool GB_Date::IsNull() const
{
	return year == 0 && month == 0 && day == 0;
}

bool GB_Date::IsValid() const
{
	if (IsNull())
	{
		return false;
	}

	if (year < 1 || year > 9999)
	{
		return false;
	}
	if (month < 1 || month > 12)
	{
		return false;
	}

	const int daysInMonth = GetDaysInMonth(year, month);
	if (daysInMonth <= 0)
	{
		return false;
	}

	return day >= 1 && day <= daysInMonth;
}

void GB_Date::Reset()
{
	year = 0;
	month = 0;
	day = 0;
}

bool GB_Date::Set(int year, int month, int day)
{
	this->year = year;
	this->month = month;
	this->day = day;

	if (!IsValid())
	{
		Reset();
		return false;
	}

	return true;
}

bool GB_Date::IsLeapYear(int year)
{
	if (year < 1 || year > 9999)
	{
		return false;
	}

	// Gregorian rule.
	if ((year % 4) != 0)
	{
		return false;
	}
	if ((year % 100) != 0)
	{
		return true;
	}
	return (year % 400) == 0;
}

int GB_Date::GetDaysInMonth(int year, int month)
{
	if (month < 1 || month > 12)
	{
		return 0;
	}

	switch (month)
	{
	case 1:
	case 3:
	case 5:
	case 7:
	case 8:
	case 10:
	case 12:
		return 31;
	case 4:
	case 6:
	case 9:
	case 11:
		return 30;
	case 2:
		return IsLeapYear(year) ? 29 : 28;
	default:
		return 0;
	}
}

int GB_Date::DaysInMonth() const
{
	if (!IsValid())
	{
		return 0;
	}

	return GetDaysInMonth(year, month);
}

int GB_Date::DaysInYear() const
{
	if (!IsValid())
	{
		return 0;
	}

	return IsLeapYear(year) ? 366 : 365;
}

int GB_Date::DayOfYear() const
{
	if (!IsValid())
	{
		return 0;
	}

	static const int cumulativeDays[12] =
	{
		0,   // Jan
		31,  // Feb
		59,  // Mar
		90,  // Apr
		120, // May
		151, // Jun
		181, // Jul
		212, // Aug
		243, // Sep
		273, // Oct
		304, // Nov
		334  // Dec
	};

	int result = cumulativeDays[month - 1] + day;
	if (month > 2 && IsLeapYear(year))
	{
		result += 1;
	}

	return result;
}

int GB_Date::DayOfWeek() const
{
	long long jdn = 0;
	if (!ToJulianDayNumber(jdn))
	{
		return 0;
	}

	// ISO 8601: Monday=1 .. Sunday=7.
	return static_cast<int>(ModFloor(jdn, 7LL)) + 1;
}

bool GB_Date::ToDaysSinceEpoch(int& outDays) const
{
	long long jdn = 0;
	if (!ToJulianDayNumber(jdn))
	{
		return false;
	}

	const long long delta = jdn - kUnixEpochJdn;
	if (delta < static_cast<long long>(std::numeric_limits<int>::min())
		|| delta > static_cast<long long>(std::numeric_limits<int>::max()))
	{
		return false;
	}

	outDays = static_cast<int>(delta);
	return true;
}

GB_Date GB_Date::CreateFromDaysSinceEpoch(int daysSinceEpoch)
{
	const long long jdn = kUnixEpochJdn + static_cast<long long>(daysSinceEpoch);
	return CreateFromJulianDayNumber(jdn);
}

bool GB_Date::ToJulianDayNumber(long long& outJulianDayNumber) const
{
	if (!IsValid())
	{
		return false;
	}

	outJulianDayNumber = DateToJdnGregorian(year, month, day);
	return true;
}

GB_Date GB_Date::CreateFromJulianDayNumber(long long julianDayNumber)
{
	// Fast rejection by JDN range of [1-1-1, 9999-12-31].
	static const long long minJdn = DateToJdnGregorian(1, 1, 1);
	static const long long maxJdn = DateToJdnGregorian(9999, 12, 31);
	if (julianDayNumber < minJdn || julianDayNumber > maxJdn)
	{
		return GB_Date();
	}

	const GB_Date result = JdnToDateGregorian(julianDayNumber);
	if (!result.IsValid())
	{
		return GB_Date::Invalid;
	}

	return result;
}

std::string GB_Date::ToIsoString() const
{
	if (!IsValid())
	{
		return {};
	}

	std::string result(kIsoDateStringLength, '0');
	result[4] = '-';
	result[7] = '-';

	const int y = year;
	result[0] = static_cast<char>('0' + (y / 1000) % 10);
	result[1] = static_cast<char>('0' + (y / 100) % 10);
	result[2] = static_cast<char>('0' + (y / 10) % 10);
	result[3] = static_cast<char>('0' + (y / 1) % 10);

	const int m = month;
	result[5] = static_cast<char>('0' + (m / 10) % 10);
	result[6] = static_cast<char>('0' + (m / 1) % 10);

	const int d = day;
	result[8] = static_cast<char>('0' + (d / 10) % 10);
	result[9] = static_cast<char>('0' + (d / 1) % 10);

	return result;
}

GB_Date GB_Date::CreateFromIsoString(const std::string& textUtf8)
{
	GB_Date date;
	if (!ParseIsoString(textUtf8, date))
	{
		return GB_Date::Invalid;
	}
	return date;
}

bool GB_Date::ParseIsoString(const std::string& textUtf8, GB_Date& outDate)
{
	size_t start = 0;
	size_t end = 0;
	GetTrimmedAsciiRange(textUtf8, start, end);
	if (end <= start)
	{
		return false;
	}

	const char* textData = textUtf8.data() + start;
	const size_t textLength = end - start;

	GB_Date parsed;
	if (!ParseIsoDateSpan(textData, textLength, parsed))
	{
		return false;
	}

	outDate = parsed;
	return true;
}


GB_Date GB_Date::AddDays(int days) const
{
	int baseDays = 0;
	if (!ToDaysSinceEpoch(baseDays))
	{
		return GB_Date::Invalid;
	}

	const long long resultDays = static_cast<long long>(baseDays) + static_cast<long long>(days);
	if (resultDays < static_cast<long long>(std::numeric_limits<int>::min())
		|| resultDays > static_cast<long long>(std::numeric_limits<int>::max()))
	{
		return GB_Date::Invalid;
	}

	return CreateFromDaysSinceEpoch(static_cast<int>(resultDays));
}

GB_Date GB_Date::AddMonths(int months) const
{
	if (!IsValid())
	{
		return GB_Date::Invalid;
	}

	// Convert to 0-based month index from year 1.
	const long long baseMonthIndex = static_cast<long long>(year - 1) * 12LL + static_cast<long long>(month - 1);
	const long long newMonthIndex = baseMonthIndex + static_cast<long long>(months);

	// Convert back using floor division to correctly handle negative months.
	const long long newYear0 = DivFloor(newMonthIndex, 12LL);
	const long long newMonth0 = ModFloor(newMonthIndex, 12LL);

	const long long newYear = newYear0 + 1;
	const int newMonth = static_cast<int>(newMonth0 + 1);

	if (newYear < 1 || newYear > 9999)
	{
		return GB_Date::Invalid;
	}

	const int daysInNewMonth = GetDaysInMonth(static_cast<int>(newYear), newMonth);
	if (daysInNewMonth <= 0)
	{
		return GB_Date::Invalid;
	}

	const int newDay = std::min(day, daysInNewMonth);
	return GB_Date(static_cast<int>(newYear), newMonth, newDay);
}

GB_Date GB_Date::AddYears(int years) const
{
	if (!IsValid())
	{
		return GB_Date::Invalid;
	}

	const long long newYear = static_cast<long long>(year) + static_cast<long long>(years);
	if (newYear < 1 || newYear > 9999)
	{
		return GB_Date::Invalid;
	}

	const int daysInNewMonth = GetDaysInMonth(static_cast<int>(newYear), month);
	if (daysInNewMonth <= 0)
	{
		return GB_Date::Invalid;
	}

	const int newDay = std::min(day, daysInNewMonth);
	return GB_Date(static_cast<int>(newYear), month, newDay);
}

int GB_Date::DaysTo(const GB_Date& other) const
{
	int a = 0;
	int b = 0;
	if (!ToDaysSinceEpoch(a) || !other.ToDaysSinceEpoch(b))
	{
		return 0;
	}
	return b - a;
}

int GB_Date::Year() const
{
	return IsValid() ? year : 0;
}

int GB_Date::Month() const
{
	return IsValid() ? month : 0;
}

int GB_Date::Day() const
{
	return IsValid() ? day : 0;
}

GB_Date GB_Date::Today()
{
	const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	const std::chrono::system_clock::time_point nowSeconds =
		std::chrono::time_point_cast<std::chrono::seconds>(now);
	const std::time_t timeValue = std::chrono::system_clock::to_time_t(nowSeconds);

	std::tm tmValue = {};
	if (!GetLocalTm(timeValue, tmValue))
	{
		return GB_Date::Invalid;
	}

	return GB_Date(tmValue.tm_year + 1900, tmValue.tm_mon + 1, tmValue.tm_mday);
}

GB_Date GB_Date::UtcToday()
{
	const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	const std::chrono::system_clock::time_point nowSeconds =
		std::chrono::time_point_cast<std::chrono::seconds>(now);
	const std::time_t timeValue = std::chrono::system_clock::to_time_t(nowSeconds);

	std::tm tmValue = {};
	if (!GetGmTm(timeValue, tmValue))
	{
		return GB_Date::Invalid;
	}

	return GB_Date(tmValue.tm_year + 1900, tmValue.tm_mon + 1, tmValue.tm_mday);
}
GB_DateTime GB_Date::ToDateTime(const GB_Time& time, GB_DateTimeSpec spec, int offsetFromUtcMinutes) const
{
	if (!IsValid() || !time.IsValid())
	{
		return GB_DateTime::Invalid;
	}

	return GB_DateTime(*this, time, spec, offsetFromUtcMinutes);
}

GB_DateTime GB_Date::ToDateTime(GB_DateTimeSpec spec, int offsetFromUtcMinutes) const
{
	if (!IsValid())
	{
		return GB_DateTime::Invalid;
	}

	return GB_DateTime(*this, GB_Time::MinValue, spec, offsetFromUtcMinutes);
}



bool GB_Date::operator==(const GB_Date& other) const
{
	return year == other.year && month == other.month && day == other.day;
}

bool GB_Date::operator!=(const GB_Date& other) const
{
	return !(*this == other);
}

bool GB_Date::operator<(const GB_Date& other) const
{
	if (year != other.year)
	{
		return year < other.year;
	}
	if (month != other.month)
	{
		return month < other.month;
	}
	return day < other.day;
}

bool GB_Date::operator<=(const GB_Date& other) const
{
	return !(*this > other);
}

bool GB_Date::operator>(const GB_Date& other) const
{
	return other < *this;
}

bool GB_Date::operator>=(const GB_Date& other) const
{
	return !(*this < other);
}

size_t GB_Date::GB_DateHash::operator()(const GB_Date& date) const noexcept
{
	if (!date.IsValid())
	{
		return std::hash<uint64_t>()(0xCBF29CE484222325ULL);
	}
	const long long packed = static_cast<long long>(date.year) * 10000LL
		+ static_cast<long long>(date.month) * 100LL
		+ static_cast<long long>(date.day);
	return std::hash<long long>()(packed);
}

std::string GB_Date::SerializeToString() const
{
	// Format: GB_Date|<version>|<year>|<month>|<day>
	// - version starts at 1
	// - null date is represented as 0|0|0
	const GB_Date normalized = IsValid() ? *this : GB_Date::Invalid;
	std::string result;
	result.reserve(64);
	result.append("GB_Date|");
	result.append(std::to_string(static_cast<int>(kDateBinaryVersion)));
	result.push_back('|');
	result.append(std::to_string(normalized.year));
	result.push_back('|');
	result.append(std::to_string(normalized.month));
	result.push_back('|');
	result.append(std::to_string(normalized.day));
	return result;
}

GB_ByteBuffer GB_Date::SerializeToBinary() const
{
	// Layout (little-endian):
	// [uint32 magic][uint32 version][int32 year][int32 month][int32 day]
	const GB_Date normalized = IsValid() ? *this : GB_Date::Invalid;
	GB_ByteBuffer buffer;
	buffer.reserve(20);

	AppendUInt32LE(buffer, GB_ClassMagicNumber);
	AppendUInt32LE(buffer, kDateBinaryVersion);
	AppendInt32LE(buffer, static_cast<int32_t>(normalized.year));
	AppendInt32LE(buffer, static_cast<int32_t>(normalized.month));
	AppendInt32LE(buffer, static_cast<int32_t>(normalized.day));

	return buffer;
}

bool GB_Date::Deserialize(const std::string& data)
{
	return DeserializeDateFromSpan(*this, data.data(), data.size());
}

bool GB_Date::Deserialize(const GB_ByteBuffer& data)
{
	return DeserializeDateFromBinary(*this, data);
}

const GB_Time GB_Time::Null = GB_Time();
const GB_Time GB_Time::Invalid = GB_Time(-1, 0, 0, 0);
const GB_Time GB_Time::MinValue = GB_Time(0, 0, 0, 0);
const GB_Time GB_Time::MaxValue = GB_Time(23, 59, 59, 999);

GB_Time::GB_Time() = default;

GB_Time::GB_Time(int hour, int minute, int second, int millisecond)
{
	Set(hour, minute, second, millisecond);
}

bool GB_Time::IsNull() const
{
	return isNullTime;
}

bool GB_Time::IsValid() const
{
	if (isNullTime)
	{
		return false;
	}

	return millisecondsSinceStartOfDay >= 0 && millisecondsSinceStartOfDay < kMillisecondsPerDay;
}

void GB_Time::Reset()
{
	isNullTime = true;
	millisecondsSinceStartOfDay = 0;
}

bool GB_Time::IsValidTime(int hour, int minute, int second, int millisecond)
{
	if (hour < 0 || hour > 23)
	{
		return false;
	}
	if (minute < 0 || minute > 59)
	{
		return false;
	}
	if (second < 0 || second > 59)
	{
		return false;
	}
	if (millisecond < 0 || millisecond > 999)
	{
		return false;
	}
	return true;
}

bool GB_Time::Set(int hour, int minute, int second, int millisecond)
{
	isNullTime = false;

	if (!IsValidTime(hour, minute, second, millisecond))
	{
		millisecondsSinceStartOfDay = -1;
		return false;
	}

	const int totalMilliseconds = ((hour * 60 + minute) * 60 + second) * 1000 + millisecond;
	millisecondsSinceStartOfDay = totalMilliseconds;
	return true;
}

int GB_Time::Hour() const
{
	if (!IsValid())
	{
		return 0;
	}
	return millisecondsSinceStartOfDay / (60 * 60 * 1000);
}

int GB_Time::Minute() const
{
	if (!IsValid())
	{
		return 0;
	}
	return (millisecondsSinceStartOfDay / (60 * 1000)) % 60;
}

int GB_Time::Second() const
{
	if (!IsValid())
	{
		return 0;
	}
	return (millisecondsSinceStartOfDay / 1000) % 60;
}

int GB_Time::Millisecond() const
{
	if (!IsValid())
	{
		return 0;
	}
	return millisecondsSinceStartOfDay % 1000;
}

int GB_Time::ToMillisecondsSinceStartOfDay() const
{
	if (!IsValid())
	{
		return -1;
	}
	return millisecondsSinceStartOfDay;
}

GB_Time GB_Time::CreateFromMillisecondsSinceStartOfDay(int millisecondsSinceStartOfDay)
{
	if (millisecondsSinceStartOfDay < 0 || millisecondsSinceStartOfDay >= kMillisecondsPerDay)
	{
		return GB_Time::Invalid;
	}

	GB_Time result;
	result.isNullTime = false;
	result.millisecondsSinceStartOfDay = millisecondsSinceStartOfDay;
	return result;
}

std::string GB_Time::ToIsoString(bool includeMilliseconds) const
{
	if (!IsValid())
	{
		return {};
	}

	const int totalMilliseconds = millisecondsSinceStartOfDay;
	const int hour = totalMilliseconds / (60 * 60 * 1000);
	const int minute = (totalMilliseconds / (60 * 1000)) % 60;
	const int second = (totalMilliseconds / 1000) % 60;
	const int millisecond = totalMilliseconds % 1000;

	if (includeMilliseconds)
	{
		std::string result(kIsoTimeStringLengthWithMs, '0');
		result[2] = ':';
		result[5] = ':';
		result[8] = '.';

		result[0] = static_cast<char>('0' + (hour / 10) % 10);
		result[1] = static_cast<char>('0' + (hour / 1) % 10);

		result[3] = static_cast<char>('0' + (minute / 10) % 10);
		result[4] = static_cast<char>('0' + (minute / 1) % 10);

		result[6] = static_cast<char>('0' + (second / 10) % 10);
		result[7] = static_cast<char>('0' + (second / 1) % 10);

		result[9] = static_cast<char>('0' + (millisecond / 100) % 10);
		result[10] = static_cast<char>('0' + (millisecond / 10) % 10);
		result[11] = static_cast<char>('0' + (millisecond / 1) % 10);

		return result;
	}

	std::string result(kIsoTimeStringLengthNoMs, '0');
	result[2] = ':';
	result[5] = ':';

	result[0] = static_cast<char>('0' + (hour / 10) % 10);
	result[1] = static_cast<char>('0' + (hour / 1) % 10);

	result[3] = static_cast<char>('0' + (minute / 10) % 10);
	result[4] = static_cast<char>('0' + (minute / 1) % 10);

	result[6] = static_cast<char>('0' + (second / 10) % 10);
	result[7] = static_cast<char>('0' + (second / 1) % 10);

	return result;
}

GB_Time GB_Time::CreateFromIsoString(const std::string& textUtf8)
{
	GB_Time time;
	if (!ParseIsoString(textUtf8, time))
	{
		return GB_Time::Invalid;
	}
	return time;
}

bool GB_Time::ParseIsoString(const std::string& textUtf8, GB_Time& outTime)
{
	size_t start = 0;
	size_t end = 0;
	GetTrimmedAsciiRange(textUtf8, start, end);
	if (end <= start)
	{
		return false;
	}

	const char* textData = textUtf8.data() + start;
	const size_t textLength = end - start;

	GB_Time parsed;
	if (!ParseIsoTimeSpan(textData, textLength, parsed))
	{
		return false;
	}

	outTime = parsed;
	return true;
}


GB_Time GB_Time::AddMSecs(int milliseconds) const
{
	if (!IsValid())
	{
		return GB_Time::Invalid;
	}

	const long long sum = static_cast<long long>(millisecondsSinceStartOfDay) + static_cast<long long>(milliseconds);
	const long long normalized = ModFloor(sum, static_cast<long long>(kMillisecondsPerDay));
	return GB_Time::CreateFromMillisecondsSinceStartOfDay(static_cast<int>(normalized));
}

GB_Time GB_Time::AddSecs(int seconds) const
{
	if (!IsValid())
	{
		return GB_Time::Invalid;
	}

	const long long delta = static_cast<long long>(seconds) * 1000LL;
	const long long sum = static_cast<long long>(millisecondsSinceStartOfDay) + delta;
	const long long normalized = ModFloor(sum, static_cast<long long>(kMillisecondsPerDay));
	return GB_Time::CreateFromMillisecondsSinceStartOfDay(static_cast<int>(normalized));
}

int GB_Time::MsecsTo(const GB_Time& other) const
{
	if (!IsValid() || !other.IsValid())
	{
		return 0;
	}

	return other.millisecondsSinceStartOfDay - millisecondsSinceStartOfDay;
}

int GB_Time::SecsTo(const GB_Time& other) const
{
	if (!IsValid() || !other.IsValid())
	{
		return 0;
	}

	// 与 QTime::secsTo 类似：忽略毫秒（截断）。
	const int ourSeconds = millisecondsSinceStartOfDay / 1000;
	const int theirSeconds = other.millisecondsSinceStartOfDay / 1000;
	return theirSeconds - ourSeconds;
}

GB_Time GB_Time::CurrentTime()
{
	const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	const std::chrono::system_clock::time_point nowSeconds =
		std::chrono::time_point_cast<std::chrono::seconds>(now);
	const std::time_t timeValue = std::chrono::system_clock::to_time_t(nowSeconds);

	std::tm tmValue = {};
	if (!GetLocalTm(timeValue, tmValue))
	{
		return GB_Time::Invalid;
	}

	const auto msDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - nowSeconds);
	int millisecond = static_cast<int>(msDuration.count());
	if (millisecond < 0)
	{
		millisecond = 0;
	}
	else if (millisecond > 999)
	{
		millisecond = 999;
	}

	int second = tmValue.tm_sec;
	if (second < 0)
	{
		second = 0;
	}
	else if (second > 59)
	{
		second = 59;
	}

	GB_Time result;
	if (!result.Set(tmValue.tm_hour, tmValue.tm_min, second, millisecond))
	{
		return GB_Time::Invalid;
	}

	return result;
}

GB_Time GB_Time::UtcCurrentTime()
{
	const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	const std::chrono::system_clock::time_point nowSeconds =
		std::chrono::time_point_cast<std::chrono::seconds>(now);
	const std::time_t timeValue = std::chrono::system_clock::to_time_t(nowSeconds);

	std::tm tmValue = {};
	if (!GetGmTm(timeValue, tmValue))
	{
		return GB_Time::Invalid;
	}

	const auto msDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - nowSeconds);
	int millisecond = static_cast<int>(msDuration.count());
	if (millisecond < 0)
	{
		millisecond = 0;
	}
	else if (millisecond > 999)
	{
		millisecond = 999;
	}

	int second = tmValue.tm_sec;
	if (second < 0)
	{
		second = 0;
	}
	else if (second > 59)
	{
		second = 59;
	}

	GB_Time result;
	if (!result.Set(tmValue.tm_hour, tmValue.tm_min, second, millisecond))
	{
		return GB_Time::Invalid;
	}

	return result;
}
GB_DateTime GB_Time::ToDateTime(const GB_Date& date, GB_DateTimeSpec spec, int offsetFromUtcMinutes) const
{
	if (!date.IsValid() || !IsValid())
	{
		return GB_DateTime::Invalid;
	}

	return GB_DateTime(date, *this, spec, offsetFromUtcMinutes);
}

GB_DateTime GB_Time::ToDateTime(GB_DateTimeSpec spec, int offsetFromUtcMinutes) const
{
	if (!IsValid())
	{
		return GB_DateTime::Invalid;
	}

	GB_Date date;

	if (spec == GB_DateTimeSpec::UtcTime)
	{
		date = GB_Date::UtcToday();
	}
	else if (spec == GB_DateTimeSpec::LocalTime)
	{
		date = GB_Date::Today();
	}
	else
	{
		const GB_DateTime nowUtc = GB_DateTime::UtcNow();
		if (!nowUtc.IsValid())
		{
			return GB_DateTime::Invalid;
		}

		const GB_DateTime nowOffset = nowUtc.ToOffsetFromUtc(offsetFromUtcMinutes);
		date = nowOffset.Date();
	}

	return ToDateTime(date, spec, offsetFromUtcMinutes);
}



bool GB_Time::operator==(const GB_Time& other) const
{
	if (isNullTime != other.isNullTime)
	{
		return false;
	}

	if (IsValid() != other.IsValid())
	{
		return false;
	}

	if (!IsValid())
	{
		// 都是空时间或无效时间
		return true;
	}

	return millisecondsSinceStartOfDay == other.millisecondsSinceStartOfDay;
}

bool GB_Time::operator!=(const GB_Time& other) const
{
	return !(*this == other);
}

bool GB_Time::operator<(const GB_Time& other) const
{
	const int thisState = isNullTime ? 0 : (IsValid() ? 2 : 1);
	const int otherState = other.isNullTime ? 0 : (other.IsValid() ? 2 : 1);

	if (thisState != otherState)
	{
		return thisState < otherState;
	}

	if (thisState != 2)
	{
		return false;
	}

	return millisecondsSinceStartOfDay < other.millisecondsSinceStartOfDay;
}

bool GB_Time::operator<=(const GB_Time& other) const
{
	return !(*this > other);
}

bool GB_Time::operator>(const GB_Time& other) const
{
	return other < *this;
}

bool GB_Time::operator>=(const GB_Time& other) const
{
	return !(*this < other);
}

size_t GB_Time::GB_TimeHash::operator()(const GB_Time& time) const noexcept
{
	const int encoded = time.isNullTime ? kNullTimeEncodedValue : time.millisecondsSinceStartOfDay;
	return std::hash<int>()(encoded);
}

std::string GB_Time::SerializeToString() const
{
	// Format: GB_Time|<version>|<encodedMsecs>
	// - version starts at 1
	// - 空时间编码为 -2
	// - 无效时间编码为 -1
	const int encoded = isNullTime ? kNullTimeEncodedValue : millisecondsSinceStartOfDay;

	std::string result;
	result.reserve(64);
	result.append("GB_Time|");
	result.append(std::to_string(static_cast<int>(kTimeBinaryVersion)));
	result.push_back('|');
	result.append(std::to_string(encoded));
	return result;
}

GB_ByteBuffer GB_Time::SerializeToBinary() const
{
	// Layout (little-endian):
	// [uint32 magic][uint32 version][int32 encodedMsecs]
	const int encoded = isNullTime ? kNullTimeEncodedValue : millisecondsSinceStartOfDay;

	GB_ByteBuffer buffer;
	buffer.reserve(12);

	AppendUInt32LE(buffer, GB_ClassMagicNumber);
	AppendUInt32LE(buffer, kTimeBinaryVersion);
	AppendInt32LE(buffer, static_cast<int32_t>(encoded));

	return buffer;
}

bool GB_Time::Deserialize(const std::string& data)
{
	return DeserializeTimeFromSpan(*this, data.data(), data.size());
}

bool GB_Time::Deserialize(const GB_ByteBuffer& data)
{
	return DeserializeTimeFromBinary(*this, data);
}
// ========================= GB_DateTime =========================

namespace
{
	constexpr uint32_t kDateTimeBinaryVersion = 1;
	constexpr size_t kExpectedDateTimeBinarySize = 28;
	constexpr int kMaxOffsetMinutesAbs = 23 * 60 + 59; // ISO 8601: up to ±23:59
	constexpr int64_t kMillisecondsPerSecond = 1000LL;
	constexpr int64_t kMillisecondsPerMinute = 60LL * kMillisecondsPerSecond;
	constexpr int64_t kMillisecondsPerDay64 = 24LL * 60LL * 60LL * kMillisecondsPerSecond;

	inline bool IsValidOffsetMinutes(int offsetMinutes)
	{
		return offsetMinutes >= -kMaxOffsetMinutesAbs && offsetMinutes <= kMaxOffsetMinutesAbs;
	}

	inline int64_t GetMinUnixMillisecondsUtc()
	{
		static const long long minJdn = DateToJdnGregorian(1, 1, 1);
		static const int64_t minUnixMs = static_cast<int64_t>(minJdn - kUnixEpochJdn) * kMillisecondsPerDay64;
		return minUnixMs;
	}

	inline int64_t GetMaxUnixMillisecondsUtc()
	{
		static const long long maxJdn = DateToJdnGregorian(9999, 12, 31);
		static const int64_t maxUnixMs = static_cast<int64_t>(maxJdn - kUnixEpochJdn) * kMillisecondsPerDay64 + (kMillisecondsPerDay64 - 1);
		return maxUnixMs;
	}

	inline bool IsUnixMillisecondsInSupportedUtcRange(int64_t unixMilliseconds)
	{
		return unixMilliseconds >= GetMinUnixMillisecondsUtc() && unixMilliseconds <= GetMaxUnixMillisecondsUtc();
	}

	inline bool TryAddInt64(int64_t a, int64_t b, int64_t& outValue)
	{
		if (b > 0 && a > (std::numeric_limits<int64_t>::max() - b))
		{
			return false;
		}
		if (b < 0 && a < (std::numeric_limits<int64_t>::min() - b))
		{
			return false;
		}

		outValue = a + b;
		return true;
	}

	inline int64_t SaturatingSubInt64(int64_t a, int64_t b)
	{
		if (b > 0 && a < (std::numeric_limits<int64_t>::min() + b))
		{
			return std::numeric_limits<int64_t>::min();
		}
		if (b < 0 && a >(std::numeric_limits<int64_t>::max() + b))
		{
			return std::numeric_limits<int64_t>::max();
		}
		return a - b;
	}

	inline bool TryParseInt64Span(const char* data, size_t length, int64_t& outValue)
	{
		if (data == nullptr)
		{
			return false;
		}
		if (length == 0)
		{
			return false;
		}

		size_t i = 0;
		bool negative = false;
		if (data[i] == '+' || data[i] == '-')
		{
			negative = (data[i] == '-');
			i++;
			if (i >= length)
			{
				return false;
			}
		}

		const uint64_t maxAbs = negative
			? (static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL)
			: static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

		uint64_t value = 0;
		for (; i < length; i++)
		{
			const char ch = data[i];
			if (ch < '0' || ch > '9')
			{
				return false;
			}
			const uint64_t digit = static_cast<uint64_t>(ch - '0');

			if (value > (maxAbs - digit) / 10ULL)
			{
				return false;
			}
			value = value * 10ULL + digit;
		}

		if (!negative)
		{
			outValue = static_cast<int64_t>(value);
			return true;
		}

		if (value == maxAbs)
		{
			outValue = std::numeric_limits<int64_t>::min();
			return true;
		}

		outValue = -static_cast<int64_t>(value);
		return true;
	}

	inline void AppendUInt64LE(GB_ByteBuffer& buffer, uint64_t value)
	{
		buffer.push_back(static_cast<unsigned char>((value >> 0) & 0xFF));
		buffer.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
		buffer.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
		buffer.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
		buffer.push_back(static_cast<unsigned char>((value >> 32) & 0xFF));
		buffer.push_back(static_cast<unsigned char>((value >> 40) & 0xFF));
		buffer.push_back(static_cast<unsigned char>((value >> 48) & 0xFF));
		buffer.push_back(static_cast<unsigned char>((value >> 56) & 0xFF));
	}

	inline void AppendInt64LE(GB_ByteBuffer& buffer, int64_t value)
	{
		AppendUInt64LE(buffer, static_cast<uint64_t>(value));
	}

	inline bool ReadUInt64LE(const GB_ByteBuffer& buffer, size_t& offset, uint64_t& value)
	{
		if (offset + 8 > buffer.size())
		{
			return false;
		}

		value = 0;
		value |= static_cast<uint64_t>(buffer[offset + 0]) << 0;
		value |= static_cast<uint64_t>(buffer[offset + 1]) << 8;
		value |= static_cast<uint64_t>(buffer[offset + 2]) << 16;
		value |= static_cast<uint64_t>(buffer[offset + 3]) << 24;
		value |= static_cast<uint64_t>(buffer[offset + 4]) << 32;
		value |= static_cast<uint64_t>(buffer[offset + 5]) << 40;
		value |= static_cast<uint64_t>(buffer[offset + 6]) << 48;
		value |= static_cast<uint64_t>(buffer[offset + 7]) << 56;
		offset += 8;
		return true;
	}

	inline bool ReadInt64LE(const GB_ByteBuffer& buffer, size_t& offset, int64_t& value)
	{
		uint64_t temp = 0;
		if (!ReadUInt64LE(buffer, offset, temp))
		{
			return false;
		}

		value = static_cast<int64_t>(temp);
		return true;
	}


	inline bool ParseIsoUtcOffsetSpan(const char* data, size_t length, int& outOffsetMinutes)
	{
		// Support:
		// ±HH:MM / ±HHMM / ±HH
		if (data == nullptr || length < 2)
		{
			return false;
		}

		const char signChar = data[0];
		if (signChar != '+' && signChar != '-')
		{
			return false;
		}

		int sign = (signChar == '-') ? -1 : 1;

		int hour = 0;
		int minute = 0;

		if (length == 3)
		{
			// ±HH
			if (!ParseFixedDigits(data, length, 1, 2, hour))
			{
				return false;
			}
		}
		else if (length == 5)
		{
			// ±HHMM
			if (!ParseFixedDigits(data, length, 1, 2, hour)
				|| !ParseFixedDigits(data, length, 3, 2, minute))
			{
				return false;
			}
		}
		else if (length == 6)
		{
			// ±HH:MM
			if (data[3] != ':')
			{
				return false;
			}

			if (!ParseFixedDigits(data, length, 1, 2, hour)
				|| !ParseFixedDigits(data, length, 4, 2, minute))
			{
				return false;
			}
		}
		else
		{
			return false;
		}

		if (hour < 0 || hour > 23)
		{
			return false;
		}
		if (minute < 0 || minute > 59)
		{
			return false;
		}

		const int offsetMinutes = sign * (hour * 60 + minute);
		if (!IsValidOffsetMinutes(offsetMinutes))
		{
			return false;
		}

		outOffsetMinutes = offsetMinutes;
		return true;
	}

	inline bool ParseIsoDateTimeSpan(
		const char* data,
		size_t length,
		GB_DateTime& outDateTime,
		GB_DateTimeSpec defaultSpec)
	{
		if (data == nullptr)
		{
			return false;
		}

		size_t start = 0;
		size_t end = 0;
		GetTrimmedAsciiRange(data, length, start, end);
		if (end <= start)
		{
			return false;
		}

		const char* textData = data + start;
		const size_t textLength = end - start;

		// date and time separator: 'T'/'t' or space
		size_t sepPos = std::string::npos;
		for (size_t i = 0; i < textLength; i++)
		{
			const char ch = textData[i];
			if (ch == 'T' || ch == 't' || ch == ' ')
			{
				sepPos = i;
				break;
			}
		}

		if (sepPos == std::string::npos || sepPos == 0 || sepPos + 1 >= textLength)
		{
			return false;
		}

		const char* datePart = textData;
		const size_t dateLength = sepPos;

		const char* timePartAll = textData + sepPos + 1;
		const size_t timeAllLength = textLength - (sepPos + 1);

		GB_Date date;
		if (!ParseIsoDateSpan(datePart, dateLength, date))
		{
			return false;
		}

		// Find timezone designator start.
		size_t tzPos = timeAllLength;
		for (size_t i = 0; i < timeAllLength; i++)
		{
			const char ch = timePartAll[i];
			if (ch == 'Z' || ch == 'z' || ch == '+' || ch == '-')
			{
				tzPos = i;
				break;
			}
		}

		const char* timePart = timePartAll;
		const size_t timeLength = tzPos;
		GB_Time time;
		if (!ParseIsoTimeSpan(timePart, timeLength, time))
		{
			return false;
		}

		GB_DateTimeSpec spec = defaultSpec;
		int offsetMinutes = 0;

		if (tzPos < timeAllLength)
		{
			const char tzStartChar = timePartAll[tzPos];
			const char* tzPart = timePartAll + tzPos;
			const size_t tzLength = timeAllLength - tzPos;

			if (tzStartChar == 'Z' || tzStartChar == 'z')
			{
				if (tzLength != 1)
				{
					return false;
				}
				spec = GB_DateTimeSpec::UtcTime;
				offsetMinutes = 0;
			}
			else
			{
				int parsedOffset = 0;
				if (!ParseIsoUtcOffsetSpan(tzPart, tzLength, parsedOffset))
				{
					return false;
				}
				spec = GB_DateTimeSpec::OffsetFromUtc;
				offsetMinutes = parsedOffset;
			}
		}
		else
		{
			// No tz suffix -> use defaultSpec.
			if (defaultSpec == GB_DateTimeSpec::UtcTime)
			{
				spec = GB_DateTimeSpec::UtcTime;
				offsetMinutes = 0;
			}
			else if (defaultSpec == GB_DateTimeSpec::OffsetFromUtc)
			{
				spec = GB_DateTimeSpec::OffsetFromUtc;
				offsetMinutes = 0;
			}
			else
			{
				spec = GB_DateTimeSpec::LocalTime;
				offsetMinutes = 0;
			}
		}

		GB_DateTime result(date, time, spec, offsetMinutes);
		if (!result.IsValid())
		{
			return false;
		}

		outDateTime = result;
		return true;
	}

	inline bool DeserializeDateTimeFromSpan(GB_DateTime& dateTime, const char* data, size_t length)
	{
		if (data == nullptr)
		{
			dateTime.Reset();
			return false;
		}

		size_t start = 0;
		size_t end = 0;
		GetTrimmedAsciiRange(data, length, start, end);
		if (end <= start)
		{
			dateTime.Reset();
			return false;
		}

		const char* textData = data + start;
		const size_t textLength = end - start;

		// 1) Prefer our explicit serialization format:
		//    GB_DateTime|version|valid|unixMilliseconds|spec|offsetMinutes
		if (StartsWith(textData, textLength, "GB_DateTime|", 12))
		{
			constexpr size_t prefixLength = 12;
			size_t cursor = prefixLength;

			size_t versionOffset = 0;
			size_t versionLength = 0;
			size_t validOffset = 0;
			size_t validLength = 0;
			size_t unixOffset = 0;
			size_t unixLength = 0;
			size_t specOffset = 0;
			size_t specLength = 0;
			size_t offOffset = 0;
			size_t offLength = 0;

			if (!ReadDelimitedField(textData, textLength, cursor, '|', versionOffset, versionLength, false)
				|| !ReadDelimitedField(textData, textLength, cursor, '|', validOffset, validLength, false)
				|| !ReadDelimitedField(textData, textLength, cursor, '|', unixOffset, unixLength, false)
				|| !ReadDelimitedField(textData, textLength, cursor, '|', specOffset, specLength, false)
				|| !ReadDelimitedField(textData, textLength, cursor, '|', offOffset, offLength, true))
			{
				dateTime.Reset();
				return false;
			}

			if (cursor != textLength)
			{
				dateTime.Reset();
				return false;
			}

			int version = 0;
			int validFlag = 0;
			int64_t unixMs = 0;
			int specValue = 0;
			int offsetMinutes = 0;

			if (!TryParseInt32Span(textData + versionOffset, versionLength, version)
				|| !TryParseInt32Span(textData + validOffset, validLength, validFlag)
				|| !TryParseInt64Span(textData + unixOffset, unixLength, unixMs)
				|| !TryParseInt32Span(textData + specOffset, specLength, specValue)
				|| !TryParseInt32Span(textData + offOffset, offLength, offsetMinutes))
			{
				dateTime.Reset();
				return false;
			}

			if (version != static_cast<int>(kDateTimeBinaryVersion))
			{
				dateTime.Reset();
				return false;
			}

			if (validFlag == 0)
			{
				dateTime.Reset();
				return true;
			}

			GB_DateTimeSpec spec = GB_DateTimeSpec::LocalTime;
			if (specValue == static_cast<int>(GB_DateTimeSpec::LocalTime))
			{
				spec = GB_DateTimeSpec::LocalTime;
			}
			else if (specValue == static_cast<int>(GB_DateTimeSpec::UtcTime))
			{
				spec = GB_DateTimeSpec::UtcTime;
			}
			else if (specValue == static_cast<int>(GB_DateTimeSpec::OffsetFromUtc))
			{
				spec = GB_DateTimeSpec::OffsetFromUtc;
			}
			else
			{
				dateTime.Reset();
				return false;
			}

			GB_DateTime parsed = GB_DateTime::CreateFromUnixMilliseconds(unixMs, spec, offsetMinutes);
			if (!parsed.IsValid())
			{
				dateTime.Reset();
				return false;
			}

			dateTime = parsed;
			return true;
		}

		// 2) Fallback: accept ISO strings.
		GB_DateTime parsed;
		if (ParseIsoDateTimeSpan(textData, textLength, parsed, GB_DateTimeSpec::LocalTime))
		{
			dateTime = parsed;
			return true;
		}

		dateTime.Reset();
		return false;
	}

	inline bool DeserializeDateTimeFromBinary(GB_DateTime& dateTime, const GB_ByteBuffer& buffer)
	{
		// Binary layout (little-endian):
		// [uint32 magic][uint32 version][int32 validFlag][int64 unixMilliseconds][int32 spec][int32 offsetMinutes]
		if (buffer.empty())
		{
			dateTime.Reset();
			return false;
		}

		if (buffer.size() >= 4)
		{
			const uint32_t magic =
				(static_cast<uint32_t>(buffer[0]) << 0)
				| (static_cast<uint32_t>(buffer[1]) << 8)
				| (static_cast<uint32_t>(buffer[2]) << 16)
				| (static_cast<uint32_t>(buffer[3]) << 24);

			if (magic == GB_ClassMagicNumber)
			{
				if (buffer.size() < kExpectedDateTimeBinarySize)
				{
					dateTime.Reset();
					return false;
				}

				size_t offset = 0;
				uint32_t readMagic = 0;
				uint32_t version = 0;
				int32_t validFlag = 0;
				int64_t unixMs = 0;
				int32_t specValue = 0;
				int32_t offsetMinutes = 0;

				if (!ReadUInt32LE(buffer, offset, readMagic)
					|| !ReadUInt32LE(buffer, offset, version)
					|| !ReadInt32LE(buffer, offset, validFlag)
					|| !ReadInt64LE(buffer, offset, unixMs)
					|| !ReadInt32LE(buffer, offset, specValue)
					|| !ReadInt32LE(buffer, offset, offsetMinutes))
				{
					dateTime.Reset();
					return false;
				}

				if (readMagic != GB_ClassMagicNumber)
				{
					dateTime.Reset();
					return false;
				}

				if (version != kDateTimeBinaryVersion)
				{
					dateTime.Reset();
					return false;
				}

				if (validFlag == 0)
				{
					dateTime.Reset();
					return true;
				}

				GB_DateTimeSpec spec = GB_DateTimeSpec::LocalTime;
				if (specValue == static_cast<int32_t>(GB_DateTimeSpec::LocalTime))
				{
					spec = GB_DateTimeSpec::LocalTime;
				}
				else if (specValue == static_cast<int32_t>(GB_DateTimeSpec::UtcTime))
				{
					spec = GB_DateTimeSpec::UtcTime;
				}
				else if (specValue == static_cast<int32_t>(GB_DateTimeSpec::OffsetFromUtc))
				{
					spec = GB_DateTimeSpec::OffsetFromUtc;
				}
				else
				{
					dateTime.Reset();
					return false;
				}

				GB_DateTime parsed = GB_DateTime::CreateFromUnixMilliseconds(unixMs, spec, static_cast<int>(offsetMinutes));
				if (!parsed.IsValid())
				{
					dateTime.Reset();
					return false;
				}

				dateTime = parsed;
				return true;
			}
		}

		// Fallback: treat it as UTF-8 text (ASCII subset) and reuse textual deserializer.
		const char* dataPtr = reinterpret_cast<const char*>(buffer.data());
		return DeserializeDateTimeFromSpan(dateTime, dataPtr, buffer.size());
	}

	inline bool TryGetUtcComponentsFromUnixMilliseconds(
		int64_t unixMilliseconds,
		int offsetMinutes,
		GB_Date& outDate,
		GB_Time& outTime)
	{
		const int64_t viewMs = unixMilliseconds + static_cast<int64_t>(offsetMinutes) * kMillisecondsPerMinute;

		if (!IsUnixMillisecondsInSupportedUtcRange(viewMs))
		{
			return false;
		}

		const int64_t daysSinceEpoch = DivFloor(viewMs, kMillisecondsPerDay64);
		const int64_t msInDay = ModFloor(viewMs, kMillisecondsPerDay64);

		if (daysSinceEpoch < static_cast<int64_t>(std::numeric_limits<int>::min())
			|| daysSinceEpoch > static_cast<int64_t>(std::numeric_limits<int>::max()))
		{
			return false;
		}

		const GB_Date date = GB_Date::CreateFromDaysSinceEpoch(static_cast<int>(daysSinceEpoch));
		const GB_Time time = GB_Time::CreateFromMillisecondsSinceStartOfDay(static_cast<int>(msInDay));

		if (!date.IsValid() || !time.IsValid())
		{
			return false;
		}

		outDate = date;
		outTime = time;
		return true;
	}

	inline bool TryGetLocalComponentsFromUnixMilliseconds(
		int64_t unixMilliseconds,
		GB_Date& outDate,
		GB_Time& outTime)
	{
		const int64_t unixSeconds = DivFloor(unixMilliseconds, kMillisecondsPerSecond);
		const int64_t ms = ModFloor(unixMilliseconds, kMillisecondsPerSecond);

		if (unixSeconds < static_cast<int64_t>(std::numeric_limits<std::time_t>::min())
			|| unixSeconds > static_cast<int64_t>(std::numeric_limits<std::time_t>::max()))
		{
			return false;
		}

		const std::time_t timeValue = static_cast<std::time_t>(unixSeconds);
		std::tm localTm = {};
		if (!GetLocalTm(timeValue, localTm))
		{
			return false;
		}

		int second = localTm.tm_sec;
		if (second < 0)
		{
			second = 0;
		}
		else if (second > 59)
		{
			second = 59;
		}

		const GB_Date date(localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday);
		const GB_Time time(localTm.tm_hour, localTm.tm_min, second, static_cast<int>(ms));

		if (!date.IsValid() || !time.IsValid())
		{
			return false;
		}

		outDate = date;
		outTime = time;
		return true;
	}

	inline bool TryGetLocalOffsetMinutesFromUnixMilliseconds(int64_t unixMilliseconds, int& outOffsetMinutes)
	{
		const int64_t unixSeconds = DivFloor(unixMilliseconds, kMillisecondsPerSecond);
		if (unixSeconds < static_cast<int64_t>(std::numeric_limits<std::time_t>::min())
			|| unixSeconds > static_cast<int64_t>(std::numeric_limits<std::time_t>::max()))
		{
			return false;
		}

		const std::time_t timeValue = static_cast<std::time_t>(unixSeconds);

		std::tm localTm = {};
		if (!GetLocalTm(timeValue, localTm))
		{
			return false;
		}

		// 计算该时间点的本地 UTC 偏移。
		// 思路：localtime(timeValue) 得到“本地民用时间”结构体 localTm。
		// 然后把 localTm 的年月日时分秒“当作 UTC”来换算回 epoch 秒（timegm/_mkgmtime）。
		// 两者之差即可得到该时刻的本地 UTC 偏移：offsetSeconds = utcSecondsFromLocalCivil - timeValue。
		std::tm workUtcTm = localTm;
		workUtcTm.tm_isdst = 0; // UTC 视角下无 DST 概念

		std::time_t utcSecondsFromLocalCivil = static_cast<std::time_t>(-1);

#if defined(_WIN32)
		utcSecondsFromLocalCivil = _mkgmtime(&workUtcTm);
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__) || defined(__FreeBSD__) || defined(__NetBSD__)
		utcSecondsFromLocalCivil = timegm(&workUtcTm);
#else
		// Fallback（极少数平台）：保持原始实现的思路（可能在 DST 边界出现 false-negative）。
		std::tm gmTm = {};
		if (!GetGmTm(timeValue, gmTm))
		{
			return false;
		}

		std::tm requestedLocalTm = gmTm;
		requestedLocalTm.tm_isdst = -1;

		std::tm workLocalTm = requestedLocalTm;
		const std::time_t localAsUtc = std::mktime(&workLocalTm);

		std::tm checkTm = {};
		if (!GetLocalTm(localAsUtc, checkTm))
		{
			return false;
		}

		if (checkTm.tm_year != requestedLocalTm.tm_year
			|| checkTm.tm_mon != requestedLocalTm.tm_mon
			|| checkTm.tm_mday != requestedLocalTm.tm_mday
			|| checkTm.tm_hour != requestedLocalTm.tm_hour
			|| checkTm.tm_min != requestedLocalTm.tm_min
			|| checkTm.tm_sec != requestedLocalTm.tm_sec)
		{
			return false;
		}

		const int64_t offsetSecondsFallback = static_cast<int64_t>(timeValue) - static_cast<int64_t>(localAsUtc);
		if ((offsetSecondsFallback % 60LL) != 0)
		{
			return false;
		}

		const int64_t offsetMinutesFallback = offsetSecondsFallback / 60LL;
		if (offsetMinutesFallback < -static_cast<int64_t>(kMaxOffsetMinutesAbs)
			|| offsetMinutesFallback > static_cast<int64_t>(kMaxOffsetMinutesAbs))
		{
			return false;
		}
		if (offsetMinutesFallback < static_cast<int64_t>(std::numeric_limits<int>::min())
			|| offsetMinutesFallback > static_cast<int64_t>(std::numeric_limits<int>::max()))
		{
			return false;
		}

		outOffsetMinutes = static_cast<int>(offsetMinutesFallback);
		return true;
#endif

		// round-trip validation (处理 time_t == -1 等边界；并确保转换结果一致)
		std::tm checkUtcTm = {};
		if (!GetGmTm(utcSecondsFromLocalCivil, checkUtcTm))
		{
			return false;
		}

		if (checkUtcTm.tm_year != workUtcTm.tm_year
			|| checkUtcTm.tm_mon != workUtcTm.tm_mon
			|| checkUtcTm.tm_mday != workUtcTm.tm_mday
			|| checkUtcTm.tm_hour != workUtcTm.tm_hour
			|| checkUtcTm.tm_min != workUtcTm.tm_min
			|| checkUtcTm.tm_sec != workUtcTm.tm_sec)
		{
			return false;
		}

		const int64_t offsetSeconds = static_cast<int64_t>(utcSecondsFromLocalCivil) - static_cast<int64_t>(timeValue);
		if ((offsetSeconds % 60LL) != 0)
		{
			return false;
		}

		const int64_t offsetMinutes = offsetSeconds / 60LL;
		if (offsetMinutes < -static_cast<int64_t>(kMaxOffsetMinutesAbs)
			|| offsetMinutes > static_cast<int64_t>(kMaxOffsetMinutesAbs))
		{
			return false;
		}
		if (offsetMinutes < static_cast<int64_t>(std::numeric_limits<int>::min())
			|| offsetMinutes > static_cast<int64_t>(std::numeric_limits<int>::max()))
		{
			return false;
		}

		outOffsetMinutes = static_cast<int>(offsetMinutes);
		return true;
	}

	inline bool TryConvertCivilToUnixMillisecondsUtc(
		const GB_Date& date,
		const GB_Time& time,
		int offsetMinutes,
		int64_t& outUnixMilliseconds)
	{
		if (!date.IsValid() || !time.IsValid())
		{
			return false;
		}

		if (!IsValidOffsetMinutes(offsetMinutes))
		{
			return false;
		}

		int daysSinceEpoch = 0;
		if (!date.ToDaysSinceEpoch(daysSinceEpoch))
		{
			return false;
		}

		const int timeMs = time.ToMillisecondsSinceStartOfDay();
		if (timeMs < 0)
		{
			return false;
		}

		const int64_t unixMs = static_cast<int64_t>(daysSinceEpoch) * kMillisecondsPerDay64
			+ static_cast<int64_t>(timeMs)
			- static_cast<int64_t>(offsetMinutes) * kMillisecondsPerMinute;

		outUnixMilliseconds = unixMs;
		return IsUnixMillisecondsInSupportedUtcRange(unixMs);
	}

	inline bool TryConvertCivilLocalToUnixMilliseconds(
		const GB_Date& date,
		const GB_Time& time,
		int64_t& outUnixMilliseconds)
	{
		if (!date.IsValid() || !time.IsValid())
		{
			return false;
		}

		const int year = date.Year();
		const int month = date.Month();
		const int day = date.Day();
		if (year == 0 || month == 0 || day == 0)
		{
			return false;
		}

		std::tm tmValue = {};
		tmValue.tm_year = year - 1900;
		tmValue.tm_mon = month - 1;
		tmValue.tm_mday = day;
		tmValue.tm_hour = time.Hour();
		tmValue.tm_min = time.Minute();
		tmValue.tm_sec = time.Second();
		tmValue.tm_isdst = -1;

		// Preserve the requested civil time BEFORE mktime() potentially normalizes the struct.
		const std::tm requestedTm = tmValue;

		const std::time_t secondsValue = std::mktime(&tmValue);

		// Round-trip validation to ensure the specified local civil time exists (i.e. was not normalized).
		std::tm checkTm = {};
		if (!GetLocalTm(secondsValue, checkTm))
		{
			return false;
		}

		if (checkTm.tm_year != requestedTm.tm_year
			|| checkTm.tm_mon != requestedTm.tm_mon
			|| checkTm.tm_mday != requestedTm.tm_mday
			|| checkTm.tm_hour != requestedTm.tm_hour
			|| checkTm.tm_min != requestedTm.tm_min
			|| checkTm.tm_sec != requestedTm.tm_sec)
		{
			return false;
		}

		const int64_t unixSeconds = static_cast<int64_t>(secondsValue);
		const int64_t unixMs = unixSeconds * kMillisecondsPerSecond + static_cast<int64_t>(time.Millisecond());
		if (!IsUnixMillisecondsInSupportedUtcRange(unixMs))
		{
			return false;
		}

		outUnixMilliseconds = unixMs;
		return true;
	}

}

const GB_DateTime GB_DateTime::Invalid = GB_DateTime();

GB_DateTime::GB_DateTime()
{
	Reset();
}

GB_DateTime::GB_DateTime(const GB_Date& date, const GB_Time& time, GB_DateTimeSpec spec, int offsetFromUtcMinutes)
{
	Reset();

	this->spec = spec;
	this->offsetMinutes = (spec == GB_DateTimeSpec::OffsetFromUtc) ? offsetFromUtcMinutes : 0;

	if (!date.IsValid() || !time.IsValid())
	{
		return;
	}

	int64_t computedUnixMs = 0;

	if (spec == GB_DateTimeSpec::UtcTime)
	{
		if (!TryConvertCivilToUnixMillisecondsUtc(date, time, 0, computedUnixMs))
		{
			return;
		}
	}
	else if (spec == GB_DateTimeSpec::OffsetFromUtc)
	{
		if (!TryConvertCivilToUnixMillisecondsUtc(date, time, offsetFromUtcMinutes, computedUnixMs))
		{
			return;
		}
	}
	else
	{
		if (!TryConvertCivilLocalToUnixMilliseconds(date, time, computedUnixMs))
		{
			return;
		}
	}

	unixMilliseconds = computedUnixMs;
	valid = true;
}

bool GB_DateTime::IsValid() const
{
	return valid;
}

void GB_DateTime::Reset()
{
	unixMilliseconds = 0;
	spec = GB_DateTimeSpec::LocalTime;
	offsetMinutes = 0;
	valid = false;
}

GB_DateTimeSpec GB_DateTime::Spec() const
{
	return spec;
}

int GB_DateTime::OffsetFromUtcMinutes() const
{
	if (!valid)
	{
		return 0;
	}

	if (spec == GB_DateTimeSpec::UtcTime)
	{
		return 0;
	}

	if (spec == GB_DateTimeSpec::OffsetFromUtc)
	{
		return offsetMinutes;
	}

	int computed = 0;
	if (!TryGetLocalOffsetMinutesFromUnixMilliseconds(unixMilliseconds, computed))
	{
		return 0;
	}

	return computed;
}

GB_Date GB_DateTime::Date() const
{
	if (!valid)
	{
		return GB_Date::Invalid;
	}

	GB_Date date;
	GB_Time time;

	if (spec == GB_DateTimeSpec::UtcTime)
	{
		if (!TryGetUtcComponentsFromUnixMilliseconds(unixMilliseconds, 0, date, time))
		{
			return GB_Date::Invalid;
		}
		return date;
	}

	if (spec == GB_DateTimeSpec::OffsetFromUtc)
	{
		if (!TryGetUtcComponentsFromUnixMilliseconds(unixMilliseconds, offsetMinutes, date, time))
		{
			return GB_Date::Invalid;
		}
		return date;
	}

	if (!TryGetLocalComponentsFromUnixMilliseconds(unixMilliseconds, date, time))
	{
		return GB_Date::Invalid;
	}

	return date;
}

GB_Time GB_DateTime::Time() const
{
	if (!valid)
	{
		return GB_Time::Invalid;
	}

	GB_Date date;
	GB_Time time;

	if (spec == GB_DateTimeSpec::UtcTime)
	{
		if (!TryGetUtcComponentsFromUnixMilliseconds(unixMilliseconds, 0, date, time))
		{
			return GB_Time::Invalid;
		}
		return time;
	}

	if (spec == GB_DateTimeSpec::OffsetFromUtc)
	{
		if (!TryGetUtcComponentsFromUnixMilliseconds(unixMilliseconds, offsetMinutes, date, time))
		{
			return GB_Time::Invalid;
		}
		return time;
	}

	if (!TryGetLocalComponentsFromUnixMilliseconds(unixMilliseconds, date, time))
	{
		return GB_Time::Invalid;
	}

	return time;
}

long long GB_DateTime::ToUnixMilliseconds() const
{
	if (!valid)
	{
		return 0;
	}

	return unixMilliseconds;
}

long long GB_DateTime::ToUnixSeconds() const
{
	if (!valid)
	{
		return 0;
	}

	return DivFloor(unixMilliseconds, kMillisecondsPerSecond);
}

GB_DateTime GB_DateTime::CreateFromUnixMilliseconds(long long unixMilliseconds, GB_DateTimeSpec spec, int offsetFromUtcMinutes)
{
	GB_DateTime result;
	result.Reset();
	result.spec = spec;
	result.offsetMinutes = (spec == GB_DateTimeSpec::OffsetFromUtc) ? offsetFromUtcMinutes : 0;
	result.unixMilliseconds = static_cast<int64_t>(unixMilliseconds);

	if (spec == GB_DateTimeSpec::OffsetFromUtc && !IsValidOffsetMinutes(offsetFromUtcMinutes))
	{
		return GB_DateTime::Invalid;
	}

	GB_Date date;
	GB_Time time;
	bool ok = false;

	if (spec == GB_DateTimeSpec::UtcTime)
	{
		ok = TryGetUtcComponentsFromUnixMilliseconds(result.unixMilliseconds, 0, date, time);
	}
	else if (spec == GB_DateTimeSpec::OffsetFromUtc)
	{
		ok = TryGetUtcComponentsFromUnixMilliseconds(result.unixMilliseconds, offsetFromUtcMinutes, date, time);
	}
	else
	{
		ok = TryGetLocalComponentsFromUnixMilliseconds(result.unixMilliseconds, date, time);
	}

	if (!ok)
	{
		return GB_DateTime::Invalid;
	}

	result.valid = true;
	return result;
}

GB_DateTime GB_DateTime::CreateFromUnixSeconds(long long unixSeconds, GB_DateTimeSpec spec, int offsetFromUtcMinutes)
{
	const int64_t seconds = static_cast<int64_t>(unixSeconds);
	if (seconds > (std::numeric_limits<int64_t>::max() / kMillisecondsPerSecond)
		|| seconds < (std::numeric_limits<int64_t>::min() / kMillisecondsPerSecond))
	{
		return GB_DateTime::Invalid;
	}

	const int64_t unixMs = seconds * kMillisecondsPerSecond;
	return CreateFromUnixMilliseconds(unixMs, spec, offsetFromUtcMinutes);
}

GB_DateTime GB_DateTime::Now()
{
	const auto now = std::chrono::system_clock::now();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
	return CreateFromUnixMilliseconds(static_cast<long long>(ms), GB_DateTimeSpec::LocalTime, 0);
}

GB_DateTime GB_DateTime::UtcNow()
{
	const auto now = std::chrono::system_clock::now();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
	return CreateFromUnixMilliseconds(static_cast<long long>(ms), GB_DateTimeSpec::UtcTime, 0);
}

GB_DateTime GB_DateTime::ToUtc() const
{
	if (!valid)
	{
		return GB_DateTime::Invalid;
	}

	return CreateFromUnixMilliseconds(unixMilliseconds, GB_DateTimeSpec::UtcTime, 0);
}

GB_DateTime GB_DateTime::ToLocal() const
{
	if (!valid)
	{
		return GB_DateTime::Invalid;
	}

	return CreateFromUnixMilliseconds(unixMilliseconds, GB_DateTimeSpec::LocalTime, 0);
}

GB_DateTime GB_DateTime::ToOffsetFromUtc(int offsetFromUtcMinutes) const
{
	if (!valid)
	{
		return GB_DateTime::Invalid;
	}

	return CreateFromUnixMilliseconds(unixMilliseconds, GB_DateTimeSpec::OffsetFromUtc, offsetFromUtcMinutes);
}

std::string GB_DateTime::ToIsoString(bool includeMilliseconds, bool includeTzSuffix) const
{
	if (!valid)
	{
		return {};
	}

	GB_Date date;
	GB_Time time;
	bool ok = false;

	if (spec == GB_DateTimeSpec::UtcTime)
	{
		ok = TryGetUtcComponentsFromUnixMilliseconds(unixMilliseconds, 0, date, time);
	}
	else if (spec == GB_DateTimeSpec::OffsetFromUtc)
	{
		ok = TryGetUtcComponentsFromUnixMilliseconds(unixMilliseconds, offsetMinutes, date, time);
	}
	else
	{
		ok = TryGetLocalComponentsFromUnixMilliseconds(unixMilliseconds, date, time);
	}

	if (!ok || !date.IsValid() || !time.IsValid())
	{
		return {};
	}

	std::string result;
	result.reserve(40);

	result.append(date.ToIsoString());
	result.push_back('T');
	result.append(time.ToIsoString(includeMilliseconds));

	if (!includeTzSuffix)
	{
		return result;
	}

	if (spec == GB_DateTimeSpec::UtcTime)
	{
		result.push_back('Z');
		return result;
	}

	int offset = 0;
	if (spec == GB_DateTimeSpec::OffsetFromUtc)
	{
		offset = offsetMinutes;
	}
	else
	{
		int computedOffset = 0;
		if (!TryGetLocalOffsetMinutesFromUnixMilliseconds(unixMilliseconds, computedOffset))
		{
			// 无法可靠计算该时间点的本地 UTC 偏移；为了避免误导，直接不输出时区后缀。
			return result;
		}
		offset = computedOffset;
	}

	if (offset == 0)
	{
		result.push_back('Z');
		return result;
	}

	const char sign = (offset < 0) ? '-' : '+';
	const int absMinutes = (offset < 0) ? -offset : offset;
	const int hour = absMinutes / 60;
	const int minute = absMinutes % 60;

	result.push_back(sign);
	result.push_back(static_cast<char>('0' + (hour / 10) % 10));
	result.push_back(static_cast<char>('0' + (hour / 1) % 10));
	result.push_back(':');
	result.push_back(static_cast<char>('0' + (minute / 10) % 10));
	result.push_back(static_cast<char>('0' + (minute / 1) % 10));
	return result;
}

GB_DateTime GB_DateTime::CreateFromIsoString(const std::string& textUtf8, GB_DateTimeSpec defaultSpec)
{
	GB_DateTime dateTime;
	if (!ParseIsoString(textUtf8, dateTime, defaultSpec))
	{
		return GB_DateTime::Invalid;
	}
	return dateTime;
}

bool GB_DateTime::ParseIsoString(const std::string& textUtf8, GB_DateTime& outDateTime, GB_DateTimeSpec defaultSpec)
{
	GB_DateTime parsed;
	if (!ParseIsoDateTimeSpan(textUtf8.data(), textUtf8.size(), parsed, defaultSpec))
	{
		return false;
	}

	outDateTime = parsed;
	return true;
}

GB_DateTime GB_DateTime::AddMSecs(long long milliseconds) const
{
	if (!valid)
	{
		return GB_DateTime::Invalid;
	}

	int64_t sum = 0;
	if (!TryAddInt64(unixMilliseconds, static_cast<int64_t>(milliseconds), sum))
	{
		return GB_DateTime::Invalid;
	}

	return CreateFromUnixMilliseconds(sum, spec, offsetMinutes);
}

GB_DateTime GB_DateTime::AddSecs(int seconds) const
{
	return AddMSecs(static_cast<long long>(seconds) * kMillisecondsPerSecond);
}

GB_DateTime GB_DateTime::AddDays(int days) const
{
	if (!valid)
	{
		return GB_DateTime::Invalid;
	}

	const GB_Date date = Date();
	const GB_Time time = Time();
	if (!date.IsValid() || !time.IsValid())
	{
		return GB_DateTime::Invalid;
	}

	const GB_Date newDate = date.AddDays(days);
	if (!newDate.IsValid())
	{
		return GB_DateTime::Invalid;
	}

	if (spec == GB_DateTimeSpec::OffsetFromUtc)
	{
		return GB_DateTime(newDate, time, GB_DateTimeSpec::OffsetFromUtc, offsetMinutes);
	}

	return GB_DateTime(newDate, time, spec, 0);
}

int64_t GB_DateTime::MsecsTo(const GB_DateTime& other) const
{
	if (!valid || !other.valid)
	{
		return 0;
	}

	return SaturatingSubInt64(other.unixMilliseconds, unixMilliseconds);
}

double GB_DateTime::SecondsTo(const GB_DateTime& other) const
{
	if (!valid || !other.valid)
	{
		return 0.0;
	}

	const int64_t deltaMs = MsecsTo(other);
	return static_cast<double>(deltaMs) / 1000.0;
}

bool GB_DateTime::operator==(const GB_DateTime& other) const
{
	if (valid != other.valid)
	{
		return false;
	}

	if (!valid)
	{
		return true;
	}

	return unixMilliseconds == other.unixMilliseconds;
}

bool GB_DateTime::operator!=(const GB_DateTime& other) const
{
	return !(*this == other);
}

bool GB_DateTime::operator<(const GB_DateTime& other) const
{
	if (valid != other.valid)
	{
		return valid ? false : true;
	}

	if (!valid)
	{
		return false;
	}

	return unixMilliseconds < other.unixMilliseconds;
}

bool GB_DateTime::operator<=(const GB_DateTime& other) const
{
	return !(*this > other);
}

bool GB_DateTime::operator>(const GB_DateTime& other) const
{
	return other < *this;
}

bool GB_DateTime::operator>=(const GB_DateTime& other) const
{
	return !(*this < other);
}

size_t GB_DateTime::GB_DateTimeHash::operator()(const GB_DateTime& dateTime) const noexcept
{
	if (!dateTime.IsValid())
	{
		return std::hash<uint64_t>()(0xCBF29CE484222325ULL);
	}

	return std::hash<int64_t>()(dateTime.ToUnixMilliseconds());
}

std::string GB_DateTime::SerializeToString() const
{
	// Format: GB_DateTime|<version>|<validFlag>|<unixMilliseconds>|<spec>|<offsetMinutes>
	std::string result;
	result.reserve(96);

	result.append("GB_DateTime|");
	result.append(std::to_string(static_cast<int>(kDateTimeBinaryVersion)));
	result.push_back('|');
	result.append(std::to_string(valid ? 1 : 0));
	result.push_back('|');
	result.append(std::to_string(unixMilliseconds));
	result.push_back('|');
	result.append(std::to_string(static_cast<int>(spec)));
	result.push_back('|');
	result.append(std::to_string((spec == GB_DateTimeSpec::OffsetFromUtc) ? offsetMinutes : 0));
	return result;
}

GB_ByteBuffer GB_DateTime::SerializeToBinary() const
{
	// Layout (little-endian):
	// [uint32 magic][uint32 version][int32 validFlag][int64 unixMilliseconds][int32 spec][int32 offsetMinutes]
	GB_ByteBuffer buffer;
	buffer.reserve(kExpectedDateTimeBinarySize);

	AppendUInt32LE(buffer, GB_ClassMagicNumber);
	AppendUInt32LE(buffer, kDateTimeBinaryVersion);
	AppendInt32LE(buffer, valid ? 1 : 0);
	AppendInt64LE(buffer, unixMilliseconds);
	AppendInt32LE(buffer, static_cast<int32_t>(spec));
	AppendInt32LE(buffer, static_cast<int32_t>((spec == GB_DateTimeSpec::OffsetFromUtc) ? offsetMinutes : 0));

	return buffer;
}

bool GB_DateTime::Deserialize(const std::string& data)
{
	return DeserializeDateTimeFromSpan(*this, data.data(), data.size());
}

bool GB_DateTime::Deserialize(const GB_ByteBuffer& data)
{
	return DeserializeDateTimeFromBinary(*this, data);
}


// -------------------------------------------------------------------------------------------------
// GB_TimeDuration
// -------------------------------------------------------------------------------------------------

namespace
{
	inline bool IsAsciiAlpha(char ch)
	{
		return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
	}

	inline char ToAsciiUpper(char ch)
	{
		if (ch >= 'a' && ch <= 'z')
		{
			return static_cast<char>(ch - 'a' + 'A');
		}
		return ch;
	}

	inline bool TryParseDoubleSpan(const char* data, size_t length, double& outValue)
	{
		if (data == nullptr)
		{
			return false;
		}
		if (length == 0)
		{
			return false;
		}

		size_t i = 0;
		bool negative = false;
		if (data[i] == '+' || data[i] == '-')
		{
			negative = (data[i] == '-');
			i++;
			if (i >= length)
			{
				return false;
			}
		}

		// integer part
		bool hasDigits = false;
		long long integerPart = 0;
		for (; i < length; i++)
		{
			const char ch = data[i];
			if (ch < '0' || ch > '9')
			{
				break;
			}
			hasDigits = true;

			const int digit = (ch - '0');
			if (integerPart > (std::numeric_limits<long long>::max() - digit) / 10LL)
			{
				return false;
			}
			integerPart = integerPart * 10LL + digit;
		}

		long double value = static_cast<long double>(integerPart);

		// fraction
		if (i < length && (data[i] == '.' || data[i] == ','))
		{
			i++;

			long double base = 1.0L;
			for (; i < length; i++)
			{
				const char ch = data[i];
				if (ch < '0' || ch > '9')
				{
					break;
				}
				hasDigits = true;

				base *= 10.0L;
				value += static_cast<long double>(ch - '0') / base;
			}
		}

		if (!hasDigits)
		{
			return false;
		}

		if (i != length)
		{
			// No exponent, no trailing garbage.
			return false;
		}

		if (negative)
		{
			value = -value;
		}

		const double asDouble = static_cast<double>(value);
		if (!std::isfinite(asDouble))
		{
			return false;
		}

		outValue = asDouble;
		return true;
	}

	inline bool IsAlmostInteger(double value, double epsilon)
	{
		if (!std::isfinite(value))
		{
			return false;
		}

		const double nearest = std::round(value);
		return std::fabs(value - nearest) <= epsilon;
	}

	inline int ClampLongLongToInt(long long value);

	inline int NegateIntSaturating(int value)
	{
		if (value == std::numeric_limits<int>::min())
		{
			// Avoid UB: -INT_MIN cannot be represented by int.
			return std::numeric_limits<int>::max();
		}
		return -value;
	}

	inline int AddIntSaturating(int a, int b)
	{
		const long long sum = static_cast<long long>(a) + static_cast<long long>(b);
		return ClampLongLongToInt(sum);
	}

	inline int SubIntSaturating(int a, int b)
	{
		const long long diff = static_cast<long long>(a) - static_cast<long long>(b);
		return ClampLongLongToInt(diff);
	}

	inline std::string DoubleToStringTrim(double value)
	{
		// Using "%.15g" to avoid trailing zeros and excessive digits.
		char buffer[96] = {};
		std::snprintf(buffer, sizeof(buffer), "%.15g", value);
		return std::string(buffer);
	}

	inline bool TryAddToIntChecked(int value, long long delta, int& outValue)
	{
		const long long sum = static_cast<long long>(value) + delta;
		if (sum < static_cast<long long>(std::numeric_limits<int>::min())
			|| sum > static_cast<long long>(std::numeric_limits<int>::max()))
		{
			return false;
		}

		outValue = static_cast<int>(sum);
		return true;
	}


	inline int ClampLongLongToInt(long long value)
	{
		if (value < static_cast<long long>(std::numeric_limits<int>::min()))
		{
			return std::numeric_limits<int>::min();
		}
		if (value > static_cast<long long>(std::numeric_limits<int>::max()))
		{
			return std::numeric_limits<int>::max();
		}
		return static_cast<int>(value);
	}

	inline bool TryParseIso8601DurationSpan(const char* data, size_t length, GB_TimeDuration& outDuration)
	{
		if (data == nullptr)
		{
			return false;
		}

		size_t start = 0;
		size_t end = 0;
		GetTrimmedAsciiRange(data, length, start, end);
		if (end <= start)
		{
			return false;
		}

		const char* textData = data + start;
		const size_t textLength = end - start;

		size_t i = 0;
		int overallSign = 1;
		if (textData[i] == '+' || textData[i] == '-')
		{
			overallSign = (textData[i] == '-') ? -1 : 1;
			i++;
			if (i >= textLength)
			{
				return false;
			}
		}

		if (ToAsciiUpper(textData[i]) != 'P')
		{
			return false;
		}
		i++;

		bool inTime = false;
		bool hasAny = false;

		bool hasYears = false;
		bool hasMonths = false;
		bool hasWeeks = false;
		bool hasDays = false;
		bool hasHours = false;
		bool hasMinutes = false;
		bool hasSeconds = false;

		GB_TimeDuration duration;

		while (i < textLength)
		{
			const char ch = textData[i];
			if (ToAsciiUpper(ch) == 'T')
			{
				inTime = true;
				i++;
				continue;
			}

			// component sign (non-standard but useful)
			int componentSign = 1;
			if (textData[i] == '+' || textData[i] == '-')
			{
				componentSign = (textData[i] == '-') ? -1 : 1;
				i++;
				if (i >= textLength)
				{
					return false;
				}
			}

			const size_t numberStart = i;
			bool seenDotOrComma = false;
			for (; i < textLength; i++)
			{
				const char nch = textData[i];
				if (nch >= '0' && nch <= '9')
				{
					continue;
				}
				if ((nch == '.' || nch == ',') && !seenDotOrComma)
				{
					seenDotOrComma = true;
					continue;
				}
				break;
			}

			if (i == numberStart)
			{
				return false;
			}

			if (i >= textLength)
			{
				return false;
			}

			const char unit = ToAsciiUpper(textData[i]);
			const size_t numberLength = i - numberStart;
			const int signedFactor = overallSign * componentSign;

			if (unit == 'S')
			{
				if (!inTime)
				{
					// "P...S" is invalid.
					return false;
				}

				if (hasSeconds)
				{
					return false;
				}

				double secondsValue = 0.0;
				if (!TryParseDoubleSpan(textData + numberStart, numberLength, secondsValue))
				{
					return false;
				}

				duration.seconds = secondsValue * static_cast<double>(signedFactor);
				hasSeconds = true;
				hasAny = true;
			}
			else
			{
				if (seenDotOrComma)
				{
					// Only seconds may be fractional.
					return false;
				}

				int value = 0;
				if (!TryParseInt32Span(textData + numberStart, numberLength, value))
				{
					return false;
				}

				value *= signedFactor;

				if (!inTime)
				{
					if (unit == 'Y')
					{
						if (hasYears)
						{
							return false;
						}
						duration.years = value;
						hasYears = true;
						hasAny = true;
					}
					else if (unit == 'M')
					{
						if (hasMonths)
						{
							return false;
						}
						duration.months = value;
						hasMonths = true;
						hasAny = true;
					}
					else if (unit == 'W')
					{
						if (hasWeeks)
						{
							return false;
						}
						duration.weeks = value;
						hasWeeks = true;
						hasAny = true;
					}
					else if (unit == 'D')
					{
						if (hasDays)
						{
							return false;
						}
						duration.days = value;
						hasDays = true;
						hasAny = true;
					}
					else
					{
						return false;
					}
				}
				else
				{
					if (unit == 'H')
					{
						if (hasHours)
						{
							return false;
						}
						duration.hours = value;
						hasHours = true;
						hasAny = true;
					}
					else if (unit == 'M')
					{
						if (hasMinutes)
						{
							return false;
						}
						duration.minutes = value;
						hasMinutes = true;
						hasAny = true;
					}
					else
					{
						return false;
					}
				}
			}

			i++; // consume unit
		}

		if (!hasAny)
		{
			return false;
		}

		outDuration = duration;
		return true;
	}

	inline bool TryParseTokenDurationSpan(const char* data, size_t length, GB_TimeDuration& outDuration)
	{
		if (data == nullptr)
		{
			return false;
		}

		size_t start = 0;
		size_t end = 0;
		GetTrimmedAsciiRange(data, length, start, end);
		if (end <= start)
		{
			return false;
		}

		const char* textData = data + start;
		const size_t textLength = end - start;

		GB_TimeDuration duration;
		bool hasAny = false;

		size_t i = 0;
		while (i < textLength)
		{
			while (i < textLength && (IsAsciiWhitespace(static_cast<unsigned char>(textData[i])) || textData[i] == ',' || textData[i] == ';'))
			{
				i++;
			}
			if (i >= textLength)
			{
				break;
			}

			const size_t tokenStart = i;
			while (i < textLength && !IsAsciiWhitespace(static_cast<unsigned char>(textData[i])) && textData[i] != ',' && textData[i] != ';')
			{
				i++;
			}
			const size_t tokenEnd = i;
			if (tokenEnd <= tokenStart)
			{
				continue;
			}

			// split into number + unit suffix
			size_t j = tokenStart;
			if (textData[j] == '+' || textData[j] == '-')
			{
				j++;
			}

			bool seenDotOrComma = false;
			while (j < tokenEnd)
			{
				const char ch = textData[j];
				if (ch >= '0' && ch <= '9')
				{
					j++;
					continue;
				}
				if ((ch == '.' || ch == ',') && !seenDotOrComma)
				{
					seenDotOrComma = true;
					j++;
					continue;
				}
				break;
			}

			if (j == tokenStart || (j == tokenStart + 1 && (textData[tokenStart] == '+' || textData[tokenStart] == '-')))
			{
				return false;
			}

			const size_t numberLength = j - tokenStart;
			const size_t unitStart = j;
			const size_t unitLength = tokenEnd - unitStart;

			double numberValue = 0.0;
			if (!TryParseDoubleSpan(textData + tokenStart, numberLength, numberValue))
			{
				return false;
			}

			auto unitEqualsIgnoreCase = [&](const char* unitLiteral) -> bool
				{
					const size_t literalLength = std::strlen(unitLiteral);
					if (unitLength != literalLength)
					{
						return false;
					}
					for (size_t k = 0; k < unitLength; k++)
					{
						if (ToAsciiUpper(textData[unitStart + k]) != ToAsciiUpper(unitLiteral[k]))
						{
							return false;
						}
					}
					return true;
				};

			auto unitIsSingleChar = [&](char c) -> bool
				{
					return unitLength == 1 && ToAsciiUpper(textData[unitStart]) == ToAsciiUpper(c);
				};

			if (unitLength == 0)
			{
				// No unit: interpret as seconds.
				duration.seconds += numberValue;
				hasAny = true;
				continue;
			}

			if (unitIsSingleChar('Y') || unitEqualsIgnoreCase("YEAR") || unitEqualsIgnoreCase("YEARS"))
			{
				if (!IsAlmostInteger(numberValue, 1e-12))
				{
					return false;
				}
				int newYears = 0;
				if (!TryAddToIntChecked(duration.years, static_cast<long long>(numberValue), newYears))
				{
					return false;
				}
				duration.years = newYears;
				hasAny = true;
			}
			else if (unitEqualsIgnoreCase("MO") || unitEqualsIgnoreCase("MON") || unitEqualsIgnoreCase("MONTH") || unitEqualsIgnoreCase("MONTHS"))
			{
				if (!IsAlmostInteger(numberValue, 1e-12))
				{
					return false;
				}
				int newMonths = 0;
				if (!TryAddToIntChecked(duration.months, static_cast<long long>(numberValue), newMonths))
				{
					return false;
				}
				duration.months = newMonths;
				hasAny = true;
			}
			else if (unitIsSingleChar('W') || unitEqualsIgnoreCase("WEEK") || unitEqualsIgnoreCase("WEEKS"))
			{
				if (!IsAlmostInteger(numberValue, 1e-12))
				{
					return false;
				}
				int newWeeks = 0;
				if (!TryAddToIntChecked(duration.weeks, static_cast<long long>(numberValue), newWeeks))
				{
					return false;
				}
				duration.weeks = newWeeks;
				hasAny = true;
			}
			else if (unitIsSingleChar('D') || unitEqualsIgnoreCase("DAY") || unitEqualsIgnoreCase("DAYS"))
			{
				if (!IsAlmostInteger(numberValue, 1e-12))
				{
					return false;
				}
				int newDays = 0;
				if (!TryAddToIntChecked(duration.days, static_cast<long long>(numberValue), newDays))
				{
					return false;
				}
				duration.days = newDays;
				hasAny = true;
			}
			else if (unitIsSingleChar('H') || unitEqualsIgnoreCase("HR") || unitEqualsIgnoreCase("HOUR") || unitEqualsIgnoreCase("HOURS"))
			{
				if (!IsAlmostInteger(numberValue, 1e-12))
				{
					return false;
				}
				int newHours = 0;
				if (!TryAddToIntChecked(duration.hours, static_cast<long long>(numberValue), newHours))
				{
					return false;
				}
				duration.hours = newHours;
				hasAny = true;
			}
			else if (unitIsSingleChar('M') || unitEqualsIgnoreCase("MIN") || unitEqualsIgnoreCase("MINS") || unitEqualsIgnoreCase("MINUTE") || unitEqualsIgnoreCase("MINUTES"))
			{
				if (!IsAlmostInteger(numberValue, 1e-12))
				{
					return false;
				}
				int newMinutes = 0;
				if (!TryAddToIntChecked(duration.minutes, static_cast<long long>(numberValue), newMinutes))
				{
					return false;
				}
				duration.minutes = newMinutes;
				hasAny = true;
			}
			else if (unitIsSingleChar('S') || unitEqualsIgnoreCase("SEC") || unitEqualsIgnoreCase("SECS") || unitEqualsIgnoreCase("SECOND") || unitEqualsIgnoreCase("SECONDS"))
			{
				duration.seconds += numberValue;
				hasAny = true;
			}
			else if (unitEqualsIgnoreCase("MS") || unitEqualsIgnoreCase("MSEC") || unitEqualsIgnoreCase("MSECS") || unitEqualsIgnoreCase("MILLISECOND") || unitEqualsIgnoreCase("MILLISECONDS"))
			{
				duration.seconds += numberValue / 1000.0;
				hasAny = true;
			}
			else
			{
				return false;
			}
		}

		if (!hasAny)
		{
			return false;
		}

		outDuration = duration;
		return true;
	}

	inline bool TryGetTimePartMilliseconds(const GB_TimeDuration& duration, int64_t& outMilliseconds)
	{
		// years/months are ignored here (they are calendar-based).
		if (!std::isfinite(duration.seconds))
		{
			return false;
		}

		int64_t total = 0;

		const int64_t weeksMs = static_cast<int64_t>(duration.weeks) * 7LL * kMillisecondsPerDay64;
		if (!TryAddInt64(total, weeksMs, total))
		{
			return false;
		}

		const int64_t daysMs = static_cast<int64_t>(duration.days) * kMillisecondsPerDay64;
		if (!TryAddInt64(total, daysMs, total))
		{
			return false;
		}

		const int64_t hoursMs = static_cast<int64_t>(duration.hours) * 60LL * 60LL * kMillisecondsPerSecond;
		if (!TryAddInt64(total, hoursMs, total))
		{
			return false;
		}

		const int64_t minutesMs = static_cast<int64_t>(duration.minutes) * 60LL * kMillisecondsPerSecond;
		if (!TryAddInt64(total, minutesMs, total))
		{
			return false;
		}

		const double secondsMsDouble = duration.seconds * 1000.0;
		if (!std::isfinite(secondsMsDouble))
		{
			return false;
		}
		if (secondsMsDouble > static_cast<double>(std::numeric_limits<int64_t>::max())
			|| secondsMsDouble < static_cast<double>(std::numeric_limits<int64_t>::min()))
		{
			return false;
		}
		if (secondsMsDouble > static_cast<double>(std::numeric_limits<int64_t>::max())
			|| secondsMsDouble < static_cast<double>(std::numeric_limits<int64_t>::min()))
		{
			return false;
		}
		const int64_t secondsMs = static_cast<int64_t>(std::llround(secondsMsDouble));
		if (!TryAddInt64(total, secondsMs, total))
		{
			return false;
		}

		outMilliseconds = total;
		return true;
	}


	inline bool TryGetClockPartMilliseconds(const GB_TimeDuration& duration, int64_t& outMilliseconds)
	{
		if (!std::isfinite(duration.seconds))
		{
			return false;
		}

		int64_t total = 0;

		const int64_t hoursMs = static_cast<int64_t>(duration.hours) * 60LL * 60LL * kMillisecondsPerSecond;
		if (!TryAddInt64(total, hoursMs, total))
		{
			return false;
		}

		const int64_t minutesMs = static_cast<int64_t>(duration.minutes) * 60LL * kMillisecondsPerSecond;
		if (!TryAddInt64(total, minutesMs, total))
		{
			return false;
		}

		const double secondsMsDouble = duration.seconds * 1000.0;
		if (!std::isfinite(secondsMsDouble))
		{
			return false;
		}
		if (secondsMsDouble > static_cast<double>(std::numeric_limits<int64_t>::max())
			|| secondsMsDouble < static_cast<double>(std::numeric_limits<int64_t>::min()))
		{
			return false;
		}

		const int64_t secondsMs = static_cast<int64_t>(std::llround(secondsMsDouble));
		if (!TryAddInt64(total, secondsMs, total))
		{
			return false;
		}

		outMilliseconds = total;
		return true;
	}
}

GB_TimeDuration GB_TimeDuration::CreateFromSeconds(double seconds)
{
	GB_TimeDuration duration;
	duration.seconds = seconds;
	return duration;
}

GB_TimeDuration GB_TimeDuration::CreateFromMinutes(long long minutes)
{
	GB_TimeDuration duration;
	duration.minutes = ClampLongLongToInt(minutes);
	return duration;
}

GB_TimeDuration GB_TimeDuration::CreateFromHours(long long hours)
{
	GB_TimeDuration duration;
	duration.hours = ClampLongLongToInt(hours);
	return duration;
}

GB_TimeDuration GB_TimeDuration::CreateFromDays(long long days)
{
	GB_TimeDuration duration;
	duration.days = ClampLongLongToInt(days);
	return duration;
}

GB_TimeDuration GB_TimeDuration::CreateFromWeeks(long long weeks)
{
	GB_TimeDuration duration;
	duration.weeks = ClampLongLongToInt(weeks);
	return duration;
}

GB_TimeDuration GB_TimeDuration::CreateFromMonths(long long months)
{
	GB_TimeDuration duration;
	duration.months = ClampLongLongToInt(months);
	return duration;
}

GB_TimeDuration GB_TimeDuration::CreateFromYears(long long years)
{
	GB_TimeDuration duration;
	duration.years = ClampLongLongToInt(years);
	return duration;
}

bool GB_TimeDuration::HasCalendarPart() const
{
	return years != 0 || months != 0;
}

GB_TimeDuration GB_TimeDuration::Negated() const
{
	GB_TimeDuration result;
	result.years = NegateIntSaturating(years);
	result.months = NegateIntSaturating(months);
	result.weeks = NegateIntSaturating(weeks);
	result.days = NegateIntSaturating(days);
	result.hours = NegateIntSaturating(hours);
	result.minutes = NegateIntSaturating(minutes);
	result.seconds = -seconds;
	return result;
}

GB_TimeDuration GB_TimeDuration::operator-() const
{
	return Negated();
}

GB_TimeDuration GB_TimeDuration::operator+(const GB_TimeDuration& other) const
{
	GB_TimeDuration result = *this;
	result += other;
	return result;
}

GB_TimeDuration GB_TimeDuration::operator-(const GB_TimeDuration& other) const
{
	GB_TimeDuration result = *this;
	result -= other;
	return result;
}

GB_TimeDuration& GB_TimeDuration::operator+=(const GB_TimeDuration& other)
{
	years = AddIntSaturating(years, other.years);
	months = AddIntSaturating(months, other.months);
	weeks = AddIntSaturating(weeks, other.weeks);
	days = AddIntSaturating(days, other.days);
	hours = AddIntSaturating(hours, other.hours);
	minutes = AddIntSaturating(minutes, other.minutes);
	seconds += other.seconds;
	return *this;
}

GB_TimeDuration& GB_TimeDuration::operator-=(const GB_TimeDuration& other)
{
	years = SubIntSaturating(years, other.years);
	months = SubIntSaturating(months, other.months);
	weeks = SubIntSaturating(weeks, other.weeks);
	days = SubIntSaturating(days, other.days);
	hours = SubIntSaturating(hours, other.hours);
	minutes = SubIntSaturating(minutes, other.minutes);
	seconds -= other.seconds;
	return *this;
}

GB_TimeDuration GB_TimeDuration::CreateFromString(const std::string& textUtf8, bool& ok)
{
	GB_TimeDuration duration;
	ok = false;

	size_t start = 0;
	size_t end = 0;
	GetTrimmedAsciiRange(textUtf8, start, end);
	if (end <= start)
	{
		return duration;
	}

	const char* textData = textUtf8.data() + start;
	const size_t textLength = end - start;

	// 1) ISO 8601 duration (PnYnMnWnDTnHnMnS), tolerate leading '+'/'-'.
	GB_TimeDuration parsed;
	if (TryParseIso8601DurationSpan(textData, textLength, parsed))
	{
		duration = parsed;
		ok = true;
		return duration;
	}

	// 2) Token form: "1y 2mo 3w 4d 5h 6m 7.5s" / "1500ms" / "10" (seconds)
	if (TryParseTokenDurationSpan(textData, textLength, parsed))
	{
		duration = parsed;
		ok = true;
		return duration;
	}

	return duration;
}

bool GB_TimeDuration::IsNull() const
{
	return years == 0
		&& months == 0
		&& weeks == 0
		&& days == 0
		&& hours == 0
		&& minutes == 0
		&& seconds == 0.0;
}

bool GB_TimeDuration::operator==(const GB_TimeDuration& other) const
{
	return years == other.years
		&& months == other.months
		&& weeks == other.weeks
		&& days == other.days
		&& hours == other.hours
		&& minutes == other.minutes
		&& seconds == other.seconds;
}

bool GB_TimeDuration::operator!=(const GB_TimeDuration& other) const
{
	return !(*this == other);
}

std::string GB_TimeDuration::ToString() const
{
	// ISO 8601 style: PnYnMnWnDTnHnMnS
	if (IsNull())
	{
		return "PT0S";
	}

	std::string result;
	result.reserve(64);
	result.push_back('P');

	auto AppendIntComponent = [&](int value, char suffix)
		{
			if (value == 0)
			{
				return;
			}
			result.append(std::to_string(value));
			result.push_back(suffix);
		};

	AppendIntComponent(years, 'Y');
	AppendIntComponent(months, 'M');
	AppendIntComponent(weeks, 'W');
	AppendIntComponent(days, 'D');

	const bool hasTime = (hours != 0) || (minutes != 0) || (seconds != 0.0);
	if (hasTime)
	{
		result.push_back('T');
		AppendIntComponent(hours, 'H');
		AppendIntComponent(minutes, 'M');
		if (seconds != 0.0)
		{
			result.append(DoubleToStringTrim(seconds));
			result.push_back('S');
		}
	}
	return result;
}

double GB_TimeDuration::ToTotalSecondsApprox() const
{
	// 注意：年/月是日历分量，不存在严格的固定秒数。
	// 这里采用近似换算：year=365d, month=30d，仅用于估算/显示。
	const long double yearSeconds = static_cast<long double>(years) * 365.0L * 86400.0L;
	const long double monthSeconds = static_cast<long double>(months) * 30.0L * 86400.0L;
	const long double weekSeconds = static_cast<long double>(weeks) * 7.0L * 86400.0L;
	const long double daySeconds = static_cast<long double>(days) * 86400.0L;
	const long double hourSeconds = static_cast<long double>(hours) * 3600.0L;
	const long double minuteSeconds = static_cast<long double>(minutes) * 60.0L;
	const long double secondSeconds = static_cast<long double>(seconds);

	const long double total = yearSeconds + monthSeconds + weekSeconds + daySeconds + hourSeconds + minuteSeconds + secondSeconds;
	return static_cast<double>(total);
}

long long GB_TimeDuration::ToSeconds() const
{
	// 与 ToTotalSecondsApprox() 一致，返回“近似总秒数”，并向 0 截断。
	const double total = ToTotalSecondsApprox();
	if (!std::isfinite(total))
	{
		return 0;
	}

	if (total >= static_cast<double>(std::numeric_limits<long long>::max()))
	{
		return std::numeric_limits<long long>::max();
	}
	if (total <= static_cast<double>(std::numeric_limits<long long>::min()))
	{
		return std::numeric_limits<long long>::min();
	}

	return static_cast<long long>(total);
}

bool GB_TimeDuration::TryToFixedSeconds(long long& outSeconds) const
{
	outSeconds = 0;

	if (years != 0 || months != 0)
	{
		return false;
	}

	if (!std::isfinite(seconds))
	{
		return false;
	}

	if (!IsAlmostInteger(seconds, 1e-12))
	{
		return false;
	}

	if (seconds > static_cast<double>(std::numeric_limits<long long>::max())
		|| seconds < static_cast<double>(std::numeric_limits<long long>::min()))
	{
		return false;
	}

	const long long secondsPart = static_cast<long long>(std::llround(seconds));

	int64_t total = 0;

	const int64_t weeksSeconds = static_cast<int64_t>(weeks) * 7LL * 86400LL;
	if (!TryAddInt64(total, weeksSeconds, total))
	{
		return false;
	}

	const int64_t daysSeconds = static_cast<int64_t>(days) * 86400LL;
	if (!TryAddInt64(total, daysSeconds, total))
	{
		return false;
	}

	const int64_t hoursSeconds = static_cast<int64_t>(hours) * 3600LL;
	if (!TryAddInt64(total, hoursSeconds, total))
	{
		return false;
	}

	const int64_t minutesSeconds = static_cast<int64_t>(minutes) * 60LL;
	if (!TryAddInt64(total, minutesSeconds, total))
	{
		return false;
	}

	if (!TryAddInt64(total, static_cast<int64_t>(secondsPart), total))
	{
		return false;
	}

	if (total < static_cast<int64_t>(std::numeric_limits<long long>::min())
		|| total > static_cast<int64_t>(std::numeric_limits<long long>::max()))
	{
		return false;
	}

	outSeconds = static_cast<long long>(total);
	return true;
}

bool GB_TimeDuration::TryToFixedMilliseconds(int64_t& outMilliseconds) const
{
	outMilliseconds = 0;

	if (years != 0 || months != 0)
	{
		return false;
	}

	if (!std::isfinite(seconds))
	{
		return false;
	}

	const double secondsMsDouble = seconds * 1000.0;
	if (!std::isfinite(secondsMsDouble))
	{
		return false;
	}
	if (secondsMsDouble > static_cast<double>(std::numeric_limits<int64_t>::max())
		|| secondsMsDouble < static_cast<double>(std::numeric_limits<int64_t>::min()))
	{
		return false;
	}

	if (!IsAlmostInteger(secondsMsDouble, 1e-6))
	{
		return false;
	}
	const int64_t secondsMs = static_cast<int64_t>(std::llround(secondsMsDouble));

	int64_t total = 0;

	const int64_t weeksMs = static_cast<int64_t>(weeks) * 7LL * kMillisecondsPerDay64;
	if (!TryAddInt64(total, weeksMs, total))
	{
		return false;
	}

	const int64_t daysMs = static_cast<int64_t>(days) * kMillisecondsPerDay64;
	if (!TryAddInt64(total, daysMs, total))
	{
		return false;
	}

	const int64_t hoursMs = static_cast<int64_t>(hours) * 60LL * 60LL * kMillisecondsPerSecond;
	if (!TryAddInt64(total, hoursMs, total))
	{
		return false;
	}

	const int64_t minutesMs = static_cast<int64_t>(minutes) * 60LL * kMillisecondsPerSecond;
	if (!TryAddInt64(total, minutesMs, total))
	{
		return false;
	}

	if (!TryAddInt64(total, secondsMs, total))
	{
		return false;
	}

	outMilliseconds = total;
	return true;
}

GB_DateTime GB_TimeDuration::AddToDateTime(const GB_DateTime& dateTime) const
{
	if (!dateTime.IsValid())
	{
		return GB_DateTime::Invalid;
	}

	GB_DateTime result = dateTime;

	// 1) Calendar part in the current Spec() view.
	if (years != 0 || months != 0 || weeks != 0 || days != 0)
	{
		const GB_Date baseDate = result.Date();
		const GB_Time baseTime = result.Time();
		if (!baseDate.IsValid() || !baseTime.IsValid())
		{
			return GB_DateTime::Invalid;
		}

		GB_Date newDate = baseDate;

		if (years != 0)
		{
			newDate = newDate.AddYears(years);
			if (!newDate.IsValid())
			{
				return GB_DateTime::Invalid;
			}
		}

		if (months != 0)
		{
			newDate = newDate.AddMonths(months);
			if (!newDate.IsValid())
			{
				return GB_DateTime::Invalid;
			}
		}

		const int64_t totalDays64 = static_cast<int64_t>(weeks) * 7LL + static_cast<int64_t>(days);
		if (totalDays64 < static_cast<int64_t>(std::numeric_limits<int>::min())
			|| totalDays64 > static_cast<int64_t>(std::numeric_limits<int>::max()))
		{
			return GB_DateTime::Invalid;
		}

		if (totalDays64 != 0)
		{
			newDate = newDate.AddDays(static_cast<int>(totalDays64));
			if (!newDate.IsValid())
			{
				return GB_DateTime::Invalid;
			}
		}

		if (result.Spec() == GB_DateTimeSpec::OffsetFromUtc)
		{
			result = GB_DateTime(newDate, baseTime, GB_DateTimeSpec::OffsetFromUtc, result.OffsetFromUtcMinutes());
		}
		else
		{
			result = GB_DateTime(newDate, baseTime, result.Spec(), 0);
		}

		if (!result.IsValid())
		{
			return GB_DateTime::Invalid;
		}
	}

	// 2) Fixed time part (absolute milliseconds shift).
	if (hours != 0 || minutes != 0 || seconds != 0.0)
	{
		int64_t timeMs = 0;
		if (!TryGetClockPartMilliseconds(*this, timeMs))
		{
			return GB_DateTime::Invalid;
		}
		result = result.AddMSecs(static_cast<long long>(timeMs));
	}

	return result;
}

// -------------------------------------------------------------------------------------------------
// GB_Date / GB_Time / GB_DateTime operators with GB_TimeDuration
// -------------------------------------------------------------------------------------------------

GB_Date GB_Date::operator+(const GB_TimeDuration& duration) const
{
	if (!IsValid())
	{
		return GB_Date::Invalid;
	}

	GB_Date result = *this;

	if (duration.years != 0)
	{
		result = result.AddYears(duration.years);
		if (!result.IsValid())
		{
			return GB_Date::Invalid;
		}
	}

	if (duration.months != 0)
	{
		result = result.AddMonths(duration.months);
		if (!result.IsValid())
		{
			return GB_Date::Invalid;
		}
	}

	const int64_t totalDays64 = static_cast<int64_t>(duration.weeks) * 7LL + static_cast<int64_t>(duration.days);
	if (totalDays64 < static_cast<int64_t>(std::numeric_limits<int>::min())
		|| totalDays64 > static_cast<int64_t>(std::numeric_limits<int>::max()))
	{
		return GB_Date::Invalid;
	}

	if (totalDays64 != 0)
	{
		result = result.AddDays(static_cast<int>(totalDays64));
		if (!result.IsValid())
		{
			return GB_Date::Invalid;
		}
	}

	// hours/minutes/seconds are ignored for GB_Date.
	return result;
}

GB_Date GB_Date::operator-(const GB_TimeDuration& duration) const
{
	return (*this) + (-duration);
}

GB_Time GB_Time::operator+(const GB_TimeDuration& duration) const
{
	if (!IsValid())
	{
		return GB_Time::Invalid;
	}

	// Only fixed-length units affect GB_Time; years/months are ignored.
	int64_t deltaMs = 0;
	if (!TryGetTimePartMilliseconds(duration, deltaMs))
	{
		return GB_Time::Invalid;
	}

	const int64_t deltaInDay = ModFloor(deltaMs, static_cast<int64_t>(kMillisecondsPerDay));
	return AddMSecs(static_cast<int>(deltaInDay));
}

GB_Time GB_Time::operator-(const GB_TimeDuration& duration) const
{
	return (*this) + (-duration);
}

GB_DateTime GB_DateTime::operator+(const GB_TimeDuration& duration) const
{
	return duration.AddToDateTime(*this);
}

GB_DateTime GB_DateTime::operator-(const GB_TimeDuration& duration) const
{
	return (-duration).AddToDateTime(*this);
}
