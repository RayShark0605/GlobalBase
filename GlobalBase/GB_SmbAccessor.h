#ifndef GLOBALBASE_SMB_ACCESSOR_H_H
#define GLOBALBASE_SMB_ACCESSOR_H_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "GlobalBasePort.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <winnetwk.h>
#include <lm.h>
#include <string>
#include <vector>
#include <mutex>
#include <stdint.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Mpr.lib")
#pragma comment(lib, "Netapi32.lib")

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

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
        std::wstring domain;   // 可为空；也可以直接把 domain\user 放到 userName 里
        std::wstring userName; // 可为空表示用当前登录用户
        std::wstring password; // 可为空表示使用当前凭据/或由系统弹 UI（本类不弹）
    };

    struct ShareInfo
    {
        std::wstring name;
        uint32_t type = 0;
        std::wstring remark;
    };

public:
    GB_SmbAccessor(const std::wstring& hostOrIp, AddressType addressType);
    GB_SmbAccessor(const std::wstring& hostOrIp, AddressType addressType, const Credentials& credentials);

    // 自动断开本实例通过 ConnectShare 连接过的共享
    ~GB_SmbAccessor();

    void SetCredentials(const Credentials& credentials);
    void SetUseLongPathPrefix(bool useLongPathPrefix);

    // 1) 纯网络层：测试 TCP 445 是否可达（不等价于“已鉴权可访问”）
    bool TestTcp445(int timeoutMs, std::wstring* errorMessage) const;

    // 2) SMB 层“能否连接上”：尝试连接 \\server\IPC$（或用当前凭据）
    bool TestSmbConnection(std::wstring* errorMessage) const;

    // 3) 枚举共享（默认只返回“共享文件夹/磁盘共享”，可选包含隐藏/管理共享）
    bool GetShares(std::vector<std::wstring>* shareNames, bool includeSpecialShares, std::wstring* errorMessage) const;
    bool GetShareInfos(std::vector<ShareInfo>* shares, bool includeSpecialShares, std::wstring* errorMessage) const;

    // 4) 连接/断开某个共享（不映射盘符，使用 UNC；可选持久化到 Profile）
    bool ConnectShare(const std::wstring& shareName, bool persistent, std::wstring* errorMessage) const;
    bool DisconnectShare(const std::wstring& shareName, bool force, std::wstring* errorMessage) const;

    // 5) 常用文件接口（基于 UNC 路径）
    bool ListDirectory(const std::wstring& shareName, const std::wstring& relativeDir, std::vector<std::wstring>* childNames,
        bool includeDirectories, bool includeFiles, std::wstring* errorMessage) const;

    bool FileExists(const std::wstring& shareName, const std::wstring& relativePath) const;
    bool DirectoryExists(const std::wstring& shareName, const std::wstring& relativePath) const;

    bool CreateDirectoryRecursive(const std::wstring& shareName, const std::wstring& relativeDir, std::wstring* errorMessage) const;
    bool DeleteFileRemote(const std::wstring& shareName, const std::wstring& relativePath, std::wstring* errorMessage) const;

    // 复制“单个文件”：本地 -> 远端。会在需要时递归创建 remoteRelativePath 的上级目录。
    bool CopyFileFromLocal(const std::wstring& localPath, const std::wstring& shareName, const std::wstring& remoteRelativePath,
        bool overwrite, std::wstring* errorMessage) const;

    // 复制“单个文件”：远端 -> 本地。会在需要时递归创建 localPath 的上级目录。
    bool CopyFileToLocal(const std::wstring& shareName, const std::wstring& remoteRelativePath, const std::wstring& localPath,
        bool overwrite, std::wstring* errorMessage) const;

    // 复制“目录（含子目录）”：本地 -> 远端。
    // 会在需要时递归创建 remoteRelativePath（以及其上级目录）。
    bool CopyDirectoryFromLocal(const std::wstring& localDirectory, const std::wstring& shareName, const std::wstring& remoteRelativePath,
        bool overwrite, std::wstring* errorMessage) const;

    // 复制“目录（含子目录）”：远端 -> 本地。
    // 会在需要时递归创建 localDirectory（以及其上级目录）。
    bool CopyDirectoryToLocal(const std::wstring& shareName, const std::wstring& remoteRelativePath, const std::wstring& localDirectory,
        bool overwrite, std::wstring* errorMessage) const;

    bool GetFileSizeRemote(const std::wstring& shareName, const std::wstring& remoteRelativePath, uint64_t* fileSize, std::wstring* errorMessage) const;

    // 6) 直接获取 UNC 根与路径（便于和项目内其它模块拼接）
    std::wstring GetUncRoot(const std::wstring& shareName) const;
    std::wstring GetUncPath(const std::wstring& shareName, const std::wstring& relativePath) const;

private:
    static std::wstring FormatWin32ErrorMessage(uint32_t errorCode);
    static std::wstring NormalizeSlashes(const std::wstring& path);
    static std::wstring JoinPath(const std::wstring& left, const std::wstring& right);

    static std::wstring GetParentPath(const std::wstring& path);
    static bool CreateLocalDirectoryRecursive(const std::wstring& fullDirectory, std::wstring* errorMessage);

    void RememberConnectedShare(const std::wstring& shareName, bool persistent) const;
    void ForgetConnectedShare(const std::wstring& shareName) const;

    std::wstring GetServerNameForUnc() const;
    std::wstring GetServerNameForNetApi() const;

    std::wstring BuildUncRootInternal(const std::wstring& shareName, bool useLongPathPrefix) const;
    std::wstring BuildUncPathInternal(const std::wstring& shareName, const std::wstring& relativePath, bool useLongPathPrefix) const;

    static std::wstring ConvertIpv6LiteralToUncHost(const std::wstring& ipv6Literal);


    bool CopyFileFromLocalInternal(const std::wstring& localPath, const std::wstring& shareName, const std::wstring& remoteRelativePath,
        bool overwrite, bool skipEnsureParentDir, std::wstring* errorMessage) const;

    bool CopyFileToLocalInternal(const std::wstring& shareName, const std::wstring& remoteRelativePath, const std::wstring& localPath,
        bool overwrite, bool skipEnsureParentDir, std::wstring* errorMessage) const;
private:
    std::wstring hostOrIp_;
    AddressType addressType_;
    Credentials credentials_;
    bool useLongPathPrefix_;

    struct ConnectedShareRecord
    {
        std::wstring shareName;
        bool persistent = false;
    };

    mutable std::mutex connectedSharesMutex_;
    mutable std::vector<ConnectedShareRecord> connectedShares_;
};


#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif