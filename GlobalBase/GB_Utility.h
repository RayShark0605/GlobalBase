#ifndef GLOBALBASE_UTILITY_H_H
#define GLOBALBASE_UTILITY_H_H

#include <string>
#include <vector>
#include <iostream>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <queue>
#include <sstream>
#include <fstream>
#include <chrono>
#include <thread>

#include "GlobalBasePort.h"


// 获取当前控制台编码
GLOBALBASE_PORT void GetConsoleEncodingString(std::string& encodingString);
GLOBALBASE_PORT void GetConsoleEncodingCode(unsigned int& codePageId); // 未知编码则返回UINT_MAX

// 设置控制台编码
GLOBALBASE_PORT bool SetConsoleEncoding(unsigned int codePageId);

// 设置控制台编码为 UTF-8
GLOBALBASE_PORT bool SetConsoleEncodingToUtf8();





#endif // !GLOBALBASE_UTILITY_H_H