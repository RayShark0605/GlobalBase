#include "GB_DateTime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{
	constexpr long long kUnixEpochJdn = 2440588LL; // 1970-01-01
	constexpr uint32_t kDateBinaryVersion = 1;
	constexpr size_t kIsoDateStringLength = 10; // YYYY-MM-DD

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
		return std::hash<int>()(0);
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
