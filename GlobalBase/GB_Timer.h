#ifndef GLOBALBASE_TIMER_H_H
#define GLOBALBASE_TIMER_H_H

#include "GlobalBasePort.h"
#include <string>

/**
 * @brief 获取当前系统“本地时间”，并格式化为 RFC 3339/ISO 8601 风格的字符串（UTF-8）。
 *
 * 生成形如 `YYYY-MM-DDTHH:MM:SS[.mmm][Z|±HH:MM]` 的时间文本：日期与时间以 'T' 分隔；
 * 可选输出 3 位毫秒；当启用时区后缀时，若本地时区为 UTC 则输出 'Z'，否则输出
 * 本地与 UTC 的偏移量 `±HH:MM`。返回值保证为 UTF-8 编码的 std::string（仅包含 ASCII）。
 *
 * @param withMs       是否输出毫秒（3 位，范围 000–999）。默认 true。
 * @param withTzSuffix 是否在末尾附加时区标志：
 *                     - true：输出 'Z'（UTC）或本地相对 UTC 的偏移量 `±HH:MM`；
 *                     - false：不附加任何时区信息。默认 false。
 *
 * @return 以 UTF-8 编码的本地时间字符串。
 *
 * @note 当 @p withTzSuffix 为 false 时，结果不再是“严格的 RFC 3339 时间戳”（RFC 3339 要求必须带 'Z' 或 `±HH:MM`），仅表示“本地时间的 ISO 8601 形式”。
 *
 * @par 示例
 * @code
 * std::string s1 = GetLocalTimeStr();                  // "2025-08-19T11:44:44.063"
 * std::string s2 = GetLocalTimeStr(true,  true);       // "2025-08-19T11:44:44.063+08:00" 或 "…Z"
 * std::string s3 = GetLocalTimeStr(false, true);       // "2025-08-19T11:44:44+08:00"
 * std::string s4 = GetLocalTimeStr(false, false);      // "2025-08-19T11:44:44"
 * @endcode
 *
 * @threadsafety 内部采用线程安全的本地/UTC 分解时间函数（Windows: localtime_s/gmtime_s；POSIX: localtime_r/gmtime_r），可在多线程环境下安全调用。
 */
GLOBALBASE_PORT std::string GetLocalTimeStr(bool withMs = true, bool withTzSuffix = false);










#endif