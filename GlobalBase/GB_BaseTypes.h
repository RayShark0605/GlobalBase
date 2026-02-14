#ifndef GLOBALBASE_BASE_TYPES_H_H
#define GLOBALBASE_BASE_TYPES_H_H

#include <vector>
#include <cstdint>
#include <string>

using GB_ByteBuffer = std::vector<unsigned char>;

inline std::string GB_ByteBufferToString(const GB_ByteBuffer& byteBuffer)
{
    if (byteBuffer.empty())
    {
        return std::string();
    }

    return std::string(reinterpret_cast<const char*>(byteBuffer.data()), byteBuffer.size());
}

inline std::string GB_ByteBufferToString(GB_ByteBuffer&& byteBuffer)
{
    if (byteBuffer.empty())
    {
        return std::string();
    }

    std::string result(reinterpret_cast<const char*>(byteBuffer.data()), byteBuffer.size());
    GB_ByteBuffer().swap(byteBuffer);
    return result;
}

inline GB_ByteBuffer GB_StringToByteBuffer(const std::string& byteString)
{
    if (byteString.empty())
    {
        return GB_ByteBuffer();
    }

    const unsigned char* beginPtr = reinterpret_cast<const unsigned char*>(byteString.data());
    const unsigned char* endPtr = beginPtr + byteString.size();

    GB_ByteBuffer result;
    result.assign(beginPtr, endPtr);
    return result;
}

inline GB_ByteBuffer GB_StringToByteBuffer(std::string&& byteString)
{
    if (byteString.empty())
    {
        return GB_ByteBuffer();
    }

    const unsigned char* beginPtr = reinterpret_cast<const unsigned char*>(byteString.data());
    const unsigned char* endPtr = beginPtr + byteString.size();

    GB_ByteBuffer result;
    result.assign(beginPtr, endPtr);

    std::string().swap(byteString);
    return result;
}

constexpr static uint32_t GB_ClassMagicNumber = 827540039;

#endif