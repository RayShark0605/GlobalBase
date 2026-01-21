#ifndef GLOBALBASE_IO_H_H
#define GLOBALBASE_IO_H_H

#include "GlobalBasePort.h"
#include <string>
#include <vector>

GLOBALBASE_PORT bool WriteUtf8ToFile(const std::string& filePathUtf8, const std::string& utf8Content, bool appendMode = true, bool addBomIfNewFile = false);

GLOBALBASE_PORT std::vector<unsigned char> ReadFileToBinary(const std::string& filePathUtf8);

GLOBALBASE_PORT bool WriteBinaryToFile(const std::vector<unsigned char>& data, const std::string& filePathUtf8);






#endif