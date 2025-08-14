#include "GB_SysInfo.h"
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
} PROCESSOR_POWER_INFORMATION, *PPROCESSOR_POWER_INFORMATION;
#endif
#include <intrin.h>        // __cpuid, __cpuidex
#include <wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")
#else
#include <unistd.h>        // sysconf
#include <sys/utsname.h>
// 仅 Linux/Unix 系列需要 dirent.h
#include <dirent.h>        // opendir/readdir/closedir
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>     // __get_cpuid, __get_cpuid_max
#include <x86intrin.h> // _xgetbv
#endif
#endif
#include "GB_Crypto.h"

using namespace std;

namespace internal
{
    static string Trim(const string& s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        if (b == string::npos) return {};
        return s.substr(b, e - b + 1);
    }

    static string DetectArchitecture()
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
                CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
            }
        }
        ~ComInit()
        {
            if (SUCCEEDED(hr))
            {
                CoUninitialize();
            }
        }
    };

    // WMI 读取 Win32_Processor.ProcessorId（首个处理器）
    static string GetProcessorIdViaWmi()
    {
        ComInit com;
        if (FAILED(com.hr))
        {
            return {};
        }

        IWbemLocator* pLoc = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc)))
        {
            return {};
        }

        IWbemServices* pSvc = nullptr;
        HRESULT hr = pLoc->ConnectServer(BSTR(L"ROOT\\CIMV2"), nullptr, nullptr, 0, 0, 0, 0, &pSvc);

        pLoc->Release();
        if (FAILED(hr))
        {
            return {};
        }

        // 设置代理安全
        CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

        IEnumWbemClassObject* pEnum = nullptr;
        hr = pSvc->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT ProcessorId FROM Win32_Processor"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
        if (FAILED(hr))
        {
            pSvc->Release();
            return {};
        }

        string id;
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
                string utf8(len ? (len - 1) : 0, '\0');
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
        vector<uint8_t> buf(len);
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
        const DWORD n = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 64;

        vector<PROCESSOR_POWER_INFORMATION> ppi(n);
        const ULONG outSize = static_cast<ULONG>(sizeof(PROCESSOR_POWER_INFORMATION) * ppi.size());
        const NTSTATUS st = CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi.data(), outSize);
        if (st != 0)
        {
            return;
        }

        uint32_t baseMhzObserved = 0;
        uint32_t maxMhzObserved = 0;
        for (size_t i = 0; i < ppi.size(); ++i)
        {
            baseMhzObserved = max((uint32_t)baseMhzObserved, (uint32_t)ppi[i].MaxMhz);
            maxMhzObserved = max((uint32_t)maxMhzObserved, max((uint32_t)ppi[i].MaxMhz, (uint32_t)ppi[i].MhzLimit));
        }
        baseHz = static_cast<uint64_t>(baseMhzObserved) * 1000000ULL;
        maxHz = static_cast<uint64_t>(maxMhzObserved) * 1000000ULL;
    }

    // x86: 读取 vendor/brand/feature 与 hypervisor bit
    static void QueryX86Cpuid(string& vendor, string& brand, bool& hypervisor, vector<string>& features)
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
            brand = Trim(string(buf));
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
        const unsigned ecx = static_cast<unsigned>(leaf1[2]);
        const unsigned edx = static_cast<unsigned>(leaf1[3]);
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
        const bool osxsave = (ecx & (1u << 27)) != 0;
        unsigned long long xcr0 = 0;
#if defined(_XCR_XFEATURE_ENABLED_MASK)
        if (osxsave) { xcr0 = _xgetbv(_XCR_XFEATURE_ENABLED_MASK); }
#else
        if (osxsave) { xcr0 = _xgetbv(0); }
