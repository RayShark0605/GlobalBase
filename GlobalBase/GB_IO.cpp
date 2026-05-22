#include "GB_IO.h"
#include "GB_FileSystem.h"
#include "GB_Utf8String.h"
#include "GB_Variant.h"
#include <fstream>
#include <limits>
#include <cstring>
#include <algorithm>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _WIN32
// 将 UTF-8 路径转为 UTF-16 以使用 *W API
static inline std::wstring Utf8ToUtf16(const std::string& s)
{
    if (s.empty())
    {
        return std::wstring();
    }

    if (s.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return std::wstring();
    }

    // 计算长度
    const int need = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(),
        static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0)
    {
        return std::wstring();
    }
    std::wstring w;
    w.resize(static_cast<size_t>(need));
    // 真正转换
    const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(),
        static_cast<int>(s.size()), &w[0], need);
    if (written <= 0)
    {
        return std::wstring();
    }
    return w;
}
#endif

namespace
{
    bool EnsureParentDirectoryBeforeSimpleWrite(const std::string& filePathUtf8)
    {
        const std::string directoryPathUtf8 = GB_GetDirectoryPath(filePathUtf8);
        if (directoryPathUtf8.empty())
        {
            return true;
        }

        return GB_CreateDirectory(directoryPathUtf8);
    }
}

bool GB_WriteUtf8ToFile(const std::string& filePathUtf8, const std::string& utf8Content, bool appendMode, bool addBomIfNewFile)
{
    if (filePathUtf8.empty())
    {
        return false;
    }

    const bool existedBefore = GB_IsFileExists(filePathUtf8);
    if (!EnsureParentDirectoryBeforeSimpleWrite(filePathUtf8))
    {
        return false;
    }

#ifdef _WIN32
    const std::wstring pathW = Utf8ToUtf16(filePathUtf8);
    if (pathW.empty())
    {
        return false;
    }

    const DWORD desiredAccess = GENERIC_WRITE;
    const DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD creationDisposition = appendMode ? OPEN_ALWAYS : CREATE_ALWAYS; // 追加：存在即开，不存在即建；非追加：直接截断重建
    HANDLE hFile = ::CreateFileW(pathW.c_str(), desiredAccess, shareMode, nullptr,
        creationDisposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    bool ok = true;

    // 追加模式且原文件已存在时，将文件指针移动到末尾
    if (appendMode && existedBefore)
    {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!::SetFilePointerEx(hFile, zero, nullptr, FILE_END)) // 64 位安全移动文件指针
        {
            ::CloseHandle(hFile);
            return false;
        }
    }

    // 新文件且需要 BOM 时写入 BOM（UTF-8 可选 BOM）
    if (!existedBefore && addBomIfNewFile)
    {
        const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        if (!::WriteFile(hFile, bom, 3, &written, nullptr) || written != 3)
        {
            ok = false;
        }
    }

    // 分块写入内容，避免 DWORD 写入长度上限
    if (ok && !utf8Content.empty())
    {
        const char* dataPtr = utf8Content.data();
        size_t remaining = utf8Content.size();
        const DWORD kChunk = 64 * 1024 * 1024; // 64 MiB/次

        while (remaining > 0)
        {
            const DWORD toWrite = static_cast<DWORD>(remaining > kChunk ? kChunk : remaining);
            DWORD written = 0;
            if (!::WriteFile(hFile, dataPtr, toWrite, &written, nullptr) || written != toWrite)
            {
                ok = false;
                break;
            }
            dataPtr += toWrite;
            remaining -= toWrite;
        }
    }

    const BOOL closed = ::CloseHandle(hFile);
    return ok && closed != FALSE;

#else
    // POSIX/Linux：路径本质是字节序列；系统默认 UTF-8，本实现直接以二进制方式写入
    std::ios_base::openmode mode = std::ios::binary | std::ios::out;
    if (appendMode)
    {
        mode |= std::ios::app | std::ios::ate;
    }
    else
    {
        mode |= std::ios::trunc;
    }

    std::ofstream ofs(filePathUtf8.c_str(), mode);
    if (!ofs.is_open())
    {
        return false;
    }

    if (!existedBefore && addBomIfNewFile)
    {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        ofs.write(reinterpret_cast<const char*>(bom), 3);
        if (!ofs)
        {
            ofs.close();
            return false;
        }
    }

    if (!utf8Content.empty())
    {
        const char* dataPtr = utf8Content.data();
        std::size_t remainingBytes = utf8Content.size();
        const std::size_t chunkBytes = 64u * 1024u * 1024u;

        while (remainingBytes > 0)
        {
            const std::size_t currentWriteBytes = remainingBytes > chunkBytes ? chunkBytes : remainingBytes;
            ofs.write(dataPtr, static_cast<std::streamsize>(currentWriteBytes));
            if (!ofs)
            {
                ofs.close();
                return false;
            }

            dataPtr += currentWriteBytes;
            remainingBytes -= currentWriteBytes;
        }
    }

    ofs.flush();
    const bool ok = static_cast<bool>(ofs);
    ofs.close();
    return ok;
#endif
}

namespace
{
    enum class GB_TextEncodingByBom
    {
        Unknown = 0,
        Utf8,
        Utf16Le,
        Utf16Be,
        Utf32Le,
        Utf32Be
    };

    GB_TextEncodingByBom DetectTextEncodingByBom(const std::string& fileData)
    {
        if (fileData.size() >= 4)
        {
            const unsigned char byte0 = static_cast<unsigned char>(fileData[0]);
            const unsigned char byte1 = static_cast<unsigned char>(fileData[1]);
            const unsigned char byte2 = static_cast<unsigned char>(fileData[2]);
            const unsigned char byte3 = static_cast<unsigned char>(fileData[3]);

            if (byte0 == 0xFF && byte1 == 0xFE && byte2 == 0x00 && byte3 == 0x00)
            {
                return GB_TextEncodingByBom::Utf32Le;
            }

            if (byte0 == 0x00 && byte1 == 0x00 && byte2 == 0xFE && byte3 == 0xFF)
            {
                return GB_TextEncodingByBom::Utf32Be;
            }
        }

        if (fileData.size() >= 3)
        {
            const unsigned char byte0 = static_cast<unsigned char>(fileData[0]);
            const unsigned char byte1 = static_cast<unsigned char>(fileData[1]);
            const unsigned char byte2 = static_cast<unsigned char>(fileData[2]);

            if (byte0 == 0xEF && byte1 == 0xBB && byte2 == 0xBF)
            {
                return GB_TextEncodingByBom::Utf8;
            }
        }

        if (fileData.size() >= 2)
        {
            const unsigned char byte0 = static_cast<unsigned char>(fileData[0]);
            const unsigned char byte1 = static_cast<unsigned char>(fileData[1]);

            if (byte0 == 0xFF && byte1 == 0xFE)
            {
                return GB_TextEncodingByBom::Utf16Le;
            }

            if (byte0 == 0xFE && byte1 == 0xFF)
            {
                return GB_TextEncodingByBom::Utf16Be;
            }
        }

        return GB_TextEncodingByBom::Unknown;
    }
}

