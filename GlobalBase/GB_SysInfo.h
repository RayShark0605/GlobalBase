#ifndef GLOBALBASE_SYSINFO_H_H
#define GLOBALBASE_SYSINFO_H_H

#include <string>
#include <vector>
#include <cstdint>
#include "GlobalBasePort.h"

// CPU 信息
struct CpuInfo
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

GLOBALBASE_PORT CpuInfo GetCpuInfo();

















#endif