#endif
        const bool ymmEnabled = osxsave && ((xcr0 & 0x6) == 0x6); // XMM|YMM
        add(ymmEnabled && (ecx & (1u << 28)), "avx");

        int leaf7[4] = { 0 };
        __cpuidex(leaf7, 7, 0);
        const unsigned ebx7 = static_cast<unsigned>(leaf7[1]);
        add(ymmEnabled && (ebx7 & (1u << 5)), "avx2");
        // AVX-512 需要更高的 XCR0 位 (opmask,zmm_hi256,hi16_zmm)，粗略检测：
        const bool zmmEnabled = osxsave && ((xcr0 & 0xE6) == 0xE6);
        add(zmmEnabled && (ebx7 & (1u << 16)), "avx512f");
        add(ebx7 & (1u << 3), "bmi1");
        add(ebx7 & (1u << 8), "bmi2");
    }

#endif // _WIN32

#if !defined(_WIN32)
    // 读文本首行
    static bool ReadFirstLine(const string& path, string& out)
    {
        ifstream ifs(path.c_str());
        if (!ifs)
        {
            return false;
        }
        getline(ifs, out);
        out = Trim(out);
        return true;
    }

    static bool ReadULL(const string& path, uint64_t& val)
    {
        string s;
        if (!ReadFirstLine(path, s))
        {
            return false;
        }
        char* end = nullptr;
        errno = 0;
        const unsigned long long x = strtoull(s.c_str(), &end, 10);
        if (errno != 0 || end == s.c_str())
        {
            return false;
        }
        val = static_cast<uint64_t>(x);
        return true;
    }

    // Linux: 解析 /proc/cpuinfo 重要字段
    struct ProcCpuInfo
    {
        string vendor;
        string modelName;
        vector<string> flags;
        map<string, string> kv; // 备用
    };

    static ProcCpuInfo ParseProcCpuinfo()
    {
        ProcCpuInfo info;
        ifstream ifs("/proc/cpuinfo");
        string line;
        while (getline(ifs, line))
        {
            const auto pos = line.find(':');
            if (pos == string::npos)
            {
                continue;
            }
            string k = Trim(line.substr(0, pos));
            string v = Trim(line.substr(pos + 1));
            if (k == "vendor_id" || k == "Vendor")
            {
                info.vendor = v;
            }
            else if (k == "model name" || k == "Processor")
            {
                if (info.modelName.empty())
                {
                    info.modelName = v;
                }
            }
            else if (k == "flags" || k == "Features")
            {
                istringstream iss(v);
                string f;
                while (iss >> f)
                {
                    info.flags.push_back(f);
                }
            }
            info.kv[k] = v;
        }
        return info;
    }

    // Linux: 枚举 cpuX 统计拓扑
    static void QueryLinuxTopology(uint32_t& logical, uint32_t& cores, uint32_t& packages, uint32_t& numa)
    {
        // 逻辑核（在线）
        const long n = sysconf(_SC_NPROCESSORS_ONLN);
        logical = (n > 0) ? static_cast<uint32_t>(n) : 0;

        // sysfs topology
        set<int> packageSet;
        set<pair<int, int>> coreSet;

        DIR* dir = opendir("/sys/devices/system/cpu");
        if (dir)
        {
            struct dirent* ent = nullptr;
            while ((ent = readdir(dir)) != nullptr)
            {
                if (strncmp(ent->d_name, "cpu", 3) != 0)
                {
                    continue;
                }
                char* end = nullptr;
                long idx = strtol(ent->d_name + 3, &end, 10);
                if (end == ent->d_name + 3)
                {
                    continue;
                }

                const string base = string("/sys/devices/system/cpu/") + ent->d_name + "/topology/";
                const string pathPkg = base + "physical_package_id";
                const string pathCore = base + "core_id";
                uint64_t pkg = 0, cid = 0;
                string s;
                if (ReadFirstLine(pathPkg, s))
                {
                    pkg = strtol(s.c_str(), nullptr, 10);
                }
                if (ReadFirstLine(pathCore, s))
                {
                    cid = strtol(s.c_str(), nullptr, 10);
                }
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
                if (strncmp(e->d_name, "node", 4) != 0)
                {
                    continue;
                }
                char* end = nullptr;
                (void)strtol(e->d_name + 4, &end, 10);
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
            if (baseHz == 0)
            {
                baseHz = maxHz; // 无 base 则退化为 max
            }
        }

        // 兜底（无 cpufreq）：尝试 /proc/cpuinfo 的 “cpu MHz”
        if (baseHz == 0 || maxHz == 0)
        {
            ifstream ifs("/proc/cpuinfo");
            string line;
            double mhz = 0;
            while (getline(ifs, line))
            {
                if (line.find("cpu MHz") != string::npos)
                {
                    const auto pos = line.find(':');
                    if (pos != string::npos)
                    {
                        mhz = atof(line.substr(pos + 1).c_str());
                        break;
                    }
                }
            }
            if (mhz > 0.0)
            {
                uint64_t hz = static_cast<uint64_t>(mhz * 1000000.0);
                if (baseHz == 0)
                {
                    baseHz = hz;
                }
                if (maxHz == 0)
                {
                    maxHz = hz;
                }
            }
        }
    }

    // x86 上用 CPUID 检查 hypervisor bit（若可用）
    static bool QueryLinuxX86Hypervisor()
    {
#if defined(__x86_64__) || defined(__i386__)
        unsigned eax, ebx, ecx, edx;
        if (__get_cpuid_max(0, nullptr) == 0)
        {
            return false;
        }
        if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        {
            return false;
        }
        return (ecx & (1u << 31)) != 0;
#else
        return false;
#endif
    }
#endif // ! _WIN32

    static string JoinStrings(const vector<string>& elems, char delim)
    {
        string result;
        for (const auto& e : elems)
        {
            if (!result.empty())
            {
                result += delim;
            }
            result += e;
        }
        return result;
    }

    static string JoinKeyValue(const string& k, const string& v)
    {
        return k + "=" + v + ";";
    }

#if defined(_WIN32)
    // 将 BSTR 转 UTF-8（安全返回空串）
    static string BstrToUtf8(BSTR b)
    {
        if (!b)
        {
            return {};
        }
        const int len = ::WideCharToMultiByte(CP_UTF8, 0, b, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1)
        {
            return {};
        }
        string out(static_cast<size_t>(len - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, b, -1, &out[0], len, nullptr, nullptr);
        return out;
    }

    // 执行 WQL 并取首条记录里若干字符串属性（不存在则留空）
    static bool WmiQueryFirstRowStrings(const wchar_t* wql, const vector<wstring>& props, vector<string>& outStrings)
    {
        outStrings.clear();

        ComInit com;
        if (FAILED(com.hr))
        {
            return false;
        }

        IWbemLocator* pLoc = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc)))
        {
            return false;
        }

        IWbemServices* pSvc = nullptr;
        HRESULT hr = pLoc->ConnectServer(BSTR(L"ROOT\\CIMV2"), nullptr, nullptr, 0, 0, 0, 0, &pSvc);
        pLoc->Release();
        if (FAILED(hr))
        {
            return false;
        }

        CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

        IEnumWbemClassObject* pEnum = nullptr;
        hr = pSvc->ExecQuery(BSTR(L"WQL"), BSTR(wql), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
        if (FAILED(hr))
        {
            pSvc->Release();
            return false;
        }

        IWbemClassObject* pObj = nullptr;
        ULONG ret = 0;
        bool ok = false;

        if (SUCCEEDED(pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret)) && ret == 1)
        {
            outStrings.reserve(props.size());
            for (size_t i = 0; i < props.size(); i++)
            {
                VARIANT vt;
                VariantInit(&vt);
                string v;
                if (SUCCEEDED(pObj->Get(props[i].c_str(), 0, &vt, nullptr, nullptr)))
                {
                    if (vt.vt == VT_BSTR)
                    {
                        v = BstrToUtf8(vt.bstrVal);
                    }
                    else if (vt.vt == (VT_ARRAY | VT_BSTR))
                    {
                        // 某些属性可能是 BSTR 数组：拼接为逗号分隔
                        SAFEARRAY* psa = vt.parray;
                        LONG l = 0, u = -1;
                        SafeArrayGetLBound(psa, 1, &l);
                        SafeArrayGetUBound(psa, 1, &u);
                        string cat;
                        for (LONG i2 = l; i2 <= u; i2++)
                        {
                            BSTR bs = nullptr;
                            if (SUCCEEDED(SafeArrayGetElement(psa, &i2, &bs)) && bs)
                            {
                                if (!cat.empty()) cat += ",";
                                cat += BstrToUtf8(bs);
                                SysFreeString(bs);
                            }
                        }
                        v = cat;
                    }
                }
                VariantClear(&vt);
                outStrings.push_back(v);
            }
            pObj->Release();
            ok = true;
        }

        pEnum->Release();
        pSvc->Release();
        return ok;
    }
