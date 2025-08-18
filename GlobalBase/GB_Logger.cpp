#include "GB_Logger.h"
#include "GB_Utf8String.h"
#include "GB_Config.h"

using namespace std;

GB_Logger& GB_Logger::GetInstance()
{
	static GB_Logger instance;
	return instance;
}

GB_Logger::GB_Logger()
{

}

GB_Logger::~GB_Logger()
{

}


bool IsLogEnabled()
{
	const static string targetKey = GB_STR("GB_EnableLog");

	if (!IsExistsGbConfig(targetKey))
	{
		return false;
	}

	string value;
	return (GetGbConfig(targetKey, value) && "1" == value);
}

bool SetLogEnabled(bool enable)
{
	const static string targetKey = GB_STR("GB_EnableLog");
	const string value = enable ? GB_STR("1") : GB_STR("0");
	return SetGbConfig(targetKey, value);
}

bool IsLogToConsole()
{
	const static string targetKey = GB_STR("GB_IsLogToConsole");

	if (!IsExistsGbConfig(targetKey))
	{
		return false;
	}

	string value;
	return (GetGbConfig(targetKey, value) && "1" == value);
}

bool SetLogToConsole(bool enable)
{
	const static string targetKey = GB_STR("GB_IsLogToConsole");
	const string value = enable ? GB_STR("1") : GB_STR("0");
	return SetGbConfig(targetKey, value);
}

GB_LogLevel GetLogFilterLevel()
{
	const static string targetKey = GB_STR("GB_LogLevel");

	if (!IsLogEnabled())
	{
		return GB_LogLevel::GBLOG_DISABLELOG;
	}

	if (!IsExistsGbConfig(targetKey))
	{
		return GB_LogLevel::GBLOG_TRACE;
	}
	string value;
	if (!GetGbConfig(targetKey, value))
	{
		return GB_LogLevel::GBLOG_TRACE;
	}
	if (value == GB_STR("TRACE") || value == GB_STR("0"))
	{
		return GB_LogLevel::GBLOG_TRACE;
	}
	else if (value == GB_STR("DEBUG") || value == GB_STR("1"))
	{
		return GB_LogLevel::GBLOG_DEBUG;
	}
	else if (value == GB_STR("INFO") || value == GB_STR("2"))
	{
		return GB_LogLevel::GBLOG_INFO;
	}
	else if (value == GB_STR("WARNING") || value == GB_STR("3"))
	{
		return GB_LogLevel::GBLOG_WARNING;
	}
	else if (value == GB_STR("ERROR") || value == GB_STR("4"))
	{
		return GB_LogLevel::GBLOG_ERROR;
	}
	else if (value == GB_STR("FATAL") || value == GB_STR("5"))
	{
		return GB_LogLevel::GBLOG_FATAL;
	}
	else if (value == GB_STR("DISABLELOG") || value == GB_STR("6"))
	{
		return GB_LogLevel::GBLOG_DISABLELOG;
	}
	return GB_LogLevel::GBLOG_TRACE;
}

bool CheckLogLevel(GB_LogLevel level)
{
	const GB_LogLevel filterLevel = GetLogFilterLevel();
	return (level >= filterLevel && filterLevel != GB_LogLevel::GBLOG_DISABLELOG);
}