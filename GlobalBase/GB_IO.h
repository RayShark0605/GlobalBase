#ifndef GLOBALBASE_IO_H_H
#define GLOBALBASE_IO_H_H

#include "GlobalBasePort.h"
#include <string>

GLOBALBASE_PORT bool WriteUtf8ToFile(const std::string& filePathUtf8, const std::string& utf8Content, bool appendMode = true, bool addBomIfNewFile = false);










#endif