std::string GB_ReadFromFile(const std::string& filePathUtf8)
{
    const GB_ByteBuffer fileBytes = GB_ReadBinaryFromFile(filePathUtf8);
    if (fileBytes.empty())
    {
        return std::string();
    }

    if (fileBytes.size() > std::string().max_size())
    {
        return std::string();
    }

    return std::string(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
}

std::string GB_ReadUtf8FromFile(const std::string& filePathUtf8, const std::string& fileEncodingName)
{
    const std::string fileData = GB_ReadFromFile(filePathUtf8);
    if (fileData.empty())
    {
        return std::string();
    }

    try
    {
        const GB_TextEncodingByBom bomEncoding = DetectTextEncodingByBom(fileData);
        if (bomEncoding == GB_TextEncodingByBom::Utf8)
        {
            return GB_BytesToUtf8(fileData, "utf-8-sig");
        }
        if (bomEncoding == GB_TextEncodingByBom::Utf16Le)
        {
            return GB_BytesToUtf8(fileData, "utf-16le");
        }
        if (bomEncoding == GB_TextEncodingByBom::Utf16Be)
        {
            return GB_BytesToUtf8(fileData, "utf-16be");
        }
        if (bomEncoding == GB_TextEncodingByBom::Utf32Le)
        {
            return GB_BytesToUtf8(fileData, "utf-32le");
        }
        if (bomEncoding == GB_TextEncodingByBom::Utf32Be)
        {
            return GB_BytesToUtf8(fileData, "utf-32be");
        }

        const std::string sourceEncodingName = fileEncodingName.empty() ? std::string("utf-8") : fileEncodingName;
        return GB_BytesToUtf8(fileData, sourceEncodingName);
    }
    catch (...)
    {
        return fileData;
    }
}

std::vector<unsigned char> GB_ReadBinaryFromFile(const std::string& filePathUtf8)
{
    if (filePathUtf8.empty())
    {
        return {};
    }

#ifdef _WIN32
    const std::wstring pathW = Utf8ToUtf16(filePathUtf8);
    if (pathW.empty())
    {
        return {};
    }

    const DWORD desiredAccess = GENERIC_READ;
    const DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE hFile = ::CreateFileW(pathW.c_str(), desiredAccess, shareMode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return {};
    }

    LARGE_INTEGER fileSizeLi;
    if (!::GetFileSizeEx(hFile, &fileSizeLi) || fileSizeLi.QuadPart < 0)
    {
        ::CloseHandle(hFile);
        return {};
    }

    const uint64_t fileSize64 = static_cast<uint64_t>(fileSizeLi.QuadPart);
    if (fileSize64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    {
        ::CloseHandle(hFile);
        return {};
    }

    GB_ByteBuffer buffer(static_cast<size_t>(fileSize64));

    unsigned char* writePtr = buffer.data();
    size_t remaining = buffer.size();
    const DWORD kChunkBytes = 64u * 1024u * 1024u;

    while (remaining > 0)
    {
        const DWORD toRead = static_cast<DWORD>(remaining > kChunkBytes ? kChunkBytes : remaining);
        DWORD readBytes = 0;
        if (!::ReadFile(hFile, writePtr, toRead, &readBytes, nullptr))
        {
            ::CloseHandle(hFile);
            return {};
        }
        if (readBytes != toRead)
        {
            // 文件可能在读取过程中被截断/变更，或者发生了异常 EOF
            ::CloseHandle(hFile);
            return {};
        }

        writePtr += readBytes;
        remaining -= readBytes;
    }

    ::CloseHandle(hFile);
    return buffer;

#else
    int fd = ::open(
        filePathUtf8.c_str(),
        O_RDONLY
#  ifdef O_CLOEXEC
        | O_CLOEXEC
#  endif
    );
    if (fd < 0)
    {
        return {};
    }

    struct stat st;
    if (::fstat(fd, &st) != 0)
    {
        ::close(fd);
        return {};
    }
    if (!S_ISREG(st.st_mode))
    {
        ::close(fd);
        return {};
    }

    // off_t 是有符号类型，对于普通文件不应为负，此处做防御性检查
    if (st.st_size < 0)
    {
        ::close(fd);
        return {};
    }

    const uint64_t fileSize64 = static_cast<uint64_t>(st.st_size);
    if (fileSize64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    {
        ::close(fd);
        return {};
    }

    std::vector<unsigned char> buffer;
    buffer.resize(static_cast<size_t>(fileSize64));

    size_t totalRead = 0;
    const size_t kChunkBytes = 64u * 1024u * 1024u;

    while (totalRead < buffer.size())
    {
        const size_t remaining = buffer.size() - totalRead;
        const size_t toRead = remaining > kChunkBytes ? kChunkBytes : remaining;

        const ssize_t n = ::read(fd, buffer.data() + totalRead, toRead);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            ::close(fd);
            return {};
        }
        if (n == 0)
        {
            // 异常 EOF：文件在读取过程中被截断/变更
            ::close(fd);
            return {};
        }
        totalRead += static_cast<size_t>(n);
    }

    ::close(fd);
    return buffer;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// 内部辅助：将原始字节块写入文件（平台专属实现）。
// 两个公开重载共用此函数，从而避免 string 重载中的多余数据拷贝。
// ─────────────────────────────────────────────────────────────────────────────
static bool WriteBinaryToFileImpl(const void* rawData, size_t byteSize, const std::string& filePathUtf8)
{
    if (filePathUtf8.empty())
    {
        return false;
    }

    if (rawData == nullptr && byteSize > 0)
    {
        return false;
    }

    // 防止把目录路径当成文件路径
    {
        const char lastChar = filePathUtf8.back();
        if (lastChar == '/' || lastChar == '\\')
        {
            return false;
        }
    }

    // 1) 确保父目录存在
    {
        const std::string dirPathUtf8 = GB_GetDirectoryPath(filePathUtf8);
        if (!dirPathUtf8.empty())
        {
            if (!GB_CreateDirectory(dirPathUtf8))
            {
                return false;
            }
        }
    }

#ifdef _WIN32
    const std::wstring filePathUtf16 = Utf8ToUtf16(filePathUtf8);
    if (filePathUtf16.empty())
    {
        return false;
    }

    // shareMode=0 会导致文件无法再被打开直到句柄关闭，更容易被外部因素影响
    const DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    HANDLE fileHandle = ::CreateFileW(
        filePathUtf16.c_str(),
        GENERIC_WRITE,
        shareMode,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, // 顺序访问 hint
        nullptr);

    if (fileHandle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    bool ok = true;

    // 2) 写入（支持 partial write）。
    //    这里不预先扩展文件长度，避免写入中途失败时留下声明长度大于有效数据长度的文件。
    if (byteSize > 0)
    {
        size_t totalWritten = 0;
        const DWORD chunkBytes = 64u * 1024u * 1024u;

        while (totalWritten < byteSize)
        {
            const size_t remainingBytes = byteSize - totalWritten;
            const DWORD toWrite = static_cast<DWORD>(remainingBytes > chunkBytes ? chunkBytes : remainingBytes);

            DWORD writtenBytes = 0;
            const BOOL writeOk = ::WriteFile(
                fileHandle,
                static_cast<const unsigned char*>(rawData) + totalWritten,
                toWrite,
                &writtenBytes,
                nullptr);

            if (writeOk == FALSE)
            {
                ok = false;
                break;
            }

            if (writtenBytes == 0)
            {
                ok = false;
                break;
            }

            totalWritten += static_cast<size_t>(writtenBytes);
        }
    }

    const BOOL closeOk = ::CloseHandle(fileHandle);
    return ok && closeOk != FALSE;

#else
    int openFlags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    openFlags |= O_CLOEXEC;
#endif

    const int fileDescriptor = ::open(filePathUtf8.c_str(), openFlags, 0644);
    if (fileDescriptor < 0)
    {
        return false;
    }

#ifdef POSIX_FADV_SEQUENTIAL
    (void)::posix_fadvise(fileDescriptor, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    bool ok = true;

    // write() 允许 partial write：成功也可能少写
    size_t totalWritten = 0;
    const size_t chunkBytes = 64u * 1024u * 1024u;

    while (totalWritten < byteSize)
    {
        const size_t remainingBytes = byteSize - totalWritten;
        const size_t toWrite = remainingBytes > chunkBytes ? chunkBytes : remainingBytes;

        const ssize_t writtenBytes = ::write(fileDescriptor,
            static_cast<const unsigned char*>(rawData) + totalWritten, toWrite);
        if (writtenBytes < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            ok = false;
            break;
        }

        if (writtenBytes == 0)
        {
            ok = false;
            break;
        }

        totalWritten += static_cast<size_t>(writtenBytes);
    }

    // POSIX：close() 若因信号中断返回 EINTR，fd 状态 unspecified，不应盲目重试
    if (::close(fileDescriptor) != 0)
    {
        ok = false;
    }

    return ok;
#endif
}

bool GB_WriteBinaryToFile(const GB_ByteBuffer& data, const std::string& filePathUtf8)
{
    return WriteBinaryToFileImpl(data.data(), data.size(), filePathUtf8);
}

bool GB_WriteBinaryToFile(const std::string& data, const std::string& filePathUtf8)
{
    // 直接将 string 的原始内存传入底层函数，避免额外的内存分配和数据拷贝
    return WriteBinaryToFileImpl(data.data(), data.size(), filePathUtf8);
}

void GB_ByteBufferIO::AppendUInt16LE(GB_ByteBuffer& buffer, uint16_t value)
{
    buffer.push_back(static_cast<unsigned char>((value >> 0) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}

void GB_ByteBufferIO::AppendUInt32LE(GB_ByteBuffer& buffer, uint32_t value)
{
    buffer.push_back(static_cast<unsigned char>((value >> 0) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
}

void GB_ByteBufferIO::AppendUInt64LE(GB_ByteBuffer& buffer, uint64_t value)
{
    buffer.push_back(static_cast<unsigned char>((value >> 0) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 32) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 40) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 48) & 0xFF));
    buffer.push_back(static_cast<unsigned char>((value >> 56) & 0xFF));
}

void GB_ByteBufferIO::AppendDoubleLE(GB_ByteBuffer& buffer, double value)
{
    uint64_t bits = 0;
    static_assert(sizeof(double) == sizeof(uint64_t), "Unexpected double size.");
    std::memcpy(&bits, &value, sizeof(bits));
    AppendUInt64LE(buffer, bits);
}

bool GB_ByteBufferIO::ReadUInt16LE(const GB_ByteBuffer& buffer, size_t& offset, uint16_t& value)
{
    if (offset > buffer.size() || buffer.size() - offset < 2)
    {
        return false;
    }

    value = static_cast<uint16_t>(buffer[offset]) | (static_cast<uint16_t>(buffer[offset + 1]) << 8);

    offset += 2;
    return true;
}

bool GB_ByteBufferIO::ReadUInt32LE(const GB_ByteBuffer& buffer, size_t& offset, uint32_t& value)
{
    if (offset > buffer.size() || buffer.size() - offset < 4)
    {
        return false;
    }

    value = static_cast<uint32_t>(buffer[offset])
        | (static_cast<uint32_t>(buffer[offset + 1]) << 8)
        | (static_cast<uint32_t>(buffer[offset + 2]) << 16)
        | (static_cast<uint32_t>(buffer[offset + 3]) << 24);

    offset += 4;
    return true;
}

bool GB_ByteBufferIO::ReadUInt64LE(const GB_ByteBuffer& buffer, size_t& offset, uint64_t& value)
{
    if (offset > buffer.size() || buffer.size() - offset < 8)
    {
        return false;
    }

    value = static_cast<uint64_t>(buffer[offset])
        | (static_cast<uint64_t>(buffer[offset + 1]) << 8)
        | (static_cast<uint64_t>(buffer[offset + 2]) << 16)
        | (static_cast<uint64_t>(buffer[offset + 3]) << 24)
        | (static_cast<uint64_t>(buffer[offset + 4]) << 32)
        | (static_cast<uint64_t>(buffer[offset + 5]) << 40)
        | (static_cast<uint64_t>(buffer[offset + 6]) << 48)
        | (static_cast<uint64_t>(buffer[offset + 7]) << 56);

    offset += 8;
    return true;
}

bool GB_ByteBufferIO::ReadDoubleLE(const GB_ByteBuffer& buffer, size_t& offset, double& value)
{
    uint64_t bits = 0;
    if (!ReadUInt64LE(buffer, offset, bits))
    {
        return false;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

namespace
{
    bool IsReadableAccess(GB_StreamAccessMode accessMode)
    {
        return accessMode == GB_StreamAccessMode::ReadOnly || accessMode == GB_StreamAccessMode::ReadWrite;
    }

    bool IsWritableAccess(GB_StreamAccessMode accessMode)
    {
        return accessMode == GB_StreamAccessMode::WriteOnly || accessMode == GB_StreamAccessMode::ReadWrite;
    }

    bool HasShareMode(GB_FileShareMode shareMode, GB_FileShareMode testMode)
    {
        return (static_cast<unsigned int>(shareMode) & static_cast<unsigned int>(testMode)) != 0;
    }

    bool IsPathLikeDirectory(const std::string& pathUtf8)
    {
        if (pathUtf8.empty())
        {
            return true;
        }

        const char lastChar = pathUtf8.back();
        return lastChar == '/' || lastChar == '\\';
    }

    bool EnsureParentDirectoryForFile(const std::string& filePathUtf8)
    {
        const std::string directoryPathUtf8 = GB_GetDirectoryPath(filePathUtf8);
        if (directoryPathUtf8.empty())
        {
            return true;
        }

        return GB_CreateDirectory(directoryPathUtf8);
    }

    bool WriteVarUInt64ToStream(GB_Stream& stream, std::uint64_t value)
    {
        unsigned char data[10];
        std::size_t byteCount = 0;

        do
        {
            unsigned char currentByte = static_cast<unsigned char>(value & 0x7F);
            value >>= 7;
            if (value != 0)
            {
                currentByte = static_cast<unsigned char>(currentByte | 0x80);
            }

            data[byteCount] = currentByte;
            byteCount++;
        } while (value != 0 && byteCount < sizeof(data));

        return stream.WriteBytes(data, byteCount);
    }

    bool ReadVarUInt64FromStream(GB_Stream& stream, std::uint64_t& outValue)
    {
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < 10; index++)
        {
            unsigned char currentByte = 0;
            if (!stream.ReadExactBytes(&currentByte, 1))
            {
                return false;
            }

            const unsigned char dataBits = static_cast<unsigned char>(currentByte & 0x7F);
            if (index == 9 && dataBits > 1)
            {
                return false;
            }

            value |= static_cast<std::uint64_t>(dataBits) << (index * 7);
            if ((currentByte & 0x80) == 0)
            {
                outValue = value;
                return true;
            }
        }

        return false;
    }

    template<typename TValue>
    bool WriteIntegralValueToStream(GB_Stream& stream, TValue value)
    {
        unsigned char data[sizeof(TValue)];
        const unsigned long long unsignedValue = static_cast<unsigned long long>(value);
        for (std::size_t index = 0; index < sizeof(TValue); index++)
        {
            data[index] = static_cast<unsigned char>((unsignedValue >> (index * 8)) & 0xFF);
        }

        return stream.WriteBytes(data, sizeof(data));
    }

    template<typename TValue>
    bool ReadIntegralValueFromStream(GB_Stream& stream, TValue& outValue)
    {
        unsigned char data[sizeof(TValue)];
        if (!stream.ReadExactBytes(data, sizeof(data)))
        {
            return false;
        }

        unsigned long long unsignedValue = 0;
        for (std::size_t index = 0; index < sizeof(TValue); index++)
        {
            unsignedValue |= static_cast<unsigned long long>(data[index]) << (index * 8);
        }

        outValue = static_cast<TValue>(unsignedValue);
        return true;
    }

    template<typename TValue>
    bool WriteObjectBytesToStream(GB_Stream& stream, const TValue& value)
    {
        return stream.WriteBytes(&value, sizeof(TValue));
    }

    template<typename TValue>
    bool ReadObjectBytesFromStream(GB_Stream& stream, TValue& outValue)
    {
        return stream.ReadExactBytes(&outValue, sizeof(TValue));
    }

    enum class GB_StreamVariantEncodingTag : unsigned char
    {
        Empty = 0,
        Bool = 1,
        Char = 2,
        SignedChar = 3,
        UnsignedChar = 4,
        Short = 5,
        UnsignedShort = 6,
        Int = 7,
        UnsignedInt = 8,
        Long = 9,
        UnsignedLong = 10,
        LongLong = 11,
        UnsignedLongLong = 12,
        Float = 13,
        Double = 14,
        LongDouble = 15,
        String = 16,
        Binary = 17,
        SerializedVariant = 18
    };

    bool WriteVariantTagToStream(GB_Stream& stream, GB_StreamVariantEncodingTag tag)
    {
        const unsigned char tagByte = static_cast<unsigned char>(tag);
        return stream.WriteBytes(&tagByte, 1);
    }

    bool ReadVariantTagFromStream(GB_Stream& stream, GB_StreamVariantEncodingTag& outTag)
    {
        unsigned char tagByte = 0;
        if (!stream.ReadExactBytes(&tagByte, 1))
        {
            return false;
        }

        if (tagByte > static_cast<unsigned char>(GB_StreamVariantEncodingTag::SerializedVariant))
        {
            return false;
        }

        outTag = static_cast<GB_StreamVariantEncodingTag>(tagByte);
        return true;
    }

    bool WriteBoolValueToStream(GB_Stream& stream, bool value)
    {
        const unsigned char data = value ? 1 : 0;
        return stream.WriteBytes(&data, 1);
    }

    bool ReadBoolValueFromStream(GB_Stream& stream, bool& outValue)
    {
        unsigned char data = 0;
        if (!stream.ReadExactBytes(&data, 1))
        {
            return false;
        }

        if (data > 1)
        {
            return false;
        }

        outValue = data != 0;
        return true;
    }

    bool WriteFloatValueToStream(GB_Stream& stream, float value)
    {
        static_assert(sizeof(float) == sizeof(std::uint32_t), "Unexpected float size.");
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return WriteIntegralValueToStream(stream, bits);
    }

    bool ReadFloatValueFromStream(GB_Stream& stream, float& outValue)
    {
        static_assert(sizeof(float) == sizeof(std::uint32_t), "Unexpected float size.");
        std::uint32_t bits = 0;
        if (!ReadIntegralValueFromStream(stream, bits))
        {
            return false;
        }

        std::memcpy(&outValue, &bits, sizeof(outValue));
        return true;
    }

    bool WriteDoubleValueToStream(GB_Stream& stream, double value)
    {
        static_assert(sizeof(double) == sizeof(std::uint64_t), "Unexpected double size.");
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return WriteIntegralValueToStream(stream, bits);
    }

    bool ReadDoubleValueFromStream(GB_Stream& stream, double& outValue)
    {
        static_assert(sizeof(double) == sizeof(std::uint64_t), "Unexpected double size.");
        std::uint64_t bits = 0;
        if (!ReadIntegralValueFromStream(stream, bits))
        {
            return false;
        }

        std::memcpy(&outValue, &bits, sizeof(outValue));
        return true;
    }

    bool WriteSerializedVariantToStream(GB_Stream& stream, const GB_Variant& value)
    {
        GB_ByteBuffer data;
        if (!value.Serialize(data))
        {
            return false;
        }

        if (!WriteVariantTagToStream(stream, GB_StreamVariantEncodingTag::SerializedVariant))
        {
            return false;
        }

        if (!WriteVarUInt64ToStream(stream, static_cast<std::uint64_t>(data.size())))
        {
            return false;
        }

        return stream.WriteBytes(data);
    }

    bool CanStoreInSizeT(std::uint64_t value)
    {
        return value <= static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)());
    }

    bool CalculateMemorySeekPosition(std::uint64_t currentPosition, std::uint64_t length, std::int64_t offset, GB_StreamSeekOrigin origin, std::uint64_t& outPosition)
    {
        std::uint64_t basePosition = 0;
        if (origin == GB_StreamSeekOrigin::Begin)
        {
            basePosition = 0;
        }
        else if (origin == GB_StreamSeekOrigin::Current)
        {
            basePosition = currentPosition;
        }
        else if (origin == GB_StreamSeekOrigin::End)
        {
            basePosition = length;
        }
        else
        {
            return false;
        }

        if (offset >= 0)
        {
            const std::uint64_t positiveOffset = static_cast<std::uint64_t>(offset);
            if (basePosition > (std::numeric_limits<std::uint64_t>::max)() - positiveOffset)
            {
                return false;
            }

            outPosition = basePosition + positiveOffset;
            return true;
        }

        const std::uint64_t negativeOffset = static_cast<std::uint64_t>(-(offset + 1)) + 1;
        if (basePosition < negativeOffset)
        {
            return false;
        }

        outPosition = basePosition - negativeOffset;
        return true;
    }
}

GB_Stream::~GB_Stream()
{
}

std::uint64_t GB_Stream::Tell() const
{
    std::uint64_t position = 0;
    if (!Tell(position))
    {
        return (std::numeric_limits<std::uint64_t>::max)();
    }

    return position;
}

std::uint64_t GB_Stream::Length() const
{
    std::uint64_t length = 0;
    if (!Length(length))
    {
        return (std::numeric_limits<std::uint64_t>::max)();
    }

    return length;
}

bool GB_Stream::IsEnd() const
{
    std::uint64_t position = 0;
    std::uint64_t length = 0;
    if (!Tell(position) || !Length(length))
    {
        return true;
    }

    return position >= length;
}

bool GB_Stream::ReadExactBytes(void* outData, std::size_t byteSize)
{
    if (byteSize == 0)
    {
        return true;
    }

    if (outData == nullptr)
    {
        return false;
    }

    unsigned char* outputBytes = static_cast<unsigned char*>(outData);
    std::size_t totalReadBytes = 0;
    while (totalReadBytes < byteSize)
    {
        std::size_t currentReadBytes = 0;
        if (!ReadBytes(outputBytes + totalReadBytes, byteSize - totalReadBytes, currentReadBytes))
        {
            return false;
        }

        if (currentReadBytes == 0)
        {
            return false;
        }

        totalReadBytes += currentReadBytes;
    }

    return true;
}

bool GB_Stream::ReadBytes(std::size_t byteSize, GB_ByteBuffer& outData)
{
    outData.clear();
    if (byteSize == 0)
    {
        return true;
    }

    try
    {
        outData.resize(byteSize);
    }
    catch (...)
    {
        outData.clear();
        return false;
    }

    std::size_t readBytes = 0;
    if (!ReadBytes(outData.data(), byteSize, readBytes))
    {
        outData.clear();
        return false;
    }

    try
    {
        outData.resize(readBytes);
    }
    catch (...)
    {
        outData.clear();
        return false;
    }

    return true;
}

bool GB_Stream::ReadToEnd(GB_ByteBuffer& outData, std::uint64_t maxBytes)
{
    outData.clear();

    std::uint64_t position = 0;
    std::uint64_t length = 0;
    if (!Tell(position) || !Length(length))
    {
        return false;
    }

    if (position >= length)
    {
        return true;
    }

    const std::uint64_t remainBytes = length - position;
    if (maxBytes > 0 && remainBytes > maxBytes)
    {
        return false;
    }

    if (!CanStoreInSizeT(remainBytes))
    {
        return false;
    }

    try
    {
        outData.resize(static_cast<std::size_t>(remainBytes));
    }
    catch (...)
    {
        outData.clear();
        return false;
    }

    if (!ReadExactBytes(outData.data(), outData.size()))
    {
        outData.clear();
        return false;
    }

    return true;
}

bool GB_Stream::WriteBytes(const GB_ByteBuffer& data)
{
    if (data.empty())
    {
        return true;
    }

    return WriteBytes(data.data(), data.size());
}

bool GB_Stream::WriteBytes(const std::string& data)
{
    if (data.empty())
    {
        return true;
    }

    return WriteBytes(data.data(), data.size());
}

bool GB_Stream::WriteByteBuffer(const GB_ByteBuffer& data)
{
    if (!WriteVarUInt64ToStream(*this, static_cast<std::uint64_t>(data.size())))
    {
        return false;
    }

    return WriteBytes(data);
}

bool GB_Stream::ReadByteBuffer(GB_ByteBuffer& outData, std::uint64_t maxBytes)
{
    outData.clear();

    std::uint64_t byteSize = 0;
    if (!ReadVarUInt64FromStream(*this, byteSize))
    {
        return false;
    }

    if (maxBytes > 0 && byteSize > maxBytes)
    {
        return false;
    }

    if (!CanStoreInSizeT(byteSize))
    {
        return false;
    }

    try
    {
        outData.resize(static_cast<std::size_t>(byteSize));
    }
    catch (...)
    {
        outData.clear();
        return false;
    }

    if (outData.empty())
    {
        return true;
    }

    if (!ReadExactBytes(outData.data(), outData.size()))
    {
        outData.clear();
        return false;
    }

    return true;
}

bool GB_Stream::WriteString(const std::string& text)
{
    if (!WriteVarUInt64ToStream(*this, static_cast<std::uint64_t>(text.size())))
    {
        return false;
    }

    return WriteBytes(text);
}

bool GB_Stream::ReadString(std::string& outText, std::uint64_t maxBytes)
{
    outText.clear();

    std::uint64_t byteSize = 0;
    if (!ReadVarUInt64FromStream(*this, byteSize))
    {
        return false;
    }

    if (maxBytes > 0 && byteSize > maxBytes)
    {
        return false;
    }

    if (!CanStoreInSizeT(byteSize))
    {
        return false;
    }

    try
    {
        outText.resize(static_cast<std::size_t>(byteSize));
    }
    catch (...)
    {
        outText.clear();
        return false;
    }

    if (outText.empty())
    {
        return true;
    }

    if (!ReadExactBytes(&outText[0], outText.size()))
    {
        outText.clear();
        return false;
    }

    return true;
}

bool GB_Stream::WriteVariant(const GB_Variant& value)
{
    if (value.IsEmpty())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::Empty);
    }

    if (const bool* valuePtr = value.AnyCast<bool>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::Bool) && WriteBoolValueToStream(*this, *valuePtr);
    }

    if (const char* valuePtr = value.AnyCast<char>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::Char) && WriteIntegralValueToStream(*this, *valuePtr);
    }
    if (const signed char* valuePtr = value.AnyCast<signed char>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::SignedChar) && WriteIntegralValueToStream(*this, *valuePtr);
    }
    if (const unsigned char* valuePtr = value.AnyCast<unsigned char>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::UnsignedChar) && WriteIntegralValueToStream(*this, *valuePtr);
    }
    if (const short* valuePtr = value.AnyCast<short>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::Short) && WriteIntegralValueToStream(*this, *valuePtr);
    }
    if (const unsigned short* valuePtr = value.AnyCast<unsigned short>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::UnsignedShort) && WriteIntegralValueToStream(*this, *valuePtr);
    }
    if (const int* valuePtr = value.AnyCast<int>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::Int) && WriteIntegralValueToStream(*this, *valuePtr);
    }
    if (const unsigned int* valuePtr = value.AnyCast<unsigned int>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::UnsignedInt) && WriteIntegralValueToStream(*this, *valuePtr);
    }
    if (const long* valuePtr = value.AnyCast<long>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::Long) && WriteIntegralValueToStream(*this, *valuePtr);
    }
    if (const unsigned long* valuePtr = value.AnyCast<unsigned long>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::UnsignedLong) && WriteIntegralValueToStream(*this, *valuePtr);
    }
    if (const long long* valuePtr = value.AnyCast<long long>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::LongLong) && WriteIntegralValueToStream(*this, *valuePtr);
    }
    if (const unsigned long long* valuePtr = value.AnyCast<unsigned long long>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::UnsignedLongLong) && WriteIntegralValueToStream(*this, *valuePtr);
    }

    if (const float* valuePtr = value.AnyCast<float>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::Float) && WriteFloatValueToStream(*this, *valuePtr);
    }

    if (const double* valuePtr = value.AnyCast<double>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::Double) && WriteDoubleValueToStream(*this, *valuePtr);
    }

    if (const long double* valuePtr = value.AnyCast<long double>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::LongDouble) && WriteObjectBytesToStream(*this, *valuePtr);
    }

    if (const std::string* valuePtr = value.AnyCast<std::string>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::String) && WriteString(*valuePtr);
    }

    if (const GB_ByteBuffer* valuePtr = value.AnyCast<GB_ByteBuffer>())
    {
        return WriteVariantTagToStream(*this, GB_StreamVariantEncodingTag::Binary) && WriteByteBuffer(*valuePtr);
    }

    return WriteSerializedVariantToStream(*this, value);
}

