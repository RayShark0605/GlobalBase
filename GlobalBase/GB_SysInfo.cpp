#include "GB_SysInfo.h"
#include "GB_Utf8String.h"
#include <string>
#include <vector>
#include <stdint.h>
#include <algorithm>
#include <set>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <powerbase.h>
#include <powrprof.h>
#pragma comment(lib, "PowrProf.lib")
// 某些 SDK 里可能没有这个结构体——做个兼容兜底
#ifndef PROCESSOR_POWER_INFORMATION_DEFINED
#define PROCESSOR_POWER_INFORMATION_DEFINED
typedef struct _PROCESSOR_POWER_INFORMATION
{
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} PROCESSOR_POWER_INFORMATION, * PPROCESSOR_POWER_INFORMATION;
#endif
#include <intrin.h>        // __cpuid, __cpuidex
#include <wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")
#else
#include <unistd.h>        // sysconf
// 仅 Linux/Unix 系列需要 dirent.h
#include <dirent.h>        // opendir/readdir/closedir
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>     // __get_cpuid, __get_cpuid_max
#include <x86intrin.h> // _xgetbv
#endif
#endif

static std::string Trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, e - b + 1);
}

static std::string DetectArchitecture()
{
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(__aarch64__)
    return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

#if defined(_WIN32)

// 简易 RAII
struct ComInit
{
    HRESULT hr = E_FAIL;
    ComInit()
    {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr))
        {
            CoInitializeSecurity(
                nullptr,
                -1,
                nullptr,
                nullptr,
                RPC_C_AUTHN_LEVEL_DEFAULT,
                RPC_C_IMP_LEVEL_IMPERSONATE,
                nullptr,
                EOAC_NONE,
                nullptr);
        }
    }
    ~ComInit()
    {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

// WMI 读取 Win32_Processor.ProcessorId（首个处理器）
static std::string GetProcessorIdViaWmi()
{
    ComInit com;
    if (FAILED(com.hr)) return {};

    IWbemLocator* pLoc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (LPVOID*)&pLoc)))
    {
        return {};
    }

    IWbemServices* pSvc = nullptr;
    HRESULT hr = pLoc->ConnectServer(
        BSTR(L"ROOT\\CIMV2"), nullptr, nullptr, 0, 0, 0, 0, &pSvc);

    pLoc->Release();
    if (FAILED(hr)) return {};

    // 设置代理安全
    CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE);

    IEnumWbemClassObject* pEnum = nullptr;
    hr = pSvc->ExecQuery(BSTR(L"WQL"),
        BSTR(L"SELECT ProcessorId FROM Win32_Processor"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &pEnum);

    if (FAILED(hr))
    {
        pSvc->Release();
        return {};
    }

    std::string id;
    IWbemClassObject* pObj = nullptr;
    ULONG ret = 0;
    if (SUCCEEDED(pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret)) && ret == 1)
    {
        VARIANT vt;
        VariantInit(&vt);
        if (SUCCEEDED(pObj->Get(L"ProcessorId", 0, &vt, nullptr, nullptr)) && vt.vt == VT_BSTR)
        {
            // 转成 UTF-8
            int len = WideCharToMultiByte(CP_UTF8, 0, vt.bstrVal, -1, nullptr, 0, nullptr, nullptr);
            std::string utf8(len ? (len - 1) : 0, '\0');
            if (len > 1)
            {
                WideCharToMultiByte(CP_UTF8, 0, vt.bstrVal, -1, &utf8[0], len, nullptr, nullptr);
            }
            id = utf8;
        }
        VariantClear(&vt);
        pObj->Release();
    }
    pEnum->Release();
    pSvc->Release();
    return id;
}

// 统计逻辑核/物理核/封装/NUMA
static void QueryWindowsTopology(uint32_t& logical, uint32_t& cores, uint32_t& packages, uint32_t& numa)
{
    logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);

    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
    std::vector<uint8_t> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
    {
        cores = packages = 0;
        // NUMA 回退
        ULONG highest = 0;
        if (GetNumaHighestNodeNumber(&highest))
        {
            numa = highest + 1;
        }
        else
        {
            numa = 1;
        }
        return;
    }

    cores = packages = 0;
    numa = 0;

    uint8_t* ptr = buf.data();
    uint8_t* end = buf.data() + len;
    while (ptr < end)
    {
        auto info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
        switch (info->Relationship)
        {
        case RelationProcessorCore:
            cores++;
            break;
        case RelationProcessorPackage:
            packages++;
            break;
        case RelationNumaNode:
#if defined(NTDDI_WIN10_RS3) && NTDDI_VERSION >= NTDDI_WIN10_RS3
        case RelationNumaNodeEx:
#endif
            numa++;
            break;
        default:
            break;
        }
        ptr += info->Size;
    }

    if (numa == 0)
    {
        ULONG highest = 0;
        numa = GetNumaHighestNodeNumber(&highest) ? (highest + 1) : 1;
    }
}

