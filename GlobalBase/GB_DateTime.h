#ifndef GLOBALBASE_DATETIME_H_H
#define GLOBALBASE_DATETIME_H_H

#include "GlobalBasePort.h"
#include "GB_BaseTypes.h"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>


// 注意：OffsetFromUtc / OffsetFromUtcMinutes 采用 RFC3339/ISO8601 语义：
// offset = local - UTC（即“加到 UTC 上得到本地时间”的分钟数）。
enum class GB_DateTimeSpec
{
	LocalTime = 0,
	UtcTime = 1,
	OffsetFromUtc = 2
};

class GLOBALBASE_PORT GB_Time;
class GLOBALBASE_PORT GB_DateTime;
class GLOBALBASE_PORT GB_TimeDuration;

class GLOBALBASE_PORT GB_Date
{
public:
	// 无效日期（0-0-0）。
	static const GB_Date Invalid;

	// 最小有效日期（1-1-1）。
	static const GB_Date MinValue;

	// 最大有效日期（9999-12-31）。
	static const GB_Date MaxValue;

	GB_Date();

	GB_Date(int year, int month, int day);

	// 是否为“空日期”（0-0-0）。
	bool IsNull() const;

	// 是否为有效日期（1..9999 年，合法月日，闰年规则为 Gregorian）。
	bool IsValid() const;

	// 设为无效（0-0-0）。
	void Reset();

	// 设置日期。成功返回 true；失败则置为无效并返回 false。
	bool Set(int year, int month, int day);

	// 闰年判定（Gregorian）。
	static bool IsLeapYear(int year);

	// 月份天数（year 仅在 2 月时影响结果）。非法 month 返回 0。
	static int GetDaysInMonth(int year, int month);

	// 本日期所在月份的天数；无效日期返回 0。
	int DaysInMonth() const;

	// 本日期所在年份的总天数（365/366）；无效日期返回 0。
	int DaysInYear() const;

	// 本日期在一年中的序号（1..365/366）；无效日期返回 0。
	int DayOfYear() const;

	// 星期几（ISO 8601：周一=1 ... 周日=7）；无效日期返回 0。
	int DayOfWeek() const;

	/**
	 * @brief 转换为“自 1970-01-01 起的天数偏移”（Unix epoch day）。
	 * @param outDays 输出：1970-01-01 为 0。
	 * @return 成功返回 true；若日期无效则返回 false。
	 */
	bool ToDaysSinceEpoch(int& outDays) const;

	// 由天数偏移构造日期（1970-01-01 为 0）。结果年份若超出 1..9999 则返回 Invalid。
	static GB_Date CreateFromDaysSinceEpoch(int daysSinceEpoch);

	/**
	 * @brief 转换为 Julian Day Number（JDN）。
	 *
	 * 约定：1970-01-01 的 JDN 为 2440588（与多种标准库实现一致）。
	 *
	 * @param outJulianDayNumber 输出 JDN。
	 * @return 成功返回 true；无效日期返回 false。
	 */
	bool ToJulianDayNumber(long long& outJulianDayNumber) const;

	// 由 JDN 构造日期。结果年份若超出 1..9999 则返回 Invalid。
	static GB_Date CreateFromJulianDayNumber(long long julianDayNumber);

	// 输出 ISO 8601 扩展日期格式：YYYY-MM-DD；无效日期返回空字符串。
	std::string ToIsoString() const;

	// 从 ISO 8601 日期字符串解析（支持 "YYYY-MM-DD" / "YYYYMMDD"，可带首尾空白）；失败返回 Invalid。
	static GB_Date CreateFromIsoString(const std::string& textUtf8);

	// 尝试解析 ISO 日期；成功写入 outDate 并返回 true。
	static bool ParseIsoString(const std::string& textUtf8, GB_Date& outDate);

	// 日期加减：返回新日期；若结果溢出有效范围或当前无效，则返回 Invalid。
	GB_Date AddDays(int days) const;
	GB_Date AddMonths(int months) const;
	GB_Date AddYears(int years) const;

	// 与 other 的天数差：other - this；若任一无效则返回 0。
	int DaysTo(const GB_Date& other) const;

	// 获取年月日；无效日期返回 0。
	int Year() const;
	int Month() const;
	int Day() const;

	// 获取本地/UTC 的“今天”日期；若系统时间获取失败则返回 Invalid。
	static GB_Date Today();
	static GB_Date UtcToday();

	/**
	 * @brief 将此日期与指定时间组合为 GB_DateTime。
	 * @param time 时间（必须有效）。
	 * @param spec 日期时间语义（本地/UTC/带偏移）。
	 * @param offsetFromUtcMinutes 当 spec==GB_DateTimeSpec::OffsetFromUtc 时有效，语义为 local - UTC（分钟）。
	 * @return 若日期或时间无效，则返回 GB_DateTime::Invalid。
	 */
	GB_DateTime ToDateTime(const GB_Time& time, GB_DateTimeSpec spec = GB_DateTimeSpec::LocalTime, int offsetFromUtcMinutes = 0) const;

	/**
	 * @brief 将此日期视作当天 00:00:00.000，转换为 GB_DateTime。
	 * @param spec 日期时间语义（本地/UTC/带偏移）。
	 * @param offsetFromUtcMinutes 当 spec==GB_DateTimeSpec::OffsetFromUtc 时有效，语义为 local - UTC（分钟）。
	 * @return 若日期无效，则返回 GB_DateTime::Invalid。
	 */
	GB_DateTime ToDateTime(GB_DateTimeSpec spec = GB_DateTimeSpec::LocalTime, int offsetFromUtcMinutes = 0) const;

	bool operator==(const GB_Date& other) const;
	bool operator!=(const GB_Date& other) const;
	bool operator<(const GB_Date& other) const;
	bool operator<=(const GB_Date& other) const;
	bool operator>(const GB_Date& other) const;
	bool operator>=(const GB_Date& other) const;

	GB_Date operator+(const GB_TimeDuration& duration) const;

	GB_Date operator-(const GB_TimeDuration& duration) const;

	struct GB_DateHash
	{
		size_t operator()(const GB_Date& date) const noexcept;
	};

	// 序列化。
	std::string SerializeToString() const;
	GB_ByteBuffer SerializeToBinary() const;

	// 反序列化。
	// 注意：当返回 false 时，对象会被重置为 GB_Date::Invalid（0-0-0）。
	// 若 data 识别为二进制格式（MagicNumber 匹配）但版本不支持，将直接返回 false（不会回退文本解析）。
	bool Deserialize(const std::string& data);
	bool Deserialize(const GB_ByteBuffer& data);

private:
	int year = 0;
	int month = 0;
	int day = 0;
};

class GLOBALBASE_PORT GB_Time
{
public:
	// “空时间”（默认构造得到的时间）：00:00:00.000，但 IsNull()=true 且 IsValid()=false。
	static const GB_Time Null;

	// 无效时间（例如 Set 失败后得到的状态）。IsNull()=false 且 IsValid()=false。
	static const GB_Time Invalid;

	// 最小有效时间（00:00:00.000）。
	static const GB_Time MinValue;

	// 最大有效时间（23:59:59.999）。
	static const GB_Time MaxValue;

	GB_Time();

	GB_Time(int hour, int minute, int second = 0, int millisecond = 0);

	// 是否为“空时间”（默认构造或 Reset 得到的状态）。
	bool IsNull() const;

	// 是否为有效时间（00:00:00.000 .. 23:59:59.999）。
	bool IsValid() const;

	// 设为空时间（IsNull()=true）。
	void Reset();

	// 设置时间。成功返回 true；失败则置为 Invalid 并返回 false。
	bool Set(int hour, int minute, int second = 0, int millisecond = 0);

	// 仅做合法性判定（不修改对象）。
	static bool IsValidTime(int hour, int minute, int second, int millisecond);

	int Hour() const;
	int Minute() const;
	int Second() const;
	int Millisecond() const;

