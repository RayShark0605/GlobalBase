#ifndef GLOBALBASE_IO_H_H
#define GLOBALBASE_IO_H_H

#include "GlobalBasePort.h"
#include "GB_BaseTypes.h"
#include <string>

GLOBALBASE_PORT bool WriteUtf8ToFile(const std::string& filePathUtf8, const std::string& utf8Content, bool appendMode = true, bool addBomIfNewFile = false);

GLOBALBASE_PORT GB_ByteBuffer ReadFileToBinary(const std::string& filePathUtf8);

GLOBALBASE_PORT bool WriteBinaryToFile(const GB_ByteBuffer& data, const std::string& filePathUtf8);

class GLOBALBASE_PORT GB_ByteBufferIO
{
public:
	static void AppendUInt16LE(GB_ByteBuffer& buffer, uint16_t value);
	static void AppendUInt32LE(GB_ByteBuffer& buffer, uint32_t value);
	static void AppendUInt64LE(GB_ByteBuffer& buffer, uint64_t value);
	static void AppendDoubleLE(GB_ByteBuffer& buffer, double value);

	static bool ReadUInt16LE(const GB_ByteBuffer& buffer, size_t& offset, uint16_t& value);
	static bool ReadUInt32LE(const GB_ByteBuffer& buffer, size_t& offset, uint32_t& value);
	static bool ReadUInt64LE(const GB_ByteBuffer& buffer, size_t& offset, uint64_t& value);
	static bool ReadDoubleLE(const GB_ByteBuffer& buffer, size_t& offset, double& value);
};




#endif