// 通过 CallNtPowerInformation 拿频率（MHz）
static void QueryWindowsFrequencies(uint64_t& baseHz, uint64_t& maxHz)
{
    baseHz = maxHz = 0;

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    DWORD n = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 64;

    std::vector<PROCESSOR_POWER_INFORMATION> ppi(n);
    ULONG outSize = static_cast<ULONG>(sizeof(PROCESSOR_POWER_INFORMATION) * ppi.size());

    // 注意：这里是 ProcessorInformation（不是 SystemProcessorInformation）
    NTSTATUS st = CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi.data(), outSize);
    if (st != 0)
    {
        return;
    }

    uint32_t baseMhzObserved = 0;
    uint32_t maxMhzObserved = 0;
    for (size_t i = 0; i < ppi.size(); ++i)
    {
        baseMhzObserved = std::max((uint32_t)baseMhzObserved, (uint32_t)ppi[i].MaxMhz);
        maxMhzObserved = std::max((uint32_t)maxMhzObserved, std::max((uint32_t)ppi[i].MaxMhz, (uint32_t)ppi[i].MhzLimit));
    }
    baseHz = static_cast<uint64_t>(baseMhzObserved) * 1000000ULL;
    maxHz = static_cast<uint64_t>(maxMhzObserved) * 1000000ULL;
}

// x86: 读取 vendor/brand/feature 与 hypervisor bit
static void QueryX86Cpuid(std::string& vendor, std::string& brand,
    bool& hypervisor, std::vector<std::string>& features)
{
    vendor.clear();
    brand.clear();
    hypervisor = false;
    features.clear();

    int regs[4] = { 0 };
    __cpuid(regs, 0);
    char ven[13] = { 0 };
    *reinterpret_cast<int*>(ven + 0) = regs[1]; // EBX
    *reinterpret_cast<int*>(ven + 4) = regs[3]; // EDX
    *reinterpret_cast<int*>(ven + 8) = regs[2]; // ECX
    vendor = ven;

    int maxExt[4] = { 0 };
    __cpuid(maxExt, 0x80000000);
    if (static_cast<unsigned>(maxExt[0]) >= 0x80000004)
    {
        char buf[49] = { 0 };
        int* p = reinterpret_cast<int*>(buf);
        __cpuid(p, 0x80000002);
        __cpuid(p + 4, 0x80000003);
        __cpuid(p + 8, 0x80000004);
        brand = Trim(std::string(buf));
    }

    int leaf1[4] = { 0 };
    __cpuid(leaf1, 1);
    // 超级管理程序存在位：Leaf1 ECX bit31
    hypervisor = !!(leaf1[2] & (1 << 31));

    // 基本特性
    auto add = [&](bool cond, const char* name)
        {
            if (cond) features.emplace_back(name);
        };
    unsigned ecx = static_cast<unsigned>(leaf1[2]);
    unsigned edx = static_cast<unsigned>(leaf1[3]);
    add(edx & (1u << 25), "sse");
    add(edx & (1u << 26), "sse2");
    add(ecx & (1u << 0), "sse3");
    add(ecx & (1u << 9), "ssse3");
    add(ecx & (1u << 19), "sse4_1");
    add(ecx & (1u << 20), "sse4_2");
    add(ecx & (1u << 25), "aes");
    add(ecx & (1u << 23), "popcnt");
    add(ecx & (1u << 12), "fma");

    // AVX/AVX2/AVX-512 需 OSXSAVE + XGETBV
    bool osxsave = (ecx & (1u << 27)) != 0;
    unsigned long long xcr0 = 0;
#if defined(_XCR_XFEATURE_ENABLED_MASK)
    if (osxsave) { xcr0 = _xgetbv(_XCR_XFEATURE_ENABLED_MASK); }
#else
    if (osxsave) { xcr0 = _xgetbv(0); }
#endif
    bool ymmEnabled = osxsave && ((xcr0 & 0x6) == 0x6); // XMM|YMM
    add(ymmEnabled && (ecx & (1u << 28)), "avx");

    int leaf7[4] = { 0 };
    __cpuidex(leaf7, 7, 0);
    unsigned ebx7 = static_cast<unsigned>(leaf7[1]);
    add(ymmEnabled && (ebx7 & (1u << 5)), "avx2");
    // AVX-512 需要更高的 XCR0 位 (opmask,zmm_hi256,hi16_zmm)，粗略检测：
    bool zmmEnabled = osxsave && ((xcr0 & 0xE6) == 0xE6);
    add(zmmEnabled && (ebx7 & (1u << 16)), "avx512f");
    add(ebx7 & (1u << 3), "bmi1");
    add(ebx7 & (1u << 8), "bmi2");
}