#else
    // POSIX: 读整个文件，去掉尾部的 '\0' 与空白（有些设备树属性以 NUL 结尾）
    static string ReadFileTrimAll(const string& path)
    {
        ifstream ifs(path.c_str(), ios::binary);
        if (!ifs)
        {
            return {};
        }
        string s((istreambuf_iterator<char>(ifs)), istreambuf_iterator<char>());
        // 去除尾部 NUL
        while (!s.empty() && s.back() == '\0')
        {
            s.pop_back();
        }
        // 去除常见空白
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        if (b == string::npos)
        {
            return {};
        }
        return s.substr(b, e - b + 1);
    }

    static string ReadFirstLineTrim(const string& path)
    {
        ifstream ifs(path.c_str());
        if (!ifs)
        {
            return {};
        }
        string line;
        getline(ifs, line);
        return internal::Trim(line);
    }

    // 读取 DMI 文件（/sys/class/dmi/id），优先首行，失败则 whole-file
    static string ReadDmi(const string& name)
    {
        const string path = string("/sys/class/dmi/id/") + name;
        string s = ReadFirstLineTrim(path);
        if (!s.empty())
        {
            return s;
        }
        return ReadFileTrimAll(path);
    }

    // 设备树回退（常见路径：/proc/device-tree 与 /sys/firmware/devicetree/base）
    static string ReadDeviceTree(const string& relPath)
    {
        string s = ReadFileTrimAll(string("/proc/device-tree/") + relPath);
        if (!s.empty())
        {
            return s;
        }
        return ReadFileTrimAll(string("/sys/firmware/devicetree/base/") + relPath);
    }
#endif

    // --- Linux: 解析 os-release ---
    // 参考规范：/etc/os-release 或 /usr/lib/os-release
    // Key=Value（Value 可带双引号；可能含转义），本实现做常用健壮解析。
    static bool ParseOsRelease(map<string, string>& kv)
    {
        auto parseFile = [&](const string& path) -> bool
            {
                ifstream ifs(path.c_str());
                if (!ifs)
                {
                    return false;
                }
                string line;
                while (getline(ifs, line))
                {
                    // 去掉注释
                    const size_t hashPos = line.find('#');
                    if (hashPos != string::npos)
                    {
                        line = line.substr(0, hashPos);
                    }
                    line = Trim(line);
                    if (line.empty())
                    {
                        continue;
                    }
                    const size_t eq = line.find('=');
                    if (eq == string::npos)
                    {
                        continue;
                    }
                    string key = Trim(line.substr(0, eq));
                    string val = Trim(line.substr(eq + 1));

                    // 去除包裹引号；支持常见的 "..." 与 '...'
                    if (!val.empty() && (val.front() == '"' || val.front() == '\'') && val.back() == val.front())
                    {
                        val = val.substr(1, val.size() - 2);
                    }
                    kv[key] = val;
                }
                return !kv.empty();
            };

        // 规范推荐优先 /etc/os-release，失败再试 /usr/lib/os-release
        if (parseFile("/etc/os-release"))
        {
            return true;
        }
        return parseFile("/usr/lib/os-release");
    }

    // --- Linux: uname ---
    static void QueryUname(string& sysname, string& nodename, string& release, string& version, string& machine)
    {
        sysname.clear();
        nodename.clear();
        release.clear();
        version.clear();
        machine.clear();

#if !defined(_WIN32)
        struct utsname u {};
        if (uname(&u) == 0)
        {
            sysname = u.sysname;
            nodename = u.nodename;
            release = u.release;
            version = u.version;
            machine = u.machine;
        }
#endif
    }

}