bool GB_Stream::ReadVariant(GB_Variant& outValue, std::uint64_t maxBytes)
{
    GB_StreamVariantEncodingTag tag = GB_StreamVariantEncodingTag::Empty;
    if (!ReadVariantTagFromStream(*this, tag))
    {
        return false;
    }

    GB_Variant value;
    switch (tag)
    {
    case GB_StreamVariantEncodingTag::Empty:
        value.Reset();
        break;
    case GB_StreamVariantEncodingTag::Bool:
    {
        bool typedValue = false;
        if (!ReadBoolValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::Char:
    {
        char typedValue = 0;
        if (!ReadIntegralValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::SignedChar:
    {
        signed char typedValue = 0;
        if (!ReadIntegralValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::UnsignedChar:
    {
        unsigned char typedValue = 0;
        if (!ReadIntegralValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::Short:
    {
        short typedValue = 0;
        if (!ReadIntegralValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::UnsignedShort:
    {
        unsigned short typedValue = 0;
        if (!ReadIntegralValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::Int:
    {
        int typedValue = 0;
        if (!ReadIntegralValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::UnsignedInt:
    {
        unsigned int typedValue = 0;
        if (!ReadIntegralValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::Long:
    {
        long typedValue = 0;
        if (!ReadIntegralValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::UnsignedLong:
    {
        unsigned long typedValue = 0;
        if (!ReadIntegralValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::LongLong:
    {
        long long typedValue = 0;
        if (!ReadIntegralValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::UnsignedLongLong:
    {
        unsigned long long typedValue = 0;
        if (!ReadIntegralValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::Float:
    {
        float typedValue = 0.0f;
        if (!ReadFloatValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::Double:
    {
        double typedValue = 0.0;
        if (!ReadDoubleValueFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::LongDouble:
    {
        long double typedValue = 0.0L;
        if (!ReadObjectBytesFromStream(*this, typedValue))
        {
            return false;
        }
        value = typedValue;
        break;
    }
    case GB_StreamVariantEncodingTag::String:
    {
        std::string typedValue;
        if (!ReadString(typedValue, maxBytes))
        {
            return false;
        }
        value = std::move(typedValue);
        break;
    }
    case GB_StreamVariantEncodingTag::Binary:
    {
        GB_ByteBuffer typedValue;
        if (!ReadByteBuffer(typedValue, maxBytes))
        {
            return false;
        }
        value = std::move(typedValue);
        break;
    }
    case GB_StreamVariantEncodingTag::SerializedVariant:
    {
        GB_ByteBuffer serializedData;
        if (!ReadByteBuffer(serializedData, maxBytes))
        {
            return false;
        }

        if (!GB_Variant::Deserialize(serializedData, value))
        {
            return false;
        }
        break;
    }
    default:
        return false;
    }

    outValue = std::move(value);
    return true;
}

struct GB_FileStream::Impl
{
#ifdef _WIN32
    HANDLE fileHandle = INVALID_HANDLE_VALUE;
#else
    int fileDescriptor = -1;
#endif
    std::string filePathUtf8;
    GB_FileStreamOpenOptions options;
};

GB_FileStream::GB_FileStream() : impl_(new Impl())
{
}

GB_FileStream::GB_FileStream(const std::string& filePathUtf8, const GB_FileStreamOpenOptions& options) : impl_(new Impl())
{
    Open(filePathUtf8, options);
}

GB_FileStream::~GB_FileStream()
{
    Close();
    delete impl_;
    impl_ = nullptr;
}

GB_FileStream::GB_FileStream(GB_FileStream&& other) noexcept : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

GB_FileStream& GB_FileStream::operator=(GB_FileStream&& other) noexcept
{
    if (this != &other)
    {
        Close();
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }

    return *this;
}

bool GB_FileStream::Open(const std::string& filePathUtf8, const GB_FileStreamOpenOptions& options)
{
    Close();
    if (impl_ == nullptr)
    {
        impl_ = new Impl();
    }

    if (filePathUtf8.empty() || IsPathLikeDirectory(filePathUtf8))
    {
        return false;
    }

    if (!IsReadableAccess(options.accessMode) && !IsWritableAccess(options.accessMode))
    {
        return false;
    }

    if (options.openMode == GB_FileStreamOpenMode::TruncateExisting && !IsWritableAccess(options.accessMode))
    {
        return false;
    }

    const bool mayCreateOrWriteFile = options.openMode == GB_FileStreamOpenMode::CreateNew
        || options.openMode == GB_FileStreamOpenMode::CreateAlways
        || options.openMode == GB_FileStreamOpenMode::OpenAlways
        || options.openMode == GB_FileStreamOpenMode::TruncateExisting
        || IsWritableAccess(options.accessMode);
    if (options.createParentDirectories && mayCreateOrWriteFile)
    {
        if (!EnsureParentDirectoryForFile(filePathUtf8))
        {
            return false;
        }
    }

#ifdef _WIN32
    const std::wstring filePathUtf16 = Utf8ToUtf16(filePathUtf8);
    if (filePathUtf16.empty())
    {
        return false;
    }

    DWORD desiredAccess = 0;
    if (IsReadableAccess(options.accessMode))
    {
        desiredAccess |= GENERIC_READ;
    }
    if (IsWritableAccess(options.accessMode))
    {
        desiredAccess |= GENERIC_WRITE;
    }

    DWORD shareMode = 0;
    if (HasShareMode(options.shareMode, GB_FileShareMode::Read))
    {
        shareMode |= FILE_SHARE_READ;
    }
    if (HasShareMode(options.shareMode, GB_FileShareMode::Write))
    {
        shareMode |= FILE_SHARE_WRITE;
    }
    if (HasShareMode(options.shareMode, GB_FileShareMode::Delete))
    {
        shareMode |= FILE_SHARE_DELETE;
    }

    DWORD creationDisposition = OPEN_EXISTING;
    if (options.openMode == GB_FileStreamOpenMode::OpenExisting)
    {
        creationDisposition = OPEN_EXISTING;
    }
    else if (options.openMode == GB_FileStreamOpenMode::CreateNew)
    {
        creationDisposition = CREATE_NEW;
    }
    else if (options.openMode == GB_FileStreamOpenMode::CreateAlways)
    {
        creationDisposition = CREATE_ALWAYS;
    }
    else if (options.openMode == GB_FileStreamOpenMode::OpenAlways)
    {
        creationDisposition = OPEN_ALWAYS;
    }
    else if (options.openMode == GB_FileStreamOpenMode::TruncateExisting)
    {
        creationDisposition = TRUNCATE_EXISTING;
    }
    else
    {
        return false;
    }

    DWORD flagsAndAttributes = FILE_ATTRIBUTE_NORMAL;
    if (options.sequentialAccessHint)
    {
        flagsAndAttributes |= FILE_FLAG_SEQUENTIAL_SCAN;
    }
    if (options.writeThrough)
    {
        flagsAndAttributes |= FILE_FLAG_WRITE_THROUGH;
    }

    HANDLE fileHandle = ::CreateFileW(filePathUtf16.c_str(), desiredAccess, shareMode, nullptr, creationDisposition, flagsAndAttributes, nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    impl_->fileHandle = fileHandle;
#else
    int openFlags = 0;
    if (options.accessMode == GB_StreamAccessMode::ReadOnly)
    {
        openFlags |= O_RDONLY;
    }
    else if (options.accessMode == GB_StreamAccessMode::WriteOnly)
    {
        openFlags |= O_WRONLY;
    }
    else if (options.accessMode == GB_StreamAccessMode::ReadWrite)
    {
        openFlags |= O_RDWR;
    }
    else
    {
        return false;
    }

    if (options.openMode == GB_FileStreamOpenMode::OpenExisting)
    {
    }
    else if (options.openMode == GB_FileStreamOpenMode::CreateNew)
    {
        openFlags |= O_CREAT | O_EXCL;
    }
    else if (options.openMode == GB_FileStreamOpenMode::CreateAlways)
    {
        openFlags |= O_CREAT | O_TRUNC;
    }
    else if (options.openMode == GB_FileStreamOpenMode::OpenAlways)
    {
        openFlags |= O_CREAT;
    }
    else if (options.openMode == GB_FileStreamOpenMode::TruncateExisting)
    {
        openFlags |= O_TRUNC;
    }
    else
    {
        return false;
    }

#ifdef O_CLOEXEC
    openFlags |= O_CLOEXEC;
#endif
#ifdef O_SYNC
    if (options.writeThrough)
    {
        openFlags |= O_SYNC;
    }
#endif

    const int fileDescriptor = ::open(filePathUtf8.c_str(), openFlags, 0644);
    if (fileDescriptor < 0)
    {
        return false;
    }

#ifdef POSIX_FADV_SEQUENTIAL
    if (options.sequentialAccessHint)
    {
        (void)::posix_fadvise(fileDescriptor, 0, 0, POSIX_FADV_SEQUENTIAL);
    }
#endif

    impl_->fileDescriptor = fileDescriptor;
#endif

    impl_->filePathUtf8 = filePathUtf8;
    impl_->options = options;

    if (options.seekToEndAfterOpen)
    {
        if (!Seek(0, GB_StreamSeekOrigin::End))
        {
            Close();
            return false;
        }
    }

    return true;
}

std::string GB_FileStream::GetFilePathUtf8() const
{
    return impl_ == nullptr ? std::string() : impl_->filePathUtf8;
}

bool GB_FileStream::IsOpen() const
{
    if (impl_ == nullptr)
    {
        return false;
    }

#ifdef _WIN32
    return impl_->fileHandle != INVALID_HANDLE_VALUE;
#else
    return impl_->fileDescriptor >= 0;
#endif
}

bool GB_FileStream::CanRead() const
{
    return IsOpen() && IsReadableAccess(impl_->options.accessMode);
}

bool GB_FileStream::CanWrite() const
{
    return IsOpen() && IsWritableAccess(impl_->options.accessMode);
}

bool GB_FileStream::IsSeekable() const
{
    return IsOpen();
}

bool GB_FileStream::Tell(std::uint64_t& outPosition) const
{
    outPosition = 0;
    if (!IsOpen())
    {
        return false;
    }

#ifdef _WIN32
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    LARGE_INTEGER currentPosition;
    currentPosition.QuadPart = 0;
    if (::SetFilePointerEx(impl_->fileHandle, zero, &currentPosition, FILE_CURRENT) == FALSE)
    {
        return false;
    }

    if (currentPosition.QuadPart < 0)
    {
        return false;
    }

    outPosition = static_cast<std::uint64_t>(currentPosition.QuadPart);
    return true;
#else
    const off_t currentPosition = ::lseek(impl_->fileDescriptor, 0, SEEK_CUR);
    if (currentPosition < 0)
    {
        return false;
    }

    outPosition = static_cast<std::uint64_t>(currentPosition);
    return true;
#endif
}

bool GB_FileStream::Length(std::uint64_t& outLength) const
{
    outLength = 0;
    if (!IsOpen())
    {
        return false;
    }

#ifdef _WIN32
    LARGE_INTEGER fileSize;
    fileSize.QuadPart = 0;
    if (::GetFileSizeEx(impl_->fileHandle, &fileSize) == FALSE || fileSize.QuadPart < 0)
    {
        return false;
    }

    outLength = static_cast<std::uint64_t>(fileSize.QuadPart);
    return true;
#else
    struct stat fileStat;
    if (::fstat(impl_->fileDescriptor, &fileStat) != 0 || fileStat.st_size < 0)
    {
        return false;
    }

    outLength = static_cast<std::uint64_t>(fileStat.st_size);
    return true;
#endif
}

bool GB_FileStream::Seek(std::int64_t offset, GB_StreamSeekOrigin origin)
{
    if (!IsOpen())
    {
        return false;
    }

#ifdef _WIN32
    DWORD moveMethod = FILE_BEGIN;
    if (origin == GB_StreamSeekOrigin::Begin)
    {
        moveMethod = FILE_BEGIN;
    }
    else if (origin == GB_StreamSeekOrigin::Current)
    {
        moveMethod = FILE_CURRENT;
    }
    else if (origin == GB_StreamSeekOrigin::End)
    {
        moveMethod = FILE_END;
    }
    else
    {
        return false;
    }

    LARGE_INTEGER distance;
    distance.QuadPart = static_cast<LONGLONG>(offset);
    LARGE_INTEGER newPosition;
    newPosition.QuadPart = 0;
    if (::SetFilePointerEx(impl_->fileHandle, distance, &newPosition, moveMethod) == FALSE)
    {
        return false;
    }

    return newPosition.QuadPart >= 0;
#else
    int whence = SEEK_SET;
    if (origin == GB_StreamSeekOrigin::Begin)
    {
        whence = SEEK_SET;
    }
    else if (origin == GB_StreamSeekOrigin::Current)
    {
        whence = SEEK_CUR;
    }
    else if (origin == GB_StreamSeekOrigin::End)
    {
        whence = SEEK_END;
    }
    else
    {
        return false;
    }

    if (offset > static_cast<std::int64_t>((std::numeric_limits<off_t>::max)()) || offset < static_cast<std::int64_t>((std::numeric_limits<off_t>::min)()))
    {
        return false;
    }

    return ::lseek(impl_->fileDescriptor, static_cast<off_t>(offset), whence) >= 0;
#endif
}

bool GB_FileStream::Truncate(std::uint64_t length)
{
    if (!CanWrite())
    {
        return false;
    }

    std::uint64_t oldPosition = 0;
    if (!Tell(oldPosition))
    {
        return false;
    }

#ifdef _WIN32
    if (length > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()))
    {
        return false;
    }

    LARGE_INTEGER targetPosition;
    targetPosition.QuadPart = static_cast<LONGLONG>(length);
    if (::SetFilePointerEx(impl_->fileHandle, targetPosition, nullptr, FILE_BEGIN) == FALSE)
    {
        return false;
    }

    if (::SetEndOfFile(impl_->fileHandle) == FALSE)
    {
        (void)Seek(static_cast<std::int64_t>(oldPosition), GB_StreamSeekOrigin::Begin);
        return false;
    }
#else
    if (length > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)()))
    {
        return false;
    }

    if (::ftruncate(impl_->fileDescriptor, static_cast<off_t>(length)) != 0)
    {
        return false;
    }
#endif

    const std::uint64_t newPosition = oldPosition > length ? length : oldPosition;
    if (newPosition > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
    {
        return false;
    }

    return Seek(static_cast<std::int64_t>(newPosition), GB_StreamSeekOrigin::Begin);
}

bool GB_FileStream::Flush()
{
    if (!IsOpen())
    {
        return false;
    }

#ifdef _WIN32
    if (!CanWrite())
    {
        return true;
    }

    return ::FlushFileBuffers(impl_->fileHandle) != FALSE;
#else
    if (!CanWrite())
    {
        return true;
    }

    while (true)
    {
        if (::fsync(impl_->fileDescriptor) == 0)
        {
            return true;
        }

        if (errno != EINTR)
        {
            return false;
        }
    }
#endif
}

void GB_FileStream::Close()
{
    if (impl_ == nullptr)
    {
        return;
    }

#ifdef _WIN32
    if (impl_->fileHandle != INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(impl_->fileHandle);
        impl_->fileHandle = INVALID_HANDLE_VALUE;
    }
#else
    if (impl_->fileDescriptor >= 0)
    {
        ::close(impl_->fileDescriptor);
        impl_->fileDescriptor = -1;
    }
#endif

    impl_->filePathUtf8.clear();
    impl_->options = GB_FileStreamOpenOptions();
}

bool GB_FileStream::ReadBytes(void* outData, std::size_t byteSize, std::size_t& outReadBytes)
{
    outReadBytes = 0;
    if (byteSize == 0)
    {
        return true;
    }

    if (!CanRead() || outData == nullptr)
    {
        return false;
    }

    unsigned char* outputBytes = static_cast<unsigned char*>(outData);
#ifdef _WIN32
    const DWORD chunkBytes = 64u * 1024u * 1024u;
    while (outReadBytes < byteSize)
    {
        const std::size_t remainingBytes = byteSize - outReadBytes;
        const DWORD toRead = static_cast<DWORD>(remainingBytes > chunkBytes ? chunkBytes : remainingBytes);

        DWORD currentReadBytes = 0;
        if (::ReadFile(impl_->fileHandle, outputBytes + outReadBytes, toRead, &currentReadBytes, nullptr) == FALSE)
        {
            return false;
        }

        if (currentReadBytes == 0)
        {
            break;
        }

        outReadBytes += static_cast<std::size_t>(currentReadBytes);
    }
#else
    const std::size_t chunkBytes = 64u * 1024u * 1024u;
    while (outReadBytes < byteSize)
    {
        const std::size_t remainingBytes = byteSize - outReadBytes;
        const std::size_t toRead = remainingBytes > chunkBytes ? chunkBytes : remainingBytes;

        const ssize_t currentReadBytes = ::read(impl_->fileDescriptor, outputBytes + outReadBytes, toRead);
        if (currentReadBytes < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return false;
        }

        if (currentReadBytes == 0)
        {
            break;
        }

        outReadBytes += static_cast<std::size_t>(currentReadBytes);
    }
#endif

    return true;
}

bool GB_FileStream::WriteBytes(const void* data, std::size_t byteSize)
{
    if (byteSize == 0)
    {
        return true;
    }

    if (!CanWrite() || data == nullptr)
    {
        return false;
    }

    const unsigned char* inputBytes = static_cast<const unsigned char*>(data);
    std::size_t totalWrittenBytes = 0;
#ifdef _WIN32
    const DWORD chunkBytes = 64u * 1024u * 1024u;
    while (totalWrittenBytes < byteSize)
    {
        const std::size_t remainingBytes = byteSize - totalWrittenBytes;
        const DWORD toWrite = static_cast<DWORD>(remainingBytes > chunkBytes ? chunkBytes : remainingBytes);

        DWORD currentWrittenBytes = 0;
        if (::WriteFile(impl_->fileHandle, inputBytes + totalWrittenBytes, toWrite, &currentWrittenBytes, nullptr) == FALSE)
        {
            return false;
        }

        if (currentWrittenBytes == 0)
        {
            return false;
        }

        totalWrittenBytes += static_cast<std::size_t>(currentWrittenBytes);
    }
#else
    const std::size_t chunkBytes = 64u * 1024u * 1024u;
    while (totalWrittenBytes < byteSize)
    {
        const std::size_t remainingBytes = byteSize - totalWrittenBytes;
        const std::size_t toWrite = remainingBytes > chunkBytes ? chunkBytes : remainingBytes;

        const ssize_t currentWrittenBytes = ::write(impl_->fileDescriptor, inputBytes + totalWrittenBytes, toWrite);
        if (currentWrittenBytes < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return false;
        }

        if (currentWrittenBytes == 0)
        {
            return false;
        }

        totalWrittenBytes += static_cast<std::size_t>(currentWrittenBytes);
    }
#endif

    return true;
}

struct GB_MemoryStream::Impl
{
    Impl() : externalBuffer(nullptr), position(0), accessMode(GB_StreamAccessMode::ReadWrite), isOpen(true)
    {
    }

    Impl(GB_StreamAccessMode streamAccessMode) : externalBuffer(nullptr), position(0), accessMode(streamAccessMode), isOpen(true)
    {
    }

    Impl(const GB_ByteBuffer& streamBuffer, GB_StreamAccessMode streamAccessMode) : buffer(streamBuffer), externalBuffer(nullptr), position(0), accessMode(streamAccessMode), isOpen(true)
    {
    }

    Impl(GB_ByteBuffer& streamBuffer, GB_StreamAccessMode streamAccessMode) : externalBuffer(&streamBuffer), position(0), accessMode(streamAccessMode), isOpen(true)
    {
    }

    Impl(GB_ByteBuffer&& streamBuffer, GB_StreamAccessMode streamAccessMode) : buffer(std::move(streamBuffer)), externalBuffer(nullptr), position(0), accessMode(streamAccessMode), isOpen(true)
    {
    }

    GB_ByteBuffer& Buffer()
    {
        return externalBuffer == nullptr ? buffer : *externalBuffer;
    }

    const GB_ByteBuffer& Buffer() const
    {
        return externalBuffer == nullptr ? buffer : *externalBuffer;
    }

    bool IsUsingExternalBuffer() const
    {
        return externalBuffer != nullptr;
    }

    GB_ByteBuffer buffer;
    GB_ByteBuffer* externalBuffer;
    std::uint64_t position;
    GB_StreamAccessMode accessMode;
    bool isOpen;
};

GB_MemoryStream::GB_MemoryStream() : impl_(new Impl())
{
}

GB_MemoryStream::GB_MemoryStream(GB_StreamAccessMode accessMode) : impl_(new Impl(accessMode))
{
}

GB_MemoryStream::GB_MemoryStream(const GB_ByteBuffer& data, GB_StreamAccessMode accessMode) : impl_(new Impl(data, accessMode))
{
}

GB_MemoryStream::GB_MemoryStream(GB_ByteBuffer& data, GB_StreamAccessMode accessMode) : impl_(new Impl(data, accessMode))
{
}

GB_MemoryStream::GB_MemoryStream(GB_ByteBuffer&& data, GB_StreamAccessMode accessMode) : impl_(new Impl(std::move(data), accessMode))
{
}

GB_MemoryStream::~GB_MemoryStream()
{
    delete impl_;
    impl_ = nullptr;
}

GB_MemoryStream::GB_MemoryStream(GB_MemoryStream&& other) noexcept : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

GB_MemoryStream& GB_MemoryStream::operator=(GB_MemoryStream&& other) noexcept
{
    if (this != &other)
    {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }

    return *this;
}

void GB_MemoryStream::Open(GB_StreamAccessMode accessMode)
{
    if (impl_ == nullptr)
    {
        impl_ = new Impl(accessMode);
        return;
    }

    impl_->externalBuffer = nullptr;
    impl_->buffer.clear();
    impl_->position = 0;
    impl_->accessMode = accessMode;
    impl_->isOpen = true;
}

void GB_MemoryStream::Reset(const GB_ByteBuffer& data, GB_StreamAccessMode accessMode)
{
    if (impl_ == nullptr)
    {
        impl_ = new Impl(data, accessMode);
        return;
    }

    impl_->externalBuffer = nullptr;
    impl_->buffer = data;
    impl_->position = 0;
    impl_->accessMode = accessMode;
    impl_->isOpen = true;
}

void GB_MemoryStream::Reset(GB_ByteBuffer& data, GB_StreamAccessMode accessMode)
{
    if (impl_ == nullptr)
    {
        impl_ = new Impl(data, accessMode);
        return;
    }

    impl_->buffer.clear();
    impl_->externalBuffer = &data;
    impl_->position = 0;
    impl_->accessMode = accessMode;
    impl_->isOpen = true;
}

void GB_MemoryStream::Reset(GB_ByteBuffer&& data, GB_StreamAccessMode accessMode)
{
    if (impl_ == nullptr)
    {
        impl_ = new Impl(std::move(data), accessMode);
        return;
    }

    impl_->externalBuffer = nullptr;
    impl_->buffer = std::move(data);
    impl_->position = 0;
    impl_->accessMode = accessMode;
    impl_->isOpen = true;
}

const GB_ByteBuffer& GB_MemoryStream::GetBuffer() const
{
    static const GB_ByteBuffer emptyBuffer;
    if (impl_ == nullptr)
    {
        return emptyBuffer;
    }

    return impl_->Buffer();
}

GB_ByteBuffer GB_MemoryStream::TakeBuffer()
{
    if (impl_ == nullptr)
    {
        return GB_ByteBuffer();
    }

    GB_ByteBuffer result = std::move(impl_->Buffer());
    impl_->Buffer().clear();
    impl_->position = 0;
    return result;
}

bool GB_MemoryStream::ClearBuffer()
{
    if (!CanWrite())
    {
        return false;
    }

    impl_->Buffer().clear();
    impl_->position = 0;
    return true;
}

bool GB_MemoryStream::Reserve(std::uint64_t capacity)
{
    if (!CanWrite())
    {
        return false;
    }

    if (!CanStoreInSizeT(capacity))
    {
        return false;
    }

    try
    {
        impl_->Buffer().reserve(static_cast<std::size_t>(capacity));
    }
    catch (...)
    {
        return false;
    }

    return true;
}

bool GB_MemoryStream::IsUsingExternalBuffer() const
{
    return impl_ != nullptr && impl_->IsUsingExternalBuffer();
}

bool GB_MemoryStream::IsOpen() const
{
    return impl_ != nullptr && impl_->isOpen;
}

bool GB_MemoryStream::CanRead() const
{
    return impl_ != nullptr && impl_->isOpen && IsReadableAccess(impl_->accessMode);
}

bool GB_MemoryStream::CanWrite() const
{
    return impl_ != nullptr && impl_->isOpen && IsWritableAccess(impl_->accessMode);
}

bool GB_MemoryStream::IsSeekable() const
{
    return impl_ != nullptr && impl_->isOpen;
}

bool GB_MemoryStream::Tell(std::uint64_t& outPosition) const
{
    outPosition = 0;
    if (impl_ == nullptr || !impl_->isOpen)
    {
        return false;
    }

    outPosition = impl_->position;
    return true;
}

bool GB_MemoryStream::Length(std::uint64_t& outLength) const
{
    outLength = 0;
    if (impl_ == nullptr || !impl_->isOpen)
    {
        return false;
    }

    outLength = static_cast<std::uint64_t>(impl_->Buffer().size());
    return true;
}

bool GB_MemoryStream::Seek(std::int64_t offset, GB_StreamSeekOrigin origin)
{
    if (impl_ == nullptr || !impl_->isOpen)
    {
        return false;
    }

    std::uint64_t newPosition = 0;
    if (!CalculateMemorySeekPosition(impl_->position, static_cast<std::uint64_t>(impl_->Buffer().size()), offset, origin, newPosition))
    {
        return false;
    }

    if (!CanStoreInSizeT(newPosition))
    {
        return false;
    }

    impl_->position = newPosition;
    return true;
}

bool GB_MemoryStream::Truncate(std::uint64_t length)
{
    if (!CanWrite())
    {
        return false;
    }

    if (!CanStoreInSizeT(length))
    {
        return false;
    }

    try
    {
        impl_->Buffer().resize(static_cast<std::size_t>(length));
    }
    catch (...)
    {
        return false;
    }

    if (impl_->position > length)
    {
        impl_->position = length;
    }

    return true;
}

bool GB_MemoryStream::Flush()
{
    return impl_ != nullptr && impl_->isOpen;
}

void GB_MemoryStream::Close()
{
    if (impl_ == nullptr)
    {
        return;
    }

    impl_->isOpen = false;
    impl_->position = 0;
}

bool GB_MemoryStream::ReadBytes(void* outData, std::size_t byteSize, std::size_t& outReadBytes)
{
    outReadBytes = 0;
    if (byteSize == 0)
    {
        return true;
    }

    if (!CanRead() || outData == nullptr)
    {
        return false;
    }

    if (impl_->position >= static_cast<std::uint64_t>(impl_->Buffer().size()))
    {
        return true;
    }

    const std::uint64_t remainBytes64 = static_cast<std::uint64_t>(impl_->Buffer().size()) - impl_->position;
    const std::size_t remainBytes = static_cast<std::size_t>(remainBytes64);
    const std::size_t currentReadBytes = remainBytes < byteSize ? remainBytes : byteSize;
    if (currentReadBytes > 0)
    {
        std::memcpy(outData, impl_->Buffer().data() + static_cast<std::size_t>(impl_->position), currentReadBytes);
        impl_->position += static_cast<std::uint64_t>(currentReadBytes);
        outReadBytes = currentReadBytes;
    }

    return true;
}

bool GB_MemoryStream::WriteBytes(const void* data, std::size_t byteSize)
{
    if (byteSize == 0)
    {
        return true;
    }

    if (!CanWrite() || data == nullptr)
    {
        return false;
    }

    if (impl_->position > (std::numeric_limits<std::uint64_t>::max)() - static_cast<std::uint64_t>(byteSize))
    {
        return false;
    }

    const std::uint64_t endPosition = impl_->position + static_cast<std::uint64_t>(byteSize);
    if (!CanStoreInSizeT(endPosition))
    {
        return false;
    }

    try
    {
        if (endPosition > static_cast<std::uint64_t>(impl_->Buffer().size()))
        {
            impl_->Buffer().resize(static_cast<std::size_t>(endPosition));
        }

        std::memcpy(impl_->Buffer().data() + static_cast<std::size_t>(impl_->position), data, byteSize);
    }
    catch (...)
    {
        return false;
    }

    impl_->position = endPosition;
    return true;
}
