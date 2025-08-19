#include "GB_IO.h"
#include "GB_FileSystem.h"
#include <fstream>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
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