	/**
	 * @brief 转换为“自当天 00:00:00.000 起的毫秒数”。
	 * @return 若有效则返回 [0, 86400000)；否则返回 -1。
	 */
	int ToMillisecondsSinceStartOfDay() const;

	// 由毫秒数构造时间（[0, 86400000)）。非法则返回 Invalid。
	static GB_Time CreateFromMillisecondsSinceStartOfDay(int millisecondsSinceStartOfDay);

	// 输出 ISO 8601 扩展时间格式：HH:MM:SS 或 HH:MM:SS.mmm；无效时间返回空字符串。
	std::string ToIsoString(bool includeMilliseconds = true) const;

	// 从 ISO 8601 时间字符串解析（支持 "HH:MM" / "HH:MM:SS" / "HH:MM:SS.mmm" / "HH:MM:SS,mmm"，可带首尾空白）；失败返回 Invalid。
	static GB_Time CreateFromIsoString(const std::string& textUtf8);

	// 尝试解析 ISO 时间；成功写入 outTime 并返回 true。
	static bool ParseIsoString(const std::string& textUtf8, GB_Time& outTime);

	// 加/减毫秒与秒：结果按 24 小时回绕；若当前时间无效，则返回 Invalid。
	GB_Time AddMSecs(int milliseconds) const;
	GB_Time AddSecs(int seconds) const;

	// 与 other 的毫秒/秒差：other - this；若任一无效则返回 0。
	int MsecsTo(const GB_Time& other) const;
	int SecsTo(const GB_Time& other) const;

	// 获取本地/UTC 的当前时间（精确到毫秒）；若系统时间获取失败则返回 Invalid。
	static GB_Time CurrentTime();
	static GB_Time UtcCurrentTime();

	/**
	 * @brief 将此时间与指定日期组合为 GB_DateTime。
	 * @param date 日期（必须有效）。
	 * @param spec 日期时间语义（本地/UTC/带偏移）。
	 * @param offsetFromUtcMinutes 当 spec==GB_DateTimeSpec::OffsetFromUtc 时有效，语义为 local - UTC（分钟）。
	 * @return 若日期或时间无效，则返回 GB_DateTime::Invalid。
	 */
	GB_DateTime ToDateTime(const GB_Date& date, GB_DateTimeSpec spec = GB_DateTimeSpec::LocalTime, int offsetFromUtcMinutes = 0) const;

	/**
	 * @brief 将此时间与“今天”（随 spec 变化）组合为 GB_DateTime。
	 * @param spec 日期时间语义（本地/UTC/带偏移）。
	 * @param offsetFromUtcMinutes 当 spec==GB_DateTimeSpec::OffsetFromUtc 时有效，语义为 local - UTC（分钟）。
	 * @return 若时间无效，或无法获取“今天”日期，则返回 GB_DateTime::Invalid。
	 */
	GB_DateTime ToDateTime(GB_DateTimeSpec spec = GB_DateTimeSpec::LocalTime, int offsetFromUtcMinutes = 0) const;


	bool operator==(const GB_Time& other) const;
	bool operator!=(const GB_Time& other) const;
	bool operator<(const GB_Time& other) const;
	bool operator<=(const GB_Time& other) const;
	bool operator>(const GB_Time& other) const;
	bool operator>=(const GB_Time& other) const;

	GB_Time operator+(const GB_TimeDuration& duration) const;

	GB_Time operator-(const GB_TimeDuration& duration) const;

	struct GB_TimeHash
	{
		size_t operator()(const GB_Time& time) const noexcept;
	};

	// 序列化。
	std::string SerializeToString() const;
	GB_ByteBuffer SerializeToBinary() const;

