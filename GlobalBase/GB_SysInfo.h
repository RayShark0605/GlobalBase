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

// Windows 环境变量操作类（仅作命名空间使用）
//
// 说明：
// - 仅在 Windows 下有实际效果；在非 Windows 平台下，各接口返回空/false。
// - 读接口返回的 std::string 均为 UTF-8 编码。
// - “用户变量/系统变量”分别对应 Windows 环境变量面板里的两组变量。
// - Set* 接口会写入注册表并广播 WM_SETTINGCHANGE 以通知系统刷新环境变量（可能需要管理员权限）。
class GLOBALBASE_PORT GB_WindowsEnvVarOperator
{
public:
    // 用户环境变量操作（HKCU\Environment）
    class GLOBALBASE_PORT UserEnvVarOperator
    {
    public:
        /**
         * @brief 获取当前用户的全部环境变量（用户变量）。
         *
         * @return std::unordered_map<std::string, std::string>
         *         - key   : 变量名（UTF-8）
         *         - value : 变量值（UTF-8）
         *
         * 备注：
         * - 若注册表值类型为 REG_EXPAND_SZ，则会对其中的“%VAR%”进行展开并返回展开后的值。
         */
        static std::unordered_map<std::string, std::string> GetAllUserEnvironmentVariables();

        /**
         * @brief 获取单个用户环境变量的值（用户变量）。
         *
         * @param varNameUtf8    变量名（UTF-8）
         * @param outValueUtf8   [out] 变量值（UTF-8）
         * @return true=存在并成功获取；false=不存在或失败。
         */
        static bool GetUserEnvironmentVariable(const std::string& varNameUtf8, std::string* outValueUtf8);

        /**
         * @brief 设置单个用户环境变量（用户变量），写入注册表并广播环境变量变更通知。
         *
         * @param varNameUtf8 变量名（UTF-8）
         * @param valueUtf8   变量值（UTF-8）
         * @return true=成功；false=失败。
         *
         * 备注：
         * - 若变量值中包含 '%' 字符，默认使用 REG_EXPAND_SZ 写入；否则使用 REG_SZ。
         * - 若目标键写入需要权限（通常用户变量不需要），失败会返回 false。
         */
        static bool SetUserEnvironmentVariable(const std::string& varNameUtf8, const std::string& valueUtf8);

        // PATH（用户 Path 变量）便捷操作
        class GLOBALBASE_PORT UserPathOperator
        {
        public:
            /**
             * @brief 获取用户 Path 变量（注册表中的 Path 值）拆分后的条目列表。
             *
             * @return std::vector<std::string> 每个条目为 UTF-8 字符串，顺序与注册表一致。
             */
            static std::vector<std::string> GetUserPathEntries();

            /**
             * @brief 判断用户 Path 是否包含指定条目。
             *
             * @param entryUtf8 Path 条目（UTF-8），不需要带 ';'。
             * @return true=包含；false=不包含或失败。
             *
             * 备注：比较时会做基础归一化（去空白/去引号/路径分隔符统一/大小写不敏感）。
             */
            static bool HasUserPathEntry(const std::string& entryUtf8);

            /**
             * @brief 向用户 Path 中添加一个条目（若已存在则不重复添加）。
             *
             * @param entryUtf8 Path 条目（UTF-8），不需要带 ';'。
             * @param append    true=追加到末尾；false=插入到开头。
             * @return true=成功；false=失败。
             */
            static bool AddUserPathEntry(const std::string& entryUtf8, bool append);

            /**
             * @brief 从用户 Path 中移除一个条目（若不存在则视为成功）。
             *
             * @param entryUtf8 Path 条目（UTF-8），不需要带 ';'。
             * @return true=成功；false=失败。
             */
            static bool RemoveUserPathEntry(const std::string& entryUtf8);
        };
    };
    
    // 系统环境变量操作（HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment）
    class GLOBALBASE_PORT SystemEnvVarOperator
    {
    public:
        /**
         * @brief 获取系统的全部环境变量（系统变量）。
         *
         * @return std::unordered_map<std::string, std::string> 变量名->变量值（均为 UTF-8）
         *
         * 备注：若注册表值类型为 REG_EXPAND_SZ，则会对其中的“%VAR%”进行展开并返回展开后的值。
         */
        static std::unordered_map<std::string, std::string> GetAllSystemEnvironmentVariables();

        /**
         * @brief 获取单个系统环境变量的值（系统变量）。
         *
         * @param varNameUtf8    变量名（UTF-8）
         * @param outValueUtf8   [out] 变量值（UTF-8）
         * @return true=存在并成功获取；false=不存在或失败。
         */
        static bool GetSystemEnvironmentVariable(const std::string& varNameUtf8, std::string* outValueUtf8);

        /**
         * @brief 设置单个系统环境变量（系统变量），写入注册表并广播环境变量变更通知（通常需要管理员权限）。
         *
         * @param varNameUtf8 变量名（UTF-8）
         * @param valueUtf8   变量值（UTF-8）
         * @return true=成功；false=失败（常见原因：无管理员权限）。
         */
        static bool SetSystemEnvironmentVariable(const std::string& varNameUtf8, const std::string& valueUtf8);

        // PATH（系统 Path 变量）便捷操作
        class GLOBALBASE_PORT SystemPathOperator
        {
        public:
            /**
             * @brief 获取系统 Path 变量（注册表中的 Path 值）拆分后的条目列表。
             *
             * @return std::vector<std::string> 每个条目为 UTF-8 字符串，顺序与注册表一致。
             */
            static std::vector<std::string> GetSystemPathEntries();

            /**
             * @brief 判断系统 Path 是否包含指定条目。
             *
             * @param entryUtf8 Path 条目（UTF-8），不需要带 ';'。
             * @return true=包含；false=不包含或失败。
             */
            static bool HasSystemPathEntry(const std::string& entryUtf8);

            /**
             * @brief 向系统 Path 中添加一个条目（若已存在则不重复添加，通常需要管理员权限）。
             *
             * @param entryUtf8 Path 条目（UTF-8），不需要带 ';'。
             * @param append    true=追加到末尾；false=插入到开头。
             * @return true=成功；false=失败。
             */
            static bool AddSystemPathEntry(const std::string& entryUtf8, bool append);

            /**
             * @brief 从系统 Path 中移除一个条目（若不存在则视为成功，通常需要管理员权限）。
             *
             * @param entryUtf8 Path 条目（UTF-8），不需要带 ';'。
             * @return true=成功；false=失败。
             */
            static bool RemoveSystemPathEntry(const std::string& entryUtf8);
        };
    };
};


#endif