#ifndef GLOBALBASE_SYSINFO_H_H
#define GLOBALBASE_SYSINFO_H_H

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include "GlobalBasePort.h"

// CPU 信息
struct GB_CpuInfo
{
    // 标识信息
    std::string vendor;           // 厂商，如 GenuineIntel / AuthenticAMD / ARM / ...
    std::string brand;            // 型号全称
    std::string architecture;     // "x86", "x86_64", "arm", "arm64", "unknown"

    // 拓扑/数量
    uint32_t logicalCpus = 0;     // 逻辑处理器数量
    uint32_t physicalCores = 0;   // 物理核心数量
    uint32_t packageCount = 0;    // 物理封装（CPU 插槽）数量
    uint32_t numaNodes = 0;       // NUMA 节点数

    // 频率（Hz）
    uint64_t baseFrequencyHz = 0; // 典型/基准频率
    uint64_t maxFrequencyHz = 0; // 最大频率

    // 低层 ID
    std::string processorId;      // Windows: WMI Win32_Processor.ProcessorId（并非严格“序列号”）
    std::string cpuSerial;        // CPU 序列号

    // 其他
    bool hypervisorPresent = false; // 是否检测到在虚拟化环境中（CPUID ECX bit31 等）
    std::vector<std::string> features; // 指令集/特性标志（Windows: 子集；Linux: 来自 /proc/cpuinfo）

    GLOBALBASE_PORT std::string Serialize() const;
};

// 获取 CPU 信息
GLOBALBASE_PORT GB_CpuInfo GB_GetCpuInfo();

// 主板信息
struct GB_MotherboardInfo
{
    std::string manufacturer;   // 主板厂商
    std::string product;        // 主板型号
    std::string version;        // 主板版本
    std::string serialNumber;   // 主板序列号，可能为空
    std::string uuid;           // SMBIOS UUID

    // BIOS 概览（放在主板处方便一次返回）
    std::string biosVendor;     // BIOS 厂商
    std::string biosVersion;    // BIOS 版本
    std::string biosDate;       // BIOS 日期

    GLOBALBASE_PORT std::string Serialize() const;
};

// 获取当前主板信息
GLOBALBASE_PORT GB_MotherboardInfo GB_GetMotherboardInfo();

// 生成硬件 ID（基于 CPU 和主板信息）
GLOBALBASE_PORT std::string GB_GenerateHardwareId();

// 操作系统信息
struct GB_OsInfo
{
    std::string name;               // 例如"Microsoft Windows 11 专业版"
    std::string version;            // 例如"10.0.26200"
    std::string buildNumber;        // 例如"26200"
    std::string architecture;       // "x86_64"/"x86"/"arm64"/...
    std::string osArchitecture;     // 例如"64 位"
    std::string kernelName;         // Windows 固定填 "Windows NT"
    std::string kernelRelease;      // 可能为空
    std::string kernelVersion;      // 例如"10.0.26200"
    std::string hostname;           // 例如"DESKTOP-5GB9TLO"

    // Linux 生态常见字段
    std::string id;                 // /etc/os-release: ID
    std::string idLike;             // /etc/os-release: ID_LIKE
    std::string codename;           // /etc/os-release: VERSION_CODENAME

    GLOBALBASE_PORT std::string Serialize() const;
};

// 获取当前操作系统信息
GLOBALBASE_PORT GB_OsInfo GB_GetOsInfo();

// 获取 Windows 当前用户环境变量（用户变量）
// - 仅在 Windows 上有效；非 Windows 平台返回空 map。
// - 返回 <变量名, 变量值>，均为 UTF-8。
GLOBALBASE_PORT std::unordered_map<std::string, std::string> GB_GetWindowsUserEnvironmentVariables();

// 获取 Windows 系统环境变量（系统变量）
// - 仅在 Windows 上有效；非 Windows 平台返回空 map。
// - 返回 <变量名, 变量值>，均为 UTF-8。
GLOBALBASE_PORT std::unordered_map<std::string, std::string> GB_GetWindowsSystemEnvironmentVariables();












#endif