	// 反序列化。
	// 注意：当返回 false 时，对象会被重置为 GB_Time::Invalid。
	// 若 data 识别为二进制格式（MagicNumber 匹配）但版本不支持，将直接返回 false（不会回退文本解析）。
	bool Deserialize(const std::string& data);
	bool Deserialize(const GB_ByteBuffer& data);

private:
	// - 空时间：isNullTime=true，millisecondsSinceStartOfDay=0
	// - 无效时间：isNullTime=false，millisecondsSinceStartOfDay=-1
	// - 有效时间：isNullTime=false，millisecondsSinceStartOfDay in [0, 86400000)
	int millisecondsSinceStartOfDay = 0;
	bool isNullTime = true;
};

class GLOBALBASE_PORT GB_DateTime
{
public:
	static const GB_DateTime Invalid;

	GB_DateTime();
	GB_DateTime(const GB_Date& date, const GB_Time& time, GB_DateTimeSpec spec = GB_DateTimeSpec::LocalTime, int offsetFromUtcMinutes = 0);

	bool IsValid() const;
	void Reset();

	GB_DateTimeSpec Spec() const;

	// 若 Spec()==OffsetFromUtc：返回构造时的固定偏移；
	// 若 Spec()==UtcTime：返回 0；
	// 若 Spec()==LocalTime：返回该时间点在本地时区下的偏移（分钟）。若无法计算则返回 0。
	int OffsetFromUtcMinutes() const;

	// 返回与 Spec() 对应视角下的日期/时间。
	GB_Date Date() const;
	GB_Time Time() const;

	// Unix epoch 起的毫秒/秒（UTC 绝对时间）。
	long long ToUnixMilliseconds() const;
	long long ToUnixSeconds() const;

	static GB_DateTime CreateFromUnixMilliseconds(long long unixMilliseconds, GB_DateTimeSpec spec = GB_DateTimeSpec::UtcTime, int offsetFromUtcMinutes = 0);
	static GB_DateTime CreateFromUnixSeconds(long long unixSeconds, GB_DateTimeSpec spec = GB_DateTimeSpec::UtcTime, int offsetFromUtcMinutes = 0);

	// 当前时间。
	static GB_DateTime Now();
	static GB_DateTime UtcNow();

	// 从网络响应头 Date 获取当前 UTC 时间；失败返回 Invalid。
	// 注意：HTTP Date 通常仅精确到秒，因此返回值的毫秒部分为 0。
	static GB_DateTime GetUtcTimeFromNetwork();

	// 仅改变 Spec（同一时间点，不改变 unixMilliseconds）。
	GB_DateTime ToUtc() const;
	GB_DateTime ToLocal() const;
	GB_DateTime ToOffsetFromUtc(int offsetFromUtcMinutes) const;

	// ISO 8601 / RFC3339 风格：YYYY-MM-DDTHH:MM:SS[.mmm][Z|±HH:MM]
	// 注意：当 Spec()==LocalTime 且 includeTzSuffix==true 时，若系统无法可靠计算本地 UTC 偏移，则不会附加时区后缀。
	std::string ToIsoString(bool includeMilliseconds = true, bool includeTzSuffix = true) const;

	// 解析 ISO 8601 / RFC3339 风格时间。
	// 支持：
	// - "YYYY-MM-DDTHH:MM:SS" / "YYYY-MM-DD HH:MM:SS"（可选 .mmm 或 ,mmm）
	// - 可选时区后缀：Z / ±HH:MM / ±HHMM / ±HH
	// - 若缺失时区后缀，则按 defaultSpec 解释（默认 LocalTime）。
	static GB_DateTime CreateFromIsoString(const std::string& textUtf8, GB_DateTimeSpec defaultSpec = GB_DateTimeSpec::LocalTime);
	static bool ParseIsoString(const std::string& textUtf8, GB_DateTime& outDateTime, GB_DateTimeSpec defaultSpec = GB_DateTimeSpec::LocalTime);

	// 变换：AddMSecs/AddSecs 是“绝对时间点”加减；AddDays 使用“当前 Spec 视角下的日历加法”。
	GB_DateTime AddMSecs(long long milliseconds) const;
	GB_DateTime AddSecs(int seconds) const;
	GB_DateTime AddDays(int days) const;

	// 差值：other - this。
	int64_t MsecsTo(const GB_DateTime& other) const;
	double SecondsTo(const GB_DateTime& other) const;

