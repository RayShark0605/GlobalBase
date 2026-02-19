#ifndef GLOBALBASE_DATETIME_H_H
#define GLOBALBASE_DATETIME_H_H

#include "GlobalBasePort.h"
#include "GB_BaseTypes.h"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

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

	// 获取本地/UTC 的“今天”日期；若系统时间获取失败则返回 Invalid。
	static GB_Date Today();
	static GB_Date UtcToday();

	bool operator==(const GB_Date& other) const;
	bool operator!=(const GB_Date& other) const;
	bool operator<(const GB_Date& other) const;
	bool operator<=(const GB_Date& other) const;
	bool operator>(const GB_Date& other) const;
	bool operator>=(const GB_Date& other) const;

	struct GB_DateHash
	{
		size_t operator()(const GB_Date& date) const noexcept;
	};

	// 序列化。
	std::string SerializeToString() const;
	GB_ByteBuffer SerializeToBinary() const;

	// 反序列化。
	// 注意：当返回 false 时，对象会被重置为 GB_Date::Invalid（0-0-0）。
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

	bool operator==(const GB_Time& other) const;
	bool operator!=(const GB_Time& other) const;
	bool operator<(const GB_Time& other) const;
	bool operator<=(const GB_Time& other) const;
	bool operator>(const GB_Time& other) const;
	bool operator>=(const GB_Time& other) const;

	struct GB_TimeHash
	{
		size_t operator()(const GB_Time& time) const noexcept;
	};

	// 序列化。
	std::string SerializeToString() const;
	GB_ByteBuffer SerializeToBinary() const;

	// 反序列化。
	// 注意：当返回 false 时，对象会被重置为 GB_Time::Invalid。
	bool Deserialize(const std::string& data);
	bool Deserialize(const GB_ByteBuffer& data);

private:
	// - 空时间：isNullTime=true，millisecondsSinceStartOfDay=0
	// - 无效时间：isNullTime=false，millisecondsSinceStartOfDay=-1
	// - 有效时间：isNullTime=false，millisecondsSinceStartOfDay in [0, 86400000)
	int millisecondsSinceStartOfDay = 0;
	bool isNullTime = true;
};

#endif
