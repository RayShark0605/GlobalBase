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
	int year = 0;
	int month = 0;
	int day = 0;

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
	bool Deserialize(const std::string& data);
	bool Deserialize(const GB_ByteBuffer& data);
};

#endif