	bool operator==(const GB_DateTime& other) const;
	bool operator!=(const GB_DateTime& other) const;
	bool operator<(const GB_DateTime& other) const;
	bool operator<=(const GB_DateTime& other) const;
	bool operator>(const GB_DateTime& other) const;
	bool operator>=(const GB_DateTime& other) const;

	GB_DateTime operator+(const GB_TimeDuration& duration) const;

	GB_DateTime operator-(const GB_TimeDuration& duration) const;

	// 两个时间点之差：this - other。
	// 返回固定时长分量（仅使用 hours/minutes/seconds，不包含 years/months/weeks/days）。
	// 若任一对象无效，则返回零时长。
	GB_TimeDuration operator-(const GB_DateTime& other) const;

	struct GB_DateTimeHash
	{
		size_t operator()(const GB_DateTime& dateTime) const noexcept;
	};

	// 序列化/反序列化。
	std::string SerializeToString() const;
	GB_ByteBuffer SerializeToBinary() const;

	// 注意：当返回 false 时，对象会被重置为 GB_DateTime::Invalid。
	// 若 data 识别为二进制格式（MagicNumber 匹配）但版本不支持，将直接返回 false（不会回退文本解析）。
	bool Deserialize(const std::string& data);
	bool Deserialize(const GB_ByteBuffer& data);

private:
	int64_t unixMilliseconds = 0;
	GB_DateTimeSpec spec = GB_DateTimeSpec::LocalTime;
	int offsetMinutes = 0; // 仅在 spec==OffsetFromUtc 时有效
	bool valid = false;
};

class GLOBALBASE_PORT GB_TimeDuration
{
public:
	int years = 0;
	int months = 0;
	int weeks = 0;
	int days = 0;
	int hours = 0;
	int minutes = 0;
	double seconds = 0;

	// 便捷构造（固定长度单位）。
	static GB_TimeDuration CreateFromSeconds(double seconds);
	static GB_TimeDuration CreateFromMinutes(long long minutes);
	static GB_TimeDuration CreateFromHours(long long hours);
	static GB_TimeDuration CreateFromDays(long long days);
	static GB_TimeDuration CreateFromWeeks(long long weeks);
	static GB_TimeDuration CreateFromMonths(long long months);
	static GB_TimeDuration CreateFromYears(long long years);

	// 是否包含“日历相关”的分量（年/月）。年/月无法精确换算为固定秒数。
	bool HasCalendarPart() const;

	// 取相反数。
	GB_TimeDuration Negated() const;
	GB_TimeDuration operator-() const;

	// 组合运算（逐分量相加/相减）。
	GB_TimeDuration operator+(const GB_TimeDuration& other) const;
	GB_TimeDuration operator-(const GB_TimeDuration& other) const;
	GB_TimeDuration& operator+=(const GB_TimeDuration& other);
	GB_TimeDuration& operator-=(const GB_TimeDuration& other);

	// 注意：对 years/months/weeks/days/hours/minutes 这些 int 分量做加减时，若发生溢出，
	// 将采用“饱和”方式截断到 int 的上下界，以避免 C++ 有符号整数溢出的未定义行为。

	// 近似总秒数（年=365天，月=30天，仅用于估算/显示，不建议用于严肃计时）。
	double ToTotalSecondsApprox() const;

	// 尝试精确换算为固定秒/毫秒（要求 years==0 且 months==0；且 seconds 可被精确表示）。
	bool TryToFixedSeconds(long long& outSeconds) const;
	bool TryToFixedMilliseconds(int64_t& outMilliseconds) const;

	static GB_TimeDuration CreateFromString(const std::string& textUtf8, bool& ok);

	bool IsNull() const;

	bool operator==(const GB_TimeDuration& other) const;

	bool operator!=(const GB_TimeDuration& other) const;

	std::string ToString() const;

	long long ToSeconds() const;

	GB_DateTime AddToDateTime(const GB_DateTime& dateTime) const;
};


#endif
