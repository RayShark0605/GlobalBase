#include "GB_IO.h"
#include "GB_FileSystem.h"
#include <fstream>
#include <limits>

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

using namespace std;

bool WriteUtf8ToFile(const string& filePathUtf8, const string& utf8Content, bool appendMode, bool addBomIfNewFile)
{
    const bool existedBefore = GB_IsFileExists(filePathUtf8);
	if (!existedBefore)
	{
		if (!GB_CreateFileRecursive(filePathUtf8))
		{
			return false; // 创建失败
		}
	}

#ifdef _WIN32
    // 将 UTF-8 路径转为 UTF-16 以使用 *W API
    auto Utf8ToUtf16 = [](const std::string& s) -> std::wstring
        {
            if (s.empty())
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
        };

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
        ofs.write(utf8Content.data(), static_cast<std::streamsize>(utf8Content.size()));
    }

    ofs.flush();
    const bool ok = static_cast<bool>(ofs);
    ofs.close();
    return ok;
#endif
}

std::vector<unsigned char> ReadFileToBinary(const std::string& filePathUtf8)
{
    if (filePathUtf8.empty())
    {
        return {};
    }

#ifdef _WIN32
    auto Utf8ToUtf16 = [](const std::string& s) -> std::wstring {
        if (s.empty())
        {
            return std::wstring();
        }

        const int need = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        if (need <= 0)
        {
            return std::wstring();
        }

        std::wstring w;
        w.resize(static_cast<size_t>(need));

        const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), static_cast<int>(s.size()), &w[0], need);
        if (written <= 0)
        {
            return std::wstring();
        }
        return w;
    };

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

    std::vector<unsigned char> buffer;
    buffer.resize(static_cast<size_t>(fileSize64));

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

bool WriteBinaryToFile(const std::vector<unsigned char>& data, const std::string& filePathUtf8)
{
    if (filePathUtf8.empty())
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
    auto Utf8ToUtf16 = [](const std::string& utf8) -> std::wstring {
        if (utf8.empty())
        {
            return std::wstring();
        }

        const int requiredChars = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            utf8.c_str(),
            static_cast<int>(utf8.size()),
            nullptr,
            0);

        if (requiredChars <= 0)
        {
            return std::wstring();
        }

        std::wstring utf16;
        utf16.resize(static_cast<size_t>(requiredChars));

        const int writtenChars = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            utf8.c_str(),
            static_cast<int>(utf8.size()),
            &utf16[0],
            requiredChars);

        if (writtenChars <= 0)
        {
            return std::wstring();
        }

        return utf16;
    };

    const std::wstring filePathUtf16 = Utf8ToUtf16(filePathUtf8);
    if (filePathUtf16.empty())
    {
        return false;
    }

    // shareMode=0 会导致文件无法再被打开直到句柄关闭，里更容易被外部因素影响
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

    // 2) 可选：预设文件大小（best-effort，失败不影响正确性）
    // 这样在部分文件系统上可能减少碎片/元数据抖动；不做 Flush/同步以保证速度优先
    if (!data.empty())
    {
        LARGE_INTEGER targetSize;
        targetSize.QuadPart = static_cast<LONGLONG>(data.size());

        if (::SetFilePointerEx(fileHandle, targetSize, nullptr, FILE_BEGIN) != FALSE)
        {
            (void)::SetEndOfFile(fileHandle);
            LARGE_INTEGER zero;
            zero.QuadPart = 0;
            (void)::SetFilePointerEx(fileHandle, zero, nullptr, FILE_BEGIN);
        }
    }

    // 3) 写入（支持 partial write）
    if (!data.empty())
    {
        const size_t totalSize = data.size();
        size_t totalWritten = 0;

        const DWORD chunkBytes = 64u * 1024u * 1024u;

        while (totalWritten < totalSize)
        {
            const size_t remainingBytes = totalSize - totalWritten;
            const DWORD toWrite = static_cast<DWORD>(remainingBytes > chunkBytes ? chunkBytes : remainingBytes);

            DWORD writtenBytes = 0;
            const BOOL writeOk = ::WriteFile(
                fileHandle,
                data.data() + totalWritten,
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

#if defined(__linux__)
    // best-effort 预分配，失败不影响正确性
    if (!data.empty())
    {
        (void)::posix_fallocate(fileDescriptor, 0, static_cast<off_t>(data.size()));
    }
#endif

    bool ok = true;

    // write() 允许 partial write：成功也可能少写
    size_t totalWritten = 0;
    const size_t totalSize = data.size();
    const size_t chunkBytes = 64u * 1024u * 1024u;

    while (totalWritten < totalSize)
    {
        const size_t remainingBytes = totalSize - totalWritten;
        const size_t toWrite = remainingBytes > chunkBytes ? chunkBytes : remainingBytes;

        const ssize_t writtenBytes = ::write(fileDescriptor, data.data() + totalWritten, toWrite);
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