string CpuInfo::Serialize() const
{
    string s;
    s += "vendor=" + vendor + ";";
    s += "brand=" + brand + ";";
    s += "architecture=" + architecture + ";";
    s += "logicalCpus=" + to_string(logicalCpus) + ";";
    s += "physicalCores=" + to_string(physicalCores) + ";";
    s += "packageCount=" + to_string(packageCount) + ";";
    s += "numaNodes=" + to_string(numaNodes) + ";";
    s += "baseFrequencyHz=" + to_string(baseFrequencyHz) + ";";
    s += "maxFrequencyHz=" + to_string(maxFrequencyHz) + ";";
    s += "processorId=" + processorId + ";";
    s += "cpuSerial=" + cpuSerial + ";";
    if (hypervisorPresent)
    {
        s += "hypervisorPresent=1;";
    }
    else
    {
		s += "hypervisorPresent=0;";
    }
    s += "features=" + internal::JoinStrings(features, ',');
    return s;
}

CpuInfo GetCpuInfo()
{
    CpuInfo info;
    info.architecture = internal::DetectArchitecture();

#if defined(_WIN32)
    // x86/x64: CPUID
#if defined(_M_IX86) || defined(_M_X64)
    internal::QueryX86Cpuid(info.vendor, info.brand, info.hypervisorPresent, info.features);
#endif

    // 拓扑
    internal::QueryWindowsTopology(info.logicalCpus, info.physicalCores, info.packageCount, info.numaNodes);

    // 频率
    internal::QueryWindowsFrequencies(info.baseFrequencyHz, info.maxFrequencyHz);

    // ProcessorId（非严格序列号）
    info.processorId = internal::GetProcessorIdViaWmi();

    // cpuSerial：Windows x86 基本没有可读序列号，保持为空
    info.cpuSerial = string();

#else
    // ---------- Linux ----------
    // 解析 /proc/cpuinfo
    internal::ProcCpuInfo pci = internal::ParseProcCpuinfo();
    if (!pci.vendor.empty())
    {
        info.vendor = pci.vendor;
    }
    if (!pci.modelName.empty())
    {
        info.brand = pci.modelName;
    }
    info.features = pci.flags; // 直接填 flags

    // hypervisor：x86 用 ECX bit31 检测
    info.hypervisorPresent = internal::QueryLinuxX86Hypervisor();

    // 拓扑
    internal::QueryLinuxTopology(info.logicalCpus, info.physicalCores, info.packageCount, info.numaNodes);

    // 频率
    internal::QueryLinuxFrequencies(info.baseFrequencyHz, info.maxFrequencyHz);

    // Linux 上的 ProcessorId / cpuSerial：
    // 1) x86 普遍没有；2) 某些 ARM 平台 /proc/cpuinfo 有 "Serial"
    const auto it = pci.kv.find("Serial");
    if (it != pci.kv.end())
    {
        info.cpuSerial = it->second;
    }
#endif

    // 收尾：若 vendor/brand 仍为空，尽量给出退化信息
    if (info.vendor.empty())
    {
        info.vendor = "Unknown";
    }
    if (info.brand.empty())
    {
        info.brand = "Unknown";
    }
    if (info.numaNodes == 0)
    {
        info.numaNodes = 1;
    }
    return info;
}

string MotherboardInfo::Serialize() const
{
    string s;
    s += internal::JoinKeyValue("manufacturer", manufacturer);
    s += internal::JoinKeyValue("product", product);
    s += internal::JoinKeyValue("version", version);
    s += internal::JoinKeyValue("serialNumber", serialNumber);
    s += internal::JoinKeyValue("uuid", uuid);
    s += internal::JoinKeyValue("biosVendor", biosVendor);
    s += internal::JoinKeyValue("biosVersion", biosVersion);
    s += internal::JoinKeyValue("biosDate", biosDate);
    return s;
}

