#ifndef GLOBALBASE_FILESYSTEM_H_H
#define GLOBALBASE_FILESYSTEM_H_H

#include "GlobalBasePort.h"
#include <string>
#include <vector>

GLOBALBASE_PORT bool GB_IsFileExists(const std::string& filePathUtf8);

GLOBALBASE_PORT bool GB_IsDirectoryExists(const std::string& dirPathUtf8);

GLOBALBASE_PORT bool GB_CreateDirectory(const std::string& dirPathUtf8);

GLOBALBASE_PORT bool GB_IsEmptyDirectory(const std::string& dirPathUtf8);

GLOBALBASE_PORT bool GB_DeleteDirectory(const std::string& dirPathUtf8);

GLOBALBASE_PORT bool GB_DeleteFile(const std::string& filePathUtf8);

GLOBALBASE_PORT bool GB_CopyFile(const std::string& srcFilePathUtf8, const std::string& dstFilePathUtf8);

GLOBALBASE_PORT std::vector<std::string> GB_GetFilesList(const std::string& dirPathUtf8, bool recursive = false);

GLOBALBASE_PORT std::string GB_GetFileName(const std::string& filePathUtf8, bool withExt = false);

GLOBALBASE_PORT std::string GB_GetFileExt(const std::string& filePathUtf8);

GLOBALBASE_PORT std::string GB_GetDirectoryPath(const std::string& filePathUtf8);

GLOBALBASE_PORT size_t GB_GetFileSizeByte(const std::string& filePathUtf8);
GLOBALBASE_PORT double GB_GetFileSizeKB(const std::string& filePathUtf8);
GLOBALBASE_PORT double GB_GetFileSizeMB(const std::string& filePathUtf8);
GLOBALBASE_PORT double GB_GetFileSizeGB(const std::string& filePathUtf8);

#endif