#endif // _WIN32

#if !defined(_WIN32)
// 读文本首行
static bool ReadFirstLine(const std::string& path, std::string& out)
{
    std::ifstream ifs(path.c_str());
    if (!ifs) return false;
    std::getline(ifs, out);
    out = Trim(out);
    return true;
}

static bool ReadULL(const std::string& path, uint64_t& val)
{
    std::string s;
    if (!ReadFirstLine(path, s)) return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long x = strtoull(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str()) return false;
    val = static_cast<uint64_t>(x);
    return true;
}

// Linux: 解析 /proc/cpuinfo 重要字段
struct ProcCpuInfo
{
    std::string vendor;
    std::string modelName;
    std::vector<std::string> flags;
    std::map<std::string, std::string> kv; // 备用
};

static ProcCpuInfo ParseProcCpuinfo()
{
    ProcCpuInfo info;
    std::ifstream ifs("/proc/cpuinfo");
    std::string line;
    while (std::getline(ifs, line))
    {
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string k = Trim(line.substr(0, pos));
        std::string v = Trim(line.substr(pos + 1));
        if (k == "vendor_id" || k == "Vendor")
        {
            info.vendor = v;
        }
        else if (k == "model name" || k == "Processor")
        {
            if (info.modelName.empty()) info.modelName = v;
        }
        else if (k == "flags" || k == "Features")
        {
            std::istringstream iss(v);
            std::string f;
            while (iss >> f) info.flags.push_back(f);
        }
        info.kv[k] = v;
    }
    return info;
}

// Linux: 枚举 cpuX 统计拓扑
static void QueryLinuxTopology(uint32_t& logical, uint32_t& cores, uint32_t& packages, uint32_t& numa)
{
    // 逻辑核（在线）
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    logical = (n > 0) ? static_cast<uint32_t>(n) : 0;

    // sysfs topology
    std::set<int> packageSet;
    std::set<std::pair<int, int>> coreSet;

    DIR* dir = opendir("/sys/devices/system/cpu");
    if (dir)
    {
        struct dirent* ent = nullptr;
        while ((ent = readdir(dir)) != nullptr)
        {
            if (std::strncmp(ent->d_name, "cpu", 3) != 0) continue;
            char* end = nullptr;
            long idx = std::strtol(ent->d_name + 3, &end, 10);
            if (end == ent->d_name + 3) continue;

            std::string base = std::string("/sys/devices/system/cpu/") + ent->d_name + "/topology/";
            std::string pathPkg = base + "physical_package_id";
            std::string pathCore = base + "core_id";
            uint64_t pkg = 0, cid = 0;
            std::string s;
            if (ReadFirstLine(pathPkg, s)) pkg = std::strtol(s.c_str(), nullptr, 10);
            if (ReadFirstLine(pathCore, s)) cid = std::strtol(s.c_str(), nullptr, 10);
            packageSet.insert(static_cast<int>(pkg));
            coreSet.insert({ static_cast<int>(pkg), static_cast<int>(cid) });
        }
        closedir(dir);
    }

    packages = packageSet.empty() ? 0u : static_cast<uint32_t>(packageSet.size());
    cores = coreSet.empty() ? 0u : static_cast<uint32_t>(coreSet.size());

    // NUMA 节点
    uint32_t nodes = 0;
    DIR* nd = opendir("/sys/devices/system/node");
    if (nd)
    {
        struct dirent* e = nullptr;
        while ((e = readdir(nd)) != nullptr)
        {
            if (std::strncmp(e->d_name, "node", 4) != 0) continue;
            char* end = nullptr;
            (void)std::strtol(e->d_name + 4, &end, 10);
            if (end != e->d_name + 4) nodes++;
        }
        closedir(nd);
    }
    numa = nodes ? nodes : 1;
}

// Linux: 频率（Hz）
static void QueryLinuxFrequencies(uint64_t& baseHz, uint64_t& maxHz)
{
    baseHz = maxHz = 0;

    // cpufreq 单位为 kHz
    uint64_t kHz = 0;
    // 优先 base_frequency（如有）
    if (ReadULL("/sys/devices/system/cpu/cpu0/cpufreq/base_frequency", kHz))
    {
        baseHz = kHz * 1000ULL;
    }
    // 最大频率
    if (ReadULL("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", kHz))
    {
        maxHz = kHz * 1000ULL;
        if (baseHz == 0) baseHz = maxHz; // 无 base 则退化为 max
    }

    // 兜底（无 cpufreq）：尝试 /proc/cpuinfo 的 “cpu MHz”
    if (baseHz == 0 || maxHz == 0)
    {
        std::ifstream ifs("/proc/cpuinfo");
        std::string line;
        double mhz = 0.0;
        while (std::getline(ifs, line))
        {
            if (line.find("cpu MHz") != std::string::npos)
            {
                auto pos = line.find(':');
                if (pos != std::string::npos)
                {
                    mhz = atof(line.substr(pos + 1).c_str());
                    break;
                }
            }
        }
        if (mhz > 0.0)
        {
            uint64_t hz = static_cast<uint64_t>(mhz * 1000000.0);
            if (baseHz == 0) baseHz = hz;
            if (maxHz == 0)  maxHz = hz;
        }
    }
}

// x86 上用 CPUID 检查 hypervisor bit（若可用）
static bool QueryLinuxX86Hypervisor()
{
#if defined(__x86_64__) || defined(__i386__)
    unsigned eax, ebx, ecx, edx;
    if (__get_cpuid_max(0, nullptr) == 0) return false;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    return (ecx & (1u << 31)) != 0;
#else
    return false;
#endif
}

#endif // ! _WIN32

CpuInfo GetCpuInfo()
{
    CpuInfo info;
    info.architecture = DetectArchitecture();

#if defined(_WIN32)
    // x86/x64: CPUID
#if defined(_M_IX86) || defined(_M_X64)
    QueryX86Cpuid(info.vendor, info.brand, info.hypervisorPresent, info.features);
#endif

    // 拓扑
    QueryWindowsTopology(info.logicalCpus, info.physicalCores, info.packageCount, info.numaNodes);

    // 频率
    QueryWindowsFrequencies(info.baseFrequencyHz, info.maxFrequencyHz);

    // ProcessorId（非严格序列号）
    info.processorId = GetProcessorIdViaWmi();

    // cpuSerial：Windows x86 基本没有可读序列号，保持为空
    info.cpuSerial = std::string();

#else
    // ---------- Linux ----------
    // 解析 /proc/cpuinfo
    ProcCpuInfo pci = ParseProcCpuinfo();
    if (!pci.vendor.empty()) info.vendor = pci.vendor;
    if (!pci.modelName.empty()) info.brand = pci.modelName;
    info.features = pci.flags; // 直接填 flags

    // hypervisor：x86 用 ECX bit31 检测
    info.hypervisorPresent = QueryLinuxX86Hypervisor();

    // 拓扑
    QueryLinuxTopology(info.logicalCpus, info.physicalCores, info.packageCount, info.numaNodes);

    // 频率
    QueryLinuxFrequencies(info.baseFrequencyHz, info.maxFrequencyHz);

    // Linux 上的 ProcessorId / cpuSerial：
    // 1) x86 普遍没有；2) 某些 ARM 平台 /proc/cpuinfo 有 "Serial"
    auto it = pci.kv.find("Serial");
    if (it != pci.kv.end())
    {
        info.cpuSerial = it->second;
    }
#endif

    // 收尾：若 vendor/brand 仍为空，尽量给出退化信息
    if (info.vendor.empty()) info.vendor = "Unknown";
    if (info.brand.empty())  info.brand = "Unknown";
    if (info.numaNodes == 0) info.numaNodes = 1;

    return info;
}