MotherboardInfo GetMotherboardInfo()
{
    MotherboardInfo info;

#if defined(_WIN32)
    // 1) BaseBoard：厂商/型号/版本/序列号
    {
        const vector<wstring> props = {
            L"Manufacturer", L"Product", L"Version", L"SerialNumber"
        };
        vector<string> vals;
        const bool ok = internal::WmiQueryFirstRowStrings(
            L"SELECT Manufacturer, Product, Version, SerialNumber FROM Win32_BaseBoard",
            props, vals
        );
        if (ok && vals.size() == props.size())
        {
            info.manufacturer = vals[0];
            info.product = vals[1];
            info.version = vals[2];
            info.serialNumber = vals[3];
        }
    }

    // 2) BIOS：厂商/版本/日期
    {
        const vector<wstring> props = {
            L"Manufacturer", L"SMBIOSBIOSVersion", L"ReleaseDate"
        };
        vector<string> vals;
        const bool ok = internal::WmiQueryFirstRowStrings(
            L"SELECT Manufacturer, SMBIOSBIOSVersion, ReleaseDate FROM Win32_BIOS",
            props, vals);
        if (ok && vals.size() == props.size())
        {
            info.biosVendor = vals[0];
            info.biosVersion = vals[1];
            info.biosDate = vals[2]; // CIM_DATETIME 原样返回，例如 20240101xxxxxx.xxx+xxx
        }
    }

    // 3) UUID：Win32_ComputerSystemProduct
    {
        const vector<wstring> props = { L"UUID" };
        vector<string> vals;
        const bool ok = internal::WmiQueryFirstRowStrings(
            L"SELECT UUID FROM Win32_ComputerSystemProduct",
            props, vals);
        if (ok && !vals.empty())
        {
            info.uuid = vals[0];
        }
    }

#else
    // —— Linux: 先读 DMI(/sys/class/dmi/id)，再设备树回退 —— //
    // DMI: 主板
    info.manufacturer = internal::ReadDmi("board_vendor");
    info.product = internal::ReadDmi("board_name");
    info.version = internal::ReadDmi("board_version");
    info.serialNumber = internal::ReadDmi("board_serial");

    // DMI: UUID + BIOS
    info.uuid = internal::ReadDmi("product_uuid");
    info.biosVendor = internal::ReadDmi("bios_vendor");
    info.biosVersion = internal::ReadDmi("bios_version");
    info.biosDate = internal::ReadDmi("bios_date");

    // 设备树回补（常见于树莓派/嵌入式等）
    if (info.product.empty())
    {
        info.product = internal::ReadDeviceTree("model");
    }
    if (info.serialNumber.empty())
    {
        info.serialNumber = internal::ReadDeviceTree("serial-number");
    }
#endif

    // 规范化/兜底
    if (info.manufacturer.empty())
    {
        info.manufacturer = "Unknown";
    }
    if (info.product.empty())
    {
        info.product = "Unknown";
    }

    return info;
}

string OsInfo::Serialize() const
{
    string s;
    s += internal::JoinKeyValue("name", name);
    s += internal::JoinKeyValue("version", version);
    s += internal::JoinKeyValue("buildNumber", buildNumber);
    s += internal::JoinKeyValue("architecture", architecture);
    s += internal::JoinKeyValue("osArchitecture", osArchitecture);
    s += internal::JoinKeyValue("kernelName", kernelName);
    s += internal::JoinKeyValue("kernelRelease", kernelRelease);
    s += internal::JoinKeyValue("kernelVersion", kernelVersion);
    s += internal::JoinKeyValue("hostname", hostname);
    s += internal::JoinKeyValue("id", id);
    s += internal::JoinKeyValue("idLike", idLike);
    s += internal::JoinKeyValue("codename", codename);
    return s;
}

string GenerateHardwareId()
{
    const CpuInfo cpuInfo = GetCpuInfo();
    const MotherboardInfo motherboardInfo = GetMotherboardInfo();

    const string cpuId = cpuInfo.cpuSerial.empty() ? cpuInfo.processorId : cpuInfo.cpuSerial;
    const string motherboardId = motherboardInfo.serialNumber.empty() ? motherboardInfo.uuid : motherboardInfo.serialNumber;
    const string hardwareId = cpuId + "--" + motherboardId;
    return GB_GetSha256(hardwareId);
}

// ===== 3) GetOsInfo() 主体 =====
OsInfo GetOsInfo()
{
    OsInfo info;
    info.architecture = internal::DetectArchitecture();

#if defined(_WIN32)
    // Windows: 通过 WMI Win32_OperatingSystem 抓取信息
    info.kernelName = "Windows NT";

    const vector<wstring> props = {
        L"Caption",        // 0: 友好名称，如 "Microsoft Windows 11 Pro"
        L"Version",        // 1: 版本号，如 "10.0.22631"
        L"BuildNumber",    // 2: 内部构建号，如 "22631"
        L"OSArchitecture", // 3: "64-bit" / "32-bit"
        L"CSDVersion",     // 4: Service Pack（新系统多为空）
        L"CSName"          // 5: 主机名
    };
    vector<string> vals;
    if (internal::WmiQueryFirstRowStrings(
        L"SELECT Caption, Version, BuildNumber, OSArchitecture, CSDVersion, CSName FROM Win32_OperatingSystem",
        props, vals))
    {
        if (vals.size() == props.size())
        {
            info.name = vals[0];
            info.version = vals[1];
            info.buildNumber = vals[2];
            info.osArchitecture = vals[3];
            // CSDVersion 可按需拼接到 name 或 version；此处保留在 Serialize 外部再用
            info.hostname = vals[5];
            info.kernelVersion = info.version; // 近似等同
        }
    }

#else
    // -------- Linux --------
    // 1) uname：内核信息与主机名
    string sysname, nodename, release, version, machine;
    internal::QueryUname(sysname, nodename, release, version, machine);
    info.kernelName = sysname.empty() ? "Linux" : sysname;
    info.kernelRelease = release;
    info.kernelVersion = version;
    info.hostname = nodename;

    // 2) os-release：发行版信息
    map<string, string> kv;
    if (internal::ParseOsRelease(kv))
    {
        auto get = [&](const char* k) -> string
            {
                auto it = kv.find(k);
                return it == kv.end() ? string() : it->second;
            };

        const string prettyName = get("PRETTY_NAME");
        const string nameField = get("NAME");
        const string versionId = get("VERSION_ID");
        const string versionStr = get("VERSION");
        info.id = get("ID");
        info.idLike = get("ID_LIKE");
        info.codename = get("VERSION_CODENAME");

        // 展示名：优先 PRETTY_NAME，否则 NAME + " " + VERSION
        if (!prettyName.empty())
        {
            info.name = prettyName;
        }
        else if (!nameField.empty() || !versionStr.empty())
        {
            info.name = nameField;
            if (!versionStr.empty())
            {
                if (!info.name.empty()) info.name += " ";
                info.name += versionStr;
            }
        }

        // 机读版本：优先 VERSION_ID，否则 VERSION
        info.version = !versionId.empty() ? versionId : versionStr;
    }

    // 3) buildNumber：Linux 下使用内核发布号更有参考意义
    if (!info.kernelRelease.empty())
    {
        info.buildNumber = info.kernelRelease;
    }

    // 4) osArchitecture：可由 uname.machine 粗略映射（可选）
    if (info.osArchitecture.empty() && !machine.empty())
    {
        // 简化映射，仅用于补全展示；真实架构字段（architecture）已由 DetectArchitecture 提供
        if (machine.find("64") != string::npos || machine == "x86_64" || machine == "aarch64")
        {
            info.osArchitecture = "64-bit";
        }
        else if (machine == "i386" || machine == "i686" || machine == "armv7l")
        {
            info.osArchitecture = "32-bit";
        }
    }
#endif

    // 兜底
    if (info.name.empty())
    {
        info.name = info.kernelName.empty() ? string("Unknown OS") : info.kernelName;
    }
    if (info.version.empty())
    {
        info.version = "Unknown";
    }
    return info;
}