#ifndef GLOBALBASE_CONFIG_H_H
#define GLOBALBASE_CONFIG_H_H

#include "GlobalBasePort.h"
#include <string>
#include <unordered_map>

// 默认为"计算机\HKEY_CURRENT_USER\Software\GlobalBase"
GLOBALBASE_PORT std::string GetGbConfigPath();

// 检查指定配置是否存在
GLOBALBASE_PORT bool IsExistsGbConfig(const std::string& keyUtf8);

// 读指定配置
GLOBALBASE_PORT bool GetGbConfig(const std::string& keyUtf8, std::string& valueUtf8);

// 写指定配置
GLOBALBASE_PORT bool SetGbConfig(const std::string& keyUtf8, const std::string& valueUtf8);

// 删除指定配置
GLOBALBASE_PORT bool DeleteGbConfig(const std::string& keyUtf8);

// 枚举所有配置
GLOBALBASE_PORT std::unordered_map<std::string, std::string> GetAllGbConfig();







#endif