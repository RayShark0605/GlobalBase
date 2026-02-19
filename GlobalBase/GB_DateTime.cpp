#include "GB_DateTime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
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

	inline std::string TrimAsciiWhitespace(const std::string& text)
	{
		size_t start = 0;
		while (start < text.size() && IsAsciiWhitespace(static_cast<unsigned char>(text[start])))
		{
			start++;
		}

		size_t end = text.size();
		while (end > start && IsAsciiWhitespace(static_cast<unsigned char>(text[end - 1])))
		{
			end--;
		}

		return text.substr(start, end - start);
	}

	inline bool IsAllDigits(const std::string& s)
	{
		for (size_t i = 0; i < s.size(); i++)
		{
			if (s[i] < '0' || s[i] > '9')
			{
				return false;
			}
		}
		return true;
	}

	inline bool ParseFixedDigits(const std::string& s, size_t offset, size_t count, int& outValue)
	{
		if (offset + count > s.size())
		{
			return false;
		}

		int value = 0;
		for (size_t i = 0; i < count; i++)
		{
			const char ch = s[offset + i];
			if (ch < '0' || ch > '9')
			{
				return false;
			}
			value = value * 10 + (ch - '0');
		}

		outValue = value;
		return true;
	}

	inline bool TryParseInt32(const std::string& text, int& outValue)
	{
		if (text.empty())
		{
			return false;
		}

		size_t i = 0;
		bool negative = false;
		if (text[0] == '+' || text[0] == '-')
		{
			negative = (text[0] == '-');
			i++;
			if (i >= text.size())
			{
				return false;
			}
		}

		constexpr static long long positiveLimit = static_cast<long long>(std::numeric_limits<int>::max());
		constexpr static long long negativeLimitAbs = -static_cast<long long>(std::numeric_limits<int>::min());
		const long long limitAbs = negative ? negativeLimitAbs : positiveLimit;

		long long value = 0;
		for (; i < text.size(); i++)
		{
			const char ch = text[i];
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
			// value is within [-INT_MIN], safe.
			outValue = static_cast<int>(-value);
			return true;
		}

		outValue = static_cast<int>(value);
		return true;
	}

	// Floor division for possibly negative values.
	inline long long DivFloor(long long a, long long b)
	{
		// b > 0 assumed
		long long q = a / b;
		long long r = a % b;
		if (r != 0 && ((r > 0) != (b > 0)))
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
	return static_cast<int>(jdn % 7LL) + 1;
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
		return GB_Date::Invalid;
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
	const std::string text = TrimAsciiWhitespace(textUtf8);
	if (text.empty())
	{
		outDate = GB_Date::Invalid;
		return false;
	}

	int parsedYear = 0;
	int parsedMonth = 0;
	int parsedDay = 0;

	if (text.size() == 10)
	{
		// YYYY-MM-DD
		if (text[4] != '-' || text[7] != '-')
		{
			outDate = GB_Date::Invalid;
			return false;
		}

		if (!ParseFixedDigits(text, 0, 4, parsedYear)
			|| !ParseFixedDigits(text, 5, 2, parsedMonth)
			|| !ParseFixedDigits(text, 8, 2, parsedDay))
		{
			outDate = GB_Date::Invalid;
			return false;
		}
	}
	else if (text.size() == 8)
	{
		// YYYYMMDD
		if (!IsAllDigits(text))
		{
			outDate = GB_Date::Invalid;
			return false;
		}
		if (!ParseFixedDigits(text, 0, 4, parsedYear)
			|| !ParseFixedDigits(text, 4, 2, parsedMonth)
			|| !ParseFixedDigits(text, 6, 2, parsedDay))
		{
			outDate = GB_Date::Invalid;
			return false;
		}
	}
	else
	{
		outDate = GB_Date::Invalid;
		return false;
	}

	GB_Date date;
	if (!date.Set(parsedYear, parsedMonth, parsedDay))
	{
		outDate = GB_Date::Invalid;
		return false;
	}

	outDate = date;
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
	std::string result;
	result.reserve(64);
	result.append("GB_Date|");
	result.append(std::to_string(static_cast<int>(kDateBinaryVersion)));
	result.push_back('|');
	result.append(std::to_string(year));
	result.push_back('|');
	result.append(std::to_string(month));
	result.push_back('|');
	result.append(std::to_string(day));
	return result;
}

GB_ByteBuffer GB_Date::SerializeToBinary() const
{
	// Layout (little-endian):
	// [uint32 magic][uint32 version][int32 year][int32 month][int32 day]
	GB_ByteBuffer buffer;
	buffer.reserve(20);

	AppendUInt32LE(buffer, GB_ClassMagicNumber);
	AppendUInt32LE(buffer, kDateBinaryVersion);
	AppendInt32LE(buffer, static_cast<int32_t>(year));
	AppendInt32LE(buffer, static_cast<int32_t>(month));
	AppendInt32LE(buffer, static_cast<int32_t>(day));

	return buffer;
}

bool GB_Date::Deserialize(const std::string& data)
{
	const std::string text = TrimAsciiWhitespace(data);
	if (text.empty())
	{
		Reset();
		return false;
	}

	// 1) Prefer our explicit serialization format.
	//    GB_Date|version|year|month|day
	if (text.rfind("GB_Date|", 0) == 0)
	{
		// Avoid allocations/exception parsing for a low-level type.
		constexpr size_t prefixLength = 8; // "GB_Date|"
		const size_t p2 = text.find('|', prefixLength);
		if (p2 == std::string::npos)
		{
			Reset();
			return false;
		}
		const size_t p3 = text.find('|', p2 + 1);
		if (p3 == std::string::npos)
		{
			Reset();
			return false;
		}
		const size_t p4 = text.find('|', p3 + 1);
		if (p4 == std::string::npos)
		{
			Reset();
			return false;
		}
		if (text.find('|', p4 + 1) != std::string::npos)
		{
			Reset();
			return false;
		}

		const std::string versionText = text.substr(prefixLength, p2 - prefixLength);
		const std::string yearText = text.substr(p2 + 1, p3 - (p2 + 1));
		const std::string monthText = text.substr(p3 + 1, p4 - (p3 + 1));
		const std::string dayText = text.substr(p4 + 1);

		int version = 0;
		int parsedYear = 0;
		int parsedMonth = 0;
		int parsedDay = 0;

		if (!TryParseInt32(versionText, version)
			|| !TryParseInt32(yearText, parsedYear)
			|| !TryParseInt32(monthText, parsedMonth)
			|| !TryParseInt32(dayText, parsedDay))
		{
			Reset();
			return false;
		}

		if (version != static_cast<int>(kDateBinaryVersion))
		{
			Reset();
			return false;
		}

		if (parsedYear == 0 && parsedMonth == 0 && parsedDay == 0)
		{
			Reset();
			return true;
		}

		if (!Set(parsedYear, parsedMonth, parsedDay))
		{
			Reset();
			return false;
		}

		return true;
	}

	// 2) Fallback: accept ISO strings.
	GB_Date parsed;
	if (ParseIsoString(text, parsed))
	{
		*this = parsed;
		return true;
	}

	Reset();
	return false;
}

bool GB_Date::Deserialize(const GB_ByteBuffer& data)
{
	if (data.size() < 20)
	{
		Reset();
		return false;
	}

	size_t offset = 0;
	uint32_t magic = 0;
	uint32_t version = 0;
	int32_t parsedYear = 0;
	int32_t parsedMonth = 0;
	int32_t parsedDay = 0;

	if (!ReadUInt32LE(data, offset, magic)
		|| !ReadUInt32LE(data, offset, version)
		|| !ReadInt32LE(data, offset, parsedYear)
		|| !ReadInt32LE(data, offset, parsedMonth)
		|| !ReadInt32LE(data, offset, parsedDay))
	{
		Reset();
		return false;
	}

	if (magic != GB_ClassMagicNumber || version != kDateBinaryVersion)
	{
		Reset();
		return false;
	}

	if (parsedYear == 0 && parsedMonth == 0 && parsedDay == 0)
	{
		Reset();
		return true;
	}

	if (!Set(static_cast<int>(parsedYear), static_cast<int>(parsedMonth), static_cast<int>(parsedDay)))
	{
		Reset();
		return false;
	}

	return true;
}
