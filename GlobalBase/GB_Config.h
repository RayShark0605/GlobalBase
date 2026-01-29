#ifndef GLOBALBASE_CONFIG_H_H
#define GLOBALBASE_CONFIG_H_H

#include "GlobalBasePort.h"
#include <string>
#include <unordered_map>
#include <vector>

// ----------------------------------------------- 默认配置项 ----------------------------------------------- //
// 获取默认配置项路径，默认为"计算机\HKEY_CURRENT_USER\Software\GlobalBase"
GLOBALBASE_PORT std::string GB_GetGbConfigPath();

// 在默认配置项中，检查配置是否存在
GLOBALBASE_PORT bool GB_IsExistsGbConfig(const std::string& keyUtf8);

// 在默认配置项中，读指定配置
GLOBALBASE_PORT bool GB_GetGbConfig(const std::string& keyUtf8, std::string& valueUtf8);

// 在默认配置项中，写指定配置
GLOBALBASE_PORT bool GB_SetGbConfig(const std::string& keyUtf8, const std::string& valueUtf8);

// 在默认配置项中，删除指定配置
GLOBALBASE_PORT bool GB_DeleteGbConfig(const std::string& keyUtf8);

// 在默认配置项中，枚举所有配置
GLOBALBASE_PORT std::unordered_map<std::string, std::string> GB_GetAllGbConfig();
// ----------------------------------------------- 默认配置项 ----------------------------------------------- //

// ----------------------------------------------- 通用配置项 ----------------------------------------------- //
// 检查指定配置路径是否存在
GLOBALBASE_PORT bool GB_IsExistsConfigPath(const std::string& configPathUtf8);

// 创建指定配置路径，可选：递归创建子路径
GLOBALBASE_PORT bool GB_CreateConfigPath(const std::string& configPathUtf8, bool recursive = false);

// 检查指定配置路径下，是否存在指定键值
GLOBALBASE_PORT bool GB_IsExistsConfigValue(const std::string& configPathUtf8, const std::string& keyNameUtf8);

// 检查指定配置路径下，是否存在指定子配置项
GLOBALBASE_PORT bool GB_IsExistsChildConfig(const std::string& configPathUtf8, const std::string& childConfigNameUtf8);

// 在指定配置路径下，新建子配置项
GLOBALBASE_PORT bool GB_AddChildConfig(const std::string& configPathUtf8, const std::string& childConfigNameUtf8);

// 在指定配置路径下，删除子配置项
GLOBALBASE_PORT bool GB_DeleteChildConfig(const std::string& configPathUtf8, const std::string& childConfigNameUtf8);

// 在指定配置路径下，重命名子配置项
GLOBALBASE_PORT bool GB_RenameChildConfig(const std::string& configPathUtf8, const std::string& childConfigNameUtf8, const std::string& newNameUtf8);

// 配置项数据类型
enum class GB_ConfigValueType
{
	GbConfigValueType_Unknown = 0,
	GbConfigValueType_String = 1,
	GbConfigValueType_Binary = 2,
	GbConfigValueType_DWord = 3,
	GbConfigValueType_QWord = 4,
	GbConfigValueType_MultiString = 5,
	GbConfigValueType_ExpandString = 6
};

// 配置项数据
struct GB_ConfigValue
{
	std::string nameUtf8 = "";
	GB_ConfigValueType valueType = GB_ConfigValueType::GbConfigValueType_Unknown;

	std::string valueUtf8 = "";							// 用于 GbConfigValueType_String 和 GbConfigValueType_ExpandString
	std::vector<std::string> multiStringValuesUtf8;		// 用于 GbConfigValueType_MultiString
	std::vector<uint8_t> binaryValue;					// 用于 GbConfigValueType_Binary
	uint32_t dwordValue = 0;							// 用于 GbConfigValueType_DWord
	uint64_t qwordValue = 0;							// 用于 GbConfigValueType_QWord
};

// 在指定配置路径下，获取指定键值的数据
GLOBALBASE_PORT bool GB_GetConfigValue(const std::string& configPathUtf8, const std::string& keyNameUtf8, GB_ConfigValue& configValue);

// 在指定配置路径下，设置指定键值的数据
GLOBALBASE_PORT bool GB_SetConfigValue(const std::string& configPathUtf8, const std::string& keyNameUtf8, const GB_ConfigValue& configValue);

// 在指定配置路径下，删除指定键值
GLOBALBASE_PORT bool GB_DeleteConfigValue(const std::string& configPathUtf8, const std::string& keyNameUtf8);

// 在指定配置路径下，删除所有键值
GLOBALBASE_PORT bool GB_ClearConfigValue(const std::string& configPathUtf8);

// 在指定配置路径下，重命名指定键值
GLOBALBASE_PORT bool GB_RenameConfigValue(const std::string& configPathUtf8, const std::string& keyNameUtf8, const std::string& newNameUtf8);

// 配置项（包含子项和键值）
struct GB_ConfigItem
{
	std::string nameUtf8 = "";
	std::vector<GB_ConfigItem> childenItems;
	std::vector<GB_ConfigValue> values;
};

// 获取指定路径下的配置项数据（可选非递归）
GLOBALBASE_PORT bool GB_GetConfigItem(const std::string& configPathUtf8, GB_ConfigItem& configItem, bool recursive = true);

// ----------------------------------------------- 通用配置项 ----------------------------------------------- //






#endif