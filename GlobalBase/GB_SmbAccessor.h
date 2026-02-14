#ifndef GLOBALBASE_SMB_ACCESSOR_H_H
#define GLOBALBASE_SMB_ACCESSOR_H_H

#include "GlobalBasePort.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief SMB（Windows 共享）访问器：以 UTF-8 字符串为公共接口，对 Windows 平台内部使用宽字符 API。
 *
 * 说明：
 * - 该类主要面向 Windows（SMB/UNC/WNet/NetApi/Win32 文件 API）。
 * - 在非 Windows 平台上也可编译，但所有操作会返回失败/空结果（用于跨平台工程中“可包含但不可用”的场景）。
 * - hostOrIpUtf8 / shareNameUtf8 / relativePathUtf8 等均要求为 UTF-8 编码的 std::string。
 */
class GLOBALBASE_PORT GB_SmbAccessor
{
public:
    enum class AddressType
    {
        HostName,
        IPv4,
        IPv6Literal
    };

    struct Credentials
    {
        // 可为空；也可以直接把 "domain\user" 放到 userName 里
        std::string domain;

        // 可为空表示使用当前登录用户
        std::string userName;

        // 可为空表示使用当前凭据（本类不弹 UI）
        std::string password;
    };

    struct ShareInfo
    {
        std::string name;
        uint32_t type = 0;
        std::string remark;
    };

public:
    GB_SmbAccessor(const std::string& hostOrIpUtf8, AddressType addressType);
    GB_SmbAccessor(const std::string& hostOrIpUtf8, AddressType addressType, const Credentials& credentialsUtf8);
    ~GB_SmbAccessor();

    GB_SmbAccessor(const GB_SmbAccessor&) = delete;
    GB_SmbAccessor& operator=(const GB_SmbAccessor&) = delete;

    GB_SmbAccessor(GB_SmbAccessor&& other) noexcept;
    GB_SmbAccessor& operator=(GB_SmbAccessor&& other) noexcept;

    void SetCredentials(const Credentials& credentialsUtf8);
    void SetUseLongPathPrefix(bool useLongPathPrefix);

    // 1) 纯网络层：测试 TCP 445 是否可达（不等价于“已鉴权可访问”）
    bool TestTcp445(int timeoutMs, std::string& errorMessageUtf8) const;

    // 2) SMB 层：尝试连接 \\server\IPC$（使用当前凭据或 SetCredentials 设置的凭据）
    bool TestSmbConnection(std::string& errorMessageUtf8) const;

    // 3) 枚举共享（默认只返回磁盘共享；可选包含隐藏/管理共享）
    bool GetShares(std::vector<std::string>& shareNamesUtf8, bool includeSpecialShares, std::string& errorMessageUtf8) const;
    bool GetShareInfos(std::vector<ShareInfo>& sharesUtf8, bool includeSpecialShares, std::string& errorMessageUtf8) const;

    // 4) 连接/断开共享（不映射盘符，使用 UNC；可选持久化到 Profile）
    bool ConnectShare(const std::string& shareNameUtf8, bool persistent, std::string& errorMessageUtf8) const;
    bool DisconnectShare(const std::string& shareNameUtf8, bool force, std::string& errorMessageUtf8) const;

    // 5) 常用文件接口（基于 shareName + relativePath，内部拼成 UNC 路径）
    bool ListDirectory(const std::string& shareNameUtf8,
        const std::string& relativeDirUtf8,
        std::vector<std::string>& childNamesUtf8,
        bool includeDirectories,
        bool includeFiles,
        std::string& errorMessageUtf8) const;

    bool FileExists(const std::string& shareNameUtf8, const std::string& relativePathUtf8) const;
    bool DirectoryExists(const std::string& shareNameUtf8, const std::string& relativePathUtf8) const;

    bool CreateDirectoryRecursive(const std::string& shareNameUtf8, const std::string& relativeDirUtf8, std::string& errorMessageUtf8) const;
    bool DeleteFileRemote(const std::string& shareNameUtf8, const std::string& relativePathUtf8, std::string& errorMessageUtf8) const;

    // 单文件复制：本地 -> 远端（会在需要时递归创建 remoteRelativePath 的上级目录）
    bool CopyFileFromLocal(const std::string& localPathUtf8,
        const std::string& shareNameUtf8,
        const std::string& remoteRelativePathUtf8,
        bool overwrite,
        std::string& errorMessageUtf8) const;

    // 单文件复制：本地 -> 远端（并行分段；threadCount==0 表示自动选择）
    bool CopyFileFromLocalParallel(const std::string& localPathUtf8,
        const std::string& shareNameUtf8,
        const std::string& remoteRelativePathUtf8,
        bool overwrite,
        std::string& errorMessageUtf8,
        size_t threadCount = 0) const;

    // 单文件复制：远端 -> 本地（会在需要时递归创建 localPath 的上级目录）
    bool CopyFileToLocal(const std::string& shareNameUtf8,
        const std::string& remoteRelativePathUtf8,
        const std::string& localPathUtf8,
        bool overwrite,
        std::string& errorMessageUtf8) const;

    // 单文件复制：远端 -> 本地（并行分段；threadCount==0 表示自动选择）
    bool CopyFileToLocalParallel(const std::string& shareNameUtf8,
        const std::string& remoteRelativePathUtf8,
        const std::string& localPathUtf8,
        bool overwrite,
        std::string& errorMessageUtf8,
        size_t threadCount = 0) const;

    // 目录复制（含子目录）：本地 -> 远端
    bool CopyDirectoryFromLocal(const std::string& localDirectoryUtf8,
        const std::string& shareNameUtf8,
        const std::string& remoteRelativePathUtf8,
        bool overwrite,
        std::string& errorMessageUtf8) const;

    // 目录复制（含子目录）：本地 -> 远端（并行化“文件拷贝”任务；threadCount==0 自动选择）
    bool CopyDirectoryFromLocalParallel(const std::string& localDirectoryUtf8,
        const std::string& shareNameUtf8,
        const std::string& remoteRelativePathUtf8,
        bool overwrite,
        std::string& errorMessageUtf8,
        size_t threadCount = 0) const;

    // 目录复制（含子目录）：远端 -> 本地
    bool CopyDirectoryToLocal(const std::string& shareNameUtf8,
        const std::string& remoteRelativePathUtf8,
        const std::string& localDirectoryUtf8,
        bool overwrite,
        std::string& errorMessageUtf8) const;

    // 目录复制（含子目录）：远端 -> 本地（并行化“文件拷贝”任务；threadCount==0 自动选择）
    bool CopyDirectoryToLocalParallel(const std::string& shareNameUtf8,
        const std::string& remoteRelativePathUtf8,
        const std::string& localDirectoryUtf8,
        bool overwrite,
        std::string& errorMessageUtf8,
        size_t threadCount = 0) const;

    // 获取远端文件大小（字节）
    bool GetFileSizeRemote(const std::string& shareNameUtf8,
        const std::string& remoteRelativePathUtf8,
        uint64_t& fileSize,
        std::string& errorMessageUtf8) const;

    // 6) 直接获取 UNC 根与路径（便于与项目内其它模块拼接）
    std::string GetUncRoot(const std::string& shareNameUtf8) const;
    std::string GetUncPath(const std::string& shareNameUtf8, const std::string& relativePathUtf8) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool hostOrIpValidUtf8_